#include "emulator/core/iop_memory.h"
#include "emulator/iop_emulator.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace ps2x::iop;
using namespace ps2x::iop::detail;
    class NullHost : public IopHost
    {
    public:
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };


struct Device final : Spu2Device
{
    unsigned reads=0, writes=0, starts=0, lastCore=99;
    uint16_t value=0;
    uint64_t cycle=0;
    uint32_t address=0;
    uint32_t expectedBcr=0x10010;
    void reset(uint64_t c) override { cycle=c; }
    uint16_t read16(uint32_t a,uint64_t c) override { ++reads; address=a; cycle=c; return value; }
    void write16(uint32_t a,uint16_t v,uint64_t c) override { ++writes; address=a; value=v; cycle=c; }
    bool dma(unsigned core,uint32_t madr,uint32_t bcr,uint32_t chcr,std::span<uint8_t> ram,uint64_t c) override
    {
        if (madr!=0x100 || bcr!=expectedBcr || chcr!=0x1000201 || ram[0x100]!=0x42) throw std::runtime_error("DMA descriptor/data lost");
        ++starts; lastCore=core; cycle=c; ram[0x101]=0x55; return true;
    }
    uint64_t ticks=0, lastTick=0, maxGap=0;
    uint32_t tick(uint64_t c) override
    { ++ticks; maxGap=std::max(maxGap,c-lastTick); lastTick=c; return 0; }
};
void require(bool ok,const char *why) { if(!ok) throw std::runtime_error(why); }
int main()
{
    try
    {
        // A sleeping/idle IOP must still clock the mixer, including the fast path.
        NullHost host;
        IopEmulator emulator(host);
        auto clocked=std::make_shared<Device>();
        emulator.setSpu2Device(clocked);
        for(unsigned i=0;i<16000;++i) emulator.runEeCycles(64);
        require(clocked->ticks>=166 && clocked->maxGap<=768,"idle fast path starved SPU2");
        emulator.reset();
        require(clocked->cycle==0,"IOP reset did not reset sound device");
        clocked->lastTick=0;
        emulator.runEeCycles(8*768*20);
        require(clocked->maxGap<=768,"large scheduler slice starved SPU2");
        emulator.setSpu2Device(nullptr);
        const auto ticks=clocked->ticks;
        emulator.runEeCycles(8000);
        require(clocked->ticks==ticks,"detached device still ticked");
        // Equal elapsed host time must produce equal IOP time regardless of
        // how much EE work each frame reports (the observed music-speed bug).
        IopEmulator realtime(host);
        auto realtimeDevice=std::make_shared<Device>();
        realtime.setSpu2Device(realtimeDevice);
        uint64_t fakeNanos=10'000'000'000ull;
        realtime.setRealtimeClock([&] { return fakeNanos; });
        for(unsigned frame=0;frame<10;++frame)
        {
            fakeNanos+=100'000'000;
            realtime.runEeCycles(frame%2 ? 8192 : 8'192'000);
        }
        require(realtime.cycles()==36'864'000,"IOP rate depends on EE work instead of host time");
        require(realtimeDevice->maxGap<=768,"host clock starved the mixer");
        const auto wallCycles=realtime.cycles();
        realtime.runEeCycles(8'192'000);
        require(realtime.cycles()==wallCycles,"EE burst advanced audio without elapsed time");
        fakeNanos+=5'000'000'000ull;
        realtime.runEeCycles(8192);
        require(realtime.cycles()==wallCycles,"suspend caused a stale-audio catchup burst");
        fakeNanos+=100'000'000;
        realtime.runEeCycles(8192);
        require(realtime.cycles()==wallCycles+3'686'400,"clock failed to resume after suspend");
        realtime.reset();
        realtimeDevice->lastTick=0;
        fakeNanos+=100'000'000;
        realtime.runEeCycles(8192);
        require(realtime.cycles()==3'686'400,"reset retained the old clock epoch");
        IopMemory m;
        // The real music driver requests 61,504 bytes after transient module
        // buffers were freed. A bump-only heap incorrectly returned zero.
        IopMemory heap;
        const auto first = heap.allocate(65536,64);
        const auto second = heap.allocate(IopMemory::HeapLimit-IopMemory::HeapBase-65536-33408,16);
        require(first && second && heap.maxFreeMemory()==33408,"heap test setup failed");
        require(heap.freeAllocation(first),"temporary buffer free failed");
        require(heap.maxFreeMemory()==65536,"largest freed block not reported");
        heap.write8(second,0x67);
        const auto audio = heap.allocate(61504,64);
        require(audio==first && heap.ownsRamRange(audio,61504),"freed buffer not reused for music");
        require(heap.read8(second)==0x67,"live allocation damaged by reuse");
        uint64_t cycle=1234;
        auto d=std::make_shared<Device>();
        m.setSpu2Device(d,&cycle);
        m.write16(0xbf9001b0,0x1234);
        require(d->writes==1 && d->reads==0 && d->address==0x1f9001b0 && d->cycle==cycle,"16-bit write duplicated or wrong alias/cycle");
        require(m.read16(0xbf9001b0)==0x1234 && d->reads==1,"16-bit read duplicated");
        m.write32(0x1f900180,0xabcd7654);
        require(d->writes==3 && d->address==0x1f900182 && d->value==0xabcd,"32-bit write not split into halfwords");
        m.write8(0x100,0x42);
        for(unsigned core=0;core<2;++core)
        {
            const uint32_t base=core?0x1f801500:0x1f8010c0;
            const uint32_t status=core?0x1f900744:0x1f900344;
            d->value=core?0:1; // core 0 AutoDMA, core 1 ordinary timed DMA
            m.write32(base,0x100); m.write32(base+4,0x10010); m.write32(base+8,0x1000201);
            const auto scheduled=m.takeDmaStart();
            require(d->lastCore==core,"device did not receive DMA");
            if(core)
                require(scheduled && scheduled->irq==40 && scheduled->delayCycles==64 && m.usesTimedSpu2Dma(core),"ordinary device DMA not timed");
            else
                require(!scheduled && !m.usesTimedSpu2Dma(core),"AutoDMA incorrectly timed by bus");
            require((m.read16(status)&0x80)==0,"DMA complete status set before completion");
            require((m.read32(base+8)&0x1000000)!=0,"DMA cleared before device completed");
            require(m.read8(0x101)==0x55,"device cannot write IOP RAM");
            m.completeSpu2Dma(core);
            require((m.read32(base+8)&0x1000000)==0,"completion did not clear DMA start");
            require((m.read16(status)&0x80)!=0,"libsd cannot see DMA complete status");
        }
        // A bank block remains bus-paced for the guest. The bridge must have
        // finished copying it into SPU2 RAM before this timer releases libsd.
        d->value=0;
        d->expectedBcr=0x00011000;
        m.write32(0x1f801500,0x100);
        m.write32(0x1f801504,d->expectedBcr);
        m.write32(0x1f801508,0x1000201);
        const auto bankTransfer=m.takeDmaStart();
        require(bankTransfer && bankTransfer->irq==40 &&
            bankTransfer->delayCycles==4096u*2u,
            "16 KiB bank upload no longer uses bus-speed guest completion");
        m.completeSpu2Dma(1);
        m.setSpu2Device(nullptr,nullptr);
        m.write16(0x1f9001b0,1);
        m.write32(0x1f8010c8,0x1000201);
        const auto fallback=m.takeDmaStart();
        require(fallback && fallback->irq==36 && fallback->delayCycles==16*768,"fallback timing changed");
        std::cout << "SPU2 device routing, DMA ownership, RAM and fallback tests passed\n";
    }
    catch(const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
