"""Exercise process interruption, durable receipts, and offline node exchange."""

import hashlib
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import threading

build = pathlib.Path(sys.argv[1]).resolve()
case = sys.argv[2]
executable = build / "shell"


def operation(name):
    return hashlib.sha256(name.encode()).hexdigest()


class Node:
    def __init__(self, root, trust="", fault=""):
        self.root = pathlib.Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        for kind in ("author", "node"):
            key = self.root / (kind + ".key")
            if not key.exists():
                key.write_bytes(hashlib.sha256(("entry-" + kind + "-" + self.root.name).encode()).digest())
                key.chmod(0o600)
        args = [executable, self.root]
        if trust:
            args.append(trust)
        env = os.environ.copy()
        if fault:
            env["NODE_FAULT"] = fault
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        self.owner = self.call("identity")["node"]

    def call(self, action, expected_code="ok", **fields):
        if action in {"apply", "fork", "integrate", "admit", "reverse", "redo", "resolve"}:
            fields.setdefault("owner", self.owner)
        if action == "initialize":
            fields.setdefault("genesis_nonce", operation("entry-genesis-" + self.root.name))
        self.process.stdin.write(json.dumps(dict(action=action, **fields)) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            self.process.wait(timeout=10)
            raise RuntimeError(f"node exited {self.process.returncode}: {self.process.stderr.read()}")
        result = json.loads(line)
        assert result["code"] == expected_code, (action, result)
        return result

    def crash(self, action, **fields):
        if action in {"apply", "fork", "integrate", "admit", "reverse", "redo", "resolve"}:
            fields.setdefault("owner", self.owner)
        self.process.stdin.write(json.dumps(dict(action=action, **fields)) + "\n")
        self.process.stdin.flush()
        self.process.wait(timeout=10)
        output = self.process.stdout.read()
        errors = self.process.stderr.read()
        assert self.process.returncode == 86 and not output, (self.process.returncode, output, errors)
        self.process.stdin.close()

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            self.process.wait(timeout=20)
        errors = self.process.stderr.read()
        assert self.process.returncode == 0, errors


def file_text(reply, path):
    return next(f["text"] for f in reply["files"] if f["path"] == path)


def seed(root):
    node = Node(root)
    genesis = node.call("initialize")
    result = node.call("apply", operation=operation("seed"), edits=[dict(kind="create", path="a", text="abcd"), dict(kind="create", path="remove", text="remove me"), dict(kind="create", path="delete_me", text="delete")])
    node.close()
    return genesis, result


def atomicity(scratch):
    for boundary in ("before_transaction", "after_objects", "before_acceptance", "before_commit", "after_commit"):
        root = scratch / boundary
        _, old = seed(root)
        edits = [dict(kind="write", path="a", text="new text"), dict(kind="move", path="a", target="moved"), dict(kind="remove", path="remove")]
        # Each path has one observed edit; use distinct files for the atomic batch.
        edits[1] = dict(kind="move", path="remove", target="moved")
        edits[2] = dict(kind="remove", path="delete_me")
        edits.append(dict(kind="create", path="created", text="created"))
        request = dict(operation=operation("atomic"), expected=old["frontier"], edits=edits, result="atomic result")
        crashing = Node(root, fault=boundary)
        crashing.crash("apply", **request)
        reopened = Node(root)
        state = reopened.call("inspect")
        if boundary == "after_commit":
            assert file_text(state, "a") == "new text"
            assert file_text(state, "moved") == "remove me"
            assert file_text(state, "created") == "created"
            recovered = reopened.call("apply", **request)
            assert recovered["replayed"] and recovered["result"] == "atomic result"
        else:
            assert state["frontier"] == old["frontier"] and file_text(state, "a") == "abcd"
            assert {f["path"] for f in state["files"]} == {"a", "remove", "delete_me"}
        reopened.close()
        print("PASS K04", boundary)
    root = scratch / "materialization"
    _, old = seed(root)
    crashing = Node(root, fault="during_materialization")
    crashing.crash("materialize")
    assert not (root / "working/main/.fughetta-version").exists()
    recovered = Node(root)
    recovered.call("recover")
    assert (root / "working/main/.fughetta-version").read_text() == old["version"]
    assert (root / "working/main/a").read_text() == "abcd"
    recovered.close()
    print("PASS K04 interrupted derived materialization")


def recovery(scratch):
    root = scratch / "repository"
    _, first = seed(root)
    node = Node(root)
    request = dict(operation=operation("accepted"), expected=first["frontier"], edits=[dict(kind="edit", path="a", text="A", offset=1)], result="original result")
    accepted = node.call("apply", **request)
    node.call("apply", operation=operation("later"), expected=accepted["frontier"], edits=[dict(kind="edit", path="a", text="B", offset=4)])
    node.call("materialize")
    node.close()
    with sqlite3.connect(root / "repository.sqlite") as db:
        db.execute("DELETE FROM bindings")
    for path in (root / "working/main").glob("*"):
        if path.is_file():
            path.unlink()
    node = Node(root)
    node.call("recover")
    retry = node.call("apply", **request)
    assert retry["receipt"] == accepted["receipt"] and retry["version"] == accepted["version"] and retry["replayed"]
    assert retry["result"] == "original result"
    assert (root / "working/main/a").read_text() == "aAbcBd"
    node.call("receipt", operation=operation("unknown"), expected_code="outcome_unknown")
    node.close()
    print("PASS K05/K07/K08 index and working-file loss, original receipt, unknown outcome")
    node = Node(root)
    before = node.call("inspect")
    head = before["frontier"]
    for index in range(300):
        latest = node.call("apply", operation=operation(f"parallel-load-{index}"), expected=head, intent=f"observation-{index}", result=f"result-{index}", tool="evaluation.verify/1")
        head = latest["frontier"]
    node.close()
    node = Node(root)
    assert node.call("inspect")["source_digest"] == before["source_digest"]
    assert node.call("receipt", target=latest["receipt"])["result"] == "result-299"
    node.close()
    writer = Node(root)
    head = writer.call("inspect")["frontier"]
    readers, failures = [], []
    def open_reader():
        try:
            readers.append(Node(root))
        except BaseException as error:
            failures.append(error)
    opening = threading.Thread(target=open_reader)
    opening.start()
    for index in range(20):
        published = writer.call("apply", operation=operation(f"during-parallel-read-{index}"), expected=head, result=f"concurrent-{index}")
        head = published["frontier"]
    opening.join(timeout=30)
    assert not opening.is_alive() and not failures
    reader = readers[0]
    assert reader.call("inspect")["frontier"] == head
    continued = reader.call("apply", operation=operation("after-parallel-read"), expected=head, result="reader continued")
    assert writer.call("inspect")["frontier"] == continued["frontier"]
    reader.close()
    writer.close()
    with sqlite3.connect(root / "repository.sqlite") as db:
        identity, data = db.execute("SELECT id,data FROM objects WHERE kind='blob' ORDER BY rowid LIMIT 1").fetchone()
        altered = bytearray(data)
        altered[-1] ^= 1
        db.execute("UPDATE objects SET data=? WHERE id=?", (bytes(altered), identity))
    lazy = subprocess.run([executable, root], input=json.dumps({"action": "identity"})+"\n"+json.dumps({"action": "send", "want": [identity]})+"\n", text=True, capture_output=True, timeout=30)
    assert lazy.returncode == 0, lazy.stderr
    loaded, damaged = map(json.loads, lazy.stdout.splitlines())
    assert loaded["code"] == "ok" and loaded["state_snapshot"] and loaded["snapshot_events"] >= 256
    assert damaged["code"] in {"storage_failure", "invalid_record"}, damaged
    rejected = subprocess.run([executable, root, "--audit"], input='{"action":"identity"}\n', text=True, capture_output=True, timeout=30)
    assert rejected.returncode == 1 and not rejected.stdout and any(message in rejected.stderr for message in ("stored object hash mismatch", "compressed object is damaged"))
    with sqlite3.connect(root / "repository.sqlite") as db:
        db.execute("UPDATE objects SET data=? WHERE id=?", (data, identity))
    node = Node(root)
    assert node.call("inspect")["source_digest"] == before["source_digest"]
    assert node.call("receipt", target=accepted["receipt"])["version"] == accepted["version"]
    node.close()
    with sqlite3.connect(root / "repository.sqlite") as db:
        snapshot = db.execute("SELECT data FROM metadata WHERE name='state_snapshot'").fetchone()[0]
        altered = bytearray(snapshot)
        altered[-1] ^= 1
        db.execute("UPDATE metadata SET data=? WHERE name='state_snapshot'", (bytes(altered),))
    rejected = subprocess.run([executable, root], input='{"action":"identity"}\n', text=True, capture_output=True, timeout=30)
    assert rejected.returncode == 1 and not rejected.stdout, (rejected.stdout, rejected.stderr)
    with sqlite3.connect(root / "repository.sqlite") as db:
        db.execute("UPDATE metadata SET data=? WHERE name='state_snapshot'", (snapshot,))
    print("PASS bounded parallel loading, coherent concurrent publication, partial final chunk, explicit archive audit rejects damage, original receipts")


def transfer(scratch):
    a = Node(scratch / "a")
    genesis = a.call("initialize")
    accepted = a.call("apply", operation=operation("large"), edits=[dict(kind="create", path="large", text="x" * 1_100_000)])
    checkpoint = a.call("checkpoint")["checkpoint"]
    b = Node(scratch / "b", genesis["project"])
    offered = a.call("offer")
    # Receive one large object's first chunk and reopen without losing its offset.
    chosen = None
    for identity in offered["objects"]:
        packet = a.call("send", want=[identity], max_bytes=4096)
        if packet["parts"][0]["total"] > 4096:
            chosen = packet["parts"][0]
            break
    assert chosen
    pending = b.call("receive", parts=[chosen])
    assert not pending["complete"] and pending["parts"][0]["offset"] == 4096
    b.close()
    b = Node(scratch / "b", genesis["project"])
    pending = b.call("offer")
    assert not pending["complete"]
    offset = 4096
    while offset < chosen["total"]:
        packet = a.call("send", want=[chosen["id"]], parts=[dict(id=chosen["id"], offset=offset)], max_bytes=65536)
        assert packet["parts"]
        b.call("receive", parts=packet["parts"])
        offset += sum(len(p["data"]) // 2 for p in packet["parts"])
    b.close()
    a.close()
    bundle = scratch / "exchange.dcn"
    with bundle.open("wb") as output:
        subprocess.run([executable, scratch / "a", "--export"], stdout=output, check=True, timeout=60)
    with bundle.open("rb") as source:
        result = subprocess.run([executable, scratch / "b", genesis["project"], "--import"], stdin=source, capture_output=True, check=True, timeout=120)
    imported = json.loads(result.stdout)
    assert imported["complete"]
    b = Node(scratch / "b")
    assert b.call("inspect")["frontier"] == []
    proof = b.call("receipt", target=accepted["receipt"])
    assert proof["issuer"] == genesis["node"]
    fork = b.call("fork", worldline="personal", operation=operation("fork"), source=accepted["frontier"])
    view = b.call("inspect", worldline="personal")
    assert len(file_text(view, "large")) == 1_100_000
    b.call("apply", worldline="personal", operation=operation("continue"), expected=fork["frontier"], edits=[dict(kind="edit", path="large", text="new", offset=0)])
    b.call("checkpoint", worldline="personal")
    b.close()
    print("PASS resumable chunks, signed binary stream, branch isolation, fresh independent continuation")


with tempfile.TemporaryDirectory(prefix=f"node-{case}-", dir=build) as name:
    function = {"atomicity": atomicity, "recovery": recovery, "transfer": transfer}[case]
    function(pathlib.Path(name))
