// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include <cstring>

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    // An IMAGE GIFtag at the end of a PATH2 payload whose data runs past the payload.
    struct TrailingImage
    {
        uint32_t tagOffset = 0u; // byte offset of the GIFtag in the payload
        uint32_t presentQw = 0u; // image qwords that follow it in this payload
        uint32_t missingQw = 0u; // image qwords still to come
        uint64_t tagLo = 0u;
        uint64_t tagHi = 0u;
    };

    bool findTrailingIncompleteImage(const uint8_t *data, uint32_t sizeBytes, TrailingImage &out)
    {
        if (!data || sizeBytes < 16u)
            return false;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            uint64_t tagHi = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            std::memcpy(&tagHi, data + offset + 8u, sizeof(tagHi));
            const uint32_t tagOffset = offset;
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage || flg == 3u) // FLG=3 ("disabled" in the GS manual) behaves exactly like IMAGE: NLOOP qwords of image data.
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    out.tagOffset = tagOffset;
                    out.presentQw = static_cast<uint32_t>(availableBytes / 16ull);
                    out.missingQw = nloop - out.presentQw;
                    out.tagLo = tagLo;
                    out.tagHi = tagHi;
                    return true;
                }
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return false;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return false;
    }

    // IMAGE GIFtag + qwc qwords of data as one self-contained packet; NLOOP and EOP replaced.
    std::vector<uint8_t> makeImagePacket(uint64_t tagLo, uint64_t tagHi, uint32_t qwc, bool eop, const uint8_t *data)
    {
        std::vector<uint8_t> packet(16u + static_cast<size_t>(qwc) * 16u);
        tagLo = (tagLo & ~0xFFFFull) | (qwc & 0x7FFFu) | (eop ? 0x8000ull : 0ull);
        std::memcpy(packet.data(), &tagLo, sizeof(tagLo));
        std::memcpy(packet.data() + 8u, &tagHi, sizeof(tagHi));
        std::memcpy(packet.data() + 16u, data, static_cast<size_t>(qwc) * 16u);
        return packet;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    syncGsWorker("direct vif0");
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

namespace
{
    // UNPACK in the formats the generic loop decodes: the same results as that loop, specialised
    // per format, mode and masking, with SSE4.1. As there, components a format does not carry keep
    // the VU memory's old value (and still get the row added in the offset/difference modes), a
    // single component (S) is written to all four fields, and fill cycles (cl < wl) write the row
    // where the mask selects data. Only the bytes of each vector are read from the source.
    static_assert(PS2_VU1_DATA_SIZE == 0x400u * 16u, "UNPACK addresses wrap at 0x3FF quadwords");

    template <uint32_t VN, uint32_t VL>
    constexpr uint32_t unpackVectorBytes()
    {
        if constexpr (VN == 3u && VL == 3u)
            return 2u;
        else
            return (VN + 1u) * (VL == 0u ? 4u : VL == 1u ? 2u : 1u);
    }

    // Unaligned little loads. MSVC calls memcpy for these at /O1, so read them directly there.
    inline uint32_t unpackLoad16(const uint8_t *p)
    {
#if defined(_MSC_VER)
        return *reinterpret_cast<const __unaligned uint16_t *>(p);
#else
        uint16_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
#endif
    }

    inline uint32_t unpackLoad32(const uint8_t *p)
    {
#if defined(_MSC_VER)
        return *reinterpret_cast<const __unaligned uint32_t *>(p);
#else
        uint32_t v;
        std::memcpy(&v, p, sizeof(v));
        return v;
#endif
    }

    // One source vector as four 32-bit fields. Fields the format does not carry come from old.
    // ext is 0 for zero extension, or the component's sign bit in every lane for sign extension
    // ((x ^ s) - s sign-extends a zero-extended x).
    template <uint32_t VN, uint32_t VL>
    inline __m128i decodeUnpackVector(const uint8_t *src, __m128i old, __m128i ext)
    {
        if constexpr (VN == 3u && VL == 3u)
        {
            const uint32_t packed = unpackLoad16(src);
            return _mm_setr_epi32(static_cast<int>(packed & 0x1Fu), static_cast<int>((packed >> 5) & 0x1Fu),
                                  static_cast<int>((packed >> 10) & 0x1Fu), static_cast<int>((packed >> 15) & 0x01u));
        }
        else if constexpr (VL == 0u)
        {
            if constexpr (VN == 3u)
                return _mm_loadu_si128(reinterpret_cast<const __m128i *>(src));
            else if constexpr (VN == 2u)
            {
                const __m128i xy = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(src));
                return _mm_blend_epi16(old, _mm_insert_epi32(xy, static_cast<int>(unpackLoad32(src + 8u)), 2), 0x3F);
            }
            else if constexpr (VN == 1u)
                return _mm_blend_epi16(old, _mm_loadl_epi64(reinterpret_cast<const __m128i *>(src)), 0x0F);
            else
                return _mm_shuffle_epi32(_mm_cvtsi32_si128(static_cast<int>(unpackLoad32(src))), 0x00);
        }
        else
        {
            __m128i v;
            if constexpr (VL == 1u)
            {
                if constexpr (VN == 3u)
                    v = _mm_cvtepu16_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(src)));
                else if constexpr (VN == 2u)
                    v = _mm_cvtepu16_epi32(_mm_insert_epi16(_mm_cvtsi32_si128(static_cast<int>(unpackLoad32(src))),
                                                            static_cast<int>(unpackLoad16(src + 4u)), 2));
                else if constexpr (VN == 1u)
                    v = _mm_cvtepu16_epi32(_mm_cvtsi32_si128(static_cast<int>(unpackLoad32(src))));
                else
                    v = _mm_shuffle_epi32(_mm_cvtsi32_si128(static_cast<int>(unpackLoad16(src))), 0x00);
            }
            else
            {
                if constexpr (VN == 3u)
                    v = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(unpackLoad32(src))));
                else if constexpr (VN == 2u)
                    v = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(unpackLoad16(src) | (static_cast<uint32_t>(src[2]) << 16))));
                else if constexpr (VN == 1u)
                    v = _mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(unpackLoad16(src))));
                else
                    v = _mm_shuffle_epi32(_mm_cvtsi32_si128(static_cast<int>(src[0])), 0x00);
            }
            v = _mm_sub_epi32(_mm_xor_si128(v, ext), ext);
            if constexpr (VN == 2u)
                return _mm_blend_epi16(old, v, 0x3F);
            else if constexpr (VN == 1u)
                return _mm_blend_epi16(old, v, 0x0F);
            else
                return v;
        }
    }

    // The mask register as lane masks per cycle position (positions past 3 use row 3).
    struct UnpackMasks
    {
        __m128i data[4];    // fields written with the data
        __m128i rowSel[4];  // fields written with the row
        __m128i fill[4];    // fields a fill cycle writes with the row (data or row)
        __m128i protect[4]; // fields left as they are
        __m128i colPart[4]; // the column value in the fields written with it, 0 elsewhere
    };

    inline void buildUnpackMasks(uint32_t mask, const uint32_t *col, UnpackMasks &mk)
    {
        // Byte 4m+f holds the mask byte of cycle m; keep only field f's two bits, in place.
        const __m128i spread = _mm_shuffle_epi8(_mm_cvtsi32_si128(static_cast<int>(mask)),
                                                _mm_setr_epi8(0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3));
        const __m128i fieldBits = _mm_set1_epi32(static_cast<int>(0xC0300C03u));
        const __m128i bits = _mm_and_si128(spread, fieldBits);
        const __m128i isData = _mm_cmpeq_epi8(bits, _mm_setzero_si128());
        const __m128i isRow = _mm_cmpeq_epi8(bits, _mm_set1_epi32(0x40100401));
        const __m128i isCol = _mm_cmpeq_epi8(bits, _mm_set1_epi32(static_cast<int>(0x80200802u)));
        const __m128i isProtect = _mm_cmpeq_epi8(bits, fieldBits);
        const __m128i colv = _mm_loadu_si128(reinterpret_cast<const __m128i *>(col));
        auto widen = [](__m128i bytes, __m128i *out)
        {
            out[0] = _mm_cvtepi8_epi32(bytes);
            out[1] = _mm_cvtepi8_epi32(_mm_srli_si128(bytes, 4));
            out[2] = _mm_cvtepi8_epi32(_mm_srli_si128(bytes, 8));
            out[3] = _mm_cvtepi8_epi32(_mm_srli_si128(bytes, 12));
        };
        widen(isData, mk.data);
        widen(isRow, mk.rowSel);
        widen(_mm_or_si128(isData, isRow), mk.fill);
        widen(isProtect, mk.protect);
        __m128i colSel[4];
        widen(isCol, colSel);
        mk.colPart[0] = _mm_and_si128(colSel[0], _mm_shuffle_epi32(colv, 0x00));
        mk.colPart[1] = _mm_and_si128(colSel[1], _mm_shuffle_epi32(colv, 0x55));
        mk.colPart[2] = _mm_and_si128(colSel[2], _mm_shuffle_epi32(colv, 0xAA));
        mk.colPart[3] = _mm_and_si128(colSel[3], _mm_shuffle_epi32(colv, 0xFF));
    }

    // Games set the mask and column once for many UNPACKs: keep the last lane masks per thread.
    struct UnpackMaskCache
    {
        UnpackMasks masks;
        __m128i col;
        uint32_t mask;
        bool valid;
    };
    thread_local UnpackMaskCache t_unpackMaskCache;

    inline const UnpackMasks &cachedUnpackMasks(uint32_t mask, const uint32_t *col)
    {
        UnpackMaskCache &cache = t_unpackMaskCache;
        const __m128i colv = _mm_loadu_si128(reinterpret_cast<const __m128i *>(col));
        if (!cache.valid || cache.mask != mask || _mm_movemask_epi8(_mm_cmpeq_epi32(cache.col, colv)) != 0xFFFF)
        {
            buildUnpackMasks(mask, col, cache.masks);
            cache.col = colv;
            cache.mask = mask;
            cache.valid = true;
        }
        return cache.masks;
    }

    // MODE: 0 = plain (also mode 3), 1 = offset, 2 = difference. MASKED with mk, else every field data.
    template <uint32_t VN, uint32_t VL, uint32_t MODE, bool MASKED>
    void unpackLoop(uint8_t *vuData, const uint8_t *src, uint32_t count, uint32_t vuAddr, uint32_t cl, uint32_t wl, __m128i ext,
                    const UnpackMasks *mk, uint32_t *rowReg)
    {
        constexpr uint32_t bytes = unpackVectorBytes<VN, VL>();
        constexpr bool needOld = MASKED || VN == 1u || VN == 2u;
        __m128i row = _mm_loadu_si128(reinterpret_cast<const __m128i *>(rowReg));
        // Without the difference mode the row is constant: the row/column part of each cycle
        // position's result is too.
        __m128i fixedPart[4];
        __m128i fillPart[4];
        if constexpr (MASKED && MODE != 2u)
        {
            for (uint32_t m = 0; m < 4u; ++m)
            {
                fixedPart[m] = _mm_or_si128(_mm_and_si128(row, mk->rowSel[m]), mk->colPart[m]);
                fillPart[m] = _mm_or_si128(_mm_and_si128(row, mk->fill[m]), mk->colPart[m]);
            }
        }
        const bool filling = cl < wl;
        uint32_t block = 0u;
        uint32_t cycle = 0u;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t destVec = filling ? vuAddr + i : vuAddr + block + cycle;
            __m128i *dest = reinterpret_cast<__m128i *>(vuData + (destVec & 0x3FFu) * 16u);
            const __m128i old = needOld ? _mm_loadu_si128(dest) : _mm_setzero_si128();
            const uint32_t m = cycle > 3u ? 3u : cycle;
            __m128i result;
            if (!filling || cycle < cl)
            {
                __m128i d = decodeUnpackVector<VN, VL>(src, old, ext);
                src += bytes;
                if constexpr (MODE != 0u)
                    d = _mm_add_epi32(d, row);
                if constexpr (!MASKED)
                {
                    if constexpr (MODE == 2u)
                        row = d;
                    result = d;
                }
                else
                {
                    result = _mm_or_si128(_mm_and_si128(d, mk->data[m]), _mm_and_si128(old, mk->protect[m]));
                    if constexpr (MODE == 2u)
                    {
                        row = _mm_blendv_epi8(row, d, mk->data[m]);
                        result = _mm_or_si128(result, _mm_or_si128(_mm_and_si128(row, mk->rowSel[m]), mk->colPart[m]));
                    }
                    else
                        result = _mm_or_si128(result, fixedPart[m]);
                }
            }
            else
            {
                // Fill cycle: the row where the data would go; no add, no row update.
                if constexpr (!MASKED)
                    result = row;
                else if constexpr (MODE == 2u)
                    result = _mm_or_si128(_mm_or_si128(_mm_and_si128(row, mk->fill[m]), mk->colPart[m]),
                                          _mm_and_si128(old, mk->protect[m]));
                else
                    result = _mm_or_si128(fillPart[m], _mm_and_si128(old, mk->protect[m]));
            }
            _mm_storeu_si128(dest, result);
            if (++cycle == wl)
            {
                cycle = 0u;
                block += cl;
            }
        }
        if constexpr (MODE == 2u)
            _mm_storeu_si128(reinterpret_cast<__m128i *>(rowReg), row);
    }

    template <uint32_t VN, uint32_t VL>
    void unpackFast(uint8_t *vuData, const uint8_t *src, uint32_t count, uint32_t vuAddr, uint32_t cl, uint32_t wl,
                    bool zeroExtend, uint32_t mode, uint32_t mask, uint32_t *row, const uint32_t *col)
    {
        const __m128i ext = (VL == 0u || VL == 3u || zeroExtend) ? _mm_setzero_si128()
                                                                  : _mm_set1_epi32(VL == 1u ? 0x8000 : 0x80);
        const bool masked = mask != 0u; // an all-data mask is the same as no mask
        const UnpackMasks *mk = masked ? &cachedUnpackMasks(mask, col) : nullptr;
        if constexpr (VN == 3u && VL == 3u)
        {
            // V4-5 is never added to the row.
            if (masked)
                unpackLoop<VN, VL, 0u, true>(vuData, src, count, vuAddr, cl, wl, ext, mk, row);
            else
                unpackLoop<VN, VL, 0u, false>(vuData, src, count, vuAddr, cl, wl, ext, nullptr, row);
        }
        else
        {
            switch ((mode == 3u ? 0u : mode) * 2u + (masked ? 1u : 0u))
            {
            case 0:
                unpackLoop<VN, VL, 0u, false>(vuData, src, count, vuAddr, cl, wl, ext, nullptr, row);
                break;
            case 1:
                unpackLoop<VN, VL, 0u, true>(vuData, src, count, vuAddr, cl, wl, ext, mk, row);
                break;
            case 2:
                unpackLoop<VN, VL, 1u, false>(vuData, src, count, vuAddr, cl, wl, ext, nullptr, row);
                break;
            case 3:
                unpackLoop<VN, VL, 1u, true>(vuData, src, count, vuAddr, cl, wl, ext, mk, row);
                break;
            case 4:
                unpackLoop<VN, VL, 2u, false>(vuData, src, count, vuAddr, cl, wl, ext, nullptr, row);
                break;
            default:
                unpackLoop<VN, VL, 2u, true>(vuData, src, count, vuAddr, cl, wl, ext, mk, row);
                break;
            }
        }
    }

    using UnpackFastFn = void (*)(uint8_t *, const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, bool, uint32_t, uint32_t,
                                  uint32_t *, const uint32_t *);

    // Indexed by vn * 4 + vl; null for the formats the generic loop does not decode (V2-5, V3-5, S-5).
    constexpr UnpackFastFn kUnpackFast[16] = {
        unpackFast<0, 0>, unpackFast<0, 1>, unpackFast<0, 2>, nullptr,
        unpackFast<1, 0>, unpackFast<1, 1>, unpackFast<1, 2>, nullptr,
        unpackFast<2, 0>, unpackFast<2, 1>, unpackFast<2, 2>, nullptr,
        unpackFast<3, 0>, unpackFast<3, 1>, unpackFast<3, 2>, unpackFast<3, 3>,
    };

    // Source bytes per vector, by vn * 4 + vl (for the formats in kUnpackFast).
    constexpr uint8_t kUnpackVectorBytes[16] = {
        4u, 2u, 1u, 0u,
        8u, 4u, 2u, 0u,
        12u, 6u, 3u, 0u,
        16u, 8u, 4u, 2u,
    };
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

