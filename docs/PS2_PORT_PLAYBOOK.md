# PS2 port playbook (lessons from Midnight Club: Street Racing)

What it took to turn a PS2 game into a native PC port with PS2Recomp, written so the next port
can skip the research. Midnight Club details are kept as examples; the rest applies to any game.
Paths such as `tools/...`, `src/...` and `analysis/...` are in the Midnight Club port's repository
(github.com/diogop-bot/midnight-club-recomp), the reference port for this branch; its full
day-by-day record is in `analysis/PHASE1.md`, `PHASE2.md` and `PHASE3.md`. Add lessons from
other ports here, in the section they belong to.

Result for this game: boots, plays races in both cities with physics matching PCSX2, renders
through PCSX2's GS at 4x, plays sound through PCSX2's SPU2, saves to a memory-card folder, and
runs 60 fps in most places (54+ in the densest).

---

## 1. The order of work that worked

1. **Map the binary** (Ghidra + PS2Recomp's analyzer). Fix function boundaries before recompiling.
2. **Recompile and boot to the title screen** with PS2Recomp's runtime as-is.
3. **Get a PCSX2 reference running early** (the user's own BIOS and disc). Every hard bug was
   solved by comparing with PCSX2: screenshots, frame rates, and above all save states.
4. **Fix correctness before speed.** At 1.5 fps we still found the physics bugs; a fast but wrong
   port wastes time later.
5. **Swap the renderer for PCSX2's GS** behind a small DLL of our own (section 6). PS2Recomp's CPU
   GS is a proof of concept only (~200 ms per frame in 3D).
6. **Recompile the VU1 microprogram ahead of time** (section 8). The VU1 interpreter caps 3D scenes
   at a few fps.
7. **Sound through PCSX2's SPU2** (section 7), with the IOP clocked from wall time.
8. **Performance**: frame pacing, a GS/VU1 worker thread, VIF unpack fast paths, VU1 codegen.
9. **Long-tail game features**: saves, career mode, menus after races, movies.

Measure at every step (section 9). Most "obvious" causes were wrong.

---

## 2. Tooling and environment

- **Ghidra + Emotion Engine: Reloaded** (language `r5900:LE:32:default`). Stock Ghidra treats the
  EE as generic MIPS64 and gets MMI/COP2 wrong. Drive it headless (`analyzeHeadless.bat` + scripts in
  `ghidra_scripts/`). Script arguments with `|` or `,` get mangled by the `.bat`.
- **REA (the reverse-engineering MCP server) cannot analyse PS2 ELFs** on Windows (PE only). Don't try.
- **PCSX2** nightly, portable mode. Keep its source at the same tag (`third_party/pcsx2`): we build
  its GS and SPU2 from it. It needs PCSX2's Windows dependency package to build.
- **Git on this machine** needs `-c http.sslBackend=schannel`; CMake FetchContent needs
  `GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=http.sslBackend GIT_CONFIG_VALUE_0=schannel`.
- **MSVC**:
  - `/fp:precise` for game code (PS2Recomp's helper uses `/fp:fast`, which breaks PS2 float rules).
  - **MSVC silently compiles huge functions unoptimized** (no warning). A 20k-line function ran
    3x slower than the same code split into ~64-instruction regions. `/d2OptimizeHugeFunctions`
    crashed the compiler. Keep generated functions small.
  - Unity builds (batch 64) keep the ~34k generated files buildable. Keep your own sources out of
    the unity batches so editing them doesn't rebuild the generated code.
  - Touching a runtime header that the PCH includes rebuilds everything (10+ minutes here).
  - MSVC output may be localised ("Gerando Código..." is a success line).
- **A running game locks its exe**: the link fails until it is closed.
- **Python heredocs through the Bash tool break on single quotes**; write scripts to a file.
- **Antivirus** (Avast CyberCapture) can block or stall every freshly linked exe.

---

## 3. Mapping the binary (Phase 1)

Pipeline (`tools/phase1_ghidra.ps1`): `ps2_analyzer` (SDK signatures) → Ghidra import and
auto-analysis → apply names → recover data-pointed functions → PS2Recomp's `ExportPS2Functions.java`
→ `*_config.toml` + function CSV.

- Ghidra alone found ~66% of functions. The rest came from: pointers in `.rodata`/`.data` to
  undisassembled code (mostly virtual methods), stack-frame prologues in gaps, and code right after
  a `jr`/`j` epilogue. Together 98.4% of `.text`.
- **Discontiguous Ghidra bodies break PS2Recomp**: Ghidra folds unrecognised tail-call targets into
  the caller, and PS2Recomp exports start..max-address, so a 48-byte function became 635 KB of
  C++. `ghidra_scripts/SplitFarChunks.java` trims each function to the range holding its entry and
  makes the other ranges functions of their own.
- The SDK sits at the end of `.text` (here `0x2A1D30–0x2C0D90`); PS2Recomp's signature database
  named 372 functions and bound 236 to runtime handlers. The C library is mostly unnamed and
  recompiles like game code (fine).
- `ps2_analyzer`'s "self-modifying code" flags were all false positives (base register rewritten
  between `lui` and the store).
- VU1 microcode is in `.vutext`, uploaded by VIF `MPG` packets; extract it with
  `tools/extract_vu1.py`, list it with `tools/vu1_disasm.py`.
- Vtables are 8-byte entries in `.rodata` (pointer + zero); `tools/find_vtables.py` finds them.
  Not needed for recompilation, only for patching.

---

## 4. Recompiling and booting (Phase 2)

- `cdrom0:` maps to the ELF's folder; put the extracted disc (`tools/iso_extract.py`) there.
- Game overrides are matched by ELF name, entry point and CRC32 (`game_overrides.h`).
- **An override that replaces a whole guest function must set `ctx->pc = $ra`**, or the dispatcher
  re-enters it forever.
- **Let the game's own libgraph run** (`config/recomp_unbind.txt`): PS2Recomp's high-level `sceGs*`
  stand-ins write "logical" display values, break double buffering and truncate packets. Keep
  `sceGsSyncV`/`sceGsSyncPath` bound (the real ones busy-wait on bits the runtime never raises).
- ps2_recomp binds functions by name too; unbound ones are renamed `guest_<name>`.
- Movies: with no MPEG decoder the player loop waits forever on a stalled stream; replace the play
  loop with `ret0` until FFmpeg video exists.
- `ps2xTest` must run from `third_party/PS2Recomp` (a test reads `instructions.h` relative to it).

---

## 5. PS2Recomp bugs found: a checklist for any new game

All fixed in our fork (`diogop-bot/PS2Recomp`, branch `midnightclub-fixes`; the full diff is
`patches/ps2recomp-midnightclub-fixes.patch`). If a new project uses upstream PS2Recomp, check each.

### EE instruction translation
| Bug | Symptom in this game | Fix |
|---|---|---|
| `bltz/bgez/blez/bgtz` (+likely/link) tested the low 32 bits | soft-float double add wrong for opposite signs (514/2000) | test the full 64-bit register |
| `SQRT.S fd, ft` read fs (always 0) | every `sqrt.s` computed sqrt($f0): non-unit contact normals, cars launched by walls | use the ft field |
| `PEXEW` swapped adjacent words (also PEXEH/PROT3W/PINTH/PINTEH wrong) | sloped collision triangles got normal z = y; car fell through curbs | per EE manual / PCSX2 `MMI.cpp` |
| `READ128/WRITE128` honoured low address bits | `lq` + `qfsrv` unaligned-load idiom double-shifted; car fell through the road | align down like hardware |
| `MOVZ/MOVN` moved 128 bits | (latent) | move the low 64 only |
| `CVT.W.S` rounded to nearest | (latent) | truncate and saturate |
| VU0 `VRSQRT`/`VSQRT`/`VDIV` forms | (latent) | fs/√|ft|, |ft|, ±max on divide by zero |

### FPU semantics (must match PCSX2's defaults)
- Game thread runs **round-toward-zero + FTZ + DAZ** (EE chop).
- **DIV.S/SQRT.S/RSQRT.S round to nearest** (only ADD/SUB/MUL chop). We compute in double and round
  ties-to-even explicitly; no MXCSR switching. Without this, constants came out one ulp low.
- FPU compares follow PS2 ordering for denormal/Inf/NaN bit patterns.
- **How we found these: a self-test** calling the game's recompiled soft-float routines
  (`src/mc_selftest.cpp`): integer-only code must match host IEEE bit for bit.

### Runtime / HLE kernel
| Bug | Symptom | Fix |
|---|---|---|
| **COP0 Count never advances** | race fade-in and physics frozen (dt clamped to 0.0001 s); menus fine because they count frames | replace the game's Count readers with a host clock (294.912 MHz) |
| `SetGsCrt` doesn't set SMODE1/SYNCV | PCSX2 can't identify PAL/NTSC | set the BIOS values in an override |
| HLE RPC/TLS pools at `0x01F00000+` inside game RAM | London: corrupted VU1 packets, runaway microprogram, missing geometry, 12.8 fps | move pools to the kernel area `0x30000–0x60000` |
| Vblank deadlines re-based on the previous deadline | bursts of vblanks after loads: uneven frames, camera jitter | re-base on "now" when more than 4 periods late |
| `dispatchGuestBranch` took "pc == callee entry" as a return | under recursion an unwinding checkpoint skipped epilogues; `$sp` 0x80 low; crash in pile-ups | count unwinds; keep unwinding if one happened inside the call |
| `sceTty*` stubs unimplemented | Career mode crashed on its first debug print | Init returns 1, Write prints |
| `sceMcGetDir` ignored flags (continue) | save list showed one profile | keep the listing, resume on flags != 0 |
| `sceSifFreeIopHeap` is game code that RPCs an IOP heap server the HLE lacks | IOP memory leaked per movie; race music buffer failed; menu hang after a race | bind it to the runtime's free |
| VIF1 `DIRECT` + `IMAGE` split across two DMA pieces | every texture shifted 8 bytes: colours rotated (yellow → purple), VU code corrupted | take the image data from the next DIRECT payload |
| GIF arbiter lost path ids | PCSX2 needs per-path state | pass `GifPathId` to the consumer |
| Pad ignored the keyboard when a gamepad was connected | automation key presses vanished | always read both |
| Tail jumps (`j func`) are emitted as direct C calls | `replaceFunction` on a game function silently misses every caller that tail-jumps into it (here 170+ callers of `free`) | override a function reached only through `jal` (check `grep "FUN_x_0xx(rdram"` in `generated/` finds no direct callers), or one level deeper |

### IOP (ps2xIOP)
| Bug | Symptom | Fix |
|---|---|---|
| `ioman` open/close/read/lseek unimplemented | music stream never opened | implement on the extracted disc |
| SPU2 AutoDMA completed at bus speed | music driver thread used 100% of the IOP (60x real time) | complete after `words × 768` cycles (48 kHz) |
| IOP scheduler pass on every EE checkpoint | 1.5M passes/s | idle skip to the next event |
| IOP heap was bump-only, partition too small | 61,504-byte music buffer allocation failed: silence | reuse freed gaps; HeapBase `0x100000` |
| `cdvdman` couldn't read EE-registered synthetic sectors | sound banks in `.DAT` archives never loaded | share the EE CD sector map |
| `timrman` hardware timer: no repeat, no status bit `0x800` | audio thread never woke: no effects | repeat via intrman, set/clear the status bit |

---

## 6. Rendering through PCSX2's GS

Architecture (`src/gs/`, `src/mc_gs_bridge.cpp`):
- **`mc_gs.dll`** links PCSX2's core library like `pcsx2-gsrunner` but initialises only the GS: a
  C API (`open/close/transfer/vsync/read_fifo/window_resized/get_stats`), our own GS thread with a
  batch buffer (handed over at vsync, wait, or 1 MB) and at most 2 queued vsyncs. No VM, MTGS or
  EE-side GIF unit.
- **Build without touching PCSX2's sources**: configure PCSX2 with
  `-DCMAKE_PROJECT_Pcsx2_INCLUDE=src/gs/pcsx2_inject.cmake`, which defers an `include()` of our
  CMake to the end of PCSX2's top-level CMakeLists (`tools/build_pcsx2_gs.bat`).
- **Host interface**: the 59 `Host::` functions a front-end defines; only ~8 need real code
  (render window, present, logging, `RunOnCPUThread` inline, translations).
- `SysMemory::Allocate()` is needed even for the GS (the SW renderer's JIT lives there).
- **Turn `SkipDuplicateFrames` off**, or frames without privileged-register writes aren't presented.
- **At vsync, copy the display registers, CSR, IMR and SIGLBLID** to the GS thread, then
  `GSvsync((CSR & 0x2000) ? 0 : 1, registers_written)`, exactly as MTGS does.
- **The runtime's own GS frontend keeps running with a `NullBackend`**, so SIGNAL/FINISH/LABEL and
  CSR stay EE-visible; every packet then also goes to `mcgs_transfer`.
- **mc_gs's command buffer is single-producer**: every call into it must come from one thread
  (with the GS worker, that thread; the vblank posts its register snapshot to the worker).
- D3D11 swap chain on the runtime's raylib (GLFW) window; a present hook replaces raylib's upload
  and only polls input. raylib never swaps.
- **PCSX2 v2.9.138 leaks ~2.3 MB per GS reset/state load** (`ResetDrawBuffers`). Load state once.
  Don't file issues automatically: PCSX2 forbids automated issues/PRs.
- Verified with our dump player vs `pcsx2-gsrunner`: pixel-identical frames.
- Graphics options are PCSX2 settings written into the in-memory settings interface:
  upscale, `MaxAnisotropy`, `fxaa`, `TriFilter`, `AspectRatio` (`Stretch` fills 16:9; true
  widescreen needs a camera patch in the game).

---

## 7. Sound through PCSX2's SPU2

- The game's own IOP sound modules (libsd, sdrdrv, modmidi/modhsyn, the game's streamer) run in
  ps2xIOP's interpreter; we give them a real SPU2.
- **Fork: a generic `Spu2Device` interface** in ps2xIOP (16-bit register routing, DMA4/7 callbacks
  with shared IOP RAM, device-driven completion, IRQ9). **Project: `src/gs/mc_spu2.cpp`** exports
  PCSX2's SPU2 from `mc_gs.dll`; `src/mc_spu2_bridge.cpp` attaches it.
- **Clock the IOP from wall time** (36.864 MHz). EE-derived IOP time drifts with the frame rate,
  and the music speed followed it.
- **Cubeb on Windows needs its own thread for open/close** (COM apartment `80010106` otherwise, and
  silent fallback to null output). Check for the null fallback explicitly.
- **PCSX2 stages ordinary SPU2 DMA writes** (a 16 KiB block first copies only ~597 halfwords). Call
  `FinishDMAwrite()` until the whole source is copied before completing; delaying guest completion
  instead caused driver timeouts.
- Diagnose silence by checking, in order: backend open, DMA buffers non-zero, allocations and file
  reads succeeding, voice key-on writes, final-mix capture (`MC_AUDIO_DUMP`).

---

## 8. VU1: ahead-of-time recompilation

- `tools/vu1_recompile.py` turns the microprogram image into C++ (`generated/vu1/`, game-derived,
  untracked); `src/mc_vu1_gen.h` + `src/mc_vu1_fast.cpp` are the runtime side.
- **Reproduce the interpreter's cycle model exactly**, because values depend on it: per-lane stall
  scoreboard, Q latency (DIV 7, RSQRT 13), delayed MAC/clip flags (4 cycles), XGKICK busy time,
  the integer-branch "old VI" quirk, intra-pair write precedence, a branch in a delay slot.
- **Verify mode is essential** (`MC_VU1=verify`): interpreter and compiled code run from the same
  state on every MSCAL; compare VF/VI/ACC/Q/clip, all 16 KB of VU data and every XGKICK packet.
  Run it after every generator change (hundreds of thousands of runs, 0 mismatches).
- Fall back to the interpreter when micro memory doesn't hash to the compiled image.
- Performance lessons, in the order they mattered:
  1. Split into regions of ~64 pairs, never cutting a loop (MSVC huge-function problem).
  2. Registers and scoreboard entries as region locals; force-inline every helper.
  3. **Emit the stall check as a balanced max of ready times, then one max with the cycle.** A
     chain `r = max(r, a); r = max(r, b)…` through the cycle counter made the whole program one
     serial dependency chain for the host CPU (~40% of VU1 time).
  4. Lanes always written together share one scoreboard local; MAC pushes no flag read can see
     are dropped.
  5. **Static schedule per straight run of pairs (2.1x).** Within a run, stalls depend only on the
     ready times read before the run writes them. Emit a fast copy with the stalls worked out at
     generation time, guarded at the run's start by `ready <= cycle + offset of its first reader`
     for each such entry, with the exact code as the fallback. Fast copies jump straight into the
     next fast copy when its guard is already decided (loop back edges), so hot loops run with no
     scoreboard work at all.
  6. Skip operand normalization (Inf/NaN → ±max) for registers known to hold FMAC results: under
     round-to-zero + FTZ with normalized operands those are always finite. Loops normalize the
     registers they never write once on entry.
- **Differential harness for generator changes** (`tools/vu1_bench`): capture real run inputs
  from the game (`MC_VU1_CAPTURE`), link the old and new generated programs into one exe in
  different namespaces (`/Dvu1gen=vu1old`), and compare everything including the cycle count,
  then time both. Faster and more repeatable than in-game verify; still run verify afterwards.

---

## 9. Performance: how to find and fix it

### Measuring
- `tools/sample_profile.py --pid N` samples a running game per thread with PDB symbols.
  `--stacks REGEX` lists return-address chains (who waits where); `--lines REGEX` counts samples
  per source line (classify them by statement type to find the real cost).
- `MC_FRAME_STATS=file`: one line per second with fps, average/worst frame time, frames over one
  and two vblanks. `tools/measure.ps1` runs a window with PCSX2's FPS overlay and that log, so the
  user can drive to slow spots and report times.
- Ask the user to park at the slow spot and leave the game running; profile it live.

### What limited the frame rate, in order
1. **Renderer** (PS2Recomp CPU GS) → PCSX2's GS.
2. **VU1 interpreter** → AOT recompilation (165 ns → 4 ns per VU cycle, then the stall tree).
3. **IOP** busy-streaming at bus speed → AutoDMA timing + idle skip.
4. **Frame pacing**: the game waits for vblanks. Things to check in any game:
   - how many vblanks per frame it waits for (here 2 in races = 25 fps PAL; a "60 fps patch" caps it at 1);
   - **whether a late frame waits for the next vblank**: this game's flip semaphore is only signalled
     by a vblank that arrives while the main loop already waits, so 18–30 ms frames became 33 ms.
     Flipping at once when the countdown is already zero removed the snap to 30;
   - vblank bursts after loads (scheduler backlog).
5. **The game thread doing everything** → a GS worker thread (next section).
6. **VIF1 UNPACK**: the generic per-vector loop. **This game masks nearly every UNPACK**, so a fast
   path for unmasked ones changed nothing; cover masks and fill cycles too. Test fast vs generic on
   random UNPACKs.
7. Remaining overhead: heap allocations, SRW locks, unordered_map lookups for IO registers, EE timer
   upkeep on the game thread; the runtime GS frontend's unused draw batching (~5%).

### The GS worker thread (fork: `PS2Memory::setGsWorker`)
- On GIF/VIF1 DMA kicks, copy the data (chained transfers already were copied) and **complete the
  DMA at once, as before**; a worker thread unpacks VIF1, runs VU1 and feeds the GS, in order.
  The game's own DMA interrupt handler (which here restarts IRQ-terminated chain segments and
  streams textures) keeps running unchanged.
- **Sync points** (the EE waits for the worker): GS privileged registers, GIF/VIF registers and
  FIFOs, VU1 memory, direct GIF/VIF processing, GS stubs, and once per frame (at most one frame in
  flight). Log how often each sync waits; a frequent one kills the parallelism.
- Result: game logic of frame N+1 overlaps VU1/GS work of frame N. New York 44 → 60, London
  35 → ~54 (with the other changes).

---

## 10. Debugging techniques that cracked the hard bugs

- **PCSX2 save states as ground truth**: a `.p2s` is a zip with zstd entries (Windows `tar.exe`
  extracts it); `eeMemory.bin` is the 32 MB EE RAM. Heaps are shifted between PCSX2 and the port,
  so compare by following pointers from globals, not by address. Constants differing by one ulp
  pointed straight at the FPU rounding bugs.
- **Read runtime values, not the ELF's initial data** (log guest RAM from an override). Getting this
  wrong cost a day: the frame timer's fixed step is 1/60 in the ELF but 0 during races.
- **Page-protection watchpoints** (`src/mc_watch.cpp`, `MC_WATCH=addr:bytes`) give the guest PC of
  each store; then decompile that function (`tools/decompile.ps1`). Compare the watched bytes
  before and after each faulting store: checking only the fault address misses 128-bit stores and
  copies that start below the range (that hid a corruption for a whole session).
- **A frozen game that still draws** (black screen, sound stops): sample it with
  `tools/sample_profile.py --pid N`. A game thread sitting in one tiny `entry_XXXXXXXX` is a guest
  `b .` loop, usually the game's own fatal-error trap (here the heap lookup inside `free()`).
- **Guest registers from a live process**: scan the host's committed memory for an
  `R5900Context` (32 × 16-byte GPRs, then `pc` at +512) whose `pc` is the stuck address and whose
  `$ra` is a guest code address; then read `$sp` and the stack from RDRAM (`tools/guest_mem.py`
  finds RDRAM). That gave the bad pointer and the caller without a rebuild.
- **Wrap a suspect game function** with an override that logs its inputs and outputs, and recompute
  the result in Python from a RAM dump.
- **RAM + register dump at the first missing-function trap** showed a stack pointer 0x80 too low
  (the pile-up crash) instead of a corrupted stack.
- **GS dumps** (`MC_GS_DUMP`) replayed in PCSX2 separate "our GS is wrong" from "the game's data is
  wrong". `tools/gsdump_draws.py` decodes a frame's primitives (it found the fade overlay that
  revealed the frozen Count).
