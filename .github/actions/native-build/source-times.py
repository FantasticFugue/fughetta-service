import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path


def git(*args):
    return subprocess.check_output(("git", *args))


def paths(*args):
    return {os.fsdecode(path) for path in git(*args).split(b"\0") if path}


mode, directory = sys.argv[1:]
build = Path(directory).resolve()
workspace = Path.cwd().resolve()
record = build / "source-times.json"
revision = git("rev-parse", "HEAD").decode().strip()
tracked = paths("ls-files", "-z")

if mode == "save":
    timestamps = {}
    for name in sorted(tracked):
        info = Path(name).lstat()
        if stat.S_ISREG(info.st_mode):
            timestamps[name] = info.st_mtime_ns
    record.write_text(
        json.dumps({"revision": revision, "workspace": str(workspace), "build": str(build), "times": timestamps}),
        encoding="utf-8",
    )
elif mode == "restore":
    if not record.exists():
        sys.exit(0)
    snapshot = json.loads(record.read_text(encoding="utf-8"))
    if snapshot["workspace"] != str(workspace) or snapshot["build"] != str(build):
        print("The cached build uses different absolute paths. Starting a fresh build.")
        shutil.rmtree(build)
        sys.exit(0)
    try:
        changed = paths("diff", "--name-only", "--no-renames", "-z", snapshot["revision"], revision, "--")
    except subprocess.CalledProcessError:
        print("The cached source revision is no longer in the checkout. Starting a fresh build.")
        shutil.rmtree(build)
        sys.exit(0)
    restored = 0
    for name in tracked - changed:
        timestamp = snapshot["times"].get(name)
        if timestamp is not None:
            path = Path(name)
            info = path.lstat()
            if stat.S_ISREG(info.st_mode):
                os.utime(path, ns=(info.st_atime_ns, timestamp))
                restored += 1
    print(f"Restored timestamps for {restored} unchanged source files. Ninja will evaluate {len(changed)} changed paths.")
else:
    sys.exit(f"Unknown source timestamp mode: {mode}")
