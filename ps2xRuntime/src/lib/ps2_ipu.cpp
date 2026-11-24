// EE Image Processing Unit (IPU). See runtime/ps2_ipu.h for the model.
//
// The decoding core is ported from PCSX2 (github.com/PCSX2/pcsx2, pcsx2/IPU at commit
// 82cac38b2af19d593dedfd9ddaa63448e7ec493c):
//   - IPU_MultiISA.cpp (SPDX GPL-2.0+, Copyright 2002-2026 PCSX2 Dev Team; parts based on the mpeg2dec
//     library, Copyright (C) 2000-2002 Michel Lespinasse <walken@zoy.org> and Copyright (C) 1999-2000
//     Aaron Holtzman <aholtzma@ess.engr.uvic.ca>): IDCT, intra/non-intra block decode, DC diffs,
//     macroblock modes and address increment, the IDEC/BDEC/VDEC/FDEC/SETIQ/SETVQ/CSC/PACK commands,
//     ipu_csc (thresholds), ipu_vq.
//   - IPU.cpp, yuv2rgb.cpp, IPUdither.cpp (SPDX GPL-3.0+, Copyright 2002-2026 PCSX2 Dev Team): command
//     set-up, register layout, yuv2rgb_reference, ipu_dither_reference.
//   - mpeg2_vlc.h (copied as ipu/mpeg2_vlc.h).
// Rewritten for PS2Recomp: the bit reader and FIFO (a byte queue with a strict "not enough input"
// check) and the resumable state machines (a stalled command is re-run from its first bit instead).
// One deliberate difference: ipu_csc's SGN pass restarts from the first pixel (PCSX2 walks past the
// macroblock when a threshold is set as well).
#include "runtime/ps2_ipu.h"
#include "ipu/mpeg2_vlc.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

using namespace ps2_ipu_vlc;

namespace
{
    enum : unsigned
    {
        kCmdBclr = 0,
        kCmdIdec = 1,
        kCmdBdec = 2,
        kCmdVdec = 3,
        kCmdFdec = 4,
        kCmdSetiq = 5,
        kCmdSetvq = 6,
        kCmdCsc = 7,
        kCmdPack = 8,
        kCmdSetth = 9,
    };

    constexpr uint32_t kCtrlEcd = 1u << 14;
    constexpr uint32_t kCtrlScd = 1u << 15;
    constexpr uint32_t kCtrlRst = 1u << 30;
    constexpr uint32_t kCtrlWritable = 0x47F30000u; // IDP, AS, IVF, QST, MP1, PCT, RST
    // Quadwords the IPU holds ahead of the bit position: the 8-quadword input FIFO plus its 2-quadword
    // internal buffer. Queued input beyond this is still "in the toIPU DMA" (see queuedInputQwords).
    constexpr uint32_t kFifoWindowQwords = 10u;

