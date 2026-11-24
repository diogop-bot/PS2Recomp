#pragma once
// Game hook for DMA channels the runtime does not emulate (today: the IPU channels, 3 = fromIPU
// and 4 = toIPU; the runtime has no IPU decoder).
//
// Without a hook a kick on those channels only stores the registers and raises no DMAC interrupt
// (CHCR reads report STR clear anyway), so a game that waits for its DMAC handler hangs. A hook
// that returns true makes the kick complete at once, as the runtime does for the GIF/VIF
// channels: STR and QWC are cleared, MADR advances, the D_STAT channel bit is set and the DMAC
// cause is queued (its handlers run at the next scheduler checkpoint). The hook decides what the
// transfer does to memory, if anything.
//
// Kept out of ps2_memory.h on purpose: that header is included by every generated file.
#include <cstdint>

class PS2Memory;

// The hook is also called for VIF1 -> memory kicks (channel 1, a GS readback): returning true there
// means the hook filled the destination; false leaves it to the GS readback source
// (runtime/ps2_gs_readback.h). Either way the kick completes.
//
// channel: DMAC channel number (1, 3 or 4). madr/qwc/chcr: the channel registers at the kick.
using DmaKickHook = bool (*)(PS2Memory &memory, uint32_t channel, uint32_t madr, uint32_t qwc, uint32_t chcr);

void setDmaKickHook(DmaKickHook hook);
