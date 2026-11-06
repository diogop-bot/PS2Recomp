// VIF1 UNPACK: the fast loops (PS2Memory::s_vif1FastUnpack) against the generic reference loop.
// Every test runs the same VIF1 packet through both paths from the same VU1 memory and VIF1
// registers and compares VU1 memory byte for byte and the VIF1 registers (ROW for the
// difference mode).
//
// Set PS2X_VIF_BENCH=1 to also register a timing micro-benchmark (not part of the default run);
// PS2X_VIF_BENCH_NUM sets the vectors per UNPACK (default 48). On a hybrid CPU pin it to a
// performance core, e.g. from cmd: start /affinity 4 /high /wait /b ps2x_tests.exe
#include "MiniTest.h"
#include "runtime/ps2_memory.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    struct Rng
    {
        uint64_t state;
        explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1u) {}
        uint32_t next()
        {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            return static_cast<uint32_t>(state >> 33);
        }
        uint32_t below(uint32_t n) { return next() % n; }
    };

    uint32_t vifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) | (static_cast<uint32_t>(num) << 16) | imm;
    }

    void put32(std::vector<uint8_t> &dst, uint32_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + 4u);
        std::memcpy(dst.data() + pos, &value, 4u);
    }

    uint32_t vectorBytes(uint32_t vn, uint32_t vl)
    {
        if (vl == 3u)
            return vn == 3u ? 2u : (vn + 1u) * 2u; // V4-5; the others are what the generic loop assumes
        return (vn + 1u) * (vl == 0u ? 4u : vl == 1u ? 2u : 1u);
    }

    uint32_t sourceVectors(uint32_t count, uint32_t cl, uint32_t wl)
    {
        if (cl >= wl)
            return count;
        const uint32_t rem = count % wl;
        return (count / wl) * cl + (rem > cl ? cl : rem);
    }

    const char *formatName(uint32_t format)
    {
        static const char *names[16] = {"S-32", "S-16", "S-8", "S-5?", "V2-32", "V2-16", "V2-8", "V2-5?",
                                        "V3-32", "V3-16", "V3-8", "V3-5?", "V4-32", "V4-16", "V4-8", "V4-5"};
        return names[format & 15u];
    }

    struct UnpackCase
    {
        uint32_t format = 0u; // vn * 4 + vl
        bool masked = false;
        bool zeroExtend = false;
        bool topsRelative = false;
        uint32_t mode = 0u;
        uint32_t cl = 1u;
        uint32_t wl = 1u;
        uint32_t num = 1u; // 0 = 256
        uint32_t addr = 0u;
        uint32_t mask = 0u;
        uint32_t row[4] = {};
        uint32_t col[4] = {};
    };

    std::string describe(const UnpackCase &c)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s %s%s mode=%u cl=%u wl=%u num=%u addr=0x%03X%s mask=0x%08X",
                      formatName(c.format), c.zeroExtend ? "zext" : "sext", c.masked ? " masked" : "", c.mode, c.cl,
                      c.wl, c.num, c.addr, c.topsRelative ? "+tops" : "", c.mask);
        return buf;
    }

    // Register setup + UNPACK + its data (padded to a word, as the VIF requires).
    void appendUnpack(std::vector<uint8_t> &packet, const UnpackCase &c, Rng &rng, bool setRegs = true)
    {
        if (setRegs)
        {
            put32(packet, vifCmd(0x01u, 0u, static_cast<uint16_t>((c.cl & 0xFFu) | ((c.wl & 0xFFu) << 8)))); // STCYCL
            put32(packet, vifCmd(0x05u, 0u, static_cast<uint16_t>(c.mode)));                                // STMOD
            put32(packet, vifCmd(0x20u, 0u, 0u));                                                           // STMASK
            put32(packet, c.mask);
            put32(packet, vifCmd(0x30u, 0u, 0u)); // STROW
            for (uint32_t r : c.row)
                put32(packet, r);
            put32(packet, vifCmd(0x31u, 0u, 0u)); // STCOL
            for (uint32_t r : c.col)
                put32(packet, r);
        }
        const uint16_t imm = static_cast<uint16_t>((c.addr & 0x3FFu) | (c.zeroExtend ? 0x4000u : 0u) |
                                                   (c.topsRelative ? 0x8000u : 0u));
        put32(packet, vifCmd(static_cast<uint8_t>(0x60u | (c.masked ? 0x10u : 0u) | c.format), static_cast<uint8_t>(c.num), imm));
        const uint32_t count = c.num == 0u ? 256u : c.num;
        const uint32_t dataBytes = (sourceVectors(count, c.cl == 0u ? 1u : c.cl, c.wl == 0u ? 1u : c.wl) *
                                        vectorBytes(c.format >> 2, c.format & 3u) +
                                    3u) &
                                   ~3u;
        for (uint32_t i = 0; i < dataBytes; ++i)
            packet.push_back(static_cast<uint8_t>(rng.next()));
    }

    // One PS2Memory; each comparison restores VU1 memory and the VIF1 registers between the paths.
    struct Harness
    {
        std::unique_ptr<PS2Memory> mem = std::make_unique<PS2Memory>();
        bool ok = mem->initialize();
        std::vector<uint8_t> initialVu = std::vector<uint8_t>(PS2_VU1_DATA_SIZE);
        std::vector<uint8_t> fastVu = std::vector<uint8_t>(PS2_VU1_DATA_SIZE);

        void randomizeVu(Rng &rng)
        {
            for (size_t i = 0; i < initialVu.size(); i += 4u)
            {
                const uint32_t v = rng.next() ^ (rng.next() << 16);
                std::memcpy(initialVu.data() + i, &v, 4u);
            }
        }

        // True when both paths leave the same VU1 memory and VIF1 registers.
        bool compare(const uint8_t *packet, uint32_t size, uint32_t tops, std::string &why)
        {
            VIFRegisters regs{};
            regs.tops = tops;
            regs.cycle = 0x0101u;

            std::memcpy(mem->getVU1Data(), initialVu.data(), PS2_VU1_DATA_SIZE);
            mem->vif1_regs = regs;
            PS2Memory::s_vif1FastUnpack = true;
            mem->processVIF1Data(packet, size);
            std::memcpy(fastVu.data(), mem->getVU1Data(), PS2_VU1_DATA_SIZE);
            const VIFRegisters fastRegs = mem->vif1_regs;

            std::memcpy(mem->getVU1Data(), initialVu.data(), PS2_VU1_DATA_SIZE);
            mem->vif1_regs = regs;
            PS2Memory::s_vif1FastUnpack = false;
            mem->processVIF1Data(packet, size);
            PS2Memory::s_vif1FastUnpack = true;

            const uint8_t *ref = mem->getVU1Data();
            for (uint32_t i = 0; i < PS2_VU1_DATA_SIZE; ++i)
            {
                if (fastVu[i] != ref[i])
                {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf), "VU1 byte 0x%04X (qword 0x%03X field %u): fast 0x%02X, reference 0x%02X", i,
                                  i / 16u, (i / 4u) & 3u, fastVu[i], ref[i]);
                    why = buf;
                    return false;
                }
            }
            if (std::memcmp(fastRegs.row, mem->vif1_regs.row, sizeof(fastRegs.row)) != 0)
            {
                char buf[200];
                std::snprintf(buf, sizeof(buf), "ROW: fast %08X %08X %08X %08X, reference %08X %08X %08X %08X", fastRegs.row[0],
                              fastRegs.row[1], fastRegs.row[2], fastRegs.row[3], mem->vif1_regs.row[0], mem->vif1_regs.row[1],
                              mem->vif1_regs.row[2], mem->vif1_regs.row[3]);
                why = buf;
                return false;
            }
            if (std::memcmp(&fastRegs, &mem->vif1_regs, sizeof(fastRegs)) != 0)
            {
                why = "VIF1 registers other than ROW differ";
                return false;
            }
            return true;
        }

        bool compare(const std::vector<uint8_t> &packet, uint32_t tops, std::string &why)
        {
            return compare(packet.data(), static_cast<uint32_t>(packet.size()), tops, why);
        }
    };

    // Masks that exercise the interesting selections: all data, all one kind, mixed.
    uint32_t pickMask(uint32_t variant, Rng &rng)
    {
        switch (variant)
        {
        case 0:
            return 0u;                         // masked UNPACK, every field data
        case 1:
            return rng.next() ^ (rng.next() << 16); // anything
        case 2:
        {
            // Mostly data with the odd row/col/protect field (typical of games).
            uint32_t m = 0u;
            for (uint32_t f = 0; f < 16u; ++f)
                if (rng.below(4u) == 0u)
                    m |= (1u + rng.below(3u)) << (f * 2u);
            return m;
        }
        case 3:
            return 0x55555555u; // all row
        case 4:
            return 0xAAAAAAAAu; // all column
        default:
            return 0xFFFFFFFFu; // all write-protect
        }
    }

    void randomRowCol(UnpackCase &c, Rng &rng)
    {
        for (int i = 0; i < 4; ++i)
        {
            c.row[i] = rng.next() ^ (rng.next() << 15);
            c.col[i] = rng.next() ^ (rng.next() << 15);
        }
        // Small rows as well, so offset/difference sums do not always wrap.
        if (rng.below(2u) == 0u)
            for (uint32_t &r : c.row)
                r &= 0xFFFu;
    }

    void runBenchmark();
}

