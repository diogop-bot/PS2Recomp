#pragma once
// VIF1 VIFcode interrupt bit (bit 31) support for game code.
//
// On hardware a VIFcode with the interrupt bit stalls VIF1 after the command, raises the VIF1
// INTC interrupt and waits for the handler to write VIF1_FBRST.STC. The runtime cannot run a
// guest handler in the middle of a VIF1 stream (the stream may be processed on the GS worker
// thread), so a game can register a host hook that does what its handler does, in stream order.
//
// Kept out of ps2_memory.h on purpose: that header is included by every generated file.
#include <cstdint>

class PS2Memory;

// Called after a VIFcode with the interrupt bit has executed. code = VIF1_CODE, mark = VIF1_MARK.
// Return true when the interrupt was handled (VIF1_STAT.INT is then cleared, as by FBRST.STC).
using Vif1InterruptHook = bool (*)(PS2Memory &memory, uint32_t code, uint32_t mark);

void setVif1InterruptHook(Vif1InterruptHook hook);

// Runs a GIF DMA source chain (channel 2, chain mode, as with D2_TADR = tadr, D2_CHCR = 0x105)
// at once, sending its data to the GS on PATH3. For interrupt hooks that start a PATH3 transfer.
// Returns the number of qwords sent.
uint32_t processGifChainInline(PS2Memory &memory, uint32_t tadr);
