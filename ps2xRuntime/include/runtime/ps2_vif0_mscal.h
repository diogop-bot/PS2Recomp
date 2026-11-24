#pragma once
// VIF0 MSCAL / MSCALF start a VU0 microprogram from the VIF0 stream (This Is Football's match sends
// `MSCAL 0x100` through VIF0 every frame). VIF0 is processed on the EE thread, so the runtime runs
// the program at once, on the current EE context's VU0 registers. The runtime installs the handler.
//
// Kept out of ps2_memory.h on purpose: that header is included by every generated file.
#include <cstdint>
#include <functional>

// startPC: byte address in VU0 micro memory (MSCAL imm * 8). itop: VIF0 ITOPS, copied to VU0 ITOP.
using Vif0MscalHandler = std::function<void(uint32_t startPC, uint32_t itop)>;

void setVif0MscalHandler(Vif0MscalHandler handler);

// Drops the VIF0 stream kept for later (an incomplete VIFcode or the rest after an i-bit stall).
// Called on VIF0_FBRST.RST.
void resetVif0Stream();
