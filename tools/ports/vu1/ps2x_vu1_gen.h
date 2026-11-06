#pragma once

// Support code for the ahead-of-time recompiled VU1 microprogram (written by
// vu1_recompile.py). The generated code follows PS2Recomp's VU1Interpreter, which is the
// reference: it reproduces the interpreter's values and its cycle model (per-lane stall
// scoreboard, Q latency, delayed MAC/clip flags, XGKICK busy time), but applies register writes
// immediately. That is equivalent because every reader of a pending write stalls until it lands.
// The program is split into regions of a few dozen instruction pairs, each a function of its own
// (MSVC does not optimize a function the size of the whole program at all). Inside a region, the
// registers, cycle counter and scoreboard entries it uses live in locals; run() carries the state
// between regions, and Ctx between runs.
//
// The port provides xgkick() (assemble the packet from data memory, hand it to Ctx::kick, return the
// cycle the transfer ends) and calls run() from its MSCAL/MSCNT handler; see README.md.

#include <immintrin.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

// The generated run() is a single function of ~20k lines, and MSVC stops inlining calls into a
// function that large. These helpers are a few SIMD instructions each and run per VU instruction,
// so a call costs more than the work: force them inline.
#if defined(_MSC_VER)
#define PS2X_VU1_INLINE __forceinline
#else
#define PS2X_VU1_INLINE inline __attribute__((always_inline))
#endif

namespace ps2x::vu1gen
{
    constexpr uint32_t kDataSize = 16u * 1024u;
    constexpr uint32_t kDataMask = kDataSize - 1u;
    constexpr uint32_t kFmacLatency = 4u;

    struct alignas(16) Ctx
    {
        __m128 vf[32];
        __m128 acc;
        int32_t vi[16];
        float q;
        float i;
        uint32_t clip; // committed clip flags
        uint32_t mac;  // committed MAC flags
        uint32_t top;
        uint32_t itop;
        uint32_t pc;     // where MSCNT would continue
        uint32_t cycles; // length of the last run, for profiling
        uint8_t *mem;    // VU1 data memory, kDataSize bytes

        // Delayed flags, committed lazily when read.
        struct MacEntry
        {
            __m128 result;
            uint32_t dest;
            uint32_t ready;
        } macRing[8];
        uint32_t macCount;
        struct ClipEntry
        {
            uint32_t clip;
            uint32_t ready;
        } clipRing[8];
        uint32_t clipCount;
        uint32_t workingClip;

        void (*kick)(void *user, const uint8_t *packet, uint32_t size);
        void *kickUser;
    };

    // Runs the program from startPc (a byte address in micro memory). Returns false, without
    // touching the context, when startPc is not an address of the generated program.
    bool run(Ctx &c, uint32_t startPc);
    // The micro memory image the generated code was built from.
    extern const uint8_t kProgram[];
    extern const uint32_t kProgramSize;

    // ---- helpers used by the generated code ----

    // The stall scoreboard's max (std::max is not inlined into run() either).
    PS2X_VU1_INLINE uint32_t umax(uint32_t a, uint32_t b)
    {
        return a > b ? a : b;
    }