// Games commonly upload textures over PATH2 as two VIF commands: a DIRECT whose payload ends
// with an IMAGE GIFtag, then (often behind a new DMAtag) a DIRECT whose payload is the pixels.
// The GIF keeps PATH2's IMAGE state across the two; here the image data is taken from the next
// DIRECT payload (never from the VIF stream between them) and each piece is sent as a
// self-contained IMAGE packet, which also matches what a stateful GS sees on hardware.
void PS2Memory::submitVif1DirectPayload(const uint8_t *data, uint32_t sizeBytes, bool directHl)
{
    uint32_t pos = 0u;

    if (m_vif1PendingPath2ImageQwc != 0u)
    {
        const uint32_t qwc = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, sizeBytes / 16u);
        if (qwc == 0u)
            return;
        m_vif1PendingPath2ImageQwc -= qwc;
        const bool last = (m_vif1PendingPath2ImageQwc == 0u);
        const bool eop = last && (m_vif1PendingPath2ImageTag & 0x8000ull) != 0ull;
        const std::vector<uint8_t> packet = makeImagePacket(m_vif1PendingPath2ImageTag, 0ull, qwc, eop, data);
        submitGifPacket(GifPathId::Path2, packet.data(), static_cast<uint32_t>(packet.size()), true, directHl);
        pos = qwc * 16u;
        if (last)
            m_vif1PendingPath2DirectHl = false;
    }

    if (pos + 16u > sizeBytes)
        return;

    TrailingImage trailing;
    if (!findTrailingIncompleteImage(data + pos, sizeBytes - pos, trailing))
    {
        submitGifPacket(GifPathId::Path2, data + pos, sizeBytes - pos, true, directHl);
        return;
    }

    // Hold back the IMAGE tag whose data is incomplete: everything before it, and the part of
    // its data that is already here, go out now; the rest follows with the next DIRECT.
    if (trailing.tagOffset != 0u)
        submitGifPacket(GifPathId::Path2, data + pos, trailing.tagOffset, true, directHl);
    if (trailing.presentQw != 0u)
    {
        const std::vector<uint8_t> packet = makeImagePacket(trailing.tagLo, trailing.tagHi, trailing.presentQw, false,
                                                            data + pos + trailing.tagOffset + 16u);
        submitGifPacket(GifPathId::Path2, packet.data(), static_cast<uint32_t>(packet.size()), true, directHl);
    }
    m_vif1PendingPath2ImageQwc = trailing.missingQw;
    m_vif1PendingPath2ImageTag = trailing.tagLo;
    m_vif1PendingPath2DirectHl = directHl;
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    syncGsWorker("direct vif1");
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        // Finish a DIRECT/DIRECTHL payload that started in an earlier call.
        if (m_vif1DirectRemainingBytes != 0u)
        {
            const uint32_t take = std::min<uint32_t>(m_vif1DirectRemainingBytes, sizeBytes - pos);
            m_vif1DirectBuffer.insert(m_vif1DirectBuffer.end(), data + pos, data + pos + take);
            m_vif1DirectRemainingBytes -= take;
            pos += take;
            if (m_vif1DirectRemainingBytes == 0u)
            {
                submitVif1DirectPayload(m_vif1DirectBuffer.data(), static_cast<uint32_t>(m_vif1DirectBuffer.size()), m_vif1DirectHl);
                m_vif1DirectBuffer.clear();
            }
            continue;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        // UNPACK, the most frequent command, first: the specialised loops. Formats they do not
        // handle (and every UNPACK with s_vif1FastUnpack off) go to the generic loop further down.
        if ((opcode & 0x60u) == 0x60u && s_vif1FastUnpack)
        {
            const uint32_t format = opcode & 0xFu;
            const UnpackFastFn fastUnpack = kUnpackFast[format];
            if (fastUnpack)
            {
                const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
                uint32_t cl = vif1_regs.cycle & 0xFFu;
                uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
                if (cl == 0u)
                    cl = 1u;
                if (wl == 0u)
                    wl = 1u;
                uint32_t sourceVectorCount = writeVectorCount;
                if (cl < wl)
                {
                    const uint32_t remainder = writeVectorCount % wl;
                    sourceVectorCount = (writeVectorCount / wl) * cl + (remainder > cl ? cl : remainder);
                }
                const uint32_t totalBytes = (sourceVectorCount * kUnpackVectorBytes[format] + 3u) & ~3u;
                if (m_vu1Data && pos + totalBytes <= sizeBytes)
                {
                    uint32_t vuAddr = static_cast<uint32_t>(imm) & 0x3FFu;
                    if ((imm & 0x8000u) != 0u)
                        vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;
                    fastUnpack(m_vu1Data, data + pos, writeVectorCount, vuAddr, cl, wl, (imm & 0x4000u) != 0u,
                               vif1_regs.mode & 3u, (opcode & 0x10u) != 0u ? vif1_regs.mask : 0u, vif1_regs.row, vif1_regs.col);
                }
                pos += totalBytes;
                if (pos > sizeBytes)
                    break;
                continue;
            }
        }

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            const uint32_t payloadBytes = (imm == 0u ? 65536u : static_cast<uint32_t>(imm)) * 16u;
            const bool directHl = (opcode == VIF_DIRECTHL);
            const uint32_t availableBytes = sizeBytes - pos;
            if (payloadBytes <= availableBytes)
            {
                submitVif1DirectPayload(data + pos, payloadBytes, directHl);
                pos += payloadBytes;
                continue;
            }

            // The payload continues in the next transfer (FIFO writes, chunked DMA): keep what
            // is here and send the whole payload once it is complete.
            m_vif1DirectBuffer.assign(data + pos, data + sizeBytes);
            m_vif1DirectRemainingBytes = payloadBytes - availableBytes;
            m_vif1DirectHl = directHl;
            pos = sizeBytes;
            break;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
}
