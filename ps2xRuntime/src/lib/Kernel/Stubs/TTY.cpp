#include "Common.h"
#include "TTY.h"
#include "ps2_log.h"

namespace ps2_stubs
{
    void scePrintf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t format_addr = getRegU32(ctx, 4);
        const std::string formatOwned = readPs2CStringBounded(rdram, runtime, format_addr, 1024);
        if (format_addr == 0)
            return;
        std::string rendered = formatPs2StringWithArgs(rdram, ctx, runtime, formatOwned.c_str(), 1);
        if (rendered.size() > 2048)
            rendered.resize(2048);
        PS2_IF_AGRESSIVE_LOGS({
            const std::string logLine = sanitizeForLog(rendered);
            uint32_t count = 0;
            {
                std::lock_guard<std::mutex> lock(g_printfLogMutex);
                count = ++g_printfLogCount;
            }
            if (count <= kMaxPrintfLogs)
            {
                RUNTIME_LOG("PS2 scePrintf: " << logLine);
                RUNTIME_LOG(std::flush);
            }
            else if (count == kMaxPrintfLogs + 1)
            {
                std::cerr << "PS2 printf logging suppressed after " << kMaxPrintfLogs << " lines" << std::endl;
            }
        });
    }

    void sceResetttyinit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceResetttyinit", rdram, ctx, runtime);
    }

    // Sony's debug TTY (libscf). Games print through it on development kits; on retail
    // hardware it is a no-op, so accept everything and forward written text to the log.
    void sceTtyHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceTtyInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceTtyRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    // int sceTtyWrite(const char *buf, int len)
    void sceTtyWrite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t bufAddr = getRegU32(ctx, 4);
        const int32_t len = static_cast<int32_t>(getRegU32(ctx, 5));
        if (bufAddr != 0u && len > 0)
        {
            std::string text;
            for (int32_t i = 0; i < len && i < 4096; ++i)
            {
                const uint8_t *byte = getMemPtr(rdram, bufAddr + static_cast<uint32_t>(i));
                if (!byte)
                    break;
                text.push_back(static_cast<char>(*byte));
            }
            std::cerr << "[tty] " << sanitizeForLog(text) << std::flush;
        }
        setReturnS32(ctx, len > 0 ? len : 0);
    }
}
