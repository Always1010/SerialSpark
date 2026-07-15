"""Verify menu cancellation, byte accuracy and terminal handoff using Linux PTYs."""
import errno
import json
import os
import pathlib
import pty
import select
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

EXE = str(pathlib.Path(sys.argv[1]).resolve())


class Menu:
    def __init__(self, env):
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen([EXE, "menu"], stdin=slave, stdout=slave,
                                     stderr=slave, env=env, close_fds=True)
        os.close(slave)
        self.buffer = b""
        self.transcript = b""

    def expect(self, text):
        target = text.encode()
        deadline = time.monotonic() + 5
        while target not in self.buffer:
            assert time.monotonic() < deadline, (text, self.transcript.decode(errors="replace"))
            if select.select([self.master], [], [], 0.05)[0]:
                try:
                    chunk = os.read(self.master, 65536)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    chunk = b""
                assert chunk, (self.proc.poll(), text, self.transcript.decode(errors="replace"))
                self.buffer += chunk
                self.transcript += chunk
        before, self.buffer = self.buffer.split(target, 1)
        return before

    def answer(self, prompt, value=""):
        self.expect(prompt)
        self.expect("> ")
        os.write(self.master, value.encode() + b"\n")

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait(timeout=3)
        os.close(self.master)


def receive(master, length):
    data = b""
    deadline = time.monotonic() + 3
    while len(data) < length and time.monotonic() < deadline:
        if select.select([master], [], [], 0.05)[0]:
            try:
                data += os.read(master, length - len(data))
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                time.sleep(0.01)
    return data


def connection(menu, port):
    menu.answer("如何设置连接", "1")
    menu.answer("选择串口（", "1")
    menu.answer("串口路径，例如", port)
    menu.answer("波特率（")
    menu.answer("是否修改", "1")


def live_tail(menu, record="", protocol=""):
    menu.answer("协议 JSON 路径", protocol)
    menu.answer("回复条件", "1")
    menu.answer("原始记录路径", record)
    menu.answer("数据展示", "1")
    menu.answer("最长运行时长", "0")
    menu.answer("是否调整高级设置", "1")


with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    env = dict(os.environ, XDG_CONFIG_HOME=str(root / "config"))
    rejected = subprocess.run([EXE, "menu"], input=b"0\n", capture_output=True, timeout=3, env=env)
    assert rejected.returncode == 2 and "交互终端" in rejected.stderr.decode()
    master, slave = pty.openpty()
    port = os.ttyname(slave)
    os.close(slave)
    menu = Menu(env)
    try:
        # Cancellation before execution must not connect, send or create a record.
        menu.answer("任务菜单", "2")
        connection(menu, port)
        menu.answer("发送方式", "2")
        menu.answer("发送十六进制", "GG")
        menu.expect("输入有误")
        menu.answer("发送十六进制", "00 FF 41")
        cancelled = root / "cancelled.ssr"
        live_tail(menu, str(cancelled))
        menu.answer("请选择下一步", "4")
        menu.expect("已取消")
        assert not cancelled.exists()
        assert receive(master, 1) == b""

        # Back navigation changes the source; editing keeps previous fields.
        menu.answer("任务菜单", "2")
        connection(menu, port)
        menu.answer("发送方式", "2")
        menu.answer("发送十六进制", ":back")
        menu.answer("发送方式", "1")
        menu.answer("发送文本", "old")
        menu.answer("文本末尾追加的换行", "4")
        log = root / "send.ssr"
        live_tail(menu, str(log))
        menu.answer("请选择下一步", "2")
        menu.answer("选择要修改的步骤", "7")
        marker = root / "must-not-exist"
        payload = f"PING ' $(touch {marker}) `echo x`"
        menu.answer("发送文本", payload)
        menu.answer("文本末尾追加的换行")
        live_tail(menu, str(log))
        menu.answer("请选择下一步", "3")
        preview = menu.expect("请选择下一步")
        assert b"serialspark 'send'" in preview and b"'\\''" in preview
        menu.expect("> ")
        os.write(menu.master, b"1\n")
        expected = payload.encode() + b"\r\n"
        assert receive(master, len(expected)) == expected
        menu.expect("操作结束，退出码：0")
        assert not marker.exists()
        db = sqlite3.connect(log)
        assert b"".join(row[0] for row in db.execute("select bytes from events where kind='data' and direction='tx' order by seq")) == expected
        assert json.loads(db.execute("select value from metadata where key='clean_shutdown'").fetchone()[0])
        db.close()
        menu.answer("接下来", "1")

        # Menu stdin must be available to the session, and return after :quit.
        menu.answer("任务菜单", "3")
        connection(menu, port)
        menu.answer("每次发送文本末尾追加的换行", "2")
        menu.answer("协议 JSON 路径")
        menu.answer("原始记录路径")
        menu.answer("数据展示", "1")
        menu.answer("最长运行时长", "0")
        menu.answer("是否调整高级设置", "1")
        menu.answer("请选择下一步", "1")
        menu.expect("session_help")
        os.write(menu.master, b"HELLO\n")
        assert receive(master, 6) == b"HELLO\n"
        os.write(menu.master, b":quit\n")
        menu.expect("操作结束，退出码：0")
        menu.answer("接下来", "1")

        # Capture closes its record on interruption and returns to the menu.
        menu.answer("任务菜单", "4")
        connection(menu, port)
        menu.answer("协议 JSON 路径")
        capture = root / "capture.ssr"
        menu.answer("保存原始记录到新的", str(capture))
        menu.answer("数据展示", "1")
        menu.answer("最长运行时长", "0")
        menu.answer("是否调整高级设置", "1")
        menu.answer("请选择下一步", "1")
        menu.expect("connect")
        os.write(master, b"CAPTURE")
        menu.expect("43415054555245")
        menu.proc.send_signal(signal.SIGINT)
        menu.expect("操作结束，退出码：130")
        menu.answer("接下来", "1")
        info = subprocess.run([EXE, "log", "info", str(capture), "--format", "jsonl"], capture_output=True, timeout=3)
        assert info.returncode == 0 and json.loads(info.stdout)["rx_data_bytes"] == 7
        assert json.loads(info.stdout)["complete"]
        menu.answer("任务菜单", "0")
        assert menu.proc.wait(timeout=3) == 0
    finally:
        menu.close()
        os.close(master)
