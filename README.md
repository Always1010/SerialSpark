# SerialSpark

SerialSpark 是面向 Linux 的串口收发、原始数据记录与离线协议分析命令行工具。它可以列出串口和可见占用、复用命名串口配置、发送或交互收发，并把 TX/RX 原始事件保存为可重解析的 `.ssr` 记录。实时协议解析是可选的；离线解析可以更换协议配置，原记录保持不变。

## 构建

目标环境为 Ubuntu 24.04，使用 GCC 13、CMake 3.28。项目是 C++20；CMake 按当前主机架构生成原生构建和 Debian 包（例如 aarch64 主机对应 Debian 的 arm64 架构）。安装构建依赖：

```sh
sudo apt install build-essential cmake pkg-config libsqlite3-dev libudev-dev nlohmann-json3-dev libcli11-dev python3 dpkg-dev file binutils
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

运行 `build/serialspark --help` 查看命令，`build/serialspark <命令> --help` 查看参数。需要运行 CTest 时，配置中默认启用测试。

生成原生 Debian 安装包：

```sh
cpack --config build/CPackConfig.cmake -B build/packages
```

生成文件名为 `serialspark_0.1.0_<Debian架构>.deb`。例如 arm64 主机可安装 `sudo apt install ./build/packages/serialspark_0.1.0_arm64.deb`；其他主机选择实际生成的架构文件。包安装命令为 `serialspark`，文档与样例位于 `/usr/share/doc/serialspark/`。源码目录的构建可以直接运行，无需安装。

CPack 在生成 Debian 包后自动检查架构、版本、动态库对应的 `Depends`、RPATH/RUNPATH，以及包内样例和在线/离线 CLI。缺少打包工具或依赖元数据时会报错并停止交付。解包检查使用当前主机运行库；干净环境安装验收入口见 [测试说明](docs/testing.md)。

## 快速上手

不熟悉参数时，可以在交互终端进入任务菜单，按步骤选择串口、填写发送内容或开始采集：

```sh
build/serialspark menu
```

菜单支持查看串口、单次发送、交互收发、接收记录，已有记录的摘要、查看、重解析和导出，以及串口配置管理。回车接受可见默认值；`:back` 返回上一步，`:cancel` 返回主菜单，`:clear` 清空可选项。输入字面量冒号命令时可加一个冒号，例如 `::cancel` 表示文本 `:cancel`。执行前展示摘要，可以返回修改或查看可复制的等价命令；执行结束后可以返回菜单或复用填写内容。菜单不会猜测设备串口参数，默认值需按设备要求确认。离线引导支持方向、文本／十六进制内容、带时区的可读时间范围，以及选择字段、比较方式和值；CSV 要求协议，BIN 要求单一方向。菜单需要标准输入与标准输出均为终端；管道原始字节发送继续使用 `send --stdin`。

先检查系统可见串口：

```sh
build/serialspark ports
```

保存常用配置（保存不打开设备），然后查看、列出、导出或删除配置：

```sh
build/serialspark profile save bench --port /dev/ttyUSB0 --baud 115200 --data-bits 8 --parity none --stop-bits 1 --flow none
build/serialspark profile list
build/serialspark profile show bench
build/serialspark profile export bench bench.json
build/serialspark profile import lab bench.json
build/serialspark profile delete lab
```

保存或导入同名配置可加 `--replace`。配置存放在 `$XDG_CONFIG_HOME/serialspark/profiles/`；未设置 `XDG_CONFIG_HOME` 时为 `$HOME/.config/serialspark/profiles/`。`--profile` 引用保存的名称；外部 JSON 文件先通过 `profile import` 导入。命令行显式参数会覆盖配置值。

通信配置根对象必须包含整数 `schema_version: 1` 和 `config` 对象。浮点数（包括 `1.0`）、布尔值、字符串和其他版本均拒绝；导入失败不会创建配置或覆盖已有配置。

用一次发送、交互会话或只接收采集：

```sh
build/serialspark send --profile bench --text 'PING' --wait-reply
printf 'raw bytes' | build/serialspark send --profile bench --stdin
build/serialspark session --profile bench --record run.ssr
build/serialspark capture --profile bench --record run.ssr --duration 30s
```

`send` 支持 `--text`、`--hex`、`--file` 和 `--stdin`，每次只能选一种。管道和文件按原始字节发送；文本不会解释反斜杠转义，换行需显式指定 `--newline lf`、`cr` 或 `crlf`，默认不追加。成功写入表示驱动接受了数据；需要确认设备回复时使用 `--wait-reply` 或 `--reply-hex`。`--drain` 只等待驱动输出队列变空，不能证明对端已收到。

交互模式中，以换行结束的普通输入行作为文本发送；命令包括 `:send text ...`、`:send hex ...`、`:send file ...`、`:display hex|text|both`、`:status`、`:disconnect`、`:connect` 和 `:quit`。输入结束时未换行的内容会明确报错且不发送，未完成发送会被取消；脚本原始字节输入使用 `send --stdin`。冒号开头的普通文本可用 `:send text ...` 发送。当前交互会话不支持实时加载/切换协议配置。

默认写入无进展超时为 2 秒，等待回复超时为 1 秒；时长默认 `0`，即不限制。显示默认 `both`；`human` 适合终端，`jsonl` 输出逐行 JSON，`json` 是同一 JSONL 格式的别名。

终端中的 `send_end` 展示发送状态、驱动接受的字节数及已读取的发送源字节数；`request_result` 分别展示发送、排空和回复结果。协议帧另有 `direction` 与 `status`，发送完成和解析成功应分别判断：

| 场景 | 判定 |
| --- | --- |
| `send_end` 为 `complete`，随后回复超时，退出 6 | 发送已被驱动接受，等待回复失败 |
| TX `data` 已出现，RX `frame` 为 `error` | 数据已收发，回复解析失败；查看帧的 `errors` |
| `request_result.reply_matched` 为 `true` | 满足所选等待条件；未配置协议时是首个 RX 数据，配置协议时是首个有效 RX 帧，使用 `--reply-hex` 时是字节串匹配 |

上述等待条件只证明对应的软件观察结果。若设备要求确认命令编号或应答码，还需检查回复内容。

## 配置协议与重解析

协议配置是声明式 JSON，版本为 1，可定义分帧、字段、校验和方向覆盖；不执行脚本。先验证，再用样例试解析：

```sh
build/serialspark protocol validate examples/lines.json
build/serialspark protocol test examples/lines.json --hex 372C32332E352C310D0A
build/serialspark protocol test examples/lines.json --file payload.bin --direction rx
build/serialspark protocol import examples/lines.json shared.json
build/serialspark protocol export shared.json copy.json
```

`protocol test` 的协议文件是位置参数；输入必须且只能选 `--hex` 或 `--file`。文件输入不含采集时间，因此不会模拟帧间超时。分帧模式、字段类型、校验算法、来源追溯等配置细节见 [协议配置说明](docs/protocol.md)，并可参考 `examples/sensor.json` 和 `examples/lines.json`。

将协议直接用于实时会话或采集可加 `--protocol examples/lines.json`。记录后可检查摘要、查看原始事件、离线解析和导出：

```sh
build/serialspark log info run.ssr
build/serialspark log view run.ssr --direction rx
build/serialspark log parse run.ssr --protocol examples/lines.json --direction rx --where 'temperature > 24'
build/serialspark log export run.ssr --protocol examples/lines.json --format csv --output parsed.csv
build/serialspark log export run.ssr --direction rx --format bin --output rx.bin
```

双向收发可使用 `examples/lines-duplex.json`：TX 只按 CRLF 组帧，不解析传感器字段；RX 解析 `7,23.5,1\r\n` 一类回复。两个方向共用 CRLF 边界，发送文本需显式追加换行。设备须支持对应命令和回复格式：

```sh
build/serialspark send --profile bench --text PING --newline crlf --protocol examples/lines-duplex.json --wait-reply --record run.ssr
build/serialspark log parse run.ssr --protocol examples/lines-duplex.json --direction rx
```

离线解析会先按完整字节流组帧，再应用消息和字段过滤。`--from-ns`、`--to-ns` 是包含边界的 Unix 墙上时间纳秒范围，时间取帧最后一个原始字节所在事件的采集时间。`--where` 支持中文字段名；含比较符或首尾空白的字段名用 JSON 双引号，例如 `'"温 度>" >= 30'`。比较值使用 JSON 数字、字符串或布尔值，字符串和布尔值仅支持 `==`、`!=`；64 位整数按精确十进制字符串比较。`--contains-text` 按 UTF-8 原始字节搜索。原始 `log view` 的内容过滤作用于单个事件，解析过滤作用于完整帧。

CSV 包含配置、来源范围、错误和单位等解析上下文；字段列采用 `field.<名称>`、单位列采用 `unit.<名称>`，避免设备字段与 `status` 等记录元数据同名。BIN 只允许导出单一方向的原始字节，会丢失事件时间；不能与协议解析同时使用。JSONL 用于保留事件结构。`.ssr` 是 SQLite 数据库，活动记录处于 WAL 模式；`.ssr` 与其 `-wal`、`-shm` 文件应作为一组保留。只有干净关闭后的记录及分段适合单独分享或复制。

记录默认按 1 GiB 阈值轮转为 `.ssr.partN.ssr` 分段，阈值在事务提交后检查，可能有少量超出；`--quota-bytes 0` 表示不限额，配额包含数据库及 WAL 大小，在达到阈值时停止采集，不会删除历史记录。分析从首段打开，分享时保留全部分段。`log info` 检查关闭状态、事件序号和持久化边界；`complete` 表示记录内部完整性，不证明物理线路上的每个字节均已被操作系统取得。

`log info` 在总计和每个分段中分别给出 `tx_data_bytes`（驱动接受的 TX）、`rx_data_bytes`（应用读取的 RX）及两者之和 `data_bytes`，只统计 `data` 事件。`event_payload_bytes` 统计所有事件携带的字节，包括 `send_intent` 的发送意图；兼容字段 `raw_bytes` 保留同一口径。例如 TX 6 字节、RX 18 字节、发送意图 6 字节时，`data_bytes` 为 24，`raw_bytes` 和 `event_payload_bytes` 均为 30。这些数字不代表物理线路传输量或记录文件大小。

内部记录队列上限 16 MiB、输出队列上限 4 MiB、最大单帧 64 KiB，SQLite 缓存上限 32 MiB；JSON 配置、单条事件细节和会话元数据分别限制为 1 MiB。记录按 100ms 或 256 KiB 批量提交，使用 WAL 与 FULL 同步；异常退出可能失去尚未提交的尾部，以 `persisted_seq` 为准。时间戳是应用观察时间，不是 UART 线上逐字节硬件时间；XON/XOFF 流控字符由驱动处理，不保证出现在应用原始记录中。无硬件背压或采集能力不足时，工具会报告故障与潜在缺口，不能保证外部持续输入永不溢出。

退出码：0 成功；2 参数或配置错误；3 串口连接错误；4 I/O 或输出错误；5 部分发送；6 超时；7 记录错误；8 协议试解析存在帧错误；130 用户取消。具体失败原因写到标准错误。

脚本应保存标准错误并立即读取退出码，需要事件细节时选择 JSONL；实时会话的协议帧错误还应检查 `frame.status` 和 `frame.errors`，不能仅根据进程退出码判断解析成功。例如：

```sh
build/serialspark send --profile bench --text PING --wait-reply --format jsonl >send.jsonl 2>send.stderr
rc=$?
cat send.stderr >&2
echo "exit=$rc"
```

## 当前边界

自动化测试使用 Linux PTY 覆盖命令行收发、记录和离线重解析路径。PTY 测试验证软件行为，不验证真实电气连接、USB 转串口芯片或具体设备。当前没有真实 UART 硬件验收结果，也没有自动重连或丢包重传功能。

后续阶段增加常用设备指令、自动发送、完整 Modbus RTU、已有上位机通信观察，再扩展重放与设备模拟。当前支持 Modbus 使用的 CRC 算法，尚未实现完整 Modbus RTU 协议。

通信观察计划先提供 PTY 代理：已有上位机改为连接工具创建的虚拟串口，工具转发到由其控制的真实串口并记录双向字节。该方式要求上位机允许更换端口，专有 ioctl、控制线和精确物理时序需单独评估。第二次打开同一真实串口无法可靠旁路观察；不能调整上位机时需要额外采集硬件，并分别确定 TTL、RS-232、RS-485 的接线和方向识别条件。首版尚未提供观察命令。

架构见 [模块说明](docs/architecture.md)，测试范围及真实设备验收见 [测试说明](docs/testing.md)。