void register_ps2_vif1_unpack_tests()
{
    MiniTest::Case("VIF1Unpack", [](TestCase &tc)
    {
        tc.Run("fast UNPACK matches the reference for every format, extension, mask, mode and cycle", [](TestCase &t)
        {
            Harness h;
            t.IsTrue(h.ok, "PS2Memory initialize should succeed");
            if (!h.ok)
                return;
            static const uint32_t cycles[][2] = {{1, 1}, {4, 4}, {2, 4}, {1, 4}, {3, 7}, {4, 2}, {4, 1}, {7, 3},
                                                 {8, 8}, {5, 6}, {6, 5}, {0, 0}, {0, 3}, {3, 0}, {16, 16}, {2, 9}};
            Rng rng(1u);
            uint32_t cases = 0u, failures = 0u;
            for (uint32_t format = 0; format < 16u; ++format)
                for (uint32_t ext = 0; ext < 2u; ++ext)
                    for (uint32_t maskVariant = 0; maskVariant < 7u; ++maskVariant)
                        for (uint32_t mode = 0; mode < 4u; ++mode)
                            for (const auto &cyc : cycles)
                            {
                                if (failures >= 8u)
                                    break;
                                h.randomizeVu(rng);
                                UnpackCase c;
                                c.format = format;
                                c.zeroExtend = ext != 0u;
                                c.masked = maskVariant != 6u;
                                c.mask = maskVariant == 6u ? rng.next() : pickMask(maskVariant, rng); // unmasked: STMASK set but ignored
                                c.mode = mode;
                                c.cl = cyc[0];
                                c.wl = cyc[1];
                                c.num = rng.below(5u) == 0u ? 0u : 1u + rng.below(255u);
                                // Start anywhere, often right before the end of VU1 memory so the write wraps.
                                const uint32_t addrKind = rng.below(3u);
                                c.addr = addrKind == 0u ? rng.below(0x400u) : 0x3FFu - rng.below(16u);
                                c.topsRelative = addrKind == 2u;
                                const uint32_t tops = c.topsRelative ? 0x3F0u + rng.below(16u) : rng.below(0x400u);
                                randomRowCol(c, rng);
                                std::vector<uint8_t> packet;
                                appendUnpack(packet, c, rng);
                                ++cases;
                                std::string why;
                                if (!h.compare(packet, tops, why))
                                {
                                    ++failures;
                                    t.Fail(describe(c) + ": " + why);
                                }
                            }
            t.IsTrue(cases > 10000u, "every combination should run");
        });

        tc.Run("fast UNPACK matches the reference on random command sequences", [](TestCase &t)
        {
            // Several UNPACKs per packet with register changes in between: the row carries over in
            // the difference mode, and cl/wl, masks and addresses are random (cl/wl up to 255).
            Harness h;
            t.IsTrue(h.ok, "PS2Memory initialize should succeed");
            if (!h.ok)
                return;
            Rng rng(7u);
            uint32_t failures = 0u;
            for (int round = 0; round < 1500 && failures < 8u; ++round)
            {
                h.randomizeVu(rng);
                std::vector<uint8_t> packet;
                std::string what;
                const uint32_t commands = 1u + rng.below(8u);
                UnpackCase c;
                for (uint32_t i = 0; i < commands; ++i)
                {
                    // Without new register settings the UNPACK uses the previous command's cycle,
                    // mode, mask, row and column (the row as the difference mode left it).
                    const bool setRegs = i == 0u || rng.below(2u) == 0u;
                    if (setRegs)
                    {
                        c.mask = pickMask(rng.below(6u), rng);
                        c.mode = rng.below(4u);
                        const uint32_t cycKind = rng.below(4u);
                        c.cl = cycKind == 0u ? rng.below(256u) : 1u + rng.below(8u);
                        c.wl = cycKind == 0u ? rng.below(256u) : cycKind == 1u ? c.cl : 1u + rng.below(8u);
                        randomRowCol(c, rng);
                    }
                    c.format = rng.below(16u);
                    c.zeroExtend = rng.below(2u) != 0u;
                    c.masked = rng.below(4u) != 0u;
                    c.num = rng.below(256u);
                    c.addr = rng.below(0x400u);
                    c.topsRelative = rng.below(3u) == 0u;
                    appendUnpack(packet, c, rng, setRegs);
                    if (rng.below(4u) == 0u)
                        put32(packet, vifCmd(0x00u, 0u, 0u)); // NOP between commands
                    what += describe(c) + "; ";
                }
                std::string why;
                if (!h.compare(packet, rng.below(0x400u), why))
                {
                    ++failures;
                    t.Fail(what + why);
                }
            }
        });

        tc.Run("fast UNPACK skips data that does not fit in the packet like the reference", [](TestCase &t)
        {
            Harness h;
            t.IsTrue(h.ok, "PS2Memory initialize should succeed");
            if (!h.ok)
                return;
            Rng rng(99u);
            bool allMatch = true;
            for (uint32_t format = 0; format < 16u && allMatch; ++format)
            {
                h.randomizeVu(rng);
                UnpackCase c;
                c.format = format;
                c.cl = 4u;
                c.wl = 4u;
                c.num = 32u;
                c.addr = 0x3F0u;
                randomRowCol(c, rng);
                std::vector<uint8_t> packet;
                appendUnpack(packet, c, rng);
                for (uint32_t cut = 1u; cut <= 8u && allMatch; ++cut)
                {
                    std::string why;
                    allMatch = h.compare(packet.data(), static_cast<uint32_t>(packet.size()) - cut * 4u, 0u, why);
                    if (!allMatch)
                        t.Fail(describe(c) + " cut " + std::to_string(cut * 4u) + ": " + why);
                }
            }
        });

#ifdef _WIN32
        tc.Run("fast UNPACK does not read past the end of the packet", [](TestCase &t)
        {
            // The packet ends exactly at a no-access page: an over-read faults.
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            const size_t page = si.dwPageSize;
            const size_t span = page * 4u;
            uint8_t *block = static_cast<uint8_t *>(VirtualAlloc(nullptr, span + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            t.IsNotNull(block, "VirtualAlloc should succeed");
            if (!block)
                return;
            DWORD oldProtect = 0;
            VirtualProtect(block + span, page, PAGE_NOACCESS, &oldProtect);

            Harness h;
            t.IsTrue(h.ok, "PS2Memory initialize should succeed");
            Rng rng(5u);
            bool allMatch = h.ok;
            for (uint32_t format = 0; format < 16u && allMatch; ++format)
                for (uint32_t masked = 0; masked < 2u && allMatch; ++masked)
                    for (uint32_t num = 1u; num <= 9u && allMatch; ++num)
                    {
                        h.randomizeVu(rng);
                        UnpackCase c;
                        c.format = format;
                        c.masked = masked != 0u;
                        c.mask = pickMask(2u, rng);
                        c.mode = rng.below(3u);
                        c.cl = num & 1u ? 4u : 2u;
                        c.wl = 4u;
                        c.num = num;
                        c.addr = rng.below(0x400u);
                        randomRowCol(c, rng);
                        std::vector<uint8_t> packet;
                        appendUnpack(packet, c, rng);
                        uint8_t *at = block + span - packet.size();
                        std::memcpy(at, packet.data(), packet.size());
                        std::string why;
                        allMatch = h.compare(at, static_cast<uint32_t>(packet.size()), 0u, why);
                        if (!allMatch)
                            t.Fail(describe(c) + ": " + why);
                    }
            VirtualFree(block, 0, MEM_RELEASE);
        });
#endif

        tc.Run("fast UNPACK writes the expected values for known inputs", [](TestCase &t)
        {
            // A few hand-checked results, so a bug shared by both paths is caught too.
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            for (int fast = 0; fast < 2; ++fast)
            {
                PS2Memory::s_vif1FastUnpack = fast != 0;
                uint8_t *vu = mem.getVU1Data();
                std::memset(vu, 0xEE, PS2_VU1_DATA_SIZE);
                mem.vif1_regs = VIFRegisters{};
                std::vector<uint8_t> p;
                put32(p, vifCmd(0x01u, 0u, 0x0404u)); // cl = wl = 4
                put32(p, vifCmd(0x30u, 0u, 0u));      // STROW 1 2 3 4
                for (uint32_t r = 1u; r <= 4u; ++r)
                    put32(p, r);
                put32(p, vifCmd(0x6Du, 1u, 0x3FFu)); // V4-16 sign extended at the last qword
                put32(p, 0xFFFF8000u);
                put32(p, 0x00027FFFu);
                put32(p, vifCmd(0x05u, 0u, 1u));      // offset mode
                put32(p, vifCmd(0x6Eu, 1u, 0x4000u)); // V4-8 zero extended at qword 0, plus the row
                put32(p, 0x80FF7F01u);
                put32(p, vifCmd(0x05u, 0u, 0u));
                put32(p, vifCmd(0x20u, 0u, 0u)); // STMASK: x data, y row, z col, w protect (cycle 0)
                put32(p, 0x000000E4u);
                put32(p, vifCmd(0x31u, 0u, 0u)); // STCOL
                for (uint32_t r = 0u; r < 4u; ++r)
                    put32(p, 0xC0u + r);
                put32(p, vifCmd(0x78u, 1u, 2u)); // masked V3-32 at qword 2
                put32(p, 0x11111111u);
                put32(p, 0x22222222u);
                put32(p, 0x33333333u);
                mem.processVIF1Data(p.data(), static_cast<uint32_t>(p.size()));

                uint32_t q[4];
                std::memcpy(q, vu + 0x3FFu * 16u, 16u);
                t.IsTrue(q[0] == 0xFFFF8000u && q[1] == 0xFFFFFFFFu && q[2] == 0x00007FFFu && q[3] == 0x00000002u,
                         std::string(fast ? "fast" : "reference") + ": V4-16 sign extension");
                std::memcpy(q, vu, 16u);
                t.IsTrue(q[0] == 0x02u && q[1] == 0x81u && q[2] == 0x102u && q[3] == 0x84u,
                         std::string(fast ? "fast" : "reference") + ": V4-8 zero extension plus row");
                std::memcpy(q, vu + 32u, 16u);
                t.IsTrue(q[0] == 0x11111111u && q[1] == 2u && q[2] == 0xC0u && q[3] == 0xEEEEEEEEu,
                         std::string(fast ? "fast" : "reference") + ": masked V3-32");
            }
            PS2Memory::s_vif1FastUnpack = true;
        });
    });

    const char *bench = std::getenv("PS2X_VIF_BENCH");
    if (bench && bench[0] == '1')
    {
        MiniTest::Case("VIF1UnpackBench", [](TestCase &tc)
        {
            tc.Run("UNPACK micro-benchmark (fast vs reference)", [](TestCase &) { runBenchmark(); });
        });
    }
}

namespace
{
    // ns per written vector over a packet of UNPACKs, for one path.
    double timePacket(PS2Memory &mem, const std::vector<uint8_t> &packet, uint32_t vectors, bool fast)
    {
        PS2Memory::s_vif1FastUnpack = fast;
        mem.vif1_regs = VIFRegisters{};
        mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size())); // warm up
        double best = 1e30;
        for (int trial = 0; trial < 7; ++trial)
        {
            const int reps = 40;
            const auto start = std::chrono::steady_clock::now();
            for (int r = 0; r < reps; ++r)
                mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            const double perVector = ns / (static_cast<double>(reps) * vectors);
            if (perVector < best)
                best = perVector;
        }
        PS2Memory::s_vif1FastUnpack = true;
        return best;
    }

    void runBenchmark()
    {
        PS2Memory mem;
        if (!mem.initialize())
            return;
        struct Variant
        {
            const char *name;
            bool masked;
            uint32_t maskVariant;
            uint32_t mode;
            uint32_t cl, wl;
        };
        static const Variant variants[] = {
            {"plain", false, 0u, 0u, 4u, 4u},
            {"mask", true, 2u, 0u, 4u, 4u},
            {"offset", false, 0u, 1u, 4u, 4u},
            {"mask+offset", true, 2u, 1u, 4u, 4u},
            {"diff", false, 0u, 2u, 4u, 4u},
            {"mask cl1wl1", true, 2u, 0u, 1u, 1u},
            {"fill cl2wl4", true, 2u, 0u, 2u, 4u},
            {"skip cl4wl2", false, 0u, 0u, 4u, 2u},
        };
        static const uint32_t formats[] = {12u, 13u, 8u, 5u, 14u, 0u, 9u, 4u, 15u};
        const uint32_t numPerCommand = static_cast<uint32_t>(std::getenv("PS2X_VIF_BENCH_NUM") ? std::atoi(std::getenv("PS2X_VIF_BENCH_NUM")) : 48);
        const uint32_t commands = 64u;
        std::printf("\n    UNPACK ns/vector, %u commands x num=%u (best of 7)\n", commands, numPerCommand);
        std::printf("    %-8s %-13s %10s %10s %8s\n", "format", "variant", "reference", "fast", "speedup");
        Rng rng(3u);
        for (uint32_t format : formats)
        {
            for (const Variant &v : variants)
            {
                std::vector<uint8_t> packet;
                for (uint32_t i = 0; i < commands; ++i)
                {
                    UnpackCase c;
                    c.format = format;
                    c.masked = v.masked;
                    c.mask = v.masked ? pickMask(v.maskVariant, rng) : 0u;
                    c.mode = v.mode;
                    c.cl = v.cl;
                    c.wl = v.wl;
                    c.num = numPerCommand & 0xFFu;
                    c.addr = (i * 61u) & 0x3FFu;
                    c.zeroExtend = (i & 1u) != 0u;
                    randomRowCol(c, rng);
                    appendUnpack(packet, c, rng, i == 0u);
                }
                const uint32_t vectors = commands * (numPerCommand == 0u ? 256u : numPerCommand);
                const double ref = timePacket(mem, packet, vectors, false);
                const double fast = timePacket(mem, packet, vectors, true);
                std::printf("    %-8s %-13s %10.2f %10.2f %7.2fx\n", formatName(format), v.name, ref, fast, ref / fast);
            }
        }
        std::fflush(stdout);
    }
}