    constexpr int kNonLinearQuantizerScale[32] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        8, 10, 12, 14, 16, 18, 20, 22,
        24, 28, 32, 36, 40, 44, 48, 52,
        56, 64, 72, 80, 88, 96, 104, 112};

    struct ScanPack
    {
        uint8_t norm[64];
        uint8_t alt[64];
    };

    ScanPack makeScanPack()
    {
        static const uint8_t scanNorm[64] = {
            0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
        static const uint8_t scanAlt[64] = {
            0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
            41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
            51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
            53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
        ScanPack pack{};
        // The IDCT below takes each row's coefficients in the order 0 2 4 6 1 3 5 7 (as mpeg2dec's).
        for (int i = 0; i < 64; i++)
        {
            int j = scanNorm[i];
            pack.norm[i] = static_cast<uint8_t>(((j & 0x36) >> 1) | ((j & 0x09) << 2));
            j = scanAlt[i];
            pack.alt[i] = static_cast<uint8_t>(((j & 0x36) >> 1) | ((j & 0x09) << 2));
        }
        return pack;
    }

    const ScanPack kScan = makeScanPack();

    inline uint8_t clip8(int v)
    {
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    struct Mb8
    {
        uint8_t Y[16][16];
        uint8_t Cb[8][8];
        uint8_t Cr[8][8];
    };

    struct Mb16
    {
        int16_t Y[16][16];
        int16_t Cb[8][8];
        int16_t Cr[8][8];
    };

    struct Rgba
    {
        uint8_t r, g, b, a;
    };

    struct MbRgb32
    {
        Rgba c[16][16];
    };

    // RGB16 as the IPU outputs it: r bits 0-4, g 5-9, b 10-14, a bit 15.
    struct MbRgb16
    {
        uint16_t c[16][16];
    };

    static_assert(sizeof(Mb8) == 384 && sizeof(Mb16) == 768 && sizeof(MbRgb32) == 1024 && sizeof(MbRgb16) == 512,
                  "IPU macroblock layouts");

    struct Decoder
    {
        alignas(16) int16_t DCTblock[64];
        uint8_t niq[64]; // non-intra quantiser matrix
        uint8_t iq[64];  // intra quantiser matrix
        Mb8 mb8;
        Mb16 mb16;
        MbRgb32 rgb32;
        MbRgb16 rgb16;
        int quantizer_scale;
        int coding_type;
        int16_t dc_dct_pred[3];
        int intra_dc_precision;
        int picture_structure;
        int frame_pred_frame_dct;
        int q_scale_type;
        int intra_vlc_format;
        int sgn;
        int dte;
        int ofm;
        int macroblock_modes;
        int dcr;
        int coded_block_pattern;
        bool scantype;
        int mpeg1;
    };

    struct NeedInput
    {
    };

    constexpr int W1 = 2841; // 2048*sqrt (2)*cos (1*pi/16)
    constexpr int W2 = 2676; // 2048*sqrt (2)*cos (2*pi/16)
    constexpr int W3 = 2408; // 2048*sqrt (2)*cos (3*pi/16)
    constexpr int W5 = 1609; // 2048*sqrt (2)*cos (5*pi/16)
    constexpr int W6 = 1108; // 2048*sqrt (2)*cos (6*pi/16)
    constexpr int W7 = 565;  // 2048*sqrt (2)*cos (7*pi/16)

    inline void butterfly(int &t0, int &t1, int w0, int w1, int d0, int d1)
    {
        const int tmp = w0 * (d0 + d1);
        t0 = tmp + (w1 - w0) * d1;
        t1 = tmp - (w1 + w0) * d0;
    }

    void idctBlock(int16_t *block)
    {
        for (int i = 0; i < 8; i++)
        {
            int16_t *const rblock = block + 8 * i;
            int32_t words[4];
            std::memcpy(words, rblock, sizeof(words));
            if (!(rblock[1] | words[1] | words[2] | words[3]))
            {
                uint32_t tmp = static_cast<uint16_t>(rblock[0] << 3);
                tmp |= tmp << 16;
                for (int k = 0; k < 4; ++k)
                    std::memcpy(rblock + 2 * k, &tmp, 4);
                continue;
            }

            int a0, a1, a2, a3;
            {
                const int d0 = (rblock[0] << 11) + 128;
                const int d1 = rblock[1];
                const int d2 = rblock[2] << 11;
                const int d3 = rblock[3];
                const int t0 = d0 + d2;
                const int t1 = d0 - d2;
                int t2, t3;
                butterfly(t2, t3, W6, W2, d3, d1);
                a0 = t0 + t2;
                a1 = t1 + t3;
                a2 = t1 - t3;
                a3 = t0 - t2;
            }

            int b0, b1, b2, b3;
            {
                const int d0 = rblock[4];
                const int d1 = rblock[5];
                const int d2 = rblock[6];
                const int d3 = rblock[7];
                int t0, t1, t2, t3;
                butterfly(t0, t1, W7, W1, d3, d0);
                butterfly(t2, t3, W3, W5, d1, d2);
                b0 = t0 + t2;
                b3 = t1 + t3;
                t0 -= t2;
                t1 -= t3;
                b1 = ((t0 + t1) * 181) >> 8;
                b2 = ((t0 - t1) * 181) >> 8;
            }

            rblock[0] = static_cast<int16_t>((a0 + b0) >> 8);
            rblock[1] = static_cast<int16_t>((a1 + b1) >> 8);
            rblock[2] = static_cast<int16_t>((a2 + b2) >> 8);
            rblock[3] = static_cast<int16_t>((a3 + b3) >> 8);
            rblock[4] = static_cast<int16_t>((a3 - b3) >> 8);
            rblock[5] = static_cast<int16_t>((a2 - b2) >> 8);
            rblock[6] = static_cast<int16_t>((a1 - b1) >> 8);
            rblock[7] = static_cast<int16_t>((a0 - b0) >> 8);
        }

        for (int i = 0; i < 8; i++)
        {
            int16_t *const cblock = block + i;

            int a0, a1, a2, a3;
            {
                const int d0 = (cblock[8 * 0] << 11) + 65536;
                const int d1 = cblock[8 * 1];
                const int d2 = cblock[8 * 2] << 11;
                const int d3 = cblock[8 * 3];
                const int t0 = d0 + d2;
                const int t1 = d0 - d2;
                int t2, t3;
                butterfly(t2, t3, W6, W2, d3, d1);
                a0 = t0 + t2;
                a1 = t1 + t3;
                a2 = t1 - t3;
                a3 = t0 - t2;
            }

            int b0, b1, b2, b3;
            {
                const int d0 = cblock[8 * 4];
                const int d1 = cblock[8 * 5];
                const int d2 = cblock[8 * 6];
                const int d3 = cblock[8 * 7];
                int t0, t1, t2, t3;
                butterfly(t0, t1, W7, W1, d3, d0);
                butterfly(t2, t3, W3, W5, d1, d2);
                b0 = t0 + t2;
                b3 = t1 + t3;
                t0 = (t0 - t2) >> 8;
                t1 = (t1 - t3) >> 8;
                b1 = (t0 + t1) * 181;
                b2 = (t0 - t1) * 181;
            }

            cblock[8 * 0] = static_cast<int16_t>((a0 + b0) >> 17);
            cblock[8 * 1] = static_cast<int16_t>((a1 + b1) >> 17);
            cblock[8 * 2] = static_cast<int16_t>((a2 + b2) >> 17);
            cblock[8 * 3] = static_cast<int16_t>((a3 + b3) >> 17);
            cblock[8 * 4] = static_cast<int16_t>((a3 - b3) >> 17);
            cblock[8 * 5] = static_cast<int16_t>((a2 - b2) >> 17);
            cblock[8 * 6] = static_cast<int16_t>((a1 - b1) >> 17);
            cblock[8 * 7] = static_cast<int16_t>((a0 - b0) >> 17);
        }
    }

    // Intra: IDCT, clip to 0..255 into dest (stride in bytes), clear the block.
    void idctCopy(int16_t *block, uint8_t *dest, int stride)
    {
        idctBlock(block);
        for (int i = 0; i < 8; i++)
        {
            for (int k = 0; k < 8; ++k)
                dest[k] = clip8(block[k]);
            std::memset(block, 0, 16);
            dest += stride;
            block += 8;
        }
    }

    // Non-intra: IDCT into dest (stride in int16 units; the IPU outputs the residual), clear the block.
    void idctAdd(int last, int16_t *block, int16_t *dest, int stride)
    {
        if (last != 129 || (block[0] & 7) == 4)
        {
            idctBlock(block);
            for (int i = 0; i < 8; i++)
            {
                std::memcpy(dest, block, 16);
                std::memset(block, 0, 16);
                dest += stride;
                block += 8;
            }
        }
        else
        {
            const int16_t dc = static_cast<int16_t>((static_cast<int32_t>(block[0]) + 4) >> 3);
            block[0] = block[63] = 0;
            for (int i = 0; i < 8; ++i)
                for (int k = 0; k < 8; ++k)
                    dest[stride * i + k] = dc;
        }
    }

    inline void saturate(int &val)
    {
        if (static_cast<uint32_t>(val + 2048) > 4095)
            val = (val >> 31) ^ 2047;
    }

    // yuv2rgb_reference (BT.601 integer method of the IPU), alpha 0x80.
    void yuv2rgb(const Mb8 &mb8, MbRgb32 &rgb32)
    {
        constexpr int kYBias = 16;
        constexpr int kYCoeff = 0x95;      //  1.1640625
        constexpr int kGcrCoeff = -0x68;   // -0.8125
        constexpr int kGcbCoeff = -0x32;   // -0.390625
        constexpr int kRcrCoeff = 0xcc;    //  1.59375
        constexpr int kBcbCoeff = 0x102;   //  2.015625
        for (int y = 0; y < 16; y++)
        {
            for (int x = 0; x < 16; x++)
            {
                const int lum = (kYCoeff * std::max(0, static_cast<int>(mb8.Y[y][x]) - kYBias)) >> 6;
                const int cr = static_cast<int>(mb8.Cr[y >> 1][x >> 1]) - 128;
                const int cb = static_cast<int>(mb8.Cb[y >> 1][x >> 1]) - 128;
                const int rcr = (kRcrCoeff * cr) >> 6;
                const int gcr = (kGcrCoeff * cr) >> 6;
                const int gcb = (kGcbCoeff * cb) >> 6;
                const int bcb = (kBcbCoeff * cb) >> 6;
                rgb32.c[y][x].r = clip8((lum + rcr + 1) >> 1);
                rgb32.c[y][x].g = clip8((lum + gcr + gcb + 1) >> 1);
                rgb32.c[y][x].b = clip8((lum + bcb + 1) >> 1);
                rgb32.c[y][x].a = 0x80;
            }
        }
    }

    // ipu_dither_reference.
    void dither(const MbRgb32 &rgb32, MbRgb16 &rgb16, int dte)
    {
        static const int kDither[4][4] = {
            {-4, 0, -3, 1},
            {2, -2, 3, -1},
            {-3, 1, -4, 0},
            {3, -1, 2, -2},
        };
        for (int i = 0; i < 16; ++i)
        {
            for (int j = 0; j < 16; ++j)
            {
                const int d = dte ? kDither[i & 3][j & 3] : 0;
                const Rgba &p = rgb32.c[i][j];
                const int r = std::max(0, std::min(p.r + d, 255));
                const int g = std::max(0, std::min(p.g + d, 255));
                const int b = std::max(0, std::min(p.b + d, 255));
                rgb16.c[i][j] = static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) |
                                                      (p.a == 0x40 ? 0x8000u : 0u));
            }
        }
    }
} // namespace