    // normalizeOperand for arithmetic: Inf/NaN bit patterns become +-max. Denormal operands are
    // treated as +-0 by DAZ, which run() turns on together with FTZ and round-toward-zero.
    PS2X_VU1_INLINE __m128 N(__m128 v)
    {
        const __m128i bits = _mm_castps_si128(v);
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i infNan = _mm_cmpeq_epi32(_mm_and_si128(bits, expMask), expMask);
        const __m128i clamped = _mm_or_si128(_mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u))),
                                             _mm_set1_epi32(0x7F7FFFFF));
        return _mm_castsi128_ps(_mm_blendv_epi8(bits, clamped, infNan));
    }

    // Full normalizeOperand (also denormal -> +-0), for operations DAZ does not cover exactly.
    PS2X_VU1_INLINE __m128 NF(__m128 v)
    {
        const __m128i bits = _mm_castps_si128(N(v));
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i denormal = _mm_cmpeq_epi32(_mm_and_si128(bits, expMask), _mm_setzero_si128());
        const __m128i signOnly = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        return _mm_castsi128_ps(_mm_blendv_epi8(bits, signOnly, denormal));
    }

    template <int L>
    PS2X_VU1_INLINE __m128 BC(__m128 v)
    {
        return _mm_shuffle_ps(v, v, _MM_SHUFFLE(L, L, L, L));
    }

    PS2X_VU1_INLINE float nrmf(float f)
    {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0xFFu)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    PS2X_VU1_INLINE float lane(__m128 v, int l)
    {
        alignas(16) float t[4];
        _mm_store_ps(t, v);
        return t[l];
    }

    PS2X_VU1_INLINE uint32_t laneBits(__m128 v, int l)
    {
        alignas(16) uint32_t t[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(t), _mm_castps_si128(v));
        return t[l];
    }

    // FTOIn: truncate toward zero and saturate like the interpreter's vuFloatToInt.
    PS2X_VU1_INLINE __m128 ftoi(__m128 v, float scale)
    {
        const __m128 scaled = _mm_mul_ps(v, _mm_set1_ps(scale));
        __m128i r = _mm_cvttps_epi32(scaled);
        const __m128 positiveOverflow = _mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f));
        r = _mm_blendv_epi8(r, _mm_set1_epi32(0x7FFFFFFF), _mm_castps_si128(positiveOverflow));
        return _mm_castsi128_ps(r);
    }

    // The Q register as an upper instruction reads it: the divide result once its latency passed.
    struct QPipe
    {
        float q;
        float value;
        uint32_t ready;
        bool pending;
    };

    PS2X_VU1_INLINE float qnow(QPipe &p, uint32_t cycle)
    {
        if (p.pending && p.ready <= cycle)
        {
            p.q = p.value;
            p.pending = false;
        }
        return nrmf(p.q);
    }

    PS2X_VU1_INLINE void qissue(QPipe &p, float value, uint32_t cycle, uint32_t latency)
    {
        if (p.pending) // the stall before a divide waited for the previous one
            p.q = p.value;
        p.pending = true;
        p.ready = cycle + latency;
        p.value = nrmf(value);
    }

    PS2X_VU1_INLINE void pushMac(Ctx &c, __m128 result, uint32_t dest, uint32_t cycle)
    {
        Ctx::MacEntry &e = c.macRing[c.macCount++ & 7u];
        e.result = result;
        e.dest = dest;
        e.ready = cycle + kFmacLatency;
    }

    // MAC flags of an FMAC result: Z (lane), S (lane << 4). The exact-result U/O cases are not
    // modelled; the game only reads MAC with FMAND.
    inline uint32_t macOf(const Ctx::MacEntry &e)
    {
        uint32_t mac = 0u;
        for (int l = 0; l < 4; ++l)
        {
            const uint32_t laneBit = 1u << (3 - l);
            if ((e.dest & laneBit) == 0u)
                continue;
            const uint32_t bits = laneBits(e.result, l);
            if ((bits & 0x7FFFFFFFu) == 0u)
                mac |= laneBit;
            if ((bits & 0x80000000u) != 0u)
                mac |= laneBit << 4;
        }
        return mac;
    }

    inline uint32_t macNow(const Ctx &c, uint32_t cycle)
    {
        for (uint32_t k = 0; k < 8u && k < c.macCount; ++k)
        {
            const Ctx::MacEntry &e = c.macRing[(c.macCount - 1u - k) & 7u];
            if (e.ready <= cycle)
                return macOf(e);
        }
        return c.mac;
    }

    PS2X_VU1_INLINE void pushClip(Ctx &c, uint32_t clip, uint32_t cycle)
    {
        Ctx::ClipEntry &e = c.clipRing[c.clipCount++ & 7u];
        e.clip = clip;
        e.ready = cycle + kFmacLatency;
    }

    inline uint32_t clipNow(const Ctx &c, uint32_t cycle)
    {
        for (uint32_t k = 0; k < 8u && k < c.clipCount; ++k)
        {
            const Ctx::ClipEntry &e = c.clipRing[(c.clipCount - 1u - k) & 7u];
            if (e.ready <= cycle)
                return e.clip;
        }
        return c.clip;
    }

    // CLIP: compares raw bits of vf[fs].xyz against |vf[ft].w| as ordered integers.
    PS2X_VU1_INLINE void clipOp(Ctx &c, __m128 vs, __m128 vt, uint32_t cycle)
    {
        const uint32_t wBits = static_cast<uint32_t>(_mm_extract_ps(vt, 3));
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        // Per lane l of x, y, z: +l when bits > limit, -l when (bits ^ sign) > limit, as signed ints.
        const __m128i bits = _mm_castps_si128(vs);
        const __m128i lim = _mm_set1_epi32(limit);
        const uint32_t over = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(bits, lim)))) & 7u;
        const uint32_t under = static_cast<uint32_t>(_mm_movemask_ps(_mm_castsi128_ps(
                                   _mm_cmpgt_epi32(_mm_xor_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u))), lim)))) & 7u;
        // Spread bits 0..2 to 0, 2, 4.
        const uint32_t flags = ((over & 1u) | ((over & 2u) << 1) | ((over & 4u) << 2)) |
                               (((under & 1u) | ((under & 2u) << 1) | ((under & 4u) << 2)) << 1);
        c.workingClip = ((c.workingClip << 6) | flags) & 0xFFFFFFu;
        pushClip(c, c.workingClip, cycle);
    }

    PS2X_VU1_INLINE uint32_t dataAddr(int32_t qwordIndex)
    {
        return (static_cast<uint32_t>(qwordIndex) * 16u) & kDataMask;
    }

    PS2X_VU1_INLINE __m128 loadQ(const uint8_t *mem, uint32_t addr)
    {
        return _mm_loadu_ps(reinterpret_cast<const float *>(mem + addr));
    }

    // dest is the VU field mask: x = 8, y = 4, z = 2, w = 1 (x is lane 0).
    PS2X_VU1_INLINE void storeQ(uint8_t *mem, uint32_t addr, __m128 value, uint32_t dest)
    {
        if (dest != 0xFu)
        {
            const __m128i laneMask = _mm_set_epi32((dest & 1u) ? -1 : 0, (dest & 2u) ? -1 : 0,
                                                   (dest & 4u) ? -1 : 0, (dest & 8u) ? -1 : 0);
            value = _mm_blendv_ps(loadQ(mem, addr), value, _mm_castsi128_ps(laneMask));
        }
        _mm_storeu_ps(reinterpret_cast<float *>(mem + addr), value);
    }

    PS2X_VU1_INLINE int32_t ilw(const uint8_t *mem, uint32_t addr, int component)
    {
        uint32_t v;
        std::memcpy(&v, mem + addr + component * 4, 4);
        return static_cast<int32_t>(static_cast<int16_t>(v & 0xFFFFu));
    }

    PS2X_VU1_INLINE void isw(uint8_t *mem, uint32_t addr, int32_t value, uint32_t dest)
    {
        const uint32_t word = static_cast<uint32_t>(static_cast<uint16_t>(value & 0xFFFF));
        for (int l = 0; l < 4; ++l)
            if (dest & (1u << (3 - l)))
                std::memcpy(mem + addr + l * 4, &word, 4);
    }

    // XGKICK: the interpreter streams the packet to PATH1 at one qword per two cycles and submits
    // it when done; here it is assembled and sent at once. Returns the cycle the transfer ends.
    uint32_t xgkick(Ctx &c, uint32_t qwordAddress, uint32_t cycle);
}
