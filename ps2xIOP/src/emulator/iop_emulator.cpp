#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(host, memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        // PS2X_IOP_PROFILE=1: every 5 s, log IOP instructions per second, the hottest guest PCs and
        // threads, and the scheduler's loop counts (to find what keeps the IOP busy).
        struct Profile
        {
            bool enabled = std::getenv("PS2X_IOP_PROFILE") != nullptr;
            std::vector<uint32_t> pcCounts;
            std::map<int, uint64_t> threadInstructions;
            std::map<int, std::pair<uint32_t, uint32_t>> threadInfo; // entry, priority
            uint64_t loops = 0, slices = 0, idleSkips = 0, idleCycles = 0, imports = 0;
            uint64_t lastInstructions = 0, lastCycles = 0;
            std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        } profile;

        void reportProfile()
        {
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - profile.last).count();
            if (seconds < 5.0)
                return;
            std::vector<std::pair<uint32_t, uint32_t>> hot; // count, pc
            for (uint32_t i = 0; i < profile.pcCounts.size(); ++i)
                if (profile.pcCounts[i])
                    hot.emplace_back(profile.pcCounts[i], i << 2);
            std::partial_sort(hot.begin(), hot.begin() + std::min<size_t>(hot.size(), 16u), hot.end(),
                              [](const auto &a, const auto &b) { return a.first > b.first; });
            const uint64_t instructions = totalInstructions - profile.lastInstructions;
            std::ostringstream out;
            out << "[IOP profile] " << static_cast<uint64_t>(instructions / seconds) << " instructions/s, "
                << static_cast<uint64_t>((totalCycles - profile.lastCycles) / seconds) << " cycles/s; per s: loops "
                << static_cast<uint64_t>(profile.loops / seconds) << ", slices " << static_cast<uint64_t>(profile.slices / seconds)
                << ", idle skips " << static_cast<uint64_t>(profile.idleSkips / seconds) << " (" << static_cast<uint64_t>(profile.idleCycles / seconds)
                << " cycles), imports " << static_cast<uint64_t>(profile.imports / seconds) << "\n  threads:";
            for (const auto &[id, count] : profile.threadInstructions)
                out << " #" << id << " entry=0x" << std::hex << profile.threadInfo[id].first << " pri=0x" << profile.threadInfo[id].second
                    << std::dec << " " << (100.0 * count / std::max<uint64_t>(1u, instructions)) << "%;";
            out << "\n  timers (calls over " << seconds << " s):" << timrman.debugSummary();
            out << "\n  pending DMA interrupts:";
            for (const auto &[irq, cycle] : pendingDmaInterrupts)
                out << " irq " << irq << " in " << (cycle > totalCycles ? cycle - totalCycles : 0u) << " cycles;";
            out << "\n  hot PCs:";
            for (size_t i = 0; i < std::min<size_t>(hot.size(), 16u); ++i)
                out << " 0x" << std::hex << hot[i].second << std::dec << "=" << (100.0 * hot[i].first / std::max<uint64_t>(1u, instructions)) << "%";
            log(LogLevel::Info, out.str());
            std::fill(profile.pcCounts.begin(), profile.pcCounts.end(), 0u);
            profile.threadInstructions.clear();
            profile.loops = profile.slices = profile.idleSkips = profile.idleCycles = profile.imports = 0;
            profile.lastInstructions = totalInstructions;
            profile.lastCycles = totalCycles;
            profile.last = now;
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            resetClockAnchor();
            nextSpu2Tick = 0;
            if (spu2) spu2->reset(0);
            totalInstructions = 0;
            eeCycleCarry = 0;
            idleUntil = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void resetClockAnchor()
        {
            clockBaseNanos = lastClockNanos = wallClock ? wallClock() : 0;
            clockBaseCycles = totalCycles;
            eeBeforeClockSample = 0;
        }

        void runWallCycles(uint64_t eeCycles) noexcept
        {
            // Checkpoints are frequent. Sample the host clock only every 8K EE
            // cycles, then budget against absolute elapsed time, not frame count.
            eeBeforeClockSample += eeCycles;
            if (advancingWallClock || eeBeforeClockSample < 8192) return;
            eeBeforeClockSample = 0;
            const uint64_t now = wallClock();
            if (now < lastClockNanos || now - lastClockNanos > 1'000'000'000ull)
            {
                // Suspend/debugger/long blocking load: do not synthesize seconds
                // of queued audio in a burst when the game resumes.
                clockBaseNanos = lastClockNanos = now;
                clockBaseCycles = totalCycles;
                return;
            }
            lastClockNanos = now;
            const uint64_t elapsed = now - clockBaseNanos;
            const uint64_t target = clockBaseCycles + (elapsed / 1'000'000'000ull) * 36'864'000ull +
                ((elapsed % 1'000'000'000ull) * 36'864'000ull) / 1'000'000'000ull;
            if (target <= totalCycles) return;
            advancingWallClock = true;
            runCycles(target - totalCycles);
            advancingWallClock = false;
        }

        void tickSpu2()
        {
            if (!spu2 || totalCycles < nextSpu2Tick) return;
            nextSpu2Tick = totalCycles + 768;
            const uint32_t events = spu2->tick(totalCycles);
            if (events & 1u) pendingDmaInterrupts[9] = totalCycles;
            for (unsigned core = 0; core < 2; ++core)
                if (events & (2u << core))
                {
                    if (memory.usesTimedSpu2Dma(core)) continue;
                    memory.completeSpu2Dma(core);
                    pendingDmaInterrupts[core ? 40 : 36] = totalCycles;
                }
        }

        void schedulePendingDma()
        {
            if (const auto dma = memory.takeDmaStart())
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            tickSpu2();
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (profile.enabled)
            {
                if (profile.pcCounts.empty())
                    profile.pcCounts.assign(kRamSize / 4u, 0u);
                ++profile.pcCounts[physicalAddress(cpu.pc) >> 2];
            }

            if (const auto import = imports.decode(cpu.pc))
            {
                if (profile.enabled)
                    ++profile.imports;
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            const uint32_t executed = runCpu(cpu, budget);
            if (std::getenv("MC_AUDIO_TRACE") && cpu.pc != kCallReturnSentinel)
            {
                static uint32_t incompleteCalls = 0;
                if (incompleteCalls++ < 24)
                {
                    std::ostringstream out;
                    out << "[mc_sound:iop-call] incomplete function=0x" << std::hex << address
                        << " pc=0x" << cpu.pc << " code=";
                    for (int offset = -12; offset <= 12; offset += 4)
                        out << ' ' << memory.read32(cpu.pc + offset);
                    out << " v0=0x" << cpu.gpr[2] << " a0=0x" << cpu.gpr[4]
                        << " a1=0x" << cpu.gpr[5] << " s0=0x" << cpu.gpr[16]
                        << " ra=0x" << cpu.gpr[31]
                        << " dma7chcr=0x" << memory.read32(0x1F801508u)
                        << " dma7bcr=0x" << memory.read32(0x1F801504u)
                        << " dma7madr=0x" << memory.read32(0x1F801500u)
                        << " dma7admas=0x" << memory.read16(0x1F9005B0u)
                        << " dma7status=0x" << memory.read16(0x1F900744u)
                        << " libsdflag=0x" << memory.read16(0x209D6u) << std::dec
                        << " instructions=" << executed << " budget=" << budget;
                    log(LogLevel::Warning, out.str());
                }
            }
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                {
                    if ((irq == 36 || irq == 40) && memory.usesTimedSpu2Dma(irq == 40 ? 1u : 0u))
                        memory.completeSpu2Dma(irq == 40 ? 1u : 0u);
                    (void)intrman.dispatchInterrupt(irq, *this);
                }
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    if (profile.enabled)
                        ++profile.loops;
                    tickSpu2();
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this, &intrman);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(UINT64_MAX);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        if (spu2) nextWake = std::min(nextWake, nextSpu2Tick);
                        idleUntil = nextWake;
                        const uint64_t skipTo = std::max(totalCycles + 1u, std::min(target, nextWake));
                        if (profile.enabled)
                        {
                            ++profile.idleSkips;
                            profile.idleCycles += skipTo - totalCycles;
                        }
                        totalCycles = skipTo;
                        continue;
                    }
                    idleUntil = 0;
                    const uint64_t before = totalCycles;
                    const uint32_t ran = runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    if (profile.enabled)
                    {
                        ++profile.slices;
                        profile.threadInstructions[next->id] += ran;
                        profile.threadInfo[next->id] = {next->entry, next->priority};
                    }
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
                if (profile.enabled)
                    reportProfile();
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        std::shared_ptr<Spu2Device> spu2;
        std::function<uint64_t()> wallClock;
        uint64_t clockBaseNanos = 0, lastClockNanos = 0, clockBaseCycles = 0, eeBeforeClockSample = 0;
        bool advancingWallClock = false;
        uint64_t nextSpu2Tick = 0;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        // No thread is ready and nothing is due before this cycle, so runEeCycles can just advance
        // time. Set by runCycles' idle path; cleared by every call that can change IOP state.
        uint64_t idleUntil = 0;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        m_impl->idleUntil = 0;
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        m_impl->idleUntil = 0;
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        m_impl->idleUntil = 0;
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::setSpu2Device(std::shared_ptr<Spu2Device> device)
    {
        m_impl->spu2 = std::move(device);
        m_impl->memory.setSpu2Device(m_impl->spu2, &m_impl->totalCycles);
        m_impl->pendingDmaInterrupts.erase(36);
        m_impl->pendingDmaInterrupts.erase(40);
        m_impl->pendingDmaInterrupts.erase(9);
        m_impl->idleUntil = 0;
        m_impl->nextSpu2Tick = m_impl->totalCycles;
        if (m_impl->spu2) m_impl->spu2->reset(m_impl->totalCycles);
        const char *clock = std::getenv("PS2X_IOP_CLOCK");
        if (m_impl->spu2 && clock && std::strcmp(clock, "wall") == 0)
            setRealtimeClock([] {
                return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            });
        else
            setRealtimeClock({});
    }

    void IopEmulator::setRealtimeClock(std::function<uint64_t()> clock)
    {
        m_impl->wallClock = std::move(clock);
        m_impl->resetClockAnchor();
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        if (m_impl->wallClock)
        {
            m_impl->runWallCycles(eeCycles);
            return;
        }
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (!iopCycles)
            return;
        // Idle IOP: the scheduler loop would only advance time, so skip it. The EE calls this at
        // every checkpoint with a few dozen IOP cycles, far more often than anything happens.
        if (m_impl->totalCycles + iopCycles < m_impl->idleUntil)
        {
            m_impl->totalCycles += iopCycles;
            return;
        }
        m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        m_impl->idleUntil = 0;
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->idleUntil = 0;
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        m_impl->idleUntil = 0;
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        m_impl->idleUntil = 0;
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        m_impl->idleUntil = 0;
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        m_impl->idleUntil = 0;
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
