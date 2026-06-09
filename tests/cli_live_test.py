"""Exercise byte accuracy, send outcomes and cancellation against Linux PTYs."""
import json
import os
import pathlib
import pty
import select
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

EXE = str(pathlib.Path(sys.argv[1]).resolve())


def events(output):
    return [json.loads(line) for line in output.splitlines()]


def serial_pair():
    master, slave = pty.openpty()
    name = os.ttyname(slave)
    os.close(slave)
    return master, name


def receive(master, length):
    result = bytearray()
    deadline = time.monotonic() + 3
    while len(result) < length and time.monotonic() < deadline:
        if select.select([master], [], [], 0.02)[0]:
            try:
                result.extend(os.read(master, length - len(result)))
            except OSError:
                time.sleep(0.005)
    return bytes(result)


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    environment = dict(os.environ, XDG_CONFIG_HOME=str(root / "config"))
    master, port = serial_pair()
    try:
        payload = bytes(range(256))
        source = root / "command.bin"
        source.write_bytes(payload)
        log = root / "capture.ssr"
        peer_result = []

        def peer():
            peer_result.append(receive(master, len(payload)))
            os.write(master, b"\x00\xff")
            time.sleep(0.03)
            os.write(master, b"OK")

        worker = threading.Thread(target=peer, daemon=True)
        worker.start()
        command = [EXE, "send", "--port", port, "--file", str(source), "--reply-hex", "00FF4F4B", "--record", str(log), "--format", "jsonl"]
        proc = subprocess.run(command, capture_output=True, timeout=5)
        worker.join(timeout=3)
        assert proc.returncode == 0, proc.stderr
        assert peer_result == [payload], peer_result
        stream = events(proc.stdout)
        tx = b"".join(bytes.fromhex(e["raw_hex"]) for e in stream if e["kind"] == "data" and e["direction"] == "tx")
        assert tx == payload
        assert any(e["kind"] == "request_result" and e["detail"]["reply_matched"] for e in stream)
        db = sqlite3.connect(f"file:{log}?mode=ro", uri=True)
        assert b"".join(row[0] for row in db.execute("select bytes from events where kind='data' and direction='tx' order by seq")) == payload
        assert json.loads(db.execute("select value from metadata where key='clean_shutdown'").fetchone()[0]) is True
        db.close()
        timeout = subprocess.run([EXE, "send", "--port", port, "--text", "x", "--wait-reply", "--reply-timeout", "60ms", "--format", "jsonl"], capture_output=True, timeout=3)
        assert timeout.returncode == 6, timeout.stderr
        assert any(e["kind"] == "send_end" and e["detail"]["status"] == "complete" for e in events(timeout.stdout))
        receive(master, 1)
        nul_source = subprocess.run([EXE, "session", "--port", port, "--format", "jsonl"], input=f":send file {source}\0suffix\n".encode(), capture_output=True, timeout=3)
        assert nul_source.returncode == 0, nul_source.stderr
        assert any(e["kind"] == "command_error" for e in events(nul_source.stdout))
        assert not any(e["kind"] == "data" and e["direction"] == "tx" for e in events(nul_source.stdout))
        saved = subprocess.run([EXE, "profile", "save", "bench", "--port", port, "--baud", "9600"], env=environment, capture_output=True)
        assert saved.returncode == 0, saved.stderr
        reused = subprocess.run([EXE, "send", "--profile", "bench", "--baud", "115200", "--text", "z", "--format", "jsonl"], env=environment, capture_output=True, timeout=3)
        assert reused.returncode == 0, reused.stderr
        assert events(reused.stdout)[0]["detail"]["actual_config"]["baud"] == 115200
        receive(master, 1)
    finally:
        os.close(master)

    master, port = serial_pair()
    try:
        large = root / "large.bin"
        large.write_bytes(b"x" * (2 * 1024 * 1024))
        blocked = subprocess.run([EXE, "send", "--port", port, "--file", str(large), "--write-timeout", "60ms", "--format", "jsonl"], capture_output=True, timeout=3)
        assert blocked.returncode == 5, (blocked.returncode, blocked.stderr)
        end = next(e for e in events(blocked.stdout) if e["kind"] == "send_end")
        assert end["detail"]["status"] == "partial" and 0 < end["detail"]["accepted_bytes"] < large.stat().st_size
        receive(master, end["detail"]["accepted_bytes"])
        stopped = subprocess.run([EXE, "session", "--port", port, "--format", "jsonl"], input=f":send file {large}\n:quit\n".encode(), capture_output=True, timeout=3)
        assert stopped.returncode == 130, (stopped.returncode, stopped.stderr)
        assert not any(e["kind"] == "data" and e["direction"] == "tx" for e in events(stopped.stdout)), "write after :quit"
    finally:
        os.close(master)

    # A frame started before the first TX must not satisfy reply waiting.
    master, port = serial_pair()
    try:
        protocol = root / "fixed.json"
        protocol.write_text(json.dumps({"schema_version": 1, "framing": {"mode": "fixed", "length": 4}}))
        child = subprocess.Popen([EXE, "send", "--port", port, "--stdin", "--protocol", str(protocol), "--wait-reply", "--reply-timeout", "80ms", "--format", "jsonl"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        prefix = []
        while True:
            line = child.stdout.readline()
            assert line, "child exited before connecting"
            prefix.append(line)
            if json.loads(line)["kind"] == "connect":
                break
        os.write(master, b"ol")
        while True:
            line = child.stdout.readline()
            prefix.append(line)
            if json.loads(line)["kind"] == "data":
                break
        child.stdin.write(b"x")
        child.stdin.close()
        child.stdin = None
        assert receive(master, 1) == b"x"
        os.write(master, b"zz")
        output, error = child.communicate(timeout=3)
        assert child.returncode == 6, (child.returncode, error, b"".join(prefix) + output)
    finally:
        os.close(master)

    invalid = subprocess.run([EXE, "send", "--port", "/does-not-exist", "--hex", "ABC"], capture_output=True)
    assert invalid.returncode == 2
    missing = subprocess.run([EXE, "send", "--port", "/does-not-exist", "--text", "x", "--format", "jsonl"], capture_output=True)
    assert missing.returncode == 3
    assert any(e["kind"] == "connect_failed" for e in events(missing.stdout))
print("CLI live tests passed")
