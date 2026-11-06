#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// One captured VU1 run, as stored in the capture file: the port writes one record per sampled
// MSCAL/MSCNT just before the compiled program runs (Midnight Club: MC_VU1_CAPTURE in
// src/mc_vu1_fast.cpp). Captures hold game data: keep them out of git.
#pragma pack(push, 4)
struct BenchRecord
{
    uint32_t entry;
    float vf[32][4];
    float acc[4];
    int32_t vi[16];
    uint32_t q, i;
    uint32_t clip, mac, top, itop;
    uint8_t mem[16384];
};
#pragma pack(pop)
static_assert(sizeof(BenchRecord) == 17004, "capture record layout");

struct BenchOut
{
    bool ran;
    float vf[32][4];
    float acc[4];
    int32_t vi[16];
    uint32_t q, i, clip, mac, pc, cycles;
    alignas(16) uint8_t mem[16384];
    std::vector<std::vector<uint8_t>> packets;
};

void runOld(const BenchRecord &r, BenchOut &out);
void runNew(const BenchRecord &r, BenchOut &out);
int64_t timeOld(const BenchRecord *records, size_t count, int repeat, uint64_t *cycles);
int64_t timeNew(const BenchRecord *records, size_t count, int repeat, uint64_t *cycles);
