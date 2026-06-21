"""Check native Ubuntu/Debian package metadata and exercise the packaged CLI."""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile


RUNTIME_PACKAGES = {
    "libc.so.6": "libc6",
    "libm.so.6": "libc6",
    "libpthread.so.0": "libc6",
    "libdl.so.2": "libc6",
    "librt.so.1": "libc6",
    "libgcc_s.so.1": "libgcc-s1",
    "libstdc++.so.6": "libstdc++6",
    "libsqlite3.so.0": "libsqlite3-0",
    "libudev.so.1": "libudev1",
}


def check_package(package):
    environment = dict(os.environ)
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT"):
        environment.pop(key, None)

    def run(*args):
        result = subprocess.run(list(map(str, args)), capture_output=True, text=True,
                                check=True, timeout=30, env=environment)
        return result.stdout.strip()

    def field(name):
        return run("dpkg-deb", "--field", package, name)

    if field("Package") != "serialspark":
        raise ValueError("包名称必须为 serialspark")
    if field("Architecture") != run("dpkg", "--print-architecture"):
        raise ValueError("包架构与当前原生验收环境不一致")
    version = field("Version")
    if not version:
        raise ValueError("包缺少 Version")
    dependencies = field("Depends")
    if not dependencies:
        raise ValueError("包缺少 Depends；请检查 dpkg-shlibdeps 和打包环境")
    declared = set(re.findall(r"(?:^|[,|])\s*([a-z0-9][a-z0-9+.-]*)(?::[a-z0-9-]+)?", dependencies))
    source = pathlib.Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="serialspark-deb-") as directory:
        root = pathlib.Path(directory)
        run("dpkg-deb", "--extract", package, root)
        executable = root / "usr/bin/serialspark"
        examples = root / "usr/share/doc/serialspark/examples"
        dynamic = run("readelf", "--dynamic", executable)
        if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
            raise ValueError("发布二进制仍包含 RPATH 或 RUNPATH")
        needed = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", dynamic)
        if not needed:
            raise ValueError("未找到运行库依赖；当前检查面向动态链接的原生包")
        for library in needed:
            if library.startswith("ld-linux-"):
                runtime_package = "libc6"
            else:
                runtime_package = RUNTIME_PACKAGES.get(library)
            if runtime_package is None:
                raise ValueError(f"未知运行库 {library}；请补充对应的发行版依赖映射")
            if runtime_package not in declared:
                raise ValueError(f"Depends 缺少 {runtime_package}（对应 {library}）")
        if run(executable, "--version") != f"SerialSpark {version}":
            raise ValueError("包版本与二进制版本不一致")
        for name in ("lines.json", "lines-duplex.json", "sensor.json"):
            run(executable, "protocol", "validate", examples / name, "--format", "jsonl")
        print(run(sys.executable, source / "tests/cli_live_test.py", executable, examples))
        print(run(sys.executable, source / "tests/cli_offline_test.py", executable, examples / "lines.json"))
    print(f"Debian 包检查通过：{package.name}；Depends: {dependencies}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=pathlib.Path)
    args = parser.parse_args()
    try:
        check_package(args.package.resolve())
    except subprocess.CalledProcessError as error:
        print(f"包检查命令失败：{error.cmd}\n{error.stdout}{error.stderr}", file=sys.stderr)
        return 1
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"包检查失败：{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