struct Ps2Ipu::State
{
    // Input: quadwords in arrival order; bit = absolute bit position in `in` (whole quadwords are
    // dropped from the front as they are consumed, so quadword boundaries stay aligned).
    std::vector<uint8_t> in;
    uint64_t bit = 0;
    uint32_t fifoWords[4] = {};

    std::vector<uint8_t> out;
    size_t outRead = 0;

    uint32_t ctrl = 0; // IDP/AS/IVF/QST/MP1/PCT + ECD/SCD (IFC, OFC, CBP, BUSY are computed)
    uint32_t data = 0;
    uint32_t top = 0;
    bool busy = false;
    bool cmdBusy = false;
    bool topBusy = false;
    int cbp = 0;
    uint16_t thresh[2] = {};
    uint16_t vqclut[16] = {};

    uint32_t cmd = 0;
    uint64_t cmdCount = 0;
    uint64_t startBit = 0;
    bool stalled = false;  // the current command is waiting for input
    uint64_t stallBit = 0; // where it ran out
    size_t produced = 0; // output bytes produced by the current attempt
    size_t emitted = 0;  // output bytes of this command already queued by earlier attempts

    Decoder d{};
    Decoder snapshot{};

    // ---- bit reader ----
    uint64_t availBits() const
    {
        const uint64_t total = static_cast<uint64_t>(in.size()) * 8u;
        return total > bit ? total - bit : 0u;
    }

    void need(unsigned n) const
    {
        if (availBits() < n)
            throw NeedInput{};
    }

    uint32_t peek(unsigned n) const
    {
        if (n == 0)
            return 0;
        need(n);
        const size_t byte = static_cast<size_t>(bit >> 3);
        const unsigned shift = static_cast<unsigned>(bit & 7u);
        uint64_t v = 0;
        for (size_t i = 0; i < 5; ++i)
        {
            v <<= 8;
            if (byte + i < in.size())
                v |= in[byte + i];
        }
        return static_cast<uint32_t>((v >> (40u - shift - n)) & ((1ull << n) - 1u));
    }

    int32_t speek(unsigned n) const
    {
        const uint32_t v = peek(n);
        return static_cast<int32_t>(v << (32u - n)) >> (32u - n);
    }

    void skip(unsigned n)
    {
        need(n);
        bit += n;
    }

    uint32_t get(unsigned n)
    {
        const uint32_t v = peek(n);
        bit += n;
        return v;
    }

    void align()
    {
        bit = (bit + 7u) & ~static_cast<uint64_t>(7u);
    }

    void readBytes(void *dst, size_t n)
    {
        need(static_cast<unsigned>(n * 8u));
        uint8_t *p = static_cast<uint8_t *>(dst);
        for (size_t i = 0; i < n; ++i)
            p[i] = static_cast<uint8_t>(get(8));
    }

    // While a command waits for input it has (conceptually) consumed everything up to where it ran
    // dry: count from there, so a toIPU transfer can complete and the game sends more.
    uint64_t consumedBit() const
    {
        return stalled ? std::max(bit, stallBit) : bit;
    }

    uint32_t queuedQwords() const
    {
        const uint64_t qw = consumedBit() >> 7;
        const uint64_t total = in.size() / 16u;
        return total > qw ? static_cast<uint32_t>(total - qw) : 0u;
    }

    // Drops what the IPU holds (FIFO + internal buffer) from the current quadword on.
    void dropFifoWindow()
    {
        const uint64_t qw = consumedBit() >> 7;
        const uint32_t drop = std::min(queuedQwords(), kFifoWindowQwords);
        bit = (qw + drop) * 128u;
        stalled = false;
    }

    void compactInput()
    {
        const uint64_t keepFrom = busy ? std::min(bit, startBit) : bit;
        const size_t dropBytes = static_cast<size_t>((keepFrom >> 7) * 16u);
        if (dropBytes >= (64u * 1024u) || (dropBytes > 0 && dropBytes == in.size()))
        {
            in.erase(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(dropBytes));
            bit -= static_cast<uint64_t>(dropBytes) * 8u;
            startBit -= std::min<uint64_t>(startBit, static_cast<uint64_t>(dropBytes) * 8u);
        }
    }

