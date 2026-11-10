"""Check managed worker allocation, child failure, and clean restart on Linux."""

import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

binary = Path(sys.argv[1]).resolve()
build = Path(sys.argv[2]).resolve()


def reserve_ports():
    leases = [socket.socket() for _ in range(3)]
    try:
        for lease in leases:
            lease.bind(("127.0.0.1", 0))
        return [lease.getsockname()[1] for lease in leases]
    finally:
        for lease in leases:
            lease.close()


def children(pid):
    result = set()
    for task in Path(f"/proc/{pid}/task").iterdir():
        result.update(map(int, (task / "children").read_text().split()))
    return result


def ready(process, port, log):
    until = time.monotonic() + 20
    while time.monotonic() < until:
        if process.poll() is not None:
            raise AssertionError(log.read_text())
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/healthz", timeout=1) as response:
                health = json.load(response)
            if health["source"] and health["online_workers"] == 2 and health["lifecycle"] == "ready":
                assert health["instance"] == "fughetta-service-lifecycle-test"
                return
        except (urllib.error.URLError, TimeoutError, ConnectionError):
            pass
        time.sleep(0.025)
    raise AssertionError("managed service readiness timed out: " + log.read_text())


with tempfile.TemporaryDirectory(prefix="managed-budget-", dir=build) as directory:
    root = Path(directory)
    http, source, worker = reserve_ports()
    config = {
        "runtime": {"threads": 10, "jobs": 6, "workers": 2},
        "server": {
            "http_port": http,
            "worker_port": worker,
            "source": f"127.0.0.1:{source}",
            "storage": str(root / "state"),
            "auth": {"tokens": [{"name": "test", "hash": hashlib.sha256(b"test").hexdigest(), "role": "owner"}]},
        },
    }
    (root / "config.json").write_text(json.dumps(config))
    for interrupt in (True, False):
        log = root / ("failure.log" if interrupt else "restart.log")
        notifications = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        notify_path = root / ("failure.sock" if interrupt else "restart.sock")
        notifications.bind(str(notify_path))
        notifications.setblocking(False)
        with log.open("w") as output:
            process = subprocess.Popen([str(binary), "--instance", "lifecycle-test", "config.json"], cwd=root, stdout=output, stderr=subprocess.STDOUT,
                                       env={**os.environ, "NOTIFY_SOCKET": str(notify_path)})
            try:
                ready(process, http, log)
                pids = children(process.pid)
                assert len(pids) == 3, pids
                workers = [pid for pid in pids if b"worker" in Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")]
                assert len(workers) == 2
                for index, parallel in enumerate((3, 3)):
                    assigned = json.loads((root / "state/runtime" / f"local-{index}.json").read_text())
                    assert assigned["parallel"] == parallel
                    assert assigned["compute_threads"] == 1
                    assert assigned["max_inflight"] == 1
                if interrupt:
                    os.kill(workers[0], signal.SIGKILL)
                    assert process.wait(timeout=15) == 1, log.read_text()
                else:
                    process.terminate()
                    assert process.wait(timeout=15) == 0, log.read_text()
                messages = []
                while True:
                    try:
                        messages.append(notifications.recv(4096).decode())
                    except BlockingIOError:
                        break
                assert any("READY=1" in message for message in messages), messages
                assert any("STOPPING=1" in message for message in messages), messages
                assert messages.index("STATUS=initialized") < next(i for i, message in enumerate(messages) if "READY=1" in message), messages
                assert not any(Path(f"/proc/{pid}").exists() for pid in pids), log.read_text()
            finally:
                notifications.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
print("PASS: two managed workers received 3/3 CPU shares; worker failure stopped the group; restart and shutdown reaped all children.")
