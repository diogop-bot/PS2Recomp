"""Ahead-of-time recompiler for a VU1 micro program. Emits C++ for ps2x_vu1_gen.h's model.

usage: python -I vu1_recompile.py [<vu1_microcode.bin> <out.cpp>] [--port ps2port.json]
                                  [--header ps2x_vu1_gen.h] [--namespace ps2x::vu1gen]

Without the two paths, they come from the "vu1" section of the port's ps2port.json
("microcode" and "program"; see ../README.md). The image is the 16 KB VU1 micro memory as the
game uploads it. The output is game-derived: keep it out of git.

The generated code follows PS2Recomp's VU1Interpreter (the reference): the same values, the same
cycle model (per-lane stall scoreboard; Q, flag and XGKICK timing), the same intra-pair rules
(both halves read old values, the upper write wins) and the branch "old VI" quirk. Instructions
the model does not cover stop the generator instead of being guessed at.
"""
import argparse
import os
import re
import struct
import sys

LANE = {0: 8, 1: 4, 2: 2, 3: 1}  # component -> VU field bit (x = 8)
FMAC_LAT = 4


def blend_imm(dest):
    return ((dest >> 3) & 1) | (((dest >> 2) & 1) << 1) | (((dest >> 1) & 1) << 2) | ((dest & 1) << 3)


def lanes_of(mask):
    return [c for c in range(4) if mask & LANE[c]]


class Usage:
    def __init__(self):
        self.vf_reads = []      # (reg, lanes)
        self.vf_write = None    # (reg, lanes)
        self.vi_reads = set()
        self.vi_writes = []
        self.acc_read = 0
        self.acc_write = 0
        self.latency = 0
        self.vf_latency = 0
        self.vi_latency = 0
        self.pipeline = None
        self.wait_q = False
        self.delays_branch = False

    def read_vf(self, reg, lanes):
        if lanes == 0:
            return
        for k, (r, l) in enumerate(self.vf_reads):
            if r == reg:
                self.vf_reads[k] = (r, l | lanes)
                return
        self.vf_reads.append((reg, lanes))

    def write_vf(self, reg, lanes):
        if reg == 0 or lanes == 0:
            return
        if self.vf_write is None:
            self.vf_write = (reg, lanes)
        elif self.vf_write[0] == reg:
            self.vf_write = (reg, self.vf_write[1] | lanes)

    def read_vi(self, reg):
        if reg:
            self.vi_reads.add(reg)

    def write_vi(self, reg):
        if reg:
            self.vi_writes.append(reg)


def special_of(word):
    return (word & 3) | ((word >> 4) & 0x7C)


def fields(word):
    return (word >> 21) & 0xF, (word >> 16) & 0x1F, (word >> 11) & 0x1F, (word >> 6) & 0x1F


# ---------------------------------------------------------------- upper

ACC_READ_OPS = set(range(0x08, 0x10)) | {0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E}
ACC_READ_SPECIAL = set(range(0x08, 0x10)) | {0x21, 0x23, 0x25, 0x27, 0x29, 0x2D}


def upper_usage(up):
    u = Usage()
    u.pipeline = 'fmac'
    u.latency = FMAC_LAT
    op = up & 0x3F
    dest, ft, fs, fd = fields(up)
    if op <= 0x2F:
        u.read_vf(fs, dest)
        u.write_vf(fd, dest)
        if op <= 0x1B:
            u.read_vf(ft, LANE[op & 3])
        elif op >= 0x28:
            u.read_vf(ft, 0xE if op == 0x2E else dest)
        if op in ACC_READ_OPS:
            u.acc_read = dest
        return u
    if op >= 0x3C:
        sp = special_of(up)
        writes_acc = sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or 0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2E
        if writes_acc:
            u.read_vf(fs, dest)
            if sp <= 0x1B:
                u.read_vf(ft, LANE[sp & 3])
            elif 0x28 <= sp <= 0x2E:
                u.read_vf(ft, 0xE if sp == 0x2E else dest)
            u.acc_write = dest
            if sp in ACC_READ_SPECIAL:
                u.acc_read = dest
        elif 0x10 <= sp <= 0x17 or sp == 0x1D:
            u.read_vf(fs, dest)
            u.write_vf(ft, dest)
        elif sp == 0x1F:
            u.read_vf(fs, 0xE)
            u.read_vf(ft, 0x1)
        elif sp not in (0x2F, 0x30):
            raise SystemExit('reserved upper %08x' % up)
        return u
    raise SystemExit('reserved upper %08x' % up)


