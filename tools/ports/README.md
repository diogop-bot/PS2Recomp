# Port tools

Tools for working on a game port built with PS2Recomp: drive the running game, profile it, poke
its PS2 RAM, decompile from Ghidra, and recompile its VU1 microprogram ahead of time. They came
from the Midnight Club: Street Racing port and know nothing about any one game: each port
describes itself in a small `ps2port.json` at the root of its own repository. Windows only
(Win32 APIs for windows, threads and process memory).

## Setting up a port: `ps2port.json`

```json
{
  "name": "Midnight Club: Street Racing (PAL, SLES_500.54)",
  "exe": "build/game/{config}/midnightclub.exe",
  "buildConfig": "RelWithDebInfo",
  "elf": "disc/SLES_500.54",
  "workdir": ".",
  "logs": "build/runs",
  "statusPattern": "mc_gs_dump\\] frame",
  "ghidra": { "project": "ghidra/MidnightClub", "program": "SLES_500.54", "scripts": ["ghidra_scripts"] },
  "vu1": { "microcode": "analysis/vu1/vu1_microcode.bin", "program": "generated/vu1/mc_vu1_program.cpp" }
}
```

| Key | Required | Meaning |
|---|---|---|
| `exe` | yes | the port's executable. `{config}` is replaced by the build configuration. The process name is the file name without `.exe`. |
| `elf` | yes | the game's ELF, passed to the exe as its only argument; also used to find RDRAM. |
| `buildConfig` | no | default for `{config}` (`RelWithDebInfo`); `game_ctl.ps1 -Config` overrides it. |
| `workdir` | no | working directory of the game (`.`). |
| `logs` | no | where `game_ctl.ps1` writes stdout/stderr logs, screenshots and `live.json` (`build/runs`). |
| `statusPattern` | no | regex; `game_ctl.ps1 status` prints the last stderr line matching it. |
| `ghidra.project` | for `decompile.ps1` | the Ghidra project: folder and name, i.e. the `.gpr` path without its extension. |
| `ghidra.program` | no | the program in the project (default: the ELF's file name). |
| `ghidra.scripts` | no | extra Ghidra script folders (the `ghidra/` folder here is always on the path). |
| `vu1.microcode`, `vu1.program` | no | default input and output of `vu1_recompile.py` and `-New` of the VU1 bench. |

Paths are relative to the folder holding `ps2port.json`. Every tool finds the file from, in
order: `--port` / `-Port <file or folder>`, the `PS2PORT_CONFIG` environment variable, or the
first `ps2port.json` in the current directory or one of its parents. A port that wants its old
command lines can keep one-line wrappers in its own `tools/` that set the config and call these
(the Midnight Club repo does: `tools/game_ctl.ps1`, `tools/sample_profile.py`, ...).

Run Python with `-I` (isolated; the tools add their own folder to `sys.path`).

## The tools

| Tool | What it does |
|---|---|
| `game_ctl.ps1 start\|key <Name> [-HoldMs N]\|shot [-Name x]\|status\|stop` | Starts the game detached (logs in `logs`), presses keys in its window (it does not need focus), saves PNGs of the window, reports its state, stops it. Environment variables set before `start` are inherited, so per-game switches (`PS2X_...`, the port's own) go there. |
| `GameWindow.psm1` | The window helpers behind `game_ctl.ps1`, for a port's own scripts: `Send-GameKey`, `Save-GameWindowImage`, `Restore-GameWindow`, `Get-GameWindowInfo`, `$GameKeys` (A-Z, 0-9, F1-F12, arrows, Enter, Escape, Space, Tab, Backspace, shifts, LCtrl). Keys are posted with their scancodes, which is what GLFW reads. |
| `sample_profile.py [--pid N] [--seconds 15] [--top 25] [--stacks REGEX] [--lines REGEX]` | Per-thread sampling profiler with PDB symbols. Without `--pid` it launches the game and waits `--warmup` seconds first. `--stacks` lists chains of return addresses into the exe (who waits where), `--lines` counts samples per source line, `--classify LABEL=REGEX,...` attributes each sample to the first label found on its stack (e.g. `VU0=executeVU0Microprogram,VU1=syncCoreSubsystems`). `--pid` needs no config. |
| `guest_mem.py read\|readw\|write ADDR ...`, `guest_mem.py dump FILE` | Reads floats or words from, or writes floats to, the running game's EE RAM; `dump` saves all 32 MB (same layout as `eeMemory.bin` in a PCSX2 save state). RDRAM is found by matching bytes of the ELF's first executable segment at their guest address. |
| `decompile.ps1 <out.c> 0xADDR ...` | Ghidra pseudocode of the functions holding those addresses, from the port's project, read-only (`ghidra/DecompileFunctions.java`). Needs `GHIDRA_INSTALL_DIR`. |
| `vu1/vudis.py <microcode.bin> [start end] [--labels]` | VU0/VU1 disassembler (mnemonics, both halves of each pair, I/E/M/D/T bits, branch labels). Input: any 16 KB micro-memory image, such as `vu1MicroMem.bin` from a PCSX2 save state or the `.vucode` files the runtime's VU1 switches write. |
| `vu1/vu1_recompile.py [<microcode.bin> <out.cpp>]` | Ahead-of-time recompiler for a VU1 microprogram (see below). |
| `vu1/bench/build.ps1 -Old <old.cpp> [-New <new.cpp>]` | Differential test and benchmark of two generated VU1 programs (see below). |

## VU1 ahead-of-time recompilation

`vu1_recompile.py` turns a 16 KB VU1 micro memory image into C++ written against the model in
`vu1/ps2x_vu1_gen.h` (namespace `ps2x::vu1gen`). It reproduces PS2Recomp's `VU1Interpreter`
exactly, including its cycle model, and stops on instructions the model doesn't cover. The
generated file is game-derived: keep it out of git. The port supplies the rest (in Midnight Club,
`src/mc_vu1_fast.cpp`):

- add `tools/ports/vu1` to the include path and compile the generated file;
- define `ps2x::vu1gen::xgkick(Ctx &, qwordAddress, cycle)`: assemble the GIF packet from data
  memory, hand it to `Ctx::kick`, return the cycle the transfer ends;
- on MSCAL/MSCNT, when micro memory equals `kProgram`, load a `Ctx` from the VU1 state, call
  `run()`, and write the state back; otherwise use the interpreter. Keep a verify mode that runs
  both from the same state and compares everything.

The bench (`vu1/bench`) links two generated programs into one exe, each in its own namespace
(`/Dvu1gen=vu1old`, `/Dvu1gen=vu1new`), runs every captured input through both and compares
registers, flags, Q, the end pc, the cycle count, data memory and every XGKICK packet, then times
both. The port writes the capture: one `BenchRecord` (`vu1_bench.h`) per sampled run, just before
the compiled program runs. Build with `build.ps1 -Old <old.cpp>`, run
`<OutDir>\vu1_bench.exe <capture> [repeat]`. When the old program was generated for an older
model header, put that header in `<OutDir>\old_include\ps2x_vu1_gen.h`.

## VU1 debugging switches (runtime, environment variables)

All off by default; the runtime reads them once at start. Pair them with `vu1/vudis.py`.

| Variable | What it does |
|---|---|
| `PS2X_VU1_TRACE=<file>` | Per-instruction VU1 trace: cycle, pc, both instruction words and the VI registers that changed. Arms when a run starts at `PS2X_VU1_TRACE_PC` (default any), after `PS2X_VU1_TRACE_SKIP` such starts, then logs `PS2X_VU1_TRACE_MAX` pairs (default 200000) across later MSCNT resumes. On arming it writes `<file>.vucode`/`.vumem` and the VF/VI state, and `<file>.end.vumem` when that first run stops. |
| `PS2X_VU1_TRACE_DATA=<byte addr>,<word>`, `PS2X_VU1_TRACE_AFTER_SEC=<s>`, `PS2X_VU1_TRACE_AFTER=<cycles>` | Extra arming conditions for `PS2X_VU1_TRACE`: a VU1 data word must hold that value (e.g. `0x24,0x3C23D70A` catches models whose up row is scale 0.01); at least that many seconds since the first VU1/VU0 run; the run's cycle counter at least that (it restarts at every MSCAL, so it is no clock). |
| `PS2X_VU1_LONG_RUNS=1`, `PS2X_VU1_RUN_PC=<pc>` | One line per run longer than 100k cycles (or starting at that pc): start/end pc, cycles, TOP and the four words at TOP. |
| `PS2X_VU1_BUDGET_DUMP=<file>` | Code and data at the first run cut off by the cycle budget (a program that never reaches its E bit). |
| `PS2X_XGKICK_DUMP=<file>` | First XGKICK with no EOP within 64 KB: every GIFtag walked, VI registers at the kick, code and data. Such packets are dropped and logged as `[vu1] XGKICK from ... packet dropped`. |
| `PS2X_VU1_STORE_WATCH=<byte addr>` | VU1 stores to that quadword (pc, value, VI registers); also EE writes into VU1 data below 0x50. |
| `PS2X_VIF1_WATCH=<qw addr>` | VIF1 UNPACKs whose destination covers that VU1 quadword. |
| `PS2X_VIF1_UNPACK_LOG=<n>` | The first n distinct UNPACK shapes (format, imm, count, CYCLE, TOPS), whatever the destination. |
| `PS2X_VU0_WATCH=<pc>,<byte addr>` | Tally of the VU0 data word at that address after each VCALLMS/VCALLMSR to that pc (a result the EE reads back), every 2000 calls. |
| `PS2X_VU0_DUMP=<file>[,<pc>]`, `PS2X_VU0_DUMP_SKIP=<n>` | Before the first VCALLMS/VCALLMSR (to that pc, after n such calls): `<file>.vucode`, `<file>.vumem` and `<file>.regs` (VF in hex, ACC/I/Q, VI). Read the code with `vudis.py`. |
| `PS2X_VU0_DATA_WATCH=<byte addr>` | Writes to that VU0 data quadword: EE stores and SPR DMA (`[ee:vu0data]`) and VU0 micro stores (`[vu0:store]`, with pc and vi14). VIF0 UNPACKs are not reported. |
| `PS2X_VIF0_LOG=1` | Distinct VIF0 UNPACK shapes, the first 20 MSCALs (with ITOPS and the next codes), codes kept waiting for more data, and unhandled VIF0 codes (MSCNT etc. drop the rest of the packet). |
| `PS2X_INTC_LOG=1` | Every AddIntcHandler/AddDmacHandler: cause, handler, next, argument. |
| `PS2X_VIF0_MSCAL=0` / `=drop` | A/B diagnostics: ignore VIF0 MSCAL/MSCALF, or the old behaviour (MSCAL unhandled, rest of the packet dropped). |
| `PS2X_VIF1_MPG_LOG=1`, `PS2X_VIF1_DROP_LOG=1` | MPG uploads (then MSCAL/MSCNT); UNPACK/MPG payloads that run past their DMA chunk. |

How they were used (This Is Football 2005): `LONG_RUNS` showed a program never reaching its E bit,
`TRACE` from its entry found the first wrong branch within 30 instructions, and `STORE_WATCH` +
`VIF1_WATCH` + a PCSX2 save state proved that nothing in the port wrote a quadword the game
expects. That led to a DMA packet the runtime never delivered.
