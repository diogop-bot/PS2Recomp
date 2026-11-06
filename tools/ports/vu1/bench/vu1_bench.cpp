// Differential test and benchmark for the recompiled VU1 program.
//   build: build.ps1 -Old <old program.cpp> [-New <new program.cpp>]
//   run:   vu1_bench.exe <capture file> [repeat]
// Runs every captured input (BenchRecord in vu1_bench.h, written by the port) through both builds of the
// generated program and compares registers, flags, Q, the end pc, the cycle count, data memory and
// every XGKICK packet. Then times both.
#include "vu1_bench.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    std::string compare(const BenchOut &a, const BenchOut &b)
    {
        std::string d;
        auto add = [&](const std::string &s) { d += (d.empty() ? "" : ", ") + s; };
        if (a.ran != b.ran)
            add("ran");
        for (int r = 1; r < 32; ++r)
            if (std::memcmp(a.vf[r], b.vf[r], 16) != 0)
                add("vf" + std::to_string(r));
        if (std::memcmp(a.acc, b.acc, 16) != 0)
            add("acc");
        for (int r = 1; r < 16; ++r)
            if (static_cast<int16_t>(a.vi[r]) != static_cast<int16_t>(b.vi[r]))
                add("vi" + std::to_string(r));
        if (a.q != b.q) add("q");
        if (a.i != b.i) add("i");
        if (a.clip != b.clip) add("clip");
        if (a.mac != b.mac) add("mac");
        if (a.pc != b.pc) add("pc");
        if (a.cycles != b.cycles) add("cycles " + std::to_string(a.cycles) + " vs " + std::to_string(b.cycles));
        if (std::memcmp(a.mem, b.mem, sizeof(a.mem)) != 0) add("data memory");
        if (a.packets != b.packets) add("xgkick packets");
        return d;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: vu1_bench <capture file> [repeat]\n");
        return 2;
    }
    const int repeat = argc > 2 ? std::atoi(argv[2]) : 5;
    FILE *f = std::fopen(argv[1], "rb");
    if (!f)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    const size_t count = static_cast<size_t>(size) / sizeof(BenchRecord);
    std::unique_ptr<BenchRecord[]> records(new BenchRecord[count]);
    if (std::fread(records.get(), sizeof(BenchRecord), count, f) != count)
        return 2;
    std::fclose(f);

    auto a = std::make_unique<BenchOut>();
    auto b = std::make_unique<BenchOut>();
    size_t mismatched = 0, cycleOnly = 0;
    for (size_t n = 0; n < count; ++n)
    {
        if (n % 500 == 0)
            std::fprintf(stderr, "record %zu (entry 0x%04x)\n", n, records[n].entry);
        runOld(records[n], *a);
        runNew(records[n], *b);
        const std::string d = compare(*a, *b);
        if (!d.empty())
        {
            const bool onlyCycles = d.rfind("cycles", 0) == 0 && d.find(',') == std::string::npos;
            ++(onlyCycles ? cycleOnly : mismatched);
            if (mismatched + cycleOnly <= 20)
                std::printf("record %zu (entry 0x%04x): %s\n", n, records[n].entry, d.c_str());
        }
    }
    std::printf("%zu runs compared: %zu differ, %zu differ only in the cycle count\n", count, mismatched, cycleOnly);

    uint64_t cyclesOld = 0, cyclesNew = 0;
    timeOld(records.get(), count, 1, &cyclesOld); // warm up
    timeNew(records.get(), count, 1, &cyclesNew);
    cyclesOld = cyclesNew = 0;
    const int64_t nsOld = timeOld(records.get(), count, repeat, &cyclesOld);
    const int64_t nsNew = timeNew(records.get(), count, repeat, &cyclesNew);
    std::printf("old: %.1f ms, %.2f ns per VU cycle\n", nsOld / 1e6, static_cast<double>(nsOld) / cyclesOld);
    std::printf("new: %.1f ms, %.2f ns per VU cycle (%.2fx)\n", nsNew / 1e6, static_cast<double>(nsNew) / cyclesNew,
                static_cast<double>(nsOld) / nsNew);
    return mismatched || cycleOnly ? 1 : 0;
}