def upper_code(up):
    """Returns (lines computing U, target, dest, writes_flags). target: ('vf', reg), ('acc',) or None."""
    op = up & 0x3F
    dest, ft, fs, fd = fields(up)
    vs, vt, acc = 'N(vf[%d])' % fs, 'N(vf[%d])' % ft, 'N(acc)'
    vsf, vtf = 'NF(vf[%d])' % fs, 'NF(vf[%d])' % ft
    q, i = '_mm_set1_ps(qnow(qp, cycle))', '_mm_set1_ps(nrmf(I))'
    qf, if_ = q, i

    def bc(b, src=vt):
        return 'BC<%d>(%s)' % (b, src)

    def add(a, b):
        return '_mm_add_ps(%s, %s)' % (a, b)

    def sub(a, b):
        return '_mm_sub_ps(%s, %s)' % (a, b)

    def mul(a, b):
        return '_mm_mul_ps(%s, %s)' % (a, b)

    yzx = '_mm_shuffle_ps(%s, %s, _MM_SHUFFLE(3, 0, 2, 1))' % (vs, vs)
    zxy = '_mm_shuffle_ps(%s, %s, _MM_SHUFFLE(3, 1, 0, 2))' % (vt, vt)
    cross = '_mm_blend_ps(%s, _mm_setzero_ps(), 8)'

    if op <= 0x2F:
        b = op & 3
        table = {
            0: add(vs, bc(b)), 1: sub(vs, bc(b)), 2: add(acc, mul(vs, bc(b))), 3: sub(acc, mul(vs, bc(b))),
            4: '_mm_max_ps(%s, %s)' % (vsf, bc(b, vtf)), 5: '_mm_min_ps(%s, %s)' % (vsf, bc(b, vtf)),
            6: mul(vs, bc(b)),
        }
        if op <= 0x1B:
            expr = table[op >> 2]
            flags = (op >> 2) not in (4, 5)
        else:
            simple = {
                0x1C: (mul(vs, q), True), 0x1D: ('_mm_max_ps(%s, %s)' % (vsf, if_), False), 0x1E: (mul(vs, i), True),
                0x1F: ('_mm_min_ps(%s, %s)' % (vsf, if_), False), 0x20: (add(vs, q), True),
                0x21: (add(acc, mul(vs, q)), True), 0x22: (add(vs, i), True), 0x23: (add(acc, mul(vs, i)), True),
                0x24: (sub(vs, q), True), 0x25: (sub(acc, mul(vs, q)), True), 0x26: (sub(vs, i), True),
                0x27: (sub(acc, mul(vs, i)), True), 0x28: (add(vs, vt), True), 0x29: (add(acc, mul(vs, vt)), True),
                0x2A: (mul(vs, vt), True), 0x2B: ('_mm_max_ps(%s, %s)' % (vsf, vtf), False), 0x2C: (sub(vs, vt), True),
                0x2D: (sub(acc, mul(vs, vt)), True), 0x2E: (cross % sub(acc, mul(yzx, zxy)), True),
                0x2F: ('_mm_min_ps(%s, %s)' % (vsf, vtf), False),
            }
            expr, flags = simple[op]
        return ['const __m128 U = %s;' % expr], ('vf', fd), dest, flags

    sp = special_of(up)
    if sp <= 0x0F or 0x18 <= sp <= 0x1B:
        b = sp & 3
        group = {0: add(vs, bc(b)), 1: sub(vs, bc(b)), 2: add(acc, mul(vs, bc(b))), 3: sub(acc, mul(vs, bc(b))),
                 6: mul(vs, bc(b))}[sp >> 2]
        return ['const __m128 U = %s;' % group], ('acc',), dest, True
    if 0x10 <= sp <= 0x13:
        scale = {0x10: None, 0x11: '0.0625f', 0x12: '0.000244140625f', 0x13: '0.000030517578125f'}[sp]
        conv = '_mm_cvtepi32_ps(_mm_castps_si128(vf[%d]))' % fs
        expr = conv if scale is None else mul(conv, '_mm_set1_ps(%s)' % scale)
        return ['const __m128 U = %s;' % expr], ('vf', ft), dest, False
    if 0x14 <= sp <= 0x17:
        scale = {0x14: '1.0f', 0x15: '16.0f', 0x16: '4096.0f', 0x17: '32768.0f'}[sp]
        return ['const __m128 U = ftoi(%s, %s);' % (vs, scale)], ('vf', ft), dest, False
    if sp == 0x1D:
        return ['const __m128 U = _mm_and_ps(%s, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));' % vsf], ('vf', ft), dest, False
    if sp == 0x1F:
        return ['clipOp(c, vf[%d], vf[%d], cycle);' % (fs, ft)], None, 0, False
    accops = {
        0x1C: mul(vs, q), 0x1E: mul(vs, i), 0x20: add(vs, q), 0x21: add(acc, mul(vs, q)), 0x22: add(vs, i),
        0x23: add(acc, mul(vs, i)), 0x24: sub(vs, q), 0x25: sub(acc, mul(vs, q)), 0x26: sub(vs, i),
        0x27: sub(acc, mul(vs, i)), 0x28: add(vs, vt), 0x29: add(acc, mul(vs, vt)), 0x2A: mul(vs, vt),
        0x2C: sub(vs, vt), 0x2D: sub(acc, mul(vs, vt)), 0x2E: cross % mul(yzx, zxy),
    }
    if sp in accops:
        return ['const __m128 U = %s;' % accops[sp]], ('acc',), dest, True
    if sp in (0x2F, 0x30):
        return [], None, 0, False
    raise SystemExit('unsupported upper %08x' % up)


# ---------------------------------------------------------------- lower

def imm11(word):
    v = word & 0x7FF
    return v - 0x800 if v & 0x400 else v


def is_lower_nop(lo):
    return lo in (0, 0x8000033C)


def lower_usage(lo):
    u = Usage()
    if is_lower_nop(lo):
        return u
    hi = (lo >> 25) & 0x7F
    dest, vft, vfs, _ = fields(lo)
    vit, vis, vid = (lo >> 16) & 0xF, (lo >> 11) & 0xF, (lo >> 6) & 0xF
    simple = {
        0x00: ('lsu', 4), 0x01: ('lsu', 1), 0x04: ('lsu', 4), 0x05: ('lsu', 1), 0x08: ('ialu', 1), 0x09: ('ialu', 1),
        0x10: ('ialu', 1), 0x12: ('ialu', 1), 0x13: ('ialu', 1), 0x11: ('fmac', 4), 0x14: ('ialu', 1),
        0x16: ('ialu', 1), 0x17: ('ialu', 1), 0x15: ('fmac', 4), 0x18: ('ialu', 1), 0x1A: ('ialu', 1),
        0x1B: ('ialu', 1), 0x1C: ('ialu', 1), 0x20: ('branch', 0), 0x21: ('branch', 1), 0x24: ('branch', 0),
        0x25: ('branch', 1), 0x28: ('branch', 0), 0x29: ('branch', 0), 0x2C: ('branch', 0), 0x2D: ('branch', 0),
        0x2E: ('branch', 0), 0x2F: ('branch', 0),
    }
    if hi in simple:
        u.pipeline, u.latency = simple[hi]
        if hi == 0x00:
            u.read_vi(vis); u.write_vf(vft, dest)
        elif hi == 0x01:
            u.read_vi(vit); u.read_vf(vfs, dest)
        elif hi == 0x04:
            u.read_vi(vis); u.write_vi(vit)
        elif hi == 0x05:
            u.read_vi(vis); u.read_vi(vit)
        elif hi in (0x08, 0x09):
            u.delays_branch = True; u.read_vi(vis); u.write_vi(vit)
        elif hi in (0x10, 0x12, 0x13):
            u.write_vi(1)
        elif hi in (0x14, 0x16, 0x17):
            u.write_vi(vit)
        elif hi in (0x18, 0x1A, 0x1B):
            u.read_vi(vis); u.write_vi(vit)
        elif hi == 0x1C:
            u.write_vi(vit)
        elif hi == 0x21:
            u.write_vi(vit)
        elif hi == 0x24:
            u.read_vi(vis)
        elif hi == 0x25:
            u.read_vi(vis); u.write_vi(vit)
        elif hi in (0x28, 0x29):
            u.read_vi(vis); u.read_vi(vit)
        elif hi in (0x2C, 0x2D, 0x2E, 0x2F):
            u.read_vi(vis)
        return u
    if hi != 0x40:
        raise SystemExit('reserved lower %08x' % lo)
    direct = lo & 0x3F
    if direct in (0x30, 0x31, 0x34, 0x35):
        u.pipeline, u.latency, u.delays_branch = 'ialu', 1, True
        u.read_vi(vis); u.read_vi(vit); u.write_vi(vid)
        return u
    if direct == 0x32:
        u.pipeline, u.latency, u.delays_branch = 'ialu', 1, True
        u.read_vi(vis); u.write_vi(vit)
        return u
    if direct < 0x3C:
        raise SystemExit('reserved lower %08x' % lo)
    sp = special_of(lo)
    fsf, ftf = (lo >> 21) & 3, (lo >> 23) & 3
    if sp in (0x30, 0x31):
        u.pipeline, u.latency = 'fmac', 4
        u.read_vf(vfs, 0xF if sp == 0x31 else dest); u.write_vf(vft, dest)
    elif sp in (0x34, 0x36):
        u.pipeline, u.latency, u.vi_latency, u.delays_branch = 'lsu', 4, 1, True
        u.read_vi(vis); u.write_vi(vis); u.write_vf(vft, dest)
    elif sp in (0x35, 0x37):
        u.pipeline, u.latency, u.delays_branch = 'lsu', 1, True
        u.read_vi(vit); u.write_vi(vit); u.read_vf(vfs, dest)
    elif sp == 0x38:
        u.pipeline, u.latency = 'fdiv', 7
        u.read_vf(vfs, LANE[fsf]); u.read_vf(vft, LANE[ftf])
    elif sp == 0x39:
        u.pipeline, u.latency = 'fdiv', 7
        u.read_vf(vft, LANE[ftf])
    elif sp == 0x3A:
        u.pipeline, u.latency = 'fdiv', 13
        u.read_vf(vfs, LANE[fsf]); u.read_vf(vft, LANE[ftf])
    elif sp == 0x3B:
        u.pipeline, u.wait_q = 'fdiv', True
    elif sp == 0x3C:
        u.pipeline, u.latency, u.delays_branch = 'ialu', 1, True
        u.read_vf(vfs, LANE[fsf]); u.write_vi(vit)
    elif sp == 0x3D:
        u.pipeline, u.latency = 'fmac', 4
        u.read_vi(vis); u.write_vf(vft, dest)
    elif sp == 0x3E:
        u.pipeline, u.latency = 'lsu', 4
        u.read_vi(vis); u.write_vi(vit)
    elif sp == 0x3F:
        u.pipeline, u.latency = 'lsu', 1
        u.read_vi(vis); u.read_vi(vit)
    elif sp in (0x68, 0x69):
        u.pipeline, u.latency = 'ialu', 1
        u.write_vi(vit)
    elif sp == 0x6C:
        u.pipeline, u.latency = 'xgkick', 2
        u.read_vi(vis)
    else:
        raise SystemExit('unsupported lower %08x (special %02x)' % (lo, sp))
    return u


