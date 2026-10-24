#pragma once

#include <cstdint>

// Hooks for an external presenter (for example a GPU GS renderer fed from the GIF arbiter) that
// takes over from the runtime's raylib frame upload. All are optional; unset hooks change nothing.
namespace ps2_host_hooks
{
    using VBlankFn = void (*)(void *user, uint64_t tick);
    using PresentFn = void (*)(void *user);
    using ShutdownFn = void (*)(void *user);

    // Game thread, at every vblank start, after the vsync tick and CSR.FIELD are updated and
    // before the guest's vblank handlers run.
    void setVBlankHook(VBlankFn fn, void *user);
    // Main thread, once per loop iteration, instead of uploading and drawing the CPU GS frame.
    // The hook must poll window input and pace the loop itself.
    void setPresentHook(PresentFn fn, void *user);
    // Main thread, after the game thread has stopped and before the window is closed.
    void setShutdownHook(ShutdownFn fn, void *user);

    void notifyVBlank(uint64_t tick);
    bool runPresentHook();
    void runShutdownHook();
}
