#!/usr/bin/env python3
"""VU0/VU1 micro-mode disassembler (mnemonics, PS2 VU User's Manual syntax).

Usage:
  vudis.py <microcode.bin> [start_hex [end_hex]] [--labels]

<microcode.bin> is a raw dump of VU micro memory (16 KB for VU1): for example the
`.vucode` file written by PS2X_XGKICK_DUMP / PS2X_VU1_BUDGET_DUMP, or `vu1MicroMem.bin`
from a PCSX2 save state (a .p2s is a zip with zstd entries). Addresses are byte
addresses; each line is one 64-bit pair: lower word first in memory, upper word second.
--labels prints a "Lxxxx:" line before every branch target in the range.
"""
import struct
import sys

BC = 'xyzw'


def dest(w):
    d = (w >> 21) & 0xF
    return ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if d & b)


def flags(up):
    f = []
    for bit, name in ((31, 'I'), (30, 'E'), (29, 'M'), (28, 'D'), (27, 'T')):
        if up >> bit & 1:
            f.append(name)
    return '[' + ''.join(f) + ']' if f else ''


UPPER_BC = ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL']
UPPER_QI = {0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi', 0x20: 'ADDq', 0x21: 'MADDq',
            0x22: 'ADDi', 0x23: 'MADDi', 0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi'}
UPPER_3 = {0x28: 'ADD', 0x29: 'MADD', 0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB',
           0x2E: 'OPMSUB', 0x2F: 'MINI'}
SPU_ACC_BC = ['ADDA', 'SUBA', 'MADDA', 'MSUBA']
SPU = {0x10: 'ITOF0', 0x11: 'ITOF4', 0x12: 'ITOF12', 0x13: 'ITOF15', 0x14: 'FTOI0', 0x15: 'FTOI4',
       0x16: 'FTOI12', 0x17: 'FTOI15', 0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIP',
       0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi', 0x23: 'MADDAi', 0x24: 'SUBAq', 0x25: 'MSUBAq',
       0x26: 'SUBAi', 0x27: 'MSUBAi', 0x28: 'ADDA', 0x29: 'MADDA', 0x2A: 'MULA', 0x2C: 'SUBA',
       0x2D: 'MSUBA', 0x2E: 'OPMULA', 0x2F: 'NOP'}


def upper(up):
    op = up & 0x3F
    ft, fs, fd = (up >> 16) & 31, (up >> 11) & 31, (up >> 6) & 31
    d = dest(up)
    if op < 0x1C:
        name = UPPER_BC[op >> 2] + BC[op & 3]
        return '%s.%s vf%02d, vf%02d, vf%02d%s' % (name, d, fd, fs, ft, BC[op & 3])
    if op in UPPER_QI:
        name = UPPER_QI[op]
        return '%s.%s vf%02d, vf%02d, %s' % (name, d, fd, fs, name[-1].upper())
    if op in UPPER_3:
        return '%s.%s vf%02d, vf%02d, vf%02d' % (UPPER_3[op], d, fd, fs, ft)
    if op >= 0x3C:
        sp = ((up >> 6) & 0x1F) << 2 | (up & 3)
        if sp < 0x10 or 0x18 <= sp <= 0x1B:
            name = (SPU_ACC_BC[sp >> 2] if sp < 0x10 else 'MULA') + BC[sp & 3]
            return '%s.%s ACC, vf%02d, vf%02d%s' % (name, d, fs, ft, BC[sp & 3])
        name = SPU.get(sp)
        if name is None:
            return 'upper?%08X' % up
        if name == 'NOP':
            return 'NOP'
        if name.startswith(('ITOF', 'FTOI', 'ABS')):
            return '%s.%s vf%02d, vf%02d' % (name, d, ft, fs)
        if name == 'CLIP':
            return 'CLIPw.xyz vf%02d, vf%02dw' % (fs, ft)
        if name[-1] in 'qi':
            return '%s.%s ACC, vf%02d, %s' % (name, d, fs, name[-1].upper())
        return '%s.%s ACC, vf%02d, vf%02d' % (name, d, fs, ft)
    return 'upper?%08X' % up


def s11(lo):
    v = lo & 0x7FF
    return v - 0x800 if v & 0x400 else v


LOWER_FLAG = {0x10: 'FCEQ', 0x11: 'FCSET', 0x12: 'FCAND', 0x13: 'FCOR'}
LOWER_STAT = {0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR'}
LOWER_MAC = {0x18: 'FMEQ', 0x1A: 'FMAND', 0x1B: 'FMOR'}
BRANCH = {0x20: 'B', 0x21: 'BAL', 0x24: 'JR', 0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE',
          0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
LSP = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV',
       0x39: 'SQRT', 0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR',
       0x3F: 'ISWR', 0x40: 'RNEXT', 0x41: 'RGET', 0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP',
       0x68: 'XTOP', 0x69: 'XITOP', 0x6C: 'XGKICK', 0x70: 'ESADD', 0x71: 'ERSADD', 0x72: 'ELENG',
       0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz', 0x76: 'ESUM', 0x78: 'ESQRT',
       0x79: 'ERSQRT', 0x7A: 'ERCPR', 0x7B: 'WAITP', 0x7C: 'ESIN', 0x7D: 'EATAN', 0x7E: 'EEXP'}


def branch_target(lo, pc):
    hi = lo >> 25
    if hi in (0x20, 0x21) or 0x28 <= hi <= 0x2F:
        return (pc + 8 + s11(lo) * 8) & 0x3FFF
    return None


def lower(lo, pc):
    hi = lo >> 25
    it, is_, id_ = (lo >> 16) & 31, (lo >> 11) & 31, (lo >> 6) & 31
    d = dest(lo)
    if hi == 0x00:
        return 'LQ.%s vf%02d, %d(vi%02d)' % (d, it, s11(lo), is_)
    if hi == 0x01:
        return 'SQ.%s vf%02d, %d(vi%02d)' % (d, is_, s11(lo), it)
    if hi == 0x04:
        return 'ILW.%s vi%02d, %d(vi%02d)' % (d, it, s11(lo), is_)
    if hi == 0x05:
        return 'ISW.%s vi%02d, %d(vi%02d)' % (d, it, s11(lo), is_)
    if hi in (0x08, 0x09):
        imm = ((lo >> 10) & 0x7800) | (lo & 0x7FF)
        return '%s vi%02d, vi%02d, 0x%X' % ('IADDIU' if hi == 0x08 else 'ISUBIU', it, is_, imm)
    if hi in LOWER_FLAG:
        return '%s 0x%06X' % (LOWER_FLAG[hi], lo & 0xFFFFFF)
    if hi in LOWER_STAT:
        imm = ((lo >> 10) & 0x800) | (lo & 0x7FF)
        return '%s vi%02d, 0x%03X' % (LOWER_STAT[hi], it, imm) if hi != 0x15 else 'FSSET 0x%03X' % imm
    if hi in LOWER_MAC:
        return '%s vi%02d, vi%02d' % (LOWER_MAC[hi], it, is_)
    if hi == 0x1C:
        return 'FCGET vi%02d' % it
    if hi in BRANCH:
        name = BRANCH[hi]
        if hi == 0x20:
            return 'B L%04X' % branch_target(lo, pc)
        if hi == 0x21:
            return 'BAL vi%02d, L%04X' % (it, branch_target(lo, pc))
        if hi == 0x24:
            return 'JR vi%02d' % is_
        if hi == 0x25:
            return 'JALR vi%02d, vi%02d' % (it, is_)
        if hi in (0x28, 0x29):
            return '%s vi%02d, vi%02d, L%04X' % (name, it, is_, branch_target(lo, pc))
        return '%s vi%02d, L%04X' % (name, is_, branch_target(lo, pc))
    if hi == 0x40:
        op = lo & 0x3F
        if op in (0x30, 0x31, 0x34, 0x35):
            name = {0x30: 'IADD', 0x31: 'ISUB', 0x34: 'IAND', 0x35: 'IOR'}[op]
            return '%s vi%02d, vi%02d, vi%02d' % (name, id_, is_, it)
        if op == 0x32:
            imm = (lo >> 6) & 31
            imm = imm - 32 if imm & 16 else imm
            return 'IADDI vi%02d, vi%02d, %d' % (it, is_, imm)
        if op >= 0x3C:
            sp = ((lo >> 6) & 0x1F) << 2 | (lo & 3)
            name = LSP.get(sp)
            if name is None:
                return 'lower?%08X' % lo
            fsf, ftf = BC[(lo >> 21) & 3], BC[(lo >> 23) & 3]
            if name in ('MOVE', 'MR32'):
                return '%s.%s vf%02d, vf%02d' % (name, d, it, is_) if lo != 0x8000033C else 'NOP'
            if name == 'LQI':
                return 'LQI.%s vf%02d, (vi%02d++)' % (d, it, is_)
            if name == 'SQI':
                return 'SQI.%s vf%02d, (vi%02d++)' % (d, is_, it)
            if name == 'LQD':
                return 'LQD.%s vf%02d, (--vi%02d)' % (d, it, is_)
            if name == 'SQD':
                return 'SQD.%s vf%02d, (--vi%02d)' % (d, is_, it)
            if name == 'DIV':
                return 'DIV Q, vf%02d%s, vf%02d%s' % (is_, fsf, it, ftf)
            if name in ('SQRT', 'RSQRT'):
                return ('SQRT Q, vf%02d%s' % (it, ftf) if name == 'SQRT'
                        else 'RSQRT Q, vf%02d%s, vf%02d%s' % (is_, fsf, it, ftf))
            if name == 'MTIR':
                return 'MTIR vi%02d, vf%02d%s' % (it, is_, fsf)
            if name == 'MFIR':
                return 'MFIR.%s vf%02d, vi%02d' % (d, it, is_)
            if name == 'ILWR':
                return 'ILWR.%s vi%02d, (vi%02d)' % (d, it, is_)
            if name == 'ISWR':
                return 'ISWR.%s vi%02d, (vi%02d)' % (d, it, is_)
            if name in ('XTOP', 'XITOP'):
                return '%s vi%02d' % (name, it)
            if name == 'XGKICK':
                return 'XGKICK vi%02d' % is_
            if name in ('MFP', 'RNEXT', 'RGET'):
                return '%s.%s vf%02d, %s' % (name, d, it, 'P' if name == 'MFP' else 'R')
            if name in ('RINIT', 'RXOR'):
                return '%s R, vf%02d%s' % (name, is_, fsf)
            if name in ('WAITQ', 'WAITP'):
                return name
            if name in ('ESADD', 'ERSADD', 'ELENG', 'ERLENG', 'EATANxy', 'EATANxz', 'ESUM'):
                return '%s P, vf%02d' % (name, is_)
            return '%s P, vf%02d%s' % (name, is_, fsf)
    return 'lower?%08X' % lo


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    labels = '--labels' in argv
    args = [a for a in argv[1:] if a != '--labels']
    code = open(args[0], 'rb').read()
    start = int(args[1], 16) if len(args) > 1 else 0
    end = int(args[2], 16) if len(args) > 2 else len(code)
    targets = set()
    if labels:
        for pc in range(0, len(code) - 7, 8):
            lo, up = struct.unpack_from('<II', code, pc)
            if not up >> 31:
                t = branch_target(lo, pc)
                if t is not None:
                    targets.add(t)
    for pc in range(start & ~7, min(end, len(code)), 8):
        lo, up = struct.unpack_from('<II', code, pc)
        if pc in targets:
            print('L%04X:' % pc)
        l = 'LOI 0x%08X' % lo if up >> 31 else lower(lo, pc)
        print('%04X: %08X %08X  %-40s %s %s' % (pc, lo, up, upper(up), l, flags(up)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