    void emit(const void *p, size_t n)
    {
        const uint8_t *s = static_cast<const uint8_t *>(p);
        const size_t skipN = emitted > produced ? std::min(n, emitted - produced) : 0u;
        out.insert(out.end(), s + skipN, s + n);
        produced += n;
        emitted = std::max(emitted, produced);
    }

    // ---- decoding (PCSX2 IPU_MultiISA.cpp) ----
    int getMacroblockModes()
    {
        int modes;
        const MBtab *tab;
        switch (d.coding_type)
        {
        case I_TYPE:
            modes = static_cast<int>(peek(2));
            if (modes == 0)
                return 0; // error
            tab = MB_I + (modes >> 1);
            skip(tab->len);
            modes = tab->modes;
            if (!d.frame_pred_frame_dct && d.picture_structure == FRAME_PICTURE)
                modes |= static_cast<int>(get(1)) * DCT_TYPE_INTERLACED;
            return modes;

        case P_TYPE:
            modes = static_cast<int>(peek(6));
            if (modes == 0)
                return 0;
            tab = MB_P + (modes >> 1);
            skip(tab->len);
            modes = tab->modes;
            if (d.picture_structure != FRAME_PICTURE)
            {
                if (modes & MACROBLOCK_MOTION_FORWARD)
                    modes |= static_cast<int>(get(2)) * MOTION_TYPE_BASE;
                return modes;
            }
            else if (d.frame_pred_frame_dct)
            {
                if (modes & MACROBLOCK_MOTION_FORWARD)
                    modes |= MC_FRAME;
                return modes;
            }
            else
            {
                if (modes & MACROBLOCK_MOTION_FORWARD)
                    modes |= static_cast<int>(get(2)) * MOTION_TYPE_BASE;
                if (modes & (MACROBLOCK_INTRA | MACROBLOCK_PATTERN))
                    modes |= static_cast<int>(get(1)) * DCT_TYPE_INTERLACED;
                return modes;
            }

        case B_TYPE:
            modes = static_cast<int>(peek(6));
            if (modes == 0)
                return 0;
            tab = MB_B + modes;
            skip(tab->len);
            modes = tab->modes;
            if (d.picture_structure != FRAME_PICTURE)
            {
                if (!(modes & MACROBLOCK_INTRA))
                    modes |= static_cast<int>(get(2)) * MOTION_TYPE_BASE;
                return modes | (tab->len << 16);
            }
            else if (d.frame_pred_frame_dct)
            {
                modes |= MC_FRAME;
                return modes | (tab->len << 16);
            }
            else
            {
                if (!(modes & MACROBLOCK_INTRA))
                    modes |= static_cast<int>(get(2)) * MOTION_TYPE_BASE;
                if (modes & (MACROBLOCK_INTRA | MACROBLOCK_PATTERN))
                    modes |= static_cast<int>(get(1)) * DCT_TYPE_INTERLACED;
                return modes | (tab->len << 16);
            }

        case D_TYPE:
            modes = static_cast<int>(get(1));
            if (modes == 0)
                return 0;
            return MACROBLOCK_INTRA | (1 << 16);

        default:
            return 0;
        }
    }

    int getMacroblockAddressIncrement()
    {
        const MBAtab *mba;
        const uint32_t code = peek(16);
        if (code >= 4096)
            mba = MBA.mba5 + (peek(5) - 2);
        else if (code >= 768)
            mba = MBA.mba11 + (peek(11) - 24);
        else
        {
            switch (peek(11))
            {
            case 8: // macroblock_escape
                skip(11);
                return 0xb0023;
            case 15: // macroblock_stuffing (MPEG1 only)
                if (d.mpeg1)
                {
                    skip(11);
                    return 0xb0022;
                }
                return 0;
            default:
                return 0; // error
            }
        }
        skip(mba->len);
        return (mba->mba + 1) | (mba->len << 16);
    }

    int getLumaDcDiff()
    {
        int size;
        uint32_t code = peek(5);
        if (code < 31)
        {
            size = DCtable.lum0[code].size;
            skip(DCtable.lum0[code].len);
        }
        else
        {
            code = peek(9) - 0x1f0;
            size = DCtable.lum1[code].size;
            skip(DCtable.lum1[code].len);
        }
        if (size == 0)
            return 0;
        int diff = static_cast<int>(get(static_cast<unsigned>(size)));
        if ((diff & (1 << (size - 1))) == 0)
            diff -= (1 << size) - 1;
        return diff;
    }

    int getChromaDcDiff()
    {
        int size;
        uint32_t code = peek(5);
        if (code < 31)
        {
            size = DCtable.chrom0[code].size;
            skip(DCtable.chrom0[code].len);
        }
        else
        {
            code = peek(10) - 0x3e0;
            size = DCtable.chrom1[code].size;
            skip(DCtable.chrom1[code].len);
        }
        if (size == 0)
            return 0;
        int diff = static_cast<int>(get(static_cast<unsigned>(size)));
        if ((diff & (1 << (size - 1))) == 0)
            diff -= (1 << size) - 1;
        return diff;
    }

    const DCTtab *dctTab(uint32_t code, bool intra, bool first) const
    {
        const bool tableB15 = intra && d.intra_vlc_format && !d.mpeg1;
        if (code >= 16384 && !tableB15)
            return first ? &DCT.first[(code >> 12) - 4] : &DCT.next[(code >> 12) - 4];
        if (code >= 1024)
            return tableB15 ? &DCT.tab0a[(code >> 8) - 4] : &DCT.tab0[(code >> 8) - 4];
        if (code >= 512)
            return tableB15 ? &DCT.tab1a[(code >> 6) - 8] : &DCT.tab1[(code >> 6) - 8];
        if (code >= 256)
            return &DCT.tab2[(code >> 4) - 16];
        if (code >= 128)
            return &DCT.tab3[(code >> 3) - 16];
        if (code >= 64)
            return &DCT.tab4[(code >> 2) - 16];
        if (code >= 32)
            return &DCT.tab5[(code >> 1) - 16];
        if (code >= 16)
            return &DCT.tab6[code - 16];
        return nullptr;
    }

