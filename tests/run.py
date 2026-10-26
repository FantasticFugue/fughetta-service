#!/usr/bin/env python3
"""Run CTest as the current developer inside the existing worker delegation."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import uuid


DELEGATION = Path("/sys/fs/cgroup/workers")

# sudo executes only this launcher. The supervisor remains outside the leaf.
# Python's isolated mode keeps user Python settings out of the privileged step.
LAUNCHER = """
import json
import os
from pathlib import Path
import sys

leaf, uid, gid, groups, setpriv, ctest, build, *arguments = sys.argv[1:]
environment = json.load(sys.stdin)
with (Path(leaf) / "cgroup.procs").open("w") as membership:
    membership.write(str(os.getpid()))
group_option = ["--groups", groups] if groups else ["--clear-groups"]
os.execve(setpriv, [setpriv, "--reuid", uid, "--regid", gid,
                  *group_option, "--", ctest, "--test-dir", build, *arguments],
          environment)
"""


def children(pid):
    """Read all threads so a child forked by a non-main thread is included."""
    found = set()
    try:
        tasks = list(Path(f"/proc/{pid}/task").iterdir())
    except FileNotFoundError:
        return found
    for task in tasks:
        try:
            found.update(map(int, (task / "children").read_text().split()))
        except FileNotFoundError:
            pass
    return found


def descendants():
    found = set()
    pending = list(children(os.getpid()))
    while pending:
        pid = pending.pop()
        if pid not in found:
            found.add(pid)
            pending.extend(children(pid))
    return found


def forward(signum):
    # pidfds prevent a PID that exits during cleanup from targeting a new process.
    for pid in descendants():
        try:
            descriptor = os.pidfd_open(pid)
            try:
                if pid in descendants():
                    signal.pidfd_send_signal(descriptor, signum)
            finally:
                os.close(descriptor)
        except (ProcessLookupError, PermissionError):
            # sudo's monitor remains privileged and exits with its launcher.
            pass


def reap(process):
    # Popen owns its direct child's wait status; only reap adopted descendants here.
    process.poll()
    for pid in children(os.getpid()) - {process.pid}:
        try:
            os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            pass


def cleanup(process, leaf, signum):
    """Stop this invocation's descendants, then remove only its own leaf."""
    forward(signum)
    until = time.monotonic() + 5
    while descendants() and time.monotonic() < until:
        reap(process)
        time.sleep(0.05)
    # Descendants may have entered their own task cgroups or sessions. The
    # subreaper and process-tree walk still cover them without scanning siblings.
    until = time.monotonic() + 5
    while True:
        forward(signal.SIGKILL)
        (leaf / "cgroup.kill").write_text("1")
        reap(process)
        if not descendants():
            try:
                leaf.rmdir()
                return
            except OSError:
                # The kernel may still be releasing a just-killed task.
                pass
        if time.monotonic() >= until:
            raise RuntimeError(f"Cleanup did not finish; retained test leaf: {leaf}")
        time.sleep(0.05)


