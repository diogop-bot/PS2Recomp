"""Sampling profiler for a recompiled game (Windows, x64).

usage: python -I sample_profile.py [--port ps2port.json] [--warmup 30] [--seconds 15] [--interval-ms 2]
                                   [--top 25] [--pid N] [--stacks REGEX] [--lines REGEX]

Launches the port's exe on its ELF (from ps2port.json, see README.md), waits for the warm-up,
then repeatedly suspends each thread, reads its RIP and resumes it. Samples are symbolised
from the exe's PDB with dbghelp and reported per thread (busiest threads first).
The game is stopped at the end. With --pid, a running game is sampled instead and left running
(no port config is needed then).
With --stacks, samples whose function matches REGEX also scan the thread's stack for return
addresses into the game's exe, and the busiest threads list the most common chains of them.
With --lines, samples in functions matching REGEX are also counted per source line.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I leaves the script's folder out
import ps2port  # noqa: E402

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
dbghelp = ctypes.WinDLL('dbghelp', use_last_error=True)

TH32CS_SNAPTHREAD = 0x4
THREAD_ACCESS = 0x0002 | 0x0008 | 0x0040  # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION
PROCESS_ALL_ACCESS = 0x1F0FFF
CONTEXT_CONTROL = 0x100001
RIP_OFFSET = 0xF8
RSP_OFFSET = 0x98
STACK_SCAN_BYTES = 8192
CONTEXT_SIZE = 1232


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ThreadID', wt.DWORD),
                ('th32OwnerProcessID', wt.DWORD), ('tpBasePri', wt.LONG), ('tpDeltaPri', wt.LONG),
                ('dwFlags', wt.DWORD)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]


class MODULEINFO(ctypes.Structure):
    _fields_ = [('lpBaseOfDll', ctypes.c_void_p), ('SizeOfImage', wt.DWORD), ('EntryPoint', ctypes.c_void_p)]


psapi = ctypes.WinDLL('psapi', use_last_error=True)
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
dbghelp.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
dbghelp.SymLoadModuleEx.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                    wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbghelp.SymLoadModuleEx.restype = ctypes.c_uint64
dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]


dbghelp.SymGetLineFromAddr64.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]
psapi.EnumProcessModules.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD, ctypes.POINTER(wt.DWORD)]
psapi.GetModuleFileNameExA.argtypes = [wt.HANDLE, wt.HMODULE, ctypes.c_char_p, wt.DWORD]
psapi.GetModuleInformation.argtypes = [wt.HANDLE, wt.HMODULE, ctypes.POINTER(MODULEINFO), wt.DWORD]


def module_path(proc, module):
    path = ctypes.create_string_buffer(520)
    psapi.GetModuleFileNameExA(proc, module, path, 520)
    return path.value


def load_symbols(proc):
    """Registers every loaded module (with its PDB when one is next to it) with dbghelp."""
    dbghelp.SymSetOptions(0x2 | 0x10)  # UNDNAME | LOAD_LINES off; eager loads
    mods = (wt.HMODULE * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModules(proc, mods, ctypes.sizeof(mods), ctypes.byref(needed))
    exe = module_path(proc, mods[0]).decode(errors='replace')  # the first module is the exe
    search = (os.path.dirname(exe) + ';' + os.environ.get('SystemRoot', r'C:\Windows') + r'\System32').encode()
    if not dbghelp.SymInitialize(proc, search, False):
        print('SymInitialize failed:', ctypes.get_last_error())
    for i in range(needed.value // ctypes.sizeof(wt.HMODULE)):
        info = MODULEINFO()
        psapi.GetModuleInformation(proc, mods[i], ctypes.byref(info), ctypes.sizeof(info))
        dbghelp.SymLoadModuleEx(proc, None, module_path(proc, mods[i]), None, info.lpBaseOfDll or 0, info.SizeOfImage, None, 0)


def thread_ids(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(THREADENTRY32)
    ids = []
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            ids.append(entry.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return ids


def main():
    ap = argparse.ArgumentParser(description='Per-thread sampling profiler for a recompiled game.')
    ap.add_argument('--port', default=None, help='ps2port.json (default: PS2PORT_CONFIG or the nearest one)')
    ap.add_argument('--warmup', type=float, default=30)
    ap.add_argument('--seconds', type=float, default=15)
    ap.add_argument('--interval-ms', type=float, default=2)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--pid', type=int, default=0)
    ap.add_argument('--stacks', default='')
    ap.add_argument('--lines', default='')
    args = ap.parse_args()

    game = None
    pid = args.pid
    if not pid:
        port = ps2port.load(args.port)
        game = subprocess.Popen([port.exe, port.elf], cwd=port.workdir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pid = game.pid
    try:
        if game:
            time.sleep(args.warmup)
        proc = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
        handles = {}
        for tid in thread_ids(pid):
            h = k32.OpenThread(THREAD_ACCESS, False, tid)
            if h:
                handles[tid] = h
        exe_lo = exe_hi = 0
        if args.stacks:
            mods = (wt.HMODULE * 1)()
            needed = wt.DWORD()
            psapi.EnumProcessModules(proc, mods, ctypes.sizeof(mods), ctypes.byref(needed))
            info = MODULEINFO()
            psapi.GetModuleInformation(proc, mods[0], ctypes.byref(info), ctypes.sizeof(info))
            exe_lo = info.lpBaseOfDll or 0
            exe_hi = exe_lo + info.SizeOfImage
        stack_buf = ctypes.create_string_buffer(STACK_SCAN_BYTES)
        stacks = collections.defaultdict(list)  # tid -> [(rip, (return addresses...))]
        ctx_buf = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
        ctx_addr = (ctypes.addressof(ctx_buf) + 15) & ~15
        samples = collections.defaultdict(collections.Counter)
        end = time.perf_counter() + args.seconds
        rounds = 0
        while time.perf_counter() < end:
            for tid, h in handles.items():
                if k32.SuspendThread(h) == 0xFFFFFFFF:
                    continue
                ctypes.memset(ctx_addr, 0, CONTEXT_SIZE)
                ctypes.c_uint32.from_address(ctx_addr + 0x30).value = CONTEXT_CONTROL
                if k32.GetThreadContext(h, ctypes.c_void_p(ctx_addr)):
                    rip = ctypes.c_uint64.from_address(ctx_addr + RIP_OFFSET).value
                    samples[tid][rip] += 1
                    if args.stacks:
                        rsp = ctypes.c_uint64.from_address(ctx_addr + RSP_OFFSET).value
                        got = ctypes.c_size_t()
                        size = STACK_SCAN_BYTES
                        while size >= 256 and not k32.ReadProcessMemory(proc, ctypes.c_void_p(rsp), stack_buf, size, ctypes.byref(got)):
                            size //= 2  # the read stops at the end of the stack's committed pages
                        if size >= 256:
                            words = (ctypes.c_uint64 * (got.value // 8)).from_buffer_copy(stack_buf.raw[:got.value // 8 * 8])
                            stacks[tid].append((rip, tuple(w for w in words if exe_lo <= w < exe_hi)[:24]))
                k32.ResumeThread(h)
            rounds += 1
            time.sleep(args.interval_ms / 1000)

        load_symbols(proc)
        cache = {}

        def name(addr):
            if addr in cache:
                return cache[addr]
            sym = SYMBOL_INFO()
            sym.SizeOfStruct = 88  # sizeof(SYMBOL_INFO) on x64, with Name[1]
            sym.MaxNameLen = 511
            disp = ctypes.c_uint64()
            if dbghelp.SymFromAddr(proc, addr, ctypes.byref(disp), ctypes.byref(sym)):
                cache[addr] = sym.Name.decode(errors='replace')
            else:
                cache[addr] = '0x%x' % addr
            return cache[addr]

        print(f'{rounds} sampling rounds over {args.seconds:.0f} s, {len(handles)} threads')
        busy = sorted(samples.items(), key=lambda kv: -sum(kv[1].values()))
        for tid, counter in busy:
            by_fn = collections.Counter()
            for addr, n in counter.items():
                by_fn[name(addr)] += n
            total = sum(by_fn.values())
            idle = sum(n for f, n in by_fn.items() if 'Wait' in f or 'Sleep' in f or 'NtDelay' in f or 'ZwWait' in f)
            if total - idle < total * 0.05:
                continue  # mostly waiting
            print(f'\n== thread {tid}: {total} samples, {100 * (total - idle) / total:.0f}% not waiting')
            for fn, n in by_fn.most_common(args.top):
                print(f'  {100 * n / total:5.1f}%  {fn}')
            if args.lines:
                rx = re.compile(args.lines)
                lines = collections.Counter()
                for addr, n in counter.items():
                    if rx.search(name(addr)):
                        line = IMAGEHLP_LINE64()
                        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
                        disp = wt.DWORD()
                        if dbghelp.SymGetLineFromAddr64(proc, addr, ctypes.byref(disp), ctypes.byref(line)):
                            lines[f'{os.path.basename(line.FileName.decode(errors="replace"))}:{line.LineNumber}'] += n
                        else:
                            lines['0x%x' % addr] += n
                for where, n in lines.most_common(args.top):
                    print(f'    {100 * n / total:5.1f}%  [{args.lines}] {where}')
            if args.stacks:
                rx = re.compile(args.stacks)
                chains = collections.Counter()
                for rip, rets in stacks[tid]:
                    if rx.search(name(rip)):
                        names = []
                        for r in rets:
                            n = name(r)
                            if n not in names and not n.startswith('0x'):
                                names.append(n)
                            if len(names) == 6:
                                break
                        chains[' < '.join(names)] += 1
                for chain, n in chains.most_common(8):
                    print(f'    {100 * n / total:5.1f}%  [{args.stacks}] {chain}')
    finally:
        if game:
            game.kill()


if __name__ == '__main__':
    sys.exit(main())
