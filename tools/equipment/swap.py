"""Swapping a finished file in over the old one, on PCs where Windows refuses the rename.

Issue #24: on one player's PC (no antivirus, they say; Chinese Windows) renaming a file over an .XML fails with
WinError 17 (ERROR_NOT_SAME_DEVICE, "cannot move the file to a different disk drive") although both names are in one
folder of one drive. Renames over other types work there: the RomFS extraction renamed 7,582 .MOD/.AB files before
its first .XML failed (tools/romfs_dump.py finish_part, setup 0.2.1), and the equipment step (setup 0.2.1) swapped in
LIVE.TEX and MLBEQUIPMENT.JSON, then failed on FRONTEND/RTTS/INVENTORYDATA.XML.new -> INVENTORYDATA.XML. That is not the
file system's answer; some file filter on that PC answers renames onto .XML that way. So every swap in this tool tries
the rename a moment (something holding the file), then copies the bytes over and deletes the source, which needs no
rename.
"""
import os
import shutil
import time

_noted = False


def swap_in(src, dst):
    """Move src over dst (both in one folder): a rename, or a copy and delete when Windows refuses the rename."""
    global _noted
    for _ in range(8):
        try:
            os.replace(src, dst)
            return
        except FileNotFoundError:
            raise
        except OSError as e:
            err = e
            time.sleep(0.25)
    if not _noted:
        _noted = True
        print(f'note: renaming {src.name} failed ({err}); copying instead', flush=True)
    shutil.copyfile(src, dst)
    if dst.stat().st_size != src.stat().st_size:
        raise OSError(f'copy of {src} to {dst} came out short')
    src.unlink()