def main():
    parser = argparse.ArgumentParser(
        description="Run CTest in a private leaf of the existing worker cgroup delegation.",
        epilog=("Example: python3 tests/run.py --test-dir build/debug -- -R '^front$' "
                "--output-on-failure. No build directories are created and nothing is built. "
                "CTest runs with noninteractive standard input. Run sudo -v first if needed."),
        allow_abbrev=False,
    )
    parser.add_argument(
        "--test-dir", type=Path,
        default=Path(__file__).resolve().parents[1] / "build/debug",
        help="Existing CTest build directory (default: this project's build/debug).",
    )
    parser.add_argument(
        "--launcher-timeout", type=float,
        help="Stop the entire invocation after this many seconds; return 124.",
    )
    options, arguments = parser.parse_known_args()
    if arguments[:1] == ["--"]:
        arguments = arguments[1:]
    if options.launcher_timeout is not None and not (0 < options.launcher_timeout < float("inf")):
        parser.error("--launcher-timeout must be a finite positive number")
    if os.geteuid() == 0 or os.getuid() != os.geteuid():
        parser.error("Run this entry as a regular user, not with sudo.")
    build = options.test_dir.resolve(strict=True)
    if not (build / "CTestTestfile.cmake").is_file():
        parser.error(f"No configured CTest suite in {build}; select an existing build directory.")
    # These modes can replace the selected directory or launch a build/script.
    if any(value.startswith(("--test-dir", "--preset", "--build-", "--script", "-S"))
           for value in arguments):
        parser.error("Pass test selection/execution options only; build and script modes are not supported.")
    commands = {name: shutil.which(name) for name in ("sudo", "setpriv", "ctest")}
    missing = [name for name, path in commands.items() if path is None]
    if missing:
        parser.error(f"Required commands not found: {', '.join(missing)}")
    enabled = set((DELEGATION / "cgroup.subtree_control").read_text().split())
    if not {"cpu", "memory"} <= enabled:
        parser.error(f"{DELEGATION} must already delegate CPU and memory controllers.")
    authorization = subprocess.run([commands["sudo"], "-n", "-v"], capture_output=True, text=True)
    if authorization.returncode:
        raise RuntimeError("sudo authorization is required. Run sudo -v, then retry.\n"
                           + authorization.stderr.strip())
    library = ctypes.CDLL(None, use_errno=True)
    # PR_SET_CHILD_SUBREAPER: orphaned test descendants return to this supervisor.
    if library.prctl(36, 1, 0, 0, 0) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))

    interrupted = 0

    def interrupt(signum, _frame):
        nonlocal interrupted
        if not interrupted:
            interrupted = signum

    handled = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    previous = {signum: signal.signal(signum, interrupt) for signum in handled}
    leaf = DELEGATION / f"ctest-{os.getuid()}-{uuid.uuid4().hex}"
    process = None
    created = False
    result = 1
    try:
        leaf.mkdir()
        created = True
        if interrupted:
            return 128 + interrupted
        environment = dict(os.environ, TMPDIR=str(build), TMP=str(build), TEMP=str(build))
        command = [commands["sudo"], "-n", "--", sys.executable, "-I", "-c", LAUNCHER,
                   str(leaf), str(os.getuid()), str(os.getgid()),
                   ",".join(map(str, os.getgroups())), commands["setpriv"],
                   commands["ctest"], str(build), *arguments]
        print(f"CTest directory: {build}\nTest cgroup: {leaf}", flush=True)
        started = time.monotonic()
        process = subprocess.Popen(command, cwd=build, stdin=subprocess.PIPE, start_new_session=True)
        # Restore the developer's environment after sudo; no credentials are put
        # in process arguments or written to disk. CTest inherits EOF on stdin.
        try:
            process.stdin.write(json.dumps(environment).encode())
            process.stdin.close()
        except BrokenPipeError:
            pass
        while process.poll() is None:
            if interrupted:
                result = 128 + interrupted
                break
            if options.launcher_timeout is not None and time.monotonic() - started >= options.launcher_timeout:
                print("CTest launcher timeout expired.", file=sys.stderr)
                result = 124
                break
            reap(process)
            time.sleep(0.05)
        else:
            result = process.returncode
            if result < 0:
                result = 128 - result
    finally:
        try:
            if process is not None:
                try:
                    cleanup(process, leaf, interrupted or signal.SIGTERM)
                except (OSError, RuntimeError) as error:
                    print(f"CTest launcher cleanup: {error}", file=sys.stderr)
                    # Preserve a failing CTest status or the initiating signal.
                    result = result or 1
            elif created:
                leaf.rmdir()
        finally:
            for signum, handler in previous.items():
                signal.signal(signum, handler)
    return 128 + interrupted if interrupted else result


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"CTest launcher: {error}", file=sys.stderr)
        sys.exit(1)