    void getIntraBlock()
    {
        const uint8_t *scan = d.scantype ? kScan.alt : kScan.norm;
        const uint8_t(&qm)[64] = d.iq;
        const int qs = d.quantizer_scale;
        int16_t *dest = d.DCTblock;
        for (int i = 1;; i++)
        {
            need(16);
            const DCTtab *tab = dctTab(peek(16), true, false);
            if (!tab)
                return;
            skip(tab->len);
            if (tab->run == 64) // end_of_block
                return;
            i += (tab->run == 65) ? static_cast<int>(get(6)) : tab->run;
            if (i >= 64)
                return;

            need(16);
            const unsigned j = scan[i];
            int val;
            if (tab->run == 65) // escape
            {
                if (!d.mpeg1)
                {
                    val = (speek(12) * qs * qm[i]) >> 4;
                    skip(12);
                }
                else
                {
                    val = speek(8);
                    skip(8);
                    if (!(val & 0x7f))
                        val = static_cast<int>(get(8)) + 2 * val;
                    val = (val * qs * qm[i]) >> 4;
                    val = (val + ~(static_cast<int32_t>(val) >> 31)) | 1;
                }
            }
            else
            {
                val = (tab->level * qs * qm[i]) >> 4;
                if (d.mpeg1)
                    val = (val - 1) | 1; // oddification
                const int bit1 = speek(1);
                val = (val ^ bit1) - bit1;
                skip(1);
            }
            saturate(val);
            dest[j] = static_cast<int16_t>(val);
        }
    }

    void getNonIntraBlock(int *last)
    {
        const uint8_t *scan = d.scantype ? kScan.alt : kScan.norm;
        const uint8_t(&qm)[64] = d.niq;
        const int qs = d.quantizer_scale;
        int16_t *dest = d.DCTblock;
        for (int i = 0;; i++)
        {
            need(16);
            const uint32_t code = peek(16);
            const DCTtab *tab = dctTab(code, false, i == 0);
            if (!tab)
                return;
            skip(tab->len);
            if (tab->run == 64)
            {
                *last = i;
                return;
            }
            i += (tab->run == 65) ? static_cast<int>(get(6)) : tab->run;
            if (i >= 64)
            {
                *last = i;
                return;
            }

            need(16);
            const unsigned j = scan[i];
            int val;
            if (tab->run == 65)
            {
                if (!d.mpeg1)
                {
                    val = ((2 * (speek(12) + speek(1)) + 1) * qs * qm[i]) >> 5;
                    skip(12);
                }
                else
                {
                    val = speek(8);
                    skip(8);
                    if (!(val & 0x7f))
                        val = static_cast<int>(get(8)) + 2 * val;
                    val = ((2 * (val + (static_cast<int32_t>(val) >> 31)) + 1) * qs * qm[i]) / 32;
                    val = (val + ~(static_cast<int32_t>(val) >> 31)) | 1;
                }
            }
            else
            {
                const int bit1 = speek(1);
                val = ((2 * tab->level + 1) * qs * qm[i]) >> 5;
                val = (val ^ bit1) - bit1;
                skip(1);
            }
            saturate(val);
            dest[j] = static_cast<int16_t>(val);
        }
    }

    void sliceIntraDct(int cc, uint8_t *dest, int stride)
    {
        need(16);
        if (cc == 0)
            d.dc_dct_pred[0] = static_cast<int16_t>(d.dc_dct_pred[0] + getLumaDcDiff());
        else
            d.dc_dct_pred[cc] = static_cast<int16_t>(d.dc_dct_pred[cc] + getChromaDcDiff());
        d.DCTblock[0] = static_cast<int16_t>(d.dc_dct_pred[cc] << (3 - d.intra_dc_precision));
        getIntraBlock();
        idctCopy(d.DCTblock, dest, stride);
    }

    void sliceNonIntraDct(int16_t *dest, int stride)
    {
        std::memset(d.DCTblock, 0, sizeof(d.DCTblock));
        int last = 0;
        getNonIntraBlock(&last);
        idctAdd(last, d.DCTblock, dest, stride);
    }

    void resetDcPredictors()
    {
        d.dc_dct_pred[0] = d.dc_dct_pred[1] = d.dc_dct_pred[2] = static_cast<int16_t>(128 << d.intra_dc_precision);
    }

    // ipu_csc: YCbCr -> RGB32, then the SETTH alpha thresholds and the SGN offset.
    void csc(int sgn)
    {
        yuv2rgb(d.mb8, d.rgb32);
        Rgba *p = &d.rgb32.c[0][0];
        if (thresh[0] > 0)
        {
            for (int i = 0; i < 256; i++)
            {
                if (p[i].r < thresh[0] && p[i].g < thresh[0] && p[i].b < thresh[0])
                    p[i] = Rgba{0, 0, 0, 0};
                else if (p[i].r < thresh[1] && p[i].g < thresh[1] && p[i].b < thresh[1])
                    p[i].a = 0x40;
            }
        }
        else if (thresh[1] > 0)
        {
            for (int i = 0; i < 256; i++)
            {
                if (p[i].r < thresh[1] && p[i].g < thresh[1] && p[i].b < thresh[1])
                    p[i].a = 0x40;
            }
        }
        if (sgn)
        {
            for (int i = 0; i < 256; i++)
            {
                p[i].r ^= 0x80;
                p[i].g ^= 0x80;
                p[i].b ^= 0x80;
            }
        }
    }

    void vq(uint8_t *indx4) const
    {
        const auto closest = [&](int i, int j)
        {
            const uint16_t c = d.rgb16.c[i][j];
            const int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            uint8_t index = 0;
            int best = std::numeric_limits<int>::max();
            for (uint8_t k = 0; k < 16; ++k)
            {
                const int dr = r - (vqclut[k] & 31);
                const int dg = g - ((vqclut[k] >> 5) & 31);
                const int db = b - ((vqclut[k] >> 10) & 31);
                const int dist = dr * dr + dg * dg + db * db;
                if (best > dist)
                {
                    index = k;
                    best = dist;
                }
            }
            return index;
        };
        for (int i = 0; i < 16; ++i)
            for (int j = 0; j < 8; ++j)
                indx4[i * 8 + j] = static_cast<uint8_t>(closest(i, 2 * j + 1) << 4 | closest(i, 2 * j));
    }

