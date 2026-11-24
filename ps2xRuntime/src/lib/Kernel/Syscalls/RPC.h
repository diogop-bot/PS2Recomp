#pragma once

#include "ps2_syscalls.h"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace ps2_syscalls
{
    // EeScheduler external-wait type of a thread parked in sceSifRpcLoop; the token is its queue
    // (t_SifRpcDataQueue address). SifRemoveRpcQueue completes it.
    inline constexpr uint32_t kSifRpcLoopWaitType = 0x53494652u; // 'SIFR'

    void SifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void SifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void sceRpcGetPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // IOP -> EE RPC: an IRX binding to / calling a server the EE registered (sceSifRegisterRpc).
    struct EeRpcServerView
    {
        uint32_t serverData = 0; // t_SifRpcServerData address
        uint32_t function = 0;
        uint32_t buffer = 0;
        uint32_t callbackBuffer = 0;
    };
    bool findEeRpcServer(uint8_t *rdram, uint32_t sid, EeRpcServerView &out);
    // Copies the request into the server's buffer and queues func(rpcNumber, buffer, sendSize) on
    // the EE scheduler; when it returns, `done` gets receiveSize bytes from the pointer it returned
    // (the server buffer if it returned 0). Returns false when no such EE server can be run.
    bool callEeRpcServerFromIop(PS2Runtime *runtime,
                                uint8_t *rdram,
                                uint32_t sid,
                                uint32_t rpcNumber,
                                const uint8_t *send,
                                size_t sendSize,
                                uint32_t receiveSize,
                                std::function<void(const uint8_t *, size_t)> done);
}
