#pragma once
// EE Image Processing Unit (IPU): the MPEG-2 macroblock decoder behind 0x10002000 (IPU_CMD, IPU_CTRL,
// IPU_BP, IPU_TOP), the FIFOs at 0x10007000 (out) / 0x10007010 (in) and DMA channels 3 (fromIPU)
// and 4 (toIPU). Commands: BCLR, IDEC, BDEC, VDEC, FDEC, SETIQ, SETVQ, CSC, PACK, SETTH.
//
// The decoding core (VLC tables, block decode, IDCT, YCbCr -> RGB, dither, VQ) is ported from PCSX2's
// pcsx2/IPU (GPL-3.0+ / GPL-2.0+, itself partly from libmpeg2); see ps2_ipu.cpp.
//
// Model: input arrives as whole quadwords (FIFO writes or a toIPU transfer, which is copied in at
// once); a command runs synchronously when written and again whenever more input arrives, from its
// starting bit position, until it has all the bits it needs (output already handed out by an
// earlier attempt is not produced twice). Output is queued on the host and handed to the fromIPU
// transfer by the caller (PS2Memory). Not thread-safe: the caller serializes access.
#include <cstddef>
#include <cstdint>
#include <memory>

class Ps2Ipu
{
public:
    Ps2Ipu();
    ~Ps2Ipu();
    Ps2Ipu(const Ps2Ipu &) = delete;
    Ps2Ipu &operator=(const Ps2Ipu &) = delete;

    // Power-on state (also what IPU_CTRL.RST does, apart from data still queued behind the FIFO).
    void reset();

    void writeCmd(uint32_t value);
    void writeCtrl(uint32_t value);
    // IPU_CMD low word: DATA (the FDEC/VDEC result, otherwise the next 32 bits of the bitstream).
    uint32_t readCmd();
    // IPU_CMD high word: bit 31 = BUSY.
    uint32_t readCmdHigh() const;
    uint32_t readCtrl() const;
    uint32_t readBp() const;
    uint32_t readTop() const;
    // IPU_TOP high word: bit 31 = BUSY (IDEC/BDEC/VDEC/FDEC still running).
    uint32_t readTopHigh() const;

    // 32-bit stores to IPU_in_FIFO (0x10007010 + 4 * index); the quadword is queued on index 3.
    void writeInFifoWord(unsigned index, uint32_t value);
    // Queues quadwords of input (IPU_in_FIFO or a toIPU DMA transfer) and resumes a waiting command.
    void pushInput(const uint8_t *data, uint32_t qwc);
    // Quadwords queued and not consumed yet, counted from the one holding the current bit position.
    uint32_t queuedInputQwords() const;
    // Drops queued input beyond the first keepQwords (a toIPU transfer stopped or replaced before the
    // IPU took all of it: only what reached the FIFO stays).
    void dropInputBeyond(uint32_t keepQwords);

    size_t queuedOutputBytes() const;
    // Moves up to maxBytes of output (whole quadwords) to dst; returns the byte count.
    size_t takeOutput(uint8_t *dst, size_t maxBytes);
    // IPU_out_FIFO read (0x10007000): one quadword, false if none is ready.
    bool readOutFifo(uint8_t qword[16]);

    bool busy() const;
    // Current command word and the total number of commands written (diagnostics).
    uint32_t currentCommand() const;
    uint64_t commandCount() const;

private:
    struct State;
    std::unique_ptr<State> m;
};
