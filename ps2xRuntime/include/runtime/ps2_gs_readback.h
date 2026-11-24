#pragma once
// GS -> memory readback through the VIF1 FIFO: a VIF1 DMA with CHCR.DIR = 0 while VIF1_STAT.FDR is
// set (sceDmaRecvN after a BITBLTBUF/TRXPOS/TRXREG + TRXDIR=1 transfer). The runtime fills the
// destination from the source installed here; by default that is its own GS
// (GS::consumeLocalToHostBytes). A port that renders with another GS (e.g. PCSX2's) installs a
// source that reads from that GS instead.
//
// PS2X_GS_READBACK_LOG=1 logs the first readbacks; PS2X_GS_READBACK_DUMP=<prefix> writes the first
// 10 to <prefix>_<n>.bin.
//
// Kept out of ps2_memory.h on purpose: that header is included by every generated file.
#include <cstdint>
#include <functional>

// Writes up to maxBytes of the pending local -> host transfer to dst; returns the bytes written.
using GsReadbackSource = std::function<uint32_t(uint8_t *dst, uint32_t maxBytes)>;

void setGsReadbackSource(GsReadbackSource source);