    // After IDEC/BDEC: look for a start code at the next byte boundary (SCD) or report garbage (ECD),
    // then latch the next 32 bits into TOP.
    void finishStartCode()
    {
        if (peek(8) == 0)
        {
            align();
            for (;;)
            {
                const uint32_t sc = peek(24);
                if (sc != 0)
                {
                    ctrl |= (sc == 1) ? kCtrlScd : kCtrlEcd;
                    break;
                }
                skip(8);
            }
        }
        top = peek(32);
    }

    void idec()
    {
        resetDcPredictors();
        top = 0;
        ctrl &= ~kCtrlEcd;
        need(32);
        int mbaCount = 0;
        for (;;)
        {
            d.macroblock_modes = getMacroblockModes();
            if (d.macroblock_modes & MACROBLOCK_QUANT)
            {
                const uint32_t code = get(5);
                d.quantizer_scale = d.q_scale_type ? kNonLinearQuantizerScale[code] : static_cast<int>(code << 1);
            }
            d.coded_block_pattern = 0x3F;
            std::memset(&d.mb8, 0, sizeof(d.mb8));
            std::memset(&d.rgb32, 0, sizeof(d.rgb32));

            const bool interlaced = (d.macroblock_modes & DCT_TYPE_INTERLACED) != 0;
            const int offset = interlaced ? 16 : 16 * 8;
            const int stride = interlaced ? 32 : 16;
            uint8_t *y = &d.mb8.Y[0][0];
            sliceIntraDct(0, y, stride);
            sliceIntraDct(0, y + 8, stride);
            sliceIntraDct(0, y + offset, stride);
            sliceIntraDct(0, y + offset + 8, stride);
            sliceIntraDct(1, &d.mb8.Cb[0][0], 8);
            sliceIntraDct(2, &d.mb8.Cr[0][0], 8);

            csc(d.sgn);
            if (d.ofm == 0)
                emit(&d.rgb32, sizeof(d.rgb32));
            else
            {
                dither(d.rgb32, d.rgb16, d.dte);
                emit(&d.rgb16, sizeof(d.rgb16));
            }

            mbaCount = 0;
            const MBAtab *mba = nullptr;
            for (;;)
            {
                need(16);
                const uint32_t code = peek(16);
                if (code >= 0x1000)
                {
                    mba = MBA.mba5 + (peek(5) - 2);
                    break;
                }
                if (code >= 0x0300)
                {
                    mba = MBA.mba11 + (peek(11) - 24);
                    break;
                }
                const uint32_t c11 = peek(11);
                if (c11 == 8) // macroblock_escape
                {
                    mbaCount += 33;
                    skip(11);
                    continue;
                }
                if (c11 == 15) // macroblock_stuffing
                {
                    skip(11);
                    continue;
                }
                break; // end of slice/frame, or error
            }
            if (!mba)
                break;
            skip(mba->len);
            mbaCount += mba->mba;
            if (mbaCount)
                resetDcPredictors();
            need(16);
        }
        ctrl &= ~kCtrlScd;
        cbp = d.coded_block_pattern;
        finishStartCode();
    }

    void bdec()
    {
        if (d.dcr)
            resetDcPredictors();
        ctrl &= ~kCtrlEcd;
        top = 0;
        std::memset(&d.mb8, 0, sizeof(d.mb8));
        std::memset(&d.mb16, 0, sizeof(d.mb16));
        need(32);

        const bool interlaced = (d.macroblock_modes & DCT_TYPE_INTERLACED) != 0;
        const int offset = interlaced ? 16 : 16 * 8;
        const int stride = interlaced ? 32 : 16;
        if (d.macroblock_modes & MACROBLOCK_INTRA)
        {
            d.coded_block_pattern = 0x3F;
            uint8_t *y = &d.mb8.Y[0][0];
            sliceIntraDct(0, y, stride);
            sliceIntraDct(0, y + 8, stride);
            sliceIntraDct(0, y + offset, stride);
            sliceIntraDct(0, y + offset + 8, stride);
            sliceIntraDct(1, &d.mb8.Cb[0][0], 8);
            sliceIntraDct(2, &d.mb8.Cr[0][0], 8);
            const uint8_t *s = reinterpret_cast<const uint8_t *>(&d.mb8);
            int16_t *t = reinterpret_cast<int16_t *>(&d.mb16);
            for (size_t i = 0; i < sizeof(d.mb8); ++i)
                t[i] = s[i];
        }
        else if (d.macroblock_modes & MACROBLOCK_PATTERN)
        {
            const CBPtab *tab;
            const uint32_t code = peek(16);
            if (code >= 0x2000)
                tab = CBP_7 + (peek(7) - 16);
            else
                tab = CBP_9 + peek(9);
            skip(tab->len);
            d.coded_block_pattern = tab->cbp;

            int16_t *y = &d.mb16.Y[0][0];
            if (d.coded_block_pattern & 0x20)
                sliceNonIntraDct(y, stride);
            if (d.coded_block_pattern & 0x10)
                sliceNonIntraDct(y + 8, stride);
            if (d.coded_block_pattern & 0x08)
                sliceNonIntraDct(y + offset, stride);
            if (d.coded_block_pattern & 0x04)
                sliceNonIntraDct(y + offset + 8, stride);
            if (d.coded_block_pattern & 0x02)
                sliceNonIntraDct(&d.mb16.Cb[0][0], 8);
            if (d.coded_block_pattern & 0x01)
                sliceNonIntraDct(&d.mb16.Cr[0][0], 8);
        }
        ctrl &= ~kCtrlScd;
        cbp = d.coded_block_pattern;
        emit(&d.mb16, sizeof(d.mb16));
        finishStartCode();
    }