BRANCH_OPS = {0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F}


def is_branch(lo):
    return not is_lower_nop(lo) and ((lo >> 25) & 0x7F) in BRANCH_OPS


def lower_code(lo, pc):
    """Returns (lines, vf_result) where vf_result is (reg, dest) of a value left in __m128 Lv, plus
    branch info (kind, cond_expr, target) for branch instructions."""
    lines, vf_result, branch = [], None, None
    if is_lower_nop(lo):
        return lines, vf_result, branch
    hi = (lo >> 25) & 0x7F
    dest, vft, vfs, _ = fields(lo)
    vit, vis, vid = (lo >> 16) & 0xF, (lo >> 11) & 0xF, (lo >> 6) & 0xF
    imm = imm11(lo)
    comp = 0 if dest & 8 else 1 if dest & 4 else 2 if dest & 2 else 3

    def set_vi(reg, expr):
        if reg:
            lines.append('vi[%d] = static_cast<int16_t>(%s);' % (reg, expr))

    if hi == 0x00:
        lines.append('const __m128 Lv = loadQ(mem, dataAddr(vi[%d] + %d));' % (vis, imm))
        vf_result = (vft, dest)
    elif hi == 0x01:
        lines.append('storeQ(mem, dataAddr(vi[%d] + %d), vf[%d], %du);' % (vit, imm, vfs, dest))
    elif hi == 0x04:
        set_vi(vit, 'ilw(mem, dataAddr(vi[%d] + %d), %d)' % (vis, imm, comp))
    elif hi == 0x05:
        lines.append('isw(mem, dataAddr(vi[%d] + %d), vi[%d], %du);' % (vis, imm, vit, dest))
    elif hi in (0x08, 0x09):
        imm15 = (lo & 0x7FF) | ((lo >> 10) & 0x7800)
        set_vi(vit, 'vi[%d] %s %d' % (vis, '+' if hi == 0x08 else '-', imm15))
    elif hi == 0x10:
        set_vi(1, '(clipNow(c, cycle) & 0xFFFFFFu) == 0x%Xu ? 1 : 0' % (lo & 0xFFFFFF))
    elif hi == 0x11:
        lines.append('c.workingClip = 0x%Xu; pushClip(c, c.workingClip, cycle);' % (lo & 0xFFFFFF))
    elif hi == 0x12:
        set_vi(1, '(clipNow(c, cycle) & 0x%Xu) != 0u ? 1 : 0' % (lo & 0xFFFFFF))
    elif hi == 0x13:
        set_vi(1, '(clipNow(c, cycle) | 0x%Xu) == 0xFFFFFFu ? 1 : 0' % (lo & 0xFFFFFF))
    elif hi == 0x1A:
        set_vi(vit, 'macNow(c, cycle) & static_cast<uint32_t>(static_cast<uint16_t>(vi[%d]))' % vis)
    elif hi == 0x18:
        set_vi(vit, '(macNow(c, cycle) & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(vi[%d])) ? 1 : 0' % vis)
    elif hi == 0x1B:
        set_vi(vit, 'macNow(c, cycle) | static_cast<uint32_t>(static_cast<uint16_t>(vi[%d]))' % vis)
    elif hi == 0x1C:
        set_vi(vit, 'clipNow(c, cycle) & 0x0FFFu')
    elif hi in (0x20, 0x21):
        target = (pc + 8 + imm * 8) & 0x3FFF
        if hi == 0x21:
            set_vi(vit, str((pc + 16) // 8))
        branch = ('always', None, target)
    elif hi in (0x24, 0x25):
        lines.append('const uint32_t jt = (static_cast<uint32_t>(static_cast<uint16_t>(rb(%d))) * 8u) & 0x3FFFu;' % vis)
        if hi == 0x25:
            set_vi(vit, str((pc + 16) // 8))
        branch = ('jump', None, None)
    elif hi in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
        target = (pc + 8 + imm * 8) & 0x3FFF
        s, t = 'static_cast<int16_t>(rb(%d))' % vis, 'static_cast<int16_t>(rb(%d))' % vit
        cond = {0x28: '%s == %s' % (s, t), 0x29: '%s != %s' % (s, t), 0x2C: '%s < 0' % s, 0x2D: '%s > 0' % s,
                0x2E: '%s <= 0' % s, 0x2F: '%s >= 0' % s}[hi]
        lines.append('const bool tk = %s;' % cond)
        branch = ('cond', 'tk', target)
    elif hi == 0x40:
        direct = lo & 0x3F
        if direct in (0x30, 0x31, 0x34, 0x35):
            o = {0x30: '+', 0x31: '-', 0x34: '&', 0x35: '|'}[direct]
            set_vi(vid, 'vi[%d] %s vi[%d]' % (vis, o, vit))
        elif direct == 0x32:
            imm5 = (lo >> 6) & 0x1F
            imm5 = imm5 - 32 if imm5 & 0x10 else imm5
            set_vi(vit, 'vi[%d] + (%d)' % (vis, imm5))
        else:
            sp = special_of(lo)
            fsf, ftf = (lo >> 21) & 3, (lo >> 23) & 3
            if sp == 0x30:
                lines.append('const __m128 Lv = vf[%d];' % vfs)
                vf_result = (vft, dest)
            elif sp == 0x31:
                lines.append('const __m128 Lv = _mm_shuffle_ps(vf[%d], vf[%d], _MM_SHUFFLE(0, 3, 2, 1));' % (vfs, vfs))
                vf_result = (vft, dest)
            elif sp == 0x34:
                lines.append('const __m128 Lv = loadQ(mem, dataAddr(vi[%d]));' % vis)
                set_vi(vis, 'vi[%d] + 1' % vis)
                vf_result = (vft, dest)
            elif sp == 0x35:
                lines.append('storeQ(mem, dataAddr(vi[%d]), vf[%d], %du);' % (vit, vfs, dest))
                set_vi(vit, 'vi[%d] + 1' % vit)
            elif sp == 0x36:
                set_vi(vis, 'vi[%d] - 1' % vis)
                lines.append('const __m128 Lv = loadQ(mem, dataAddr(vi[%d]));' % vis)
                vf_result = (vft, dest)
            elif sp == 0x37:
                set_vi(vit, 'vi[%d] - 1' % vit)
                lines.append('storeQ(mem, dataAddr(vi[%d]), vf[%d], %du);' % (vit, vfs, dest))
            elif sp in (0x38, 0x39, 0x3A):
                latency = 13 if sp == 0x3A else 7
                if sp == 0x38:
                    lines.append('{ const float n = nrmf(lane(vf[%d], %d)); const float d = nrmf(lane(vf[%d], %d));' % (vfs, fsf, vft, ftf))
                    lines.append('  float r; if (d == 0.0f) r = (std::signbit(n) != std::signbit(d)) ? -3.40282347e38f : 3.40282347e38f; else r = n / d;')
                elif sp == 0x39:
                    lines.append('{ const float r = std::sqrt(std::fabs(nrmf(lane(vf[%d], %d))));' % (vft, ftf))
                else:
                    lines.append('{ const float n = nrmf(lane(vf[%d], %d)); const float d = std::sqrt(std::fabs(nrmf(lane(vf[%d], %d))));' % (vfs, fsf, vft, ftf))
                    lines.append('  float r; if (d != 0.0f) r = n / d; else r = std::signbit(n) ? -3.40282347e38f : 3.40282347e38f;')
                lines.append('  qissue(qp, r, cycle, %du); }' % latency)
            elif sp == 0x3B:
                pass  # WAITQ only stalls
            elif sp == 0x3C:
                set_vi(vit, 'static_cast<int16_t>(laneBits(vf[%d], %d) & 0xFFFFu)' % (vfs, fsf))
            elif sp == 0x3D:
                lines.append('const __m128 Lv = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int16_t>(vi[%d])));' % vis)
                vf_result = (vft, dest)
            elif sp == 0x3E:
                set_vi(vit, 'ilw(mem, dataAddr(vi[%d]), %d)' % (vis, comp))
            elif sp == 0x3F:
                lines.append('isw(mem, dataAddr(vi[%d]), vi[%d], %du);' % (vis, vit, dest))
            elif sp == 0x68:
                set_vi(vit, 'c.top & 0x3FFu')
            elif sp == 0x69:
                set_vi(vit, 'c.itop & 0x3FFu')
            elif sp == 0x6C:
                lines.append('xgkickDone = xgkick(c, static_cast<uint32_t>(static_cast<uint16_t>(vi[%d])), cycle);' % vis)
            else:
                raise SystemExit('unsupported lower %08x' % lo)
    else:
        raise SystemExit('unsupported lower %08x' % lo)
    return lines, vf_result, branch


# ---------------------------------------------------------------- pairs

# Lanes of a register that every instruction of the program writes together always hold the same
# ready cycle, so one local stands for the group (named after its first lane): reads take one value
# instead of up to four and writes store one. VF_REP maps (reg, comp) to the group's first lane,
# GROUP_LANES (reg, first lane) to the group; reg 'acc' is the accumulator. Lanes never written
# keep their own locals. Filled by lane_groups() before any code is generated.
VF_REP = {}
GROUP_LANES = {}


def lane_groups(pairs):
    masks = {}
    for lo, up in pairs:
        uu = upper_usage(up)
        lu = Usage() if (up >> 31) & 1 else lower_usage(lo)
        for usage in (uu, lu):
            if usage.vf_write:
                masks.setdefault(usage.vf_write[0], set()).add(usage.vf_write[1])
        if uu.acc_write:
            masks.setdefault('acc', set()).add(uu.acc_write)
    for reg, written in masks.items():
        groups = {}
        for comp in range(4):
            member = tuple(sorted(m for m in written if m & LANE[comp]))
            if member:
                groups.setdefault(member, []).append(comp)
        for comps in groups.values():
            for comp in comps:
                VF_REP[(reg, comp)] = comps[0]
            GROUP_LANES[(reg, comps[0])] = comps


# MAC flag pushes no read can observe. A flag read (FMAND/FMEQ/FMOR, FSxx) takes the newest entry
# whose 4-cycle latency has passed, so a push is dead when, in straight-line code after it, another
# push follows and four more pairs pass (at least four cycles) before any flag read, branch or
# E-bit. Pushes in delay slots or on branch pairs are kept. Filled by mac_elision().
MAC_READS = {0x14, 0x16, 0x17, 0x18, 0x1A, 0x1B}
DEAD_MAC = set()


def mac_elision(pairs):
    n = len(pairs)
    pushes, stops = [], []
    for lo, up in pairs:
        ibit = (up >> 31) & 1
        _, _, udest, uflags = upper_code(up)
        pushes.append(bool(uflags and udest))
        reads = not ibit and not is_lower_nop(lo) and ((lo >> 25) & 0x7F) in MAC_READS
        stops.append(reads or (not ibit and is_branch(lo)) or bool((up >> 30) & 1))
    for k in range(n):
        if not pushes[k] or stops[k] or (k > 0 and stops[k - 1]):
            continue
        j = k + 1
        while j < n and not stops[j] and not pushes[j]:
            j += 1
        if j < n and not stops[j] and j + 4 < n and not any(stops[j + 1:j + 5]):
            DEAD_MAC.add(k)


def vf_ready(reg, comp):
    return 'vfReady[%d][%d]' % (reg, VF_REP.get((reg, comp), comp))


def acc_ready(comp):
    return 'accReady[%d]' % VF_REP.get(('acc', comp), comp)


def max_tree(terms):
    if len(terms) == 1:
        return terms[0]
    half = (len(terms) + 1) // 2
    return 'umax(%s, %s)' % (max_tree(terms[:half]), max_tree(terms[half:]))


def pair_code(pairs, k, pc):
    lo, up = pairs[k]
    ibit = (up >> 31) & 1
    uu = upper_usage(up)
    lu = Usage() if ibit else lower_usage(lo)
    out = []

    # Stall: the interpreter's calculatePairReadyCycle.
    reads = []
    for usage in (uu, lu):
        for reg, lanes in usage.vf_reads:
            if reg:
                reads += [vf_ready(reg, comp) for comp in lanes_of(lanes)]
        reads += ['viReady[%d]' % r for r in sorted(usage.vi_reads)]
        reads += [acc_ready(comp) for comp in lanes_of(usage.acc_read)]
    if lu.pipeline == 'fdiv' or lu.wait_q:
        reads.append('(qp.pending ? qp.ready : 0u)')
    if lu.pipeline == 'xgkick':
        reads.append('xgkickDone')
    reads = list(dict.fromkeys(reads))
    stall = None
    if reads:
        # A balanced max of the ready times, then the cycle: every pair's cycle depends on the
        # previous pair's, so a chain of maxes through it made the whole program one long
        # dependency chain for the host CPU.
        stall = 'cycle = umax(cycle, %s);' % max_tree(reads)

    ulines, utarget, udest, uflags = upper_code(up)
    out += ulines
    llines, lvf, branch = ([], None, None) if ibit else lower_code(lo, pc)

    written_vi = lu.vi_writes[0] if lu.vi_writes else 0
    if written_vi and lu.delays_branch:
        out.append('const int32_t oldVi = vi[%d];' % written_vi)
    out += llines

    upper_reg = utarget[1] if utarget and utarget[0] == 'vf' else 0
    if utarget and utarget[0] == 'vf' and upper_reg and udest:
        out.append('vf[%d] = %s;' % (upper_reg, 'U' if udest == 0xF else '_mm_blend_ps(vf[%d], U, %d)' % (upper_reg, blend_imm(udest))))
    elif utarget and utarget[0] == 'acc' and udest:
        out.append('acc = %s;' % ('U' if udest == 0xF else '_mm_blend_ps(acc, U, %d)' % blend_imm(udest)))
    if uflags and udest and k not in DEAD_MAC:
        out.append('pushMac(c, U, %du, cycle);' % udest)
    lower_suppressed = lvf is not None and upper_reg and lvf[0] == upper_reg and udest
    if lvf is not None and lvf[0] and lvf[1] and not lower_suppressed:
        reg, dest = lvf
        out.append('vf[%d] = %s;' % (reg, 'Lv' if dest == 0xF else '_mm_blend_ps(vf[%d], Lv, %d)' % (reg, blend_imm(dest))))
    if ibit:
        out.append('{ uint32_t bits = 0x%08Xu; float f; std::memcpy(&f, &bits, 4); I = nrmf(f); }' % lo)

    # Ready cycles: the interpreter's markPairWrites.
    marks = []
    if lu.vf_write and not (upper_reg and lu.vf_write[0] == upper_reg and udest):
        reg, lanes = lu.vf_write
        lat = lu.vf_latency or lu.latency
        marks += [(vf_ready(reg, comp), lat) for comp in lanes_of(lanes)]
    if uu.vf_write:
        reg, lanes = uu.vf_write
        marks += [(vf_ready(reg, comp), FMAC_LAT) for comp in lanes_of(lanes)]
    for reg in lu.vi_writes:
        marks.append(('viReady[%d]' % reg, lu.vi_latency or lu.latency))
    marks += [(acc_ready(comp), 1) for comp in lanes_of(uu.acc_write)]
    marks = list(dict.fromkeys(marks))

    core = out
    bb = ('bbReg = %d; bbVal = oldVi;' % written_vi) if written_vi and lu.delays_branch else 'bbReg = 0;'
    slow = ([stall] if stall else []) + core + ['%s = cycle + %d;' % m for m in marks] + [bb, 'cycle += 1;']
    # Timing for the static schedule (static_blocks): the ready times read, the ones written, and
    # the Q pipe ('Q': the divide result, written with its latency) and XGKICK ('X': written with
    # a length known only at run time).
    timing = {'reads': reads, 'marks': marks, 'core': core + [bb]}
    # Register writes for the finite-value tracking in fast copies (see FINITE_NOTE): (reg or
    # 'acc', dest, finite) for the upper result, (reg, dest, move source or None) for the lower.
    timing['up'] = None
    if utarget and udest and (utarget[0] == 'acc' or upper_reg):
        ftoi = (up & 0x3F) >= 0x3C and 0x14 <= special_of(up) <= 0x17
        timing['up'] = ('acc' if utarget[0] == 'acc' else upper_reg, udest, not ftoi)
    timing['lo'] = None
    if lvf is not None and lvf[0] and lvf[1] and not lower_suppressed:
        hi = (lo >> 25) & 0x7F
        move = hi == 0x40 and (lo & 0x3F) >= 0x3C and special_of(lo) in (0x30, 0x31)
        timing['lo'] = (lvf[0], lvf[1], ((lo >> 11) & 0x1F) if move else None)
    if lu.pipeline == 'fdiv' and not lu.wait_q:
        timing['marks'] = marks + [('Q', lu.latency)]
    if lu.pipeline == 'xgkick':
        timing['marks'] = marks + [('X', None)]
    return slow, branch, (up >> 30) & 1, timing


# Region sizes, in pairs. MSVC silently stops optimizing a function past some size (the whole
# program as one function compiled like /Od), so the program is split into regions of about
# REGION_TARGET pairs, each a function of its own. A loop up to LOOP_KEEP pairs long is never
# cut, so the hot inner loops run without leaving their function.
REGION_TARGET = 64
LOOP_KEEP = 160

NAME_SUBS = [
    (re.compile(r'\bvfReady\[(\d+)\]\[(\d+)\]'), r'vfR_\1_\2'),
    (re.compile(r'\bviReady\[(\d+)\]'), r'viR_\1'),
    (re.compile(r'\baccReady\[(\d+)\]'), r'accR_\1'),
    (re.compile(r'\bvf\[(\d+)\]'), r'vf_\1'),
    (re.compile(r'\bvi\[(\d+)\]'), r'vi_\1'),
    (re.compile(r'\brb\(0\)'), '0'),
    (re.compile(r'\brb\((\d+)\)'), r'(bbReg == \1 ? bbVal : vi_\1)'),
]
STATE_NAME = re.compile(r'\b(vf_\d+|vi_\d+|vfR_\d+_\d+|viR_\d+|accR_\d+)\b')


def partition(pairs, n):
    """Contiguous regions [a, b) of pairs, cut only where no kept loop spans the cut."""
    info = [pair_code(pairs, k, 8 * k)[1:] for k in range(n)]
    forbidden = [False] * (n + 1)  # forbidden[j]: no region may start at pair j
    for k in range(n):
        branch = info[k][0]
        targets = [branch[2]] if branch and branch[2] is not None else []
        if (branch or info[k][1]) and k + 1 < n and info[k + 1][0] and info[k + 1][0][2] is not None:
            targets.append(info[k + 1][0][2])
        for t in targets:
            t //= 8
            last = k + 1  # the delay slot belongs with its branch
            if t <= k and last - t + 1 <= LOOP_KEEP:
                for j in range(t + 1, last + 1):
                    forbidden[j] = True
    regions, start = [], 0
    for j in range(1, n):
        if j - start >= REGION_TARGET and not forbidden[j]:
            regions.append((start, j))
            start = j
    regions.append((start, n))
    return regions


def state_decl(name):
    """The load into a region local and the store back to the state, for one state name."""
    if name.startswith('vfR_'):
        r, comp = (int(x) for x in name[4:].split('_'))
        lanes = ['s.vfReady[%d][%d]' % (r, c) for c in GROUP_LANES.get((r, comp), [comp])]
        return ('uint32_t %s = %s;' % (name, max_tree(lanes)),
                ' '.join('%s = %s;' % (lane, name) for lane in lanes))
    if name.startswith('viR_'):
        return 'uint32_t %s = s.viReady[%s];' % (name, name[4:]), 's.viReady[%s] = %s;' % (name[4:], name)
    if name.startswith('accR_'):
        comp = int(name[5:])
        lanes = ['s.accReady[%d]' % c for c in GROUP_LANES.get(('acc', comp), [comp])]
        return ('uint32_t %s = %s;' % (name, max_tree(lanes)),
                ' '.join('%s = %s;' % (lane, name) for lane in lanes))
    if name.startswith('vf_'):
        return '__m128 %s = s.vf[%s];' % (name, name[3:]), 's.vf[%s] = %s;' % (name[3:], name)
    return 'int32_t %s = s.vi[%s];' % (name, name[3:]), 's.vi[%s] = %s;' % (name[3:], name)


def state_key(name):
    parts = name.split('_')
    return (parts[0], [int(x) for x in parts[1:]])


# ---------------------------------------------------------------- static schedule
# Inside a straight run of pairs, the stalls depend only on the ready times read before the run
# writes them. When each of those is early enough not to stall (ready <= the cycle its first
# reader issues at in a stall-free schedule), every stall in the run is fixed, so the generator
# works them out ahead of time: the run's fast copy (F_xxxx) has no scoreboard reads, sets the
# cycle only where a pair uses it (Q, flags, XGKICK) and writes each ready time once, at the end.
# The check runs where the run starts (L_xxxx); when it fails, the original code runs instead.
# A fast run that ends by entering another run whose check it can already decide goes straight
# to its fast copy. The cycle counter, flags and Q come out the same either way.

Q_TERM = '(qp.pending ? qp.ready : 0u)'


def term_key(term):
    return 'Q' if term == Q_TERM else 'X' if term == 'xgkickDone' else term


def find_leaders(info, n, regions):
    """Pairs where a run starts: region starts, branch targets, and the pair after a delay slot."""
    leaders = {a for a, _ in regions}
    for k in range(n):
        _, branch, ebit, _ = info[k]
        if branch or ebit:
            if branch and branch[2] is not None:
                leaders.add(branch[2] // 8)
            if k + 2 < n:
                leaders.add(k + 2)
    return leaders


class Run:
    pass


def build_run(info, t, end, leaders):
    """The run from leader t: its pairs (a branch's delay slot included), exits and schedule.
    Returns None when the run can't be scheduled statically."""
    seq, k = [], t
    while True:
        _, branch, ebit, _ = info[k]
        seq.append(k)
        if branch or ebit:
            _, slot_branch, slot_ebit, _ = info[k + 1]
            if slot_branch or slot_ebit:
                return None
            seq.append(k + 1)
            if ebit:
                exits = [('exit', (8 * k + 16) & 0x3FFF)]
            elif branch[0] == 'always':
                exits = [('go', branch[2])]
            elif branch[0] == 'cond':
                exits = [('cond', branch[2]), ('go', (8 * k + 16) & 0x3FFF)]
            else:
                exits = [('jump', None)]
            break
        if k + 1 >= end or (k + 1) in leaders:
            exits = [('go', 8 * (k + 1))]
            break
        k += 1
    known, entry, offsets, d = {}, {}, [], 0
    for k in seq:
        timing = info[k][3]
        inside = [known[term_key(r)] for r in timing['reads'] if term_key(r) in known]
        if any(v is None for v in inside):
            return None  # waits for an XGKICK started in the run
        dk = max([d] + inside)
        for r in timing['reads']:
            key = term_key(r)
            if key not in known:
                entry[key] = min(entry.get(key, dk), dk)
        for var, lat in timing['marks']:
            known[var] = None if lat is None else dk + lat
        offsets.append(dk)
        d = dk + 1
    run = Run()
    run.start, run.seq, run.exits = t, seq, exits
    run.known, run.entry, run.offsets, run.length = known, entry, offsets, d
    return run


def entry_check(run):
    terms = []
    for key, s in sorted(run.entry.items()):
        rhs = 'cycle' if s == 0 else 'cycle + %du' % s
        if key == 'Q':
            terms.append('(!qp.pending | (qp.ready <= %s))' % rhs)
        elif key == 'X':
            terms.append('(xgkickDone <= %s)' % rhs)
        else:
            terms.append('(%s <= %s)' % (key, rhs))
    return ' & '.join(terms)


def decided(src, dst):
    """True when leaving src's fast copy, dst's check is known to pass."""
    for key, s in dst.entry.items():
        if key in src.known:
            v = src.known[key]
            if v is None or v > src.length + s:
                return False
        elif not (key in src.entry and src.entry[key] <= src.length + s):
            return False
    return True


CYCLE_USE = re.compile(r'\bcycle\b')

# FINITE_NOTE: N()/NF() turn Inf/NaN (and for NF, denormal) bit patterns into what the VU would
# compute with. A register holding an FMAC result never has those: its operands were normalized,
# run() rounds toward zero (an overflow gives +-max, not Inf) with FTZ, so no Inf, NaN or denormal
# comes out. Inside a fast copy, which is only entered at its start, the generator knows which
# registers hold such results (written by an upper op other than FTOI, or moved from one, with a
# partial write only over a register that already did) and reads them without the normalization.
# vf0 is the constant (0, 0, 0, 1).
NORMALIZED_READ = re.compile(r'\bNF?\((vf\[(\d+)\]|acc)\)')


def skip_normalize(line, finite):
    def sub(m):
        key = 'acc' if m.group(1) == 'acc' else int(m.group(2))
        return m.group(1) if key in finite else m.group(0)
    return NORMALIZED_READ.sub(sub, line)


def track_finite(finite, timing):
    new = set(finite)
    up, lo = timing['up'], timing['lo']
    for reg, dest, ok in ([up] if up else []) + ([(lo[0], lo[1], lo[2] is not None and lo[2] in finite)] if lo else []):
        if ok and (dest == 0xF or reg in finite):
            new.add(reg)
        else:
            new.discard(reg)
    return new


VF_WRITE = re.compile(r'\bvf\[(\d+)\] = ')
NORMALIZED_VF = re.compile(r'\b(NF?)\(vf\[(\d+)\]\)')


def fast_copy(run, info, edge):
    """The run's fast copy. edge(pc) gives (code entering the run at pc, the run it enters
    directly through its fast copy, or None).
    - The ready times the run writes are stored on the way out, except where the next fast copy
      stores all of them again before anything can read them (the loop back edge, mostly).
    - A run that loops to itself normalizes the registers it reads but never writes once, at
      F_xxxx, and loops to F2_xxxx."""
    start = 8 * run.start
    written = {v for v in run.known if v not in ('Q', 'X')}
    marks = ['%s = cycle0 + %du;' % (var, run.known[var]) for var in run.known if var in written]
    targets = [t for kind, t in run.exits if kind in ('cond', 'go')]
    self_loop = start in targets and decided(run, run)

    def exit_code(target):
        if self_loop and target == start:
            return ['goto F2_%04X;' % start]
        code, dst = edge(target)
        if dst is not None and written <= {v for v in dst.known if v not in ('Q', 'X')}:
            return [code]
        return marks + [code]

    tail = ['cycle = cycle0 + %du;' % run.length]
    kind = run.exits[0][0]
    if kind == 'exit':
        tail += marks + ['c.pc = 0x%04Xu;' % run.exits[0][1], '{ next = kExit; goto leave; }']
    elif kind == 'jump':
        tail += marks + ['{ next = jt; goto leave; }']
    elif kind == 'cond':
        tail += ['if (tk)', '{'] + ['    ' + line for line in exit_code(run.exits[0][1])] + ['}']
        tail += exit_code(run.exits[1][1])
    else:
        tail += exit_code(run.exits[0][1])

    cores, finite = [], {0}
    for k in run.seq:
        cores.append([skip_normalize(line, finite) for line in info[k][3]['core']])
        finite = track_finite(finite, info[k][3])
    shadows = []
    if self_loop:
        written_vf = {int(m) for core in cores for line in core for m in VF_WRITE.findall(line)}
        for core in cores:
            for line in core:
                for fn, reg in NORMALIZED_VF.findall(line):
                    if int(reg) not in written_vf and (fn, reg) not in shadows:
                        shadows.append((fn, reg))
        names = {(fn, reg): '%svf_%s' % ('n' if fn == 'N' else 'nf', reg) for fn, reg in shadows}
        cores = [[NORMALIZED_VF.sub(lambda m: names.get((m.group(1), m.group(2)), m.group(0)), line)
                  for line in core] for core in cores]

    out = ['F_%04X:' % start, '{']
    if self_loop:
        if shadows:
            out.append('    __m128 %s;' % ', '.join('%svf_%s' % ('n' if fn == 'N' else 'nf', reg) for fn, reg in shadows))
            out += ['    %svf_%s = %s(vf[%s]);' % ('n' if fn == 'N' else 'nf', reg, fn, reg) for fn, reg in shadows]
        out.append('F2_%04X:' % start)
    out.append('    cycle0 = cycle;')
    depth = 1
    for idx, k in enumerate(run.seq):
        core = cores[idx]
        pad = '    ' * depth
        if any(CYCLE_USE.search(line) for line in core):
            out.append(pad + 'cycle = cycle0 + %du;' % run.offsets[idx])
        out.append(pad + '{   // 0x%04X' % (8 * k))
        out += [pad + '    ' + line for line in core]
        if info[k][1] or info[k][2]:
            depth += 1  # the delay slot and the exit stay inside the branch's scope (tk, jt)
        else:
            out.append(pad + '}')
    out += ['    ' * depth + line for line in tail]
    while depth > 1:
        depth -= 1
        out.append('    ' * depth + '}')
    out.append('}')
    return out


def main():
    ap = argparse.ArgumentParser(description='Recompile a VU1 micro program into C++.')
    ap.add_argument('paths', nargs='*', metavar='PATH', help='<vu1_microcode.bin> <out.cpp>')
    ap.add_argument('--port', default=None, help='ps2port.json (when the paths are not given)')
    ap.add_argument('--header', default='ps2x_vu1_gen.h', help='the model header the output includes')
    ap.add_argument('--namespace', default='ps2x::vu1gen', help="the model header's namespace")
    args = ap.parse_args()
    if len(args.paths) == 2:
        src, dst = args.paths
    elif not args.paths:
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        import ps2port
        port = ps2port.load(args.port)
        vu1 = port.section('vu1')
        if not vu1.get('microcode') or not vu1.get('program'):
            sys.exit('%s has no vu1.microcode / vu1.program' % port.path)
        src, dst = port.resolve(vu1['microcode']), port.resolve(vu1['program'])
    else:
        ap.error('give both <vu1_microcode.bin> and <out.cpp>, or neither')
    code = open(src, 'rb').read()
    n = len(code) // 8
    pairs = [struct.unpack_from('<II', code, 8 * k) for k in range(n)]
    lane_groups(pairs)
    mac_elision(pairs)
    regions = partition(pairs, n)
    region_of = [0] * n
    for i, (a, b) in enumerate(regions):
        for k in range(a, b):
            region_of[k] = i
    info = [pair_code(pairs, k, 8 * k) for k in range(n)]
    leaders = find_leaders(info, n, regions)
    runs = {}
    for a, b in regions:
        for t in range(a, b):
            if t in leaders:
                run = build_run(info, t, b, leaders)
                if run:
                    runs[t] = run
    fast_pairs = sum(len(r.seq) for r in runs.values())

    out = ['// Generated by vu1_recompile.py from %s. Do not edit.' % os.path.basename(src),
           '// Derived from game code: not tracked in git.',
           '#include "%s"' % args.header, '', '#include <cmath>', '', 'namespace %s' % args.namespace, '{']
    out.append('const uint32_t kProgramSize = %d;' % len(code))
    out.append('const uint8_t kProgram[] = {')
    for i in range(0, len(code), 32):
        out.append('    ' + ', '.join('0x%02X' % b for b in code[i:i + 32]) + ',')
    out.append('};')
    out += ['',
            'namespace',
            '{',
            '// Machine state between regions. Inside a region, the registers, timing and scoreboard entries',
            '// it uses live in locals (MSVC treats every store to mem as possibly aliasing memory, so',
            '// members would be reloaded after each one).',
            'struct St',
            '{',
            '    __m128 vf[32];',
            '    __m128 acc;',
            '    int32_t vi[16];',
            '    float I;',
            '    QPipe qp;',
            '    uint32_t cycle, xgkickDone;',
            '    uint32_t vfReady[32][4], viReady[16], accReady[4];',
            '    int32_t bbReg, bbVal;',
            '};',
            'constexpr uint32_t kExit = 0xFFFFFFFFu;',
            '']

    for ri, (a, b) in enumerate(regions):
        def go(target, ri=ri):
            t = target // 8
            if target % 8 == 0 and t < n and region_of[t] == ri:
                return 'goto L_%04X;' % target
            return '{ next = 0x%04Xu; goto leave; }' % target

        def edge(target, src, ri=ri):
            t = target // 8
            if target % 8 == 0 and t < n and region_of[t] == ri:
                if t in runs and decided(src, runs[t]):
                    return 'goto F_%04X;' % target, runs[t]
                return 'goto L_%04X;' % target, None
            return '{ next = 0x%04Xu; goto leave; }' % target, None

        body = []
        for k in range(a, b):
            pc = 8 * k
            pbody, branch, ebit, _ = info[k]
            body.append('L_%04X:' % pc)
            if k in runs:
                check = entry_check(runs[k])
                body.append(('if (%s) goto F_%04X;' % (check, pc)) if check else 'goto F_%04X;' % pc)
            body.append('{')
            body += ['    ' + line for line in pbody]
            if branch or ebit:
                if k + 1 >= n:
                    raise SystemExit('branch or E-bit in the last pair')
                slot_body, slot_branch, slot_ebit, _ = info[k + 1]
                if slot_ebit or (slot_branch and not branch):
                    raise SystemExit('branch or E-bit in a delay slot at 0x%04x' % (pc + 8))
                if branch and ebit:
                    raise SystemExit('branch with an E-bit at 0x%04x' % pc)
                for bb in (branch, slot_branch):
                    if bb and bb[2] is not None and bb[2] >= 8 * n:
                        raise SystemExit('branch near 0x%04x leaves the program (0x%04x)' % (pc, bb[2]))
                if slot_branch:
                    # A branch in the delay slot: as in the interpreter, when taken it replaces the
                    # pending branch (its own delay slot runs, then its target); when not taken, the
                    # first branch proceeds as usual.
                    if branch[0] == 'jump' or slot_branch[0] == 'jump':
                        raise SystemExit('jump with a branch in its delay slot at 0x%04x' % pc)
                    slot2_body, slot2_branch, slot2_ebit, _ = info[k + 2]
                    if slot2_branch or slot2_ebit:
                        raise SystemExit('branch or E-bit in the delay slot of 0x%04x' % (pc + 8))
                    body.append('    {   // delay slot 0x%04X holds a branch' % (pc + 8))
                    body += ['        ' + line.replace('const bool tk = ', 'const bool tk2 = ') for line in slot_body]
                    guard = '' if slot_branch[0] == 'always' else 'if (tk2) '
                    body.append('        %s{' % guard)
                    body += ['            ' + line for line in slot2_body]
                    body.append('            ' + go(slot_branch[2]))
                    body.append('        }')
                    body.append('    }')
                    if branch[0] == 'always':
                        body.append('    ' + go(branch[2]))
                    else:
                        body.append('    if (tk) ' + go(branch[2]))
                        body.append('    ' + go((pc + 16) & 0x3FFF))
                    body.append('}')
                    continue
                body.append('    {   // delay slot 0x%04X' % (pc + 8))
                body += ['        ' + line for line in slot_body]
                body.append('    }')
                if ebit:
                    body.append('    c.pc = 0x%04Xu;' % ((pc + 16) & 0x3FFF))
                    body.append('    { next = kExit; goto leave; }')
                elif branch[0] == 'always':
                    body.append('    ' + go(branch[2]))
                elif branch[0] == 'cond':
                    body.append('    if (tk) ' + go(branch[2]))
                    body.append('    ' + go((pc + 16) & 0x3FFF))
                else:
                    body.append('    { next = jt; goto leave; }')
            body.append('}')
        body.append('    next = 0x%04Xu; // the next region' % (8 * b))
        body.append('    goto leave;')
        for t in range(a, b):
            if t in runs:
                run = runs[t]
                body += fast_copy(run, info, lambda target, run=run: edge(target, src=run))

        for i, line in enumerate(body):
            for rx, rep in NAME_SUBS:
                line = rx.sub(rep, line)
            body[i] = line
        names = sorted({m for line in body for m in STATE_NAME.findall(line)}, key=state_key)

        out.append('// pairs 0x%04X..0x%04X' % (8 * a, 8 * (b - 1)))
        out.append('uint32_t region_%04X(Ctx &c, St &s, uint32_t pc)' % (8 * a))
        out.append('{')
        out += ['    ' + state_decl(name)[0] for name in names]
        out += ['    __m128 acc = s.acc;',
                '    float I = s.I;',
                '    QPipe qp = s.qp;',
                '    uint8_t *const mem = c.mem;',
                '    uint32_t cycle = s.cycle, xgkickDone = s.xgkickDone, cycle0 = 0u;',
                '    int32_t bbReg = s.bbReg, bbVal = s.bbVal;',
                '    uint32_t next;',
                '    switch (pc)',
                '    {']
        out += ['    case 0x%04X: goto L_%04X;' % (8 * k, 8 * k) for k in range(a, b)]
        out += ['    default: next = pc; goto leave;', '    }']
        out += body
        out.append('leave:')
        out += ['    ' + state_decl(name)[1] for name in names]
        out += ['    s.acc = acc;',
                '    s.I = I;',
                '    s.qp = qp;',
                '    s.cycle = cycle;',
                '    s.xgkickDone = xgkickDone;',
                '    s.bbReg = bbReg;',
                '    s.bbVal = bbVal;',
                '    return next;',
                '}',
                '']

    out.append('using RegionFn = uint32_t (*)(Ctx &, St &, uint32_t);')
    out.append('// The region holding each pair.')
    out.append('const RegionFn kRegionOf[%d] = {' % n)
    for i in range(0, n, 8):
        out.append('    ' + ', '.join('region_%04X' % (8 * regions[region_of[k]][0]) for k in range(i, min(i + 8, n))) + ',')
    out += ['};', '} // namespace', '',
            'bool run(Ctx &c, uint32_t startPc)',
            '{',
            '    if ((startPc & 7u) != 0u || startPc >= kProgramSize)',
            '        return false;',
            '    const unsigned int savedCsr = _mm_getcsr();',
            '    _mm_setcsr((savedCsr & ~0x6000u) | 0x6000u | 0x8000u | 0x0040u); // round to zero, FTZ, DAZ',
            '    St s;',
            '    for (int r = 0; r < 32; ++r) s.vf[r] = c.vf[r];',
            '    s.acc = c.acc;',
            '    for (int r = 0; r < 16; ++r) s.vi[r] = c.vi[r];',
            '    s.I = c.i;',
            '    s.qp = QPipe{c.q, 0.0f, 0u, false};',
            '    s.cycle = 0u;',
            '    s.xgkickDone = 0u;',
            '    std::memset(s.vfReady, 0, sizeof(s.vfReady));',
            '    std::memset(s.viReady, 0, sizeof(s.viReady));',
            '    std::memset(s.accReady, 0, sizeof(s.accReady));',
            '    s.bbReg = 0;',
            '    s.bbVal = 0;',
            '    c.macCount = 0u;',
            '    c.clipCount = 0u;',
            '    c.workingClip = c.clip;',
            '    uint32_t pc = startPc;',
            '    while (pc != kExit)',
            '    {',
            '        if ((pc & 7u) != 0u || pc >= kProgramSize)',
            '        {',
            '            c.pc = pc; // a jump outside the program, or ran off its end',
            '            break;',
            '        }',
            '        pc = kRegionOf[pc >> 3](c, s, pc);',
            '    }',
            '    for (int r = 1; r < 32; ++r) c.vf[r] = s.vf[r];',
            '    c.acc = s.acc;',
            '    for (int r = 1; r < 16; ++r) c.vi[r] = s.vi[r];',
            '    c.i = s.I;',
            '    c.q = s.qp.pending ? s.qp.value : s.qp.q;',
            '    c.cycles = umax(s.cycle, s.xgkickDone);',
            '    if (c.macCount) c.mac = macOf(c.macRing[(c.macCount - 1u) & 7u]);',
            '    if (c.clipCount) c.clip = c.clipRing[(c.clipCount - 1u) & 7u].clip;',
            '    _mm_setcsr(savedCsr);',
            '    return true;',
            '}', '}']
    # Branch targets must be pair labels inside the program.
    with open(dst, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('%d pairs in %d regions, %d runs with a static schedule (%d pairs) -> %s (%d lines)'
          % (n, len(regions), len(runs), fast_pairs, dst, len(out)))


if __name__ == '__main__':
    main()
