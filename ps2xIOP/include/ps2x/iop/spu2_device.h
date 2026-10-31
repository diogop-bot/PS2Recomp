#pragma once
#include <cstdint>
#include <span>

namespace ps2x::iop
{
    // Called on the IOP owner thread. RAM spans remain valid until detach/reset.
    // tick returns latched events: bit 0 = SPU2 IRQ, bit 1 = DMA4, bit 2 = DMA7.
    class Spu2Device
    {
    public:
        virtual ~Spu2Device() = default;
        virtual void reset(uint64_t cycle) = 0;
        virtual uint16_t read16(uint32_t address, uint64_t cycle) = 0;
        virtual void write16(uint32_t address, uint16_t value, uint64_t cycle) = 0;
        virtual bool dma(unsigned core, uint32_t madr, uint32_t bcr, uint32_t chcr,
                         std::span<uint8_t> ram, uint64_t cycle) = 0;
        virtual uint32_t tick(uint64_t cycle) = 0;
    };
}