    void vdec(uint32_t v)
    {
        need(32);
        switch ((v >> 26) & 3u)
        {
        case 0: // macroblock address increment
            d.mpeg1 = (ctrl >> 23) & 1u;
            data = static_cast<uint32_t>(getMacroblockAddressIncrement());
            break;
        case 1: // macroblock type
        {
            d.frame_pred_frame_dct = 1;
            const int pct = static_cast<int>((ctrl >> 24) & 7u);
            d.coding_type = pct > 0 ? pct : 1;
            data = static_cast<uint32_t>(getMacroblockModes());
            break;
        }
        case 2: // motion code
        {
            const uint32_t code = peek(16);
            if (code & 0x8000)
            {
                skip(1);
                data = 0x00010000;
            }
            else
            {
                const MVtab *tab;
                if ((code & 0xf000) || ((code & 0xfc00) == 0x0c00))
                    tab = MV_4 + peek(4);
                else
                    tab = MV_10 + peek(10);
                const int delta = tab->delta + 1;
                skip(tab->len);
                const int sign = speek(1);
                skip(1);
                data = static_cast<uint32_t>(((delta ^ sign) - sign) | (tab->len << 16));
            }
            break;
        }
        default: // dual-prime vector
        {
            const DMVtab *tab = DMV_2 + peek(2);
            skip(tab->len);
            data = static_cast<uint32_t>(tab->dmv | (tab->len << 16));
            break;
        }
        }
        if (data == 0)
            ctrl |= kCtrlEcd;
        else
            ctrl &= ~kCtrlEcd;
        top = peek(32);
    }

    void cscCommand(uint32_t v)
    {
        const uint32_t mbc = v & 0x7FFu;
        const int dte = (v >> 26) & 1u;
        const bool ofm = ((v >> 27) & 1u) != 0;
        for (uint32_t i = 0; i < mbc; ++i)
        {
            readBytes(&d.mb8, sizeof(d.mb8));
            csc(0);
            if (ofm)
            {
                dither(d.rgb32, d.rgb16, dte);
                emit(&d.rgb16, sizeof(d.rgb16));
            }
            else
                emit(&d.rgb32, sizeof(d.rgb32));
        }
    }

    void packCommand(uint32_t v)
    {
        const uint32_t mbc = v & 0x7FFu;
        const int dte = (v >> 26) & 1u;
        const bool ofm = ((v >> 27) & 1u) != 0;
        for (uint32_t i = 0; i < mbc; ++i)
        {
            readBytes(&d.rgb32, sizeof(d.rgb32));
            dither(d.rgb32, d.rgb16, dte);
            if (ofm)
                emit(&d.rgb16, sizeof(d.rgb16));
            else
            {
                uint8_t indx4[128];
                vq(indx4);
                emit(indx4, sizeof(indx4));
            }
        }
    }

    void execute()
    {
        const uint32_t v = cmd;
        switch (v >> 28)
        {
        case kCmdIdec:
            skip(v & 0x3Fu);
            idec();
            break;
        case kCmdBdec:
            skip(v & 0x3Fu);
            bdec();
            break;
        case kCmdVdec:
            skip(v & 0x3Fu);
            vdec(v);
            break;
        case kCmdFdec:
            skip(v & 0x3Fu);
            data = peek(32);
            top = data;
            break;
        case kCmdSetiq:
            skip(v & 0x3Fu);
            readBytes(((v >> 27) & 1u) ? d.niq : d.iq, 64);
            break;
        case kCmdSetvq:
            readBytes(vqclut, sizeof(vqclut));
            break;
        case kCmdCsc:
            cscCommand(v);
            break;
        case kCmdPack:
            packCommand(v);
            break;
        default:
            break;
        }
    }

    void attempt()
    {
        if (!busy)
            return;
        d = snapshot;
        bit = startBit;
        produced = 0;
        try
        {
            execute();
            stalled = false;
            busy = false;
            cmdBusy = false;
            topBusy = false;
            compactInput();
        }
        catch (const NeedInput &)
        {
            stalled = true;
            stallBit = bit;
            d = snapshot;
            bit = startBit;
        }
    }

    void softReset()
    {
        dropFifoWindow();
        out.clear();
        outRead = 0;
        cbp = 0;
        thresh[0] = thresh[1] = 0;
        if (!d.intra_dc_precision)
            resetDcPredictors();
        ctrl &= 0x7F33F00u;
        top = 0;
        data = 0;
        busy = cmdBusy = topBusy = false;
        compactInput();
    }
};

Ps2Ipu::Ps2Ipu() : m(std::make_unique<State>())
{
    reset();
}

Ps2Ipu::~Ps2Ipu() = default;

void Ps2Ipu::reset()
{
    *m = State{};
    m->d.picture_structure = FRAME_PICTURE;
}

void Ps2Ipu::writeCmd(uint32_t value)
{
    State &s = *m;
    s.ctrl &= ~(kCtrlEcd | kCtrlScd);
    s.stalled = false;
    s.cmd = value;
    ++s.cmdCount;
    switch (value >> 28)
    {
    case kCmdBclr:
        s.dropFifoWindow();
        s.bit += value & 0x7Fu;
        s.busy = s.cmdBusy = s.topBusy = false;
        s.compactInput();
        return;
    case kCmdSetth:
        s.thresh[0] = static_cast<uint16_t>(value & 0x1FFu);
        s.thresh[1] = static_cast<uint16_t>((value >> 16) & 0x1FFu);
        s.busy = s.cmdBusy = s.topBusy = false;
        return;
    case kCmdIdec:
        s.ctrl = (s.ctrl & ~(7u << 24)) | (static_cast<uint32_t>(I_TYPE) << 24); // IDEC: CTRL.PCT = I
        s.d.coding_type = I_TYPE;
        s.d.mpeg1 = static_cast<int>((s.ctrl >> 23) & 1u);
        s.d.q_scale_type = static_cast<int>((s.ctrl >> 22) & 1u);
        s.d.intra_vlc_format = static_cast<int>((s.ctrl >> 21) & 1u);
        s.d.scantype = ((s.ctrl >> 20) & 1u) != 0;
        s.d.intra_dc_precision = static_cast<int>((s.ctrl >> 16) & 3u);
        s.d.quantizer_scale = static_cast<int>((value >> 16) & 0x1Fu); // as PCSX2: the raw QSC
        s.d.frame_pred_frame_dct = ((value >> 24) & 1u) ? 0 : 1;
        s.d.sgn = static_cast<int>((value >> 25) & 1u);
        s.d.dte = static_cast<int>((value >> 26) & 1u);
        s.d.ofm = static_cast<int>((value >> 27) & 1u);
        s.d.dcr = 1;
        s.cmdBusy = s.topBusy = true;
        break;
    case kCmdBdec:
    {
        s.d.coding_type = I_TYPE;
        s.d.mpeg1 = static_cast<int>((s.ctrl >> 23) & 1u);
        s.d.q_scale_type = static_cast<int>((s.ctrl >> 22) & 1u);
        s.d.intra_vlc_format = static_cast<int>((s.ctrl >> 21) & 1u);
        s.d.scantype = ((s.ctrl >> 20) & 1u) != 0;
        s.d.intra_dc_precision = static_cast<int>((s.ctrl >> 16) & 3u);
        const uint32_t qsc = (value >> 16) & 0x1Fu;
        s.d.quantizer_scale = s.d.q_scale_type ? kNonLinearQuantizerScale[qsc] : static_cast<int>(qsc << 1);
        s.d.macroblock_modes = ((value >> 25) & 1u) ? DCT_TYPE_INTERLACED : 0;
        s.d.dcr = static_cast<int>((value >> 26) & 1u);
        s.d.macroblock_modes |= ((value >> 27) & 1u) ? MACROBLOCK_INTRA : MACROBLOCK_PATTERN;
        s.cmdBusy = s.topBusy = true;
        break;
    }
    case kCmdVdec:
    case kCmdFdec:
        s.cmdBusy = true;
        break;
    case kCmdSetiq:
    case kCmdSetvq:
    case kCmdCsc:
    case kCmdPack:
        break;
    default: // 0xA..0xF: not commands
        s.busy = s.cmdBusy = s.topBusy = false;
        return;
    }
    s.busy = true;
    s.startBit = s.bit;
    s.snapshot = s.d;
    s.produced = 0;
    s.emitted = 0;
    s.attempt();
}

