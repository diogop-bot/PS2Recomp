"""Reads a port's ps2port.json (see README.md in this folder) for the tools next to it.

The config is found from, in order: an explicit path (the tools' --port option), the
PS2PORT_CONFIG environment variable, or the first ps2port.json in the current directory or
one of its parents. Paths in the file are relative to the file's own folder.
"""
import json
import os

FILE_NAME = 'ps2port.json'


def find(explicit=None):
    path = explicit or os.environ.get('PS2PORT_CONFIG')
    if path:
        if os.path.isdir(path):
            path = os.path.join(path, FILE_NAME)
        if not os.path.isfile(path):
            raise SystemExit('port config not found: %s' % path)
        return os.path.abspath(path)
    here = os.getcwd()
    while True:
        candidate = os.path.join(here, FILE_NAME)
        if os.path.isfile(candidate):
            return candidate
        parent = os.path.dirname(here)
        if parent == here:
            raise SystemExit('no %s here or above (pass --port or set PS2PORT_CONFIG)' % FILE_NAME)
        here = parent


class Port:
    """Resolved settings of one port. Optional sections come back as None when absent."""

    def __init__(self, path, build_config=None):
        self.path = path
        self.root = os.path.dirname(path)
        with open(path, encoding='utf-8') as f:
            self.raw = json.load(f)
        self.build_config = build_config or self.raw.get('buildConfig', 'RelWithDebInfo')
        self.exe = self.resolve(self.raw['exe'])
        self.elf = self.resolve(self.raw['elf'])
        self.workdir = self.resolve(self.raw.get('workdir', '.'))
        self.logs = self.resolve(self.raw.get('logs', 'build/runs'))
        self.process_name = os.path.splitext(os.path.basename(self.exe))[0]

    def resolve(self, relative):
        if relative is None:
            return None
        relative = relative.replace('{config}', self.build_config)
        return os.path.normpath(os.path.join(self.root, relative))

    def section(self, name):
        return self.raw.get(name) or {}


def load(explicit=None, build_config=None):
    return Port(find(explicit), build_config)