- **Instruction census + audit**: list the EE-specific instructions the game actually uses (MMI,
  128-bit loads, `*1` pipeline ops, unaligned loads) and check each translation against the EE
  manual / PCSX2's interpreter. Two of the worst bugs (PEXEW, SQRT.S) were found this way late;
  do it on day one next time.
- The IOP: `PS2X_IOP_PROFILE=1` (fork) for per-thread share and hot PCs; `tools/mips_disasm.py`
  for IRX code with import names.

---

## 11. Things this game does that others probably do too

- An AGE-style frame timer reading COP0 Count, clamped to 0.0001–0.1 s, with a fixed step that
  races turn off. Below the console's frame rate, cap the clock per frame (here 40 ms) or physics
  overshoots.
- Vblank-counted pacing via an INTC handler + semaphore rather than `sceGsSyncV`.
- Some effects count vblanks (a pulsing alpha here), so changing the vblank rate speeds them up;
  prefer changing the frames-per-vblank instead.
- Memory cards as a folder per save (`disc/mc0/<save name>/`).
- Files the game asks for that don't exist on the disc (`AUDVAG\...`) fail on hardware too.
- Distant buildings use low-detail models (pop-in is original behaviour, not a port bug).

---

## 12. Still open for this port (2026-11-01)

- London's densest spots: ~54 fps (VU1 flag elision, drop the runtime GS's unused batching).
- Black screens on "Next Race" in career mode (and after the secret car in London): the race
  teardown (`FUN_00104370`) frees `*0x2C4128`, which something overwrites with garbage. The heap
  lookup override now logs and skips such frees (`[mc_free]`, `MC_BADFREE_DUMP`); the writer is
  still unknown (watch `MC_WATCH=2C4128:4`). It doesn't happen every session.
- New York at x2 draw distance: ~34 fps in the worst spot (GS worker 92% busy in VU1 programs).
- Movies (FFmpeg is in `third_party/ffmpeg`), true widescreen.
