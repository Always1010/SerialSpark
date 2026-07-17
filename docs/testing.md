# 测试与设备验收

## 自动化测试

CMake 默认启用 CTest，测试覆盖核心配置与字节处理、PTY 串口行为、SQLite 记录轮转/配额、协议组帧与字段解析，以及在线/离线 CLI 工作流。菜单测试使用独立终端 PTY 和串口 PTY，覆盖取消时不产生副作用、返回修改、发送字节准确性、会话输入交接、采集中断收尾、离线导出和配置管理。配置并构建后可运行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

日常修改完成构建后，仅运行相关测试。例如协议改动：

```sh
ctest --test-dir build --output-on-failure -R '^protocol$'
```

菜单与 CLI 改动可运行相关检查：

```sh
ctest --test-dir build --output-on-failure -R '^(menu|cli_live|cli_offline)$'
```

`serial_test` 和 CLI 测试通过伪终端（PTY）验证软件收发；离线测试还比较实时解析与重解析结果、过滤、CSV/BIN 导出、来源字节重建和记录不变性。PTY 不提供电气层验证。以上是仓库提供的测试范围说明，不代表某一构建在当前机器已执行通过；发布前应记录实际执行结果。

## Debian 包与干净环境验收

原生打包需要 `dpkg-dev`、`file`、`binutils` 和 Python 3。CPack 保留自动依赖分析，并调用 `scripts/check_deb.py` 检查产物。检查包括包名、原生架构、版本、ELF 运行库与 `Depends` 对应关系、RPATH/RUNPATH、包内协议样例及在线/离线 CLI。子进程移除 `LD_LIBRARY_PATH`、`LD_PRELOAD` 和 `LD_AUDIT`；检查只解包到临时目录，不安装到主机。已有包也可单独检查：

```sh
python3 scripts/check_deb.py build/packages/serialspark_0.1.0_arm64.deb
```

替换文件名中的架构为实际原生架构。检查失败时包不得交付；重复打包目录中的旧包不会因本次检查失败而自动删除，应确认交付文件来自成功的打包命令。

使用已安装的 Docker 可在 Ubuntu 24.04 中完成独立构建和干净环境安装验收：

```sh
docker build -f docker/release.Dockerfile -t serialspark-release-check .
```

构建阶段执行 Release 构建、CTest 和带产物检查的 CPack；独立验收阶段只通过包管理器安装 `.deb` 与测试所需的 Python，运行包内版本、协议及 PTY 在线/离线检查，不继承构建阶段的开发依赖。这会下载容器镜像和容器内软件包，但不安装本机依赖。该入口的存在不代表容器验收已执行通过；Docker 不可用时，应在干净目标机器执行对应安装验收并记录结果。

## 真实设备验收清单

在真实目标串口、线缆和设备上逐项记录设备型号、转换器芯片、线缆、电平类型、接线、驱动版本、端口、全部串口参数、预期和结果：

- `ports` 能发现目标节点；运行用户具有访问权限，连接失败时保存完整标准错误。
- 双端波特率、数据位、校验位、停止位和流控匹配；验证驱动实际回读值与目标要求一致。
- 收发已知文本和二进制样本，检查字节顺序、换行策略、完整写入及设备实际回复。
- 在目标设备的真实分包节奏下验证交互收发、回复匹配、协议分帧、超时和断连处理。
- 使用代表性运行时长与数据速率检查 `.ssr` 分段、磁盘配额、干净关闭、完整性信息和离线重解析。
- 对 RTS/CTS 或 XON/XOFF 单独验证流控效果；确认应用日志中的字节是否受驱动处理影响。
- 验证设备断电、拔插和权限变化后的行为。当前版本没有自动重连或重传，需确认调用方能识别错误并安全恢复。

避免把 PTY 结果当成 UART 线上时序或电气特性的证明。当前版本没有真实 UART 硬件验收结果；任何硬件组合的结论都应基于对应设备实测。
