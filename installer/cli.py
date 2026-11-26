"""Command line: python -m installer --dest DIR [--base NSP --update NSP --keys prod.keys] [--titlekeys FILE]
                                     [--root DIR] [--dlss|--no-dlss] [--fsr|--no-fsr] [--shortcut] [--import-save] [--launcher]

A first install needs the game files and the keys. On a folder with fastball-install.json (resume after a cancel or a
failure, or update with a newer release) only --dest is needed, unless the extraction must run again. Prints JSON
events on stdout (events.py). Exit code: 0 done, 1 failed, 2 bad command line, 3 cancelled (Ctrl+C or Ctrl+Break).

--launcher: run by a launcher without a window (RecompLauncher, recomplauncher.json `prepare`; progress.py): prints
`progress <0..1> <text>` lines instead of the events (events.ProgressLines) and ends the tools when the launcher kills
the setup (_arm_lifeline).

The install folder may be inside the setup folder (the window's default is MLBTS\\ there: portable), only not the setup
folder itself or one of its own folders (layout.reserved). --temp moves the build's temporary files (about 2 GB, gone
after the build) out of <setup>/temp. Every option is a flag: --help lists them.
"""
import argparse
import ctypes
import dataclasses
import os
import signal
import sys
import time
from pathlib import Path

from . import layout as layouts
from .events import ProgressLines, Reporter
from .steps import Context, all_steps, run_steps

EXIT = {'ok': 0, 'failed': 1, 'cancelled': 3}

# A launcher stops the setup by killing it: no Ctrl+Break arrives, so the cancel path in steps.py never runs and ninja,
# clang or the extraction would carry on. On Windows this process joins a job object that kills its processes when the
# last handle closes; the only handle is ours, so the tools started from here end with it however it ends.
_job = None   # Windows: the job handle, kept open for the life of the process
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JobObjectExtendedLimitInformation = 9


def _arm_lifeline():
    global _job
    if os.name != 'nt' or _job:
        return
    from ctypes import wintypes

    class Basic(ctypes.Structure):
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                    ('LimitFlags', wintypes.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wintypes.DWORD),
                    ('Affinity', ctypes.c_size_t), ('PriorityClass', wintypes.DWORD), ('SchedulingClass', wintypes.DWORD)]

    class Io(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in ('ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount',
                                                   'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]

    class Extended(ctypes.Structure):
        _fields_ = [('BasicLimitInformation', Basic), ('IoInfo', Io), ('ProcessMemoryLimit', ctypes.c_size_t),
                    ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                    ('PeakJobMemoryUsed', ctypes.c_size_t)]

    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateJobObjectW.restype = wintypes.HANDLE
    k.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
    k.SetInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
    k.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
    k.GetCurrentProcess.restype = wintypes.HANDLE
    job = k.CreateJobObjectW(None, None)
    if not job:
        return
    info = Extended()
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if k.SetInformationJobObject(job, JobObjectExtendedLimitInformation, ctypes.byref(info), ctypes.sizeof(info)) \
            and k.AssignProcessToJobObject(job, k.GetCurrentProcess()):
        _job = job   # kept open for the life of the process


class Parser(argparse.ArgumentParser):
    def error(self, message):
        Reporter().error(f'Bad command line: {message}')
        sys.exit(2)


def cancel(signum, frame):
    raise KeyboardInterrupt


def main(argv=None):
    ap = Parser(prog='python -m installer', description=__doc__.splitlines()[0])
    ap.add_argument('--dest', type=Path, required=True, help='install folder')
    ap.add_argument('--base', type=Path, help='the base game NSP')
    ap.add_argument('--update', type=Path, help='the update NSP: 1.0.20 or 1.0.15')
    ap.add_argument('--keys', type=Path, help='prod.keys')
    ap.add_argument('--titlekeys', type=Path, help='title.keys (only when the NSPs lack their tickets)')
    ap.add_argument('--root', type=Path, help='the setup folder or a repository (default: the folder above installer/)')
    ap.add_argument('--temp', type=Path, help="the build's temporary folder (default: temp in the setup folder)")
    ap.add_argument('--dlss', action='store_true',
                    help="download NVIDIA's nvngx_dlss.dll next to the game on any graphics card (by default: on an "
                         'NVIDIA GeForce RTX, found by itself)')
    ap.add_argument('--no-dlss', action='store_true', help="never download NVIDIA's DLSS library")
    ap.add_argument('--fsr', action='store_true',
                    help="download AMD's FSR 3.1 / 4 runtime next to the game on any graphics card (by default: on "
                         'an AMD, NVIDIA or Intel card, found by itself)')
    ap.add_argument('--no-fsr', action='store_true', help="never download AMD's FSR runtime")
    ap.add_argument('--shortcut', action='store_true', help='desktop and Start menu shortcuts (off by default)')
    ap.add_argument('--import-save', action='store_true', help="copy the game's Ryujinx save (mlb.exe --import-save)")
    ap.add_argument('--equipment', action='store_true',
                    help='add every Road to the Show equipment item, offline (tools/equipment)')
    ap.add_argument('--no-equipment', action='store_true', help='take that equipment out again')
    ap.add_argument('--packages', action='store_true',
                    help="Road to the Show's Packages, offline: a server on this PC hands out the equipment packs "
                         '(tools/packages); adds the equipment, not owned at the start')
    ap.add_argument('--no-packages', action='store_true',
                    help='take the packages out again; this unlocks all equipment items in every Road to the Show '
                         'save')
    ap.add_argument('--cpu', choices=('auto', 'v3', 'v2'), default='auto',
                    help='the instruction set the game is built for: auto (default: from the CPU), v3 (AVX2) or v2 '
                         '(SSE4.2, slower: for CPUs without AVX2, or to test that build)')
    ap.add_argument('--launcher', action='store_true',
                    help='run by a launcher: progress lines instead of JSON events')
    args = ap.parse_args(argv)
    out = None
    if args.launcher:
        _arm_lifeline()   # before any tool starts: they join its job
        if hasattr(sys.stdout, 'reconfigure'):
            sys.stdout.reconfigure(encoding='utf-8', errors='replace')   # the launcher reads UTF-8
        out = ProgressLines(sys.stdout)

    # The window cancels with Ctrl+Break to the setup's process group, which reaches the tools it runs too; Python's
    # default for it ends the process without recording anything
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), cancel)
    layout = layouts.find(args.root)
    if args.temp:
        layout = dataclasses.replace(layout, temp=args.temp.resolve())
    dest = args.dest.resolve()
    log = layout.logs / time.strftime('setup-%Y%m%d-%H%M%S.log')   # release: logs/ of the setup folder
    rep = Reporter(out=out, log_path=log)
    t0 = time.monotonic()
    ctx = Context(layout, dest, rep, args)
    steps = [s for s in all_steps() if s.applies(ctx)]
    rep.emit('start', release=layout.release, dev=layout.dev, dest=str(dest), log=str(log),
             steps=[{'id': s.id, 'title': s.title} for s in steps])
    result = run_steps(ctx, steps)
    rep.emit('end', result=result, seconds=round(time.monotonic() - t0, 1))
    rep.close()
    return EXIT[result]