void Ps2Ipu::writeCtrl(uint32_t value)
{
    State &s = *m;
    s.ctrl = (s.ctrl & ~kCtrlWritable) | (value & kCtrlWritable & ~kCtrlRst);
    if (((s.ctrl >> 16) & 3u) == 3u) // invalid intra DC precision: PCSX2 uses 9 bits
        s.ctrl = (s.ctrl & ~(3u << 16)) | (1u << 16);
    if (value & kCtrlRst)
        s.softReset();
}

uint32_t Ps2Ipu::readCmd()
{
    State &s = *m;
    const unsigned code = s.cmd >> 28;
    if (code != kCmdFdec && code != kCmdVdec && s.availBits() >= 32)
        s.data = s.peek(32);
    return s.data;
}

uint32_t Ps2Ipu::readCmdHigh() const
{
    return m->cmdBusy ? 0x80000000u : 0u;
}

uint32_t Ps2Ipu::readCtrl() const
{
    const State &s = *m;
    const uint32_t queued = s.queuedQwords();
    const uint32_t fp = std::min(queued, 2u);
    const uint32_t ifc = std::min(queued - fp, 8u);
    const uint32_t ofc = static_cast<uint32_t>(std::min<size_t>(queuedOutputBytes() / 16u, 8u));
    return (s.ctrl & ~0x80003FFFu) | ifc | (ofc << 4) | (static_cast<uint32_t>(s.cbp & 0x3F) << 8) |
           (s.busy ? 0x80000000u : 0u);
}

uint32_t Ps2Ipu::readBp() const
{
    const State &s = *m;
    const uint32_t queued = s.queuedQwords();
    const uint32_t fp = std::min(queued, 2u);
    const uint32_t ifc = std::min(queued - fp, 8u);
    return static_cast<uint32_t>(s.bit & 0x7Fu) | (ifc << 8) | (fp << 16);
}

uint32_t Ps2Ipu::readTop() const
{
    return m->top;
}

uint32_t Ps2Ipu::readTopHigh() const
{
    return m->topBusy ? 0x80000000u : 0u;
}

void Ps2Ipu::writeInFifoWord(unsigned index, uint32_t value)
{
    State &s = *m;
    s.fifoWords[index & 3u] = value;
    if ((index & 3u) == 3u)
    {
        uint8_t qw[16];
        std::memcpy(qw, s.fifoWords, sizeof(qw));
        pushInput(qw, 1);
    }
}

void Ps2Ipu::pushInput(const uint8_t *data, uint32_t qwc)
{
    State &s = *m;
    if (!qwc)
        return;
    s.in.insert(s.in.end(), data, data + static_cast<size_t>(qwc) * 16u);
    s.attempt();
}

uint32_t Ps2Ipu::queuedInputQwords() const
{
    return m->queuedQwords();
}

void Ps2Ipu::dropInputBeyond(uint32_t keepQwords)
{
    State &s = *m;
    const uint32_t queued = s.queuedQwords();
    if (queued <= keepQwords)
        return;
    const size_t keepEnd = static_cast<size_t>(((s.consumedBit() >> 7) + keepQwords) * 16u);
    if (keepEnd < s.in.size())
        s.in.resize(keepEnd);
}

size_t Ps2Ipu::queuedOutputBytes() const
{
    return m->out.size() - m->outRead;
}

size_t Ps2Ipu::takeOutput(uint8_t *dst, size_t maxBytes)
{
    State &s = *m;
    size_t n = std::min(maxBytes, queuedOutputBytes());
    n &= ~static_cast<size_t>(15u);
    if (n)
    {
        std::memcpy(dst, s.out.data() + s.outRead, n);
        s.outRead += n;
    }
    if (s.outRead == s.out.size())
    {
        s.out.clear();
        s.outRead = 0;
    }
    else if (s.outRead >= (256u * 1024u))
    {
        s.out.erase(s.out.begin(), s.out.begin() + static_cast<std::ptrdiff_t>(s.outRead));
        s.outRead = 0;
    }
    return n;
}

bool Ps2Ipu::readOutFifo(uint8_t qword[16])
{
    if (queuedOutputBytes() < 16u)
        return false;
    return takeOutput(qword, 16) == 16u;
}

bool Ps2Ipu::busy() const
{
    return m->busy;
}

uint32_t Ps2Ipu::currentCommand() const
{
    return m->cmd;
}

uint64_t Ps2Ipu::commandCount() const
{
    return m->cmdCount;
}
