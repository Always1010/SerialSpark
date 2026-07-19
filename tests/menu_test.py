"""Verify guided terminal workflows and their side effects using Linux PTYs."""
import datetime
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
    def __init__(self, env, cwd=None):
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen([EXE, "menu"], stdin=slave, stdout=slave,
                                     stderr=slave, env=env, cwd=cwd, close_fds=True)
        os.close(slave)
        self.buffer = b""
        self.transcript = b""

    def expect(self, text):
        target = text.encode()
        deadline = time.monotonic() + 5
        while target not in self.buffer:
            assert time.monotonic() < deadline, (text, self.transcript[-4000:].decode(errors="replace"))
            if select.select([self.master], [], [], 0.05)[0]:
                try:
                    chunk = os.read(self.master, 65536)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    chunk = b""
                assert chunk, (self.proc.poll(), text, self.transcript[-4000:].decode(errors="replace"))
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
    menu = Menu(env, root)
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
        capture_payload = b"7,23.5,1\r\n8,25.5,0\r\n"
        os.write(master, capture_payload)
        menu.expect("372C32332E352C310D0A")
        menu.proc.send_signal(signal.SIGINT)
        menu.expect("操作结束，退出码：130")
        menu.answer("接下来", "1")
        info = subprocess.run([EXE, "log", "info", str(capture), "--format", "jsonl"], capture_output=True, timeout=3)
        assert info.returncode == 0 and json.loads(info.stdout)["rx_data_bytes"] == len(capture_payload)
        assert json.loads(info.stdout)["complete"]
        # Export limits are guided: BIN has one direction and no protocol prompt.
        menu.answer("任务菜单", "5")
        menu.answer("如何处理已有记录", "4")
        menu.answer("记录首段", str(log))
        menu.answer("导出格式", "3")
        menu.answer("数据方向", "2")
        menu.answer("是否按原始字节内容筛选", "1")
        menu.answer("是否限制时间范围", "1")
        menu.answer("导出到新的文件路径", str(log))
        menu.expect("目标已存在")
        binary = root / "tx.bin"
        menu.answer("导出到新的文件路径", str(binary))
        menu.answer("请选择下一步", "1")
        menu.expect("操作结束，退出码：0")
        assert binary.read_bytes() == expected
        menu.answer("接下来", "1")

        # CSV parsing, explicit timezone conversion and typed field validation.
        original_record = capture.read_bytes()
        menu.answer("任务菜单", "5")
        menu.answer("如何处理已有记录", "4")
        menu.answer("记录首段", str(capture))
        menu.answer("导出格式", "2")
        protocol = pathlib.Path(__file__).resolve().parents[1] / "examples" / "lines.json"
        menu.answer("用于解析的协议 JSON 路径", str(protocol))
        menu.answer("数据方向", "2")
        menu.answer("是否按原始字节内容筛选", "1")
        menu.answer("是否限制时间范围", "2")
        menu.answer("开始时间，", "invalid")
        menu.expect("输入有误")
        epoch = datetime.datetime.fromtimestamp(0, datetime.timezone(datetime.timedelta(hours=8))).isoformat()
        menu.answer("开始时间，", epoch)
        menu.answer("结束时间，")
        menu.answer("是否按解析字段筛选", "2")
        menu.answer("选择解析字段", "3")
        menu.answer("比较方式", "3")
        menu.answer("比较值：", '"bad"')
        menu.expect("输入有误")
        menu.answer("比较值：", "24")
        csv = root / "parsed.csv"
        menu.answer("导出到新的文件路径", str(csv))
        menu.answer("请选择下一步", "3")
        preview = menu.expect("请选择下一步")
        assert b"'--from-ns' '0'" in preview and b"'--where'" in preview
        menu.expect("> ")
        os.write(menu.master, b"1\n")
        menu.expect("操作结束，退出码：0")
        import csv as csv_module
        with csv.open() as stream:
            rows = list(csv_module.DictReader(stream))
        assert len(rows) == 1 and rows[0]["field.temperature"] == "25.5"
        assert capture.read_bytes() == original_record
        menu.answer("接下来", "1")
        # Profile writes/deletes require execution; names starting with '-' are positional.
        menu.answer("任务菜单", "6")
        menu.answer("管理常用串口配置", "3")
        connection(menu, port)
        menu.answer("保存后的配置名称", "../bad")
        menu.expect("输入有误")
        menu.answer("保存后的配置名称", "--help")
        menu.answer("请选择下一步", "1")
        menu.expect("操作结束，退出码：0")
        stored = root / "config" / "serialspark" / "profiles" / "--help.json"
        assert json.loads(stored.read_text())["config"]["baud"] == 115200
        menu.answer("接下来", "1")

        # Reuse a stored configuration with an explicit baud override.
        menu.answer("任务菜单", "2")
        menu.answer("如何设置连接", "2")
        menu.answer("选择已保存的串口配置", "1")
        menu.answer("波特率（", "9600")
        menu.answer("是否修改", "1")
        menu.answer("发送方式", "2")
        menu.answer("发送十六进制", "00 FF")
        live_tail(menu)
        menu.answer("请选择下一步", "1")
        assert receive(master, 2) == b"\x00\xff"
        menu.expect("操作结束，退出码：0")
        assert json.loads(stored.read_text())["config"]["baud"] == 115200
        menu.answer("接下来", "1")

        menu.answer("任务菜单", "6")
        menu.answer("管理常用串口配置", "5")
        menu.answer("选择已保存的串口配置", "1")
        exported = root / "profile.json"
        menu.answer("配置导出到新的", str(exported))
        menu.answer("请选择下一步", "1")
        menu.expect("操作结束，退出码：0")
        assert exported.read_bytes() == stored.read_bytes()
        menu.answer("接下来", "1")

        menu.answer("任务菜单", "6")
        menu.answer("管理常用串口配置", "6")
        menu.answer("选择已保存的串口配置", "1")
        menu.answer("请选择下一步", "4")
        menu.expect("已取消")
        assert stored.exists()
        menu.answer("任务菜单", "6")
        menu.answer("管理常用串口配置", "6")
        menu.answer("选择已保存的串口配置", "1")
        menu.answer("请选择下一步", "1")
        menu.expect("操作结束，退出码：0")
        assert not stored.exists()
        menu.answer("接下来", "1")
        menu.answer("任务菜单", "6")
        menu.answer("管理常用串口配置", "4")
        menu.answer("要导入的配置 JSON 路径", str(exported))
        menu.answer("保存后的配置名称", "imported")
        menu.answer("请选择下一步", "1")
        menu.expect("操作结束，退出码：0")
        imported = stored.with_name("imported.json")
        assert json.loads(imported.read_text()) == json.loads(exported.read_text())
        menu.answer("接下来", "1")
        # Protocol operations remain offline and share the existing parser.
        menu.answer("任务菜单", "7")
        menu.answer("处理已有协议配置", "2")
        menu.answer("协议 JSON 路径", str(protocol))
        menu.answer("试解析的数据来源", "1")
        menu.answer("用于试解析的十六进制字节", capture_payload.hex())
        menu.answer("试解析方向", "1")
        menu.answer("请选择下一步", "1")
        frames = menu.expect("操作结束，退出码：0")
        assert b"temperature" in frames and b"23.5" in frames and b"25.5" in frames
        menu.answer("接下来", "1")

        menu.answer("任务菜单", "7")
        menu.answer("处理已有协议配置", "4")
        menu.answer("协议 JSON 路径", str(protocol))
        menu.answer("保存协议到新的", str(exported))
        menu.expect("目标已存在")
        copied = root / "protocol.json"
        menu.answer("保存协议到新的", str(copied))
        menu.answer("请选择下一步", "1")
        menu.expect("操作结束，退出码：0")
        assert json.loads(copied.read_text()) == json.loads(protocol.read_text())
        menu.answer("接下来", "1")
        # Empty text is valid with a newline; zero-byte sends are rejected in the form.
        menu.answer("任务菜单", "2")
        menu.answer("如何设置连接", "2")
        menu.answer("选择已保存的串口配置", "1")
        menu.answer("波特率（")
        menu.answer("是否修改", "1")
        menu.answer("发送方式", "1")
        menu.answer("发送文本")
        menu.answer("文本末尾追加的换行", "1")
        menu.expect("发送内容为空")
        menu.answer("文本末尾追加的换行", "4")
        live_tail(menu)
        menu.answer("请选择下一步", "1")
        assert receive(master, 2) == b"\r\n"
        menu.expect("操作结束，退出码：0")
        menu.answer("接下来", "1")
        # Relative filenames beginning with '-' must remain file operands.
        unusual_record = root / "-capture.ssr"
        unusual_record.write_bytes(original_record)
        menu.answer("任务菜单", "5")
        menu.answer("如何处理已有记录", "1")
        menu.answer("记录首段", unusual_record.name)
        menu.answer("请选择下一步", "1")
        menu.expect("操作结束，退出码：0")
        menu.answer("接下来", "1")
        menu.answer("任务菜单", "0")
        assert menu.proc.wait(timeout=3) == 0
    finally:
        menu.close()
        os.close(master)
