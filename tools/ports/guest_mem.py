"""Reads and writes a running recompiled game's PS2 RAM (EE RDRAM) from outside, for live experiments.

usage: python -I guest_mem.py read  ADDR [COUNT]          floats at a guest address
       python -I guest_mem.py readw ADDR [COUNT]          32-bit words
       python -I guest_mem.py write ADDR FLOAT [FLOAT...] floats
  options: --port ps2port.json   the port (default: PS2PORT_CONFIG or the nearest ps2port.json)
           --pid N               pick a process (default: the only one running the port's exe)
           --elf PATH            the game's ELF (default: the port's)

The 32 MB RDRAM block is found by matching bytes of the ELF's first executable segment at
their guest address. Writes change the game's memory immediately; the game may overwrite them.
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python -I leaves the script's folder out
import ps2port  # noqa: E402

RAM_SIZE = 32 * 1024 * 1024
SIGNATURE_SIZE = 64

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, wt.LPVOID, wt.LPCVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, wt.LPCVOID, ctypes.c_void_p, ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_uint64), ('AllocationBase', ctypes.c_uint64), ('AllocationProtect', wt.DWORD),
                ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_uint64), ('State', wt.DWORD), ('Protect', wt.DWORD),
                ('Type', wt.DWORD)]


def game_pid(process_name):
    image = process_name + '.exe'
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ' + image, '/FO', 'CSV', '/NH'],
                         capture_output=True, text=True).stdout
    pids = [int(line.split(',')[1].strip('"')) for line in out.splitlines()
            if line.lower().startswith('"%s"' % image.lower())]
    if len(pids) != 1:
        sys.exit('expected one %s, found %d (use --pid)' % (image, len(pids)))
    return pids[0]


def elf_signature(path):
    """(guest address, bytes) of a distinctive 64-byte piece of the ELF's first executable segment
    in RDRAM, to find that segment in the host process."""
    with open(path, 'rb') as f:
        elf = f.read()
    if elf[:4] != b'\x7fELF':
        sys.exit('not an ELF: %s' % path)
    phoff, = struct.unpack_from('<I', elf, 0x1C)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x2A)
    for k in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from('<8I', elf, phoff + k * phentsize)
        guest = p_vaddr & 0x1FFFFFFF  # kseg0/kseg1 mirrors
        if p_type != 1 or not p_flags & 1 or guest >= RAM_SIZE:
            continue  # PT_LOAD, executable, in RDRAM (not the scratchpad)
        for at in range(0, min(p_filesz, 0x10000) - SIGNATURE_SIZE + 1, SIGNATURE_SIZE):
            piece = elf[p_offset + at:p_offset + at + SIGNATURE_SIZE]
            if len(set(piece)) >= 16:  # skip padding and zero fill
                return guest + at, piece
    sys.exit('no executable RDRAM segment in %s' % path)


def read(proc, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not k32.ReadProcessMemory(proc, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)) or got.value != size:
        return None
    return buf.raw


def find_rdram(proc, elf):
    signature_guest, signature = elf_signature(elf)
    # RDRAM is a heap block, not necessarily the start of a region: search committed memory.
    mbi = MBI()
    addr = 0
    chunk = 16 * 1024 * 1024
    while k32.VirtualQueryEx(proc, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        readable = mbi.Protect & 0xEE and not mbi.Protect & 0x100  # any read access, no guard page
        if mbi.State == 0x1000 and readable and mbi.RegionSize >= RAM_SIZE:
            for off in range(0, mbi.RegionSize, chunk):
                size = min(chunk + 64, mbi.RegionSize - off)
                data = read(proc, mbi.BaseAddress + off, size)
                if not data:
                    continue
                at = data.find(signature)
                while at >= 0:
                    base = mbi.BaseAddress + off + at - signature_guest
                    if base % 16 == 0:
                        return base
                    at = data.find(signature, at + 1)
        addr = mbi.BaseAddress + mbi.RegionSize
    sys.exit('RDRAM not found')


def take_option(args, name):
    if name not in args:
        return None
    i = args.index(name)
    if i + 1 >= len(args):
        sys.exit('%s needs a value' % name)
    value = args[i + 1]
    del args[i:i + 2]
    return value


def main():
    args = sys.argv[1:]
    pid = take_option(args, '--pid')
    port_path = take_option(args, '--port')
    elf = take_option(args, '--elf')
    if len(args) < 2:
        sys.exit(__doc__)
    cmd, addr = args[0], int(args[1], 16) & (RAM_SIZE - 1)
    port = None
    if not pid or not elf:
        port = ps2port.load(port_path)
    pid = int(pid) if pid else game_pid(port.process_name)
    proc = k32.OpenProcess(0x0010 | 0x0020 | 0x0008 | 0x0400, False, pid)
    if not proc:
        sys.exit('cannot open process %d: %d' % (pid, ctypes.get_last_error()))
    base = find_rdram(proc, elf or port.elf)
    if cmd in ('read', 'readw'):
        count = int(args[2]) if len(args) > 2 else 1
        raw = read(proc, base + addr, 4 * count)
        fmt = '<%df' % count if cmd == 'read' else '<%dI' % count
        for k, v in enumerate(struct.unpack(fmt, raw)):
            print('0x%08x  %s' % (addr + 4 * k, ('%g' % v) if cmd == 'read' else '0x%08x' % v))
    elif cmd == 'write':
        data = struct.pack('<%df' % (len(args) - 2), *map(float, args[2:]))
        got = ctypes.c_size_t()
        if not k32.WriteProcessMemory(proc, ctypes.c_void_p(base + addr), data, len(data), ctypes.byref(got)):
            sys.exit('write failed: %d' % ctypes.get_last_error())
        print('wrote %d floats at 0x%08x' % (len(args) - 2, addr))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
