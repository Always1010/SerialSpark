"""Verify live/offline equivalence, immutable logs and provenance exports."""
import csv
import hashlib
import io
import json
import os
import pathlib
import pty
import sqlite3
import subprocess
import sys
import tempfile
import time

EXE = str(pathlib.Path(sys.argv[1]).resolve())
PROTOCOL = pathlib.Path(sys.argv[2]).resolve()


def run(*args, expected=0):
    result = subprocess.run([EXE, *map(str, args)], capture_output=True, timeout=5)
    assert result.returncode == expected, (args, result.returncode, result.stderr, result.stdout)
    return result.stdout


def frames(data):
    return [j for j in (json.loads(line) for line in data.splitlines()) if j.get("kind") == "frame"]


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    master, slave = pty.openpty()
    port = os.ttyname(slave)
    os.close(slave)
    log = root / "capture.ssr"
    raw = b"7,23.5,1\r\n8,25,0\r\ntail"
    try:
        child = subprocess.Popen([EXE, "capture", "--port", port, "--protocol", str(PROTOCOL), "--record", str(log), "--rotate-bytes", "65536", "--duration", "180ms", "--format", "jsonl"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        prefix = child.stdout.readline()
        assert json.loads(prefix)["kind"] == "connect", prefix
        os.write(master, raw[:5])
        time.sleep(0.015)
        os.write(master, raw[5:10])
        time.sleep(0.015)
        os.write(master, raw[10:])
        output, error = child.communicate(timeout=3)
        assert child.returncode == 0, error
        live = frames(prefix + output)
        assert [f["fields"].get("temperature") for f in live if f["status"] == "ok"] == [23.5, 25.0]
        assert live[-1]["status"] == "incomplete" and bytes.fromhex(live[-1]["raw_hex"]) == b"tail"
    finally:
        os.close(master)

    original = {p: hashlib.sha256(p.read_bytes()).digest() for p in root.glob("*.ssr")}
    parsed = frames(run("log", "parse", log, "--protocol", PROTOCOL, "--format", "jsonl"))
    for f in parsed:
        assert f.pop("parse_config_ref") == 0
    assert parsed == live
    info = json.loads(run("log", "info", log, "--format", "json"))
    assert info["complete"] is True and info["raw_bytes"] == len(raw)
    assert info["event_payload_bytes"] == info["data_bytes"] == info["rx_data_bytes"] == len(raw) and info["tx_data_bytes"] == 0
    selected = frames(run("log", "parse", log, "--protocol", PROTOCOL, "--direction", "rx", "--where", "temperature > 24", "--format", "jsonl"))
    assert len(selected) == 1 and selected[0]["fields"]["temperature"] == 25
    content = frames(run("log", "parse", log, "--protocol", PROTOCOL, "--contains-text", "23.5", "--format", "jsonl"))
    assert len(content) == 1 and content[0]["fields"]["temperature"] == 23.5
    selected_time = frames(run("log", "parse", log, "--protocol", PROTOCOL, "--from-ns", live[1]["wall_ns"], "--to-ns", live[1]["wall_ns"], "--format", "jsonl"))
    assert [f["fields"]["temperature"] for f in selected_time if f["status"] == "ok"] == [25]
    assert all(f["wall_ns"] == live[1]["wall_ns"] for f in selected_time)
    exported = root / "rx.bin"
    run("log", "export", log, "--format", "bin", "--direction", "rx", "--output", exported)
    assert exported.read_bytes() == raw
    run("log", "export", log, "--format", "bin", expected=2)
    run("log", "export", log, "--format", "bin", "--direction", "rx", "--output", exported, expected=4)
    rows = list(csv.DictReader(io.StringIO(run("log", "export", log, "--protocol", PROTOCOL, "--format", "csv").decode())))
    assert len(rows) == 3 and rows[0]["field.temperature"] == "23.5"
    assert json.loads(rows[0]["sources"]) == live[0]["sources"]
    assert json.loads(rows[0]["protocol_config"]) == json.loads(PROTOCOL.read_text())
    modified = json.loads(PROTOCOL.read_text())
    modified["fields"][1]["scale"] = 2
    new_protocol = root / "modified.json"
    new_protocol.write_text(json.dumps(modified))
    reinterpreted = frames(run("log", "parse", log, "--protocol", new_protocol, "--where", "temperature >= 47", "--format", "jsonl"))
    assert [f["fields"]["temperature"] for f in reinterpreted] == [47, 50]
    modified["fields"][1]["name"] = "温度"
    new_protocol.write_text(json.dumps(modified))
    chinese = frames(run("log", "parse", log, "--protocol", new_protocol, "--where", "温度 > 49", "--format", "jsonl"))
    assert len(chinese) == 1 and chinese[0]["fields"]["温度"] == 50
    modified["fields"][1]["name"] = "温 度>"
    new_protocol.write_text(json.dumps(modified))
    quoted = frames(run("log", "parse", log, "--protocol", new_protocol, "--where", '"温 度>" == 47', "--format", "jsonl"))
    assert len(quoted) == 1 and quoted[0]["fields"]["温 度>"] == 47
    modified["fields"][1]["name"] = "status"
    new_protocol.write_text(json.dumps(modified))
    collision = list(csv.DictReader(io.StringIO(run("log", "export", log, "--protocol", new_protocol, "--format", "csv").decode())))
    assert collision[0]["status"] == "ok" and collision[0]["field.status"] == "47.0"
    assert collision[0]["unit.status"] == "°C"
    assert {p: hashlib.sha256(p.read_bytes()).digest() for p in root.glob("*.ssr")} == original

    # Every parsed byte remains attributable to stored raw data.
    stored = {}
    for part in info["segments"]:
        db = sqlite3.connect(f"file:{part['path']}?mode=ro", uri=True)
        stored.update(db.execute("select seq,bytes from events"))
        db.close()
    for frame in live:
        recovered = b"".join(stored[s["event"]][s["offset"]:s["offset"] + s["length"]] for s in frame["sources"])
        assert recovered == bytes.fromhex(frame["raw_hex"])
    test = frames(run("protocol", "test", PROTOCOL, "--hex", "372C32332E352C310D0A", "--format", "jsonl"))
    assert test[0]["fields"]["temperature"] == 23.5
    run("protocol", "test", PROTOCOL, "--hex", "00", expected=8)
    shared = root / "shared.json"
    run("protocol", "export", PROTOCOL, shared)
    assert json.loads(shared.read_text()) == json.loads(PROTOCOL.read_text())
    bad = root / "bad.json"
    bad.write_text('{"schema_version":1,"framing":{"mode":"fixed","length":4,"typo":1}}')
    run("protocol", "validate", bad, expected=2)
    empty = root / "empty.ssr"
    db = sqlite3.connect(empty)
    db.executescript("create table metadata(key text primary key,value text); create table events(seq integer primary key,wall_ns integer,mono_ns integer,kind text,direction text,bytes blob,detail text);")
    db.executemany("insert into metadata values(?,?)", [(key, json.dumps(value)) for key, value in {"schema_version": 1, "session": {}, "segment": 0, "clean_shutdown": True, "persisted_seq": 1}.items()])
    db.execute("insert into events values(1,0,0,'data','rx',?, '{}')", (b"",))
    db.commit()
    db.close()
    assert run("log", "export", empty, "--format", "bin", "--direction", "rx") == b""
    empty_info = json.loads(run("log", "info", empty, "--format", "jsonl"))
    assert all(empty_info[key] == 0 for key in ("raw_bytes", "event_payload_bytes", "data_bytes", "tx_data_bytes", "rx_data_bytes"))
    # Damaged external records fail with the record error code, not a config error.
    db = sqlite3.connect(log)
    db.execute("update metadata set value='not json' where key='session'")
    db.commit()
    db.close()
    run("log", "info", log, expected=7)
print("CLI offline tests passed")
