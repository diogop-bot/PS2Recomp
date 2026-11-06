// One side of tools/vu1_bench: a generated VU1 program plus the support it needs, compiled once
// per variant with /Dvu1gen=<namespace> /DBENCH_SIDE=<function name>.
#include "ps2x_vu1_gen.h"
#include "vu1_bench.h"

#include <chrono>
#include <cstring>

namespace ps2x::vu1gen
{
    // Same as the Midnight Club port's src/mc_vu1_fast.cpp: assemble the packet, record it.
    uint32_t xgkick(Ctx &c, uint32_t qwordAddress, uint32_t cycle)
    {
        static uint8_t packet[0x10000];
        const uint32_t source = (qwordAddress * 16u) % kDataSize;
        uint32_t copied = 0u;
        auto copyBytes = [&](uint32_t count)
        {
            for (uint32_t k = 0; k < count; k += 16u)
                std::memcpy(packet + copied + k, c.mem + (source + copied + k) % kDataSize, 16u);
            copied += count;
        };
        for (;;)
        {
            if (copied > sizeof(packet) - 16u)
                return cycle;
            const uint32_t tagOffset = copied;
            copyBytes(16u);
            uint64_t tagLo;
            std::memcpy(&tagLo, packet + tagOffset, 8);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            uint64_t payload = 0u;
            if (format == 0u)
                payload = static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                payload = ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else
                payload = static_cast<uint64_t>(nloop) * 16u;
            if (payload > sizeof(packet) - copied)
                return cycle;
            copyBytes(static_cast<uint32_t>(payload));
            if ((tagLo >> 15) & 1u)
                break;
        }
        c.kick(c.kickUser, packet, copied);
        return cycle + 2u * (copied / 16u) - 1u;
    }
}

namespace
{
    void keepPacket(void *user, const uint8_t *packet, uint32_t size)
    {
        static_cast<BenchOut *>(user)->packets.emplace_back(packet, packet + size);
    }

    void countPacket(void *user, const uint8_t *, uint32_t size)
    {
        *static_cast<uint64_t *>(user) += size;
    }

    void load(ps2x::vu1gen::Ctx &c, const BenchRecord &r, uint8_t *mem)
    {
        std::memcpy(c.vf, r.vf, sizeof(r.vf));
        std::memcpy(&c.acc, r.acc, 16);
        std::memcpy(c.vi, r.vi, sizeof(r.vi));
        std::memcpy(&c.q, &r.q, 4);
        std::memcpy(&c.i, &r.i, 4);
        c.clip = r.clip;
        c.mac = r.mac;
        c.top = r.top;
        c.itop = r.itop;
        c.pc = 0u;
        c.cycles = 0u;
        std::memcpy(mem, r.mem, sizeof(r.mem));
        c.mem = mem;
        c.macCount = 0u;
        c.clipCount = 0u;
        c.workingClip = r.clip;
    }
}

void BENCH_SIDE(const BenchRecord &r, BenchOut &out)
{
    alignas(16) static ps2x::vu1gen::Ctx c;
    load(c, r, out.mem);
    out.packets.clear();
    c.kick = keepPacket;
    c.kickUser = &out;
    out.ran = ps2x::vu1gen::run(c, r.entry);
    std::memcpy(out.vf, c.vf, sizeof(out.vf));
    std::memcpy(out.acc, &c.acc, 16);
    std::memcpy(out.vi, c.vi, sizeof(out.vi));
    std::memcpy(&out.q, &c.q, 4);
    std::memcpy(&out.i, &c.i, 4);
    out.clip = c.clip;
    out.mac = c.mac;
    out.pc = c.pc;
    out.cycles = c.cycles;
}

// Runs every record `repeat` times; returns the nanoseconds spent inside run() and adds the VU
// cycles to *cycles.
int64_t BENCH_TIME(const BenchRecord *records, size_t count, int repeat, uint64_t *cycles)
{
    alignas(16) static ps2x::vu1gen::Ctx c;
    alignas(16) static uint8_t mem[ps2x::vu1gen::kDataSize];
    uint64_t bytes = 0u;
    int64_t ns = 0;
    for (int k = 0; k < repeat; ++k)
        for (size_t n = 0; n < count; ++n)
        {
            load(c, records[n], mem);
            c.kick = countPacket;
            c.kickUser = &bytes;
            const auto t0 = std::chrono::steady_clock::now();
            ps2x::vu1gen::run(c, records[n].entry);
            ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            *cycles += c.cycles;
        }
    return ns;
}
