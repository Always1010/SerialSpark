# 协议配置与帧输出

`schema_version` 必须为整数 `1`。根对象可配置 `framing`、`fields`、`checksum`、`emit_raw_fields`、`split_separator`，以及 `tx`、`rx` 两个方向对象。方向对象使用相同配置项，逐项替换共享配置；`framing` 和 `checksum` 对象整体替换，字段数组整体替换。每个方向最终必须具有有效 `framing`。未知选项、错误类型、整数溢出和不适用的选项均在读取配置时拒绝；不支持脚本执行。

[传感器配置](../examples/sensor.json) 展示带帧头和 CRC 的二进制数据；[行文本配置](../examples/lines.json) 解析 `7,23.5,1\r\n`。

[双向行文本配置](../examples/lines-duplex.json) 为 TX、RX 共用 CRLF 分帧，并分别配置字段：TX 的空字段数组允许 `PING\r\n` 一类命令，RX 解析传感器字段。共享字段会同时用于两个方向；命令和回复结构不同时应使用方向对象，避免把 TX 命令套入 RX 的字段规则。该样例仍拒绝不符合字段规则的 RX。

## 分帧

每个 `framing` 对象都可使用 `min_frame_bytes`（默认 1）、`max_frame_bytes`（默认 65536）和 `assembly_timeout_ms`（默认 0，关闭超时）。最小值和最大值均为包含帧头、分隔符和校验的线上字节数，范围为 1～65536。最大值限制每个方向的缓存；连续输入也不能积累无界残留。

| `mode` | 必要配置 | 行为 |
| --- | --- | --- |
| `newline` | 可选 `newline`: `LF`、`CRLF`、`CR`，默认 `LF` | 保留分隔符于原始数据，允许跨事件拆分 CRLF |
| `delimiter` | `delimiter_hex`，非空十六进制字节串 | 精确匹配多字节分隔符 |
| `fixed` | `length` | 按固定总长度切帧 |
| `header_fixed` | `header_hex`、`length` | 查找帧头，再读取固定总长度 |
| `header_length` | `header_hex`、`length_field` | 根据帧头内长度字段计算总长度 |

`newline`、`delimiter` 可设 `include_delimiter`（默认 false）。false 时字段与校验作用于去掉末尾分隔符的内容；true 时作用于完整线上帧。输出 `raw_hex` 和 `sources` 始终包含分隔符。达到最大长度而未匹配分隔符会输出 `error`，然后重新组帧。

`length_field` 支持 `offset`（默认 0）、`type`（`u8`、`u16`、`u32`）、`endian`、`unit_bytes`（默认 1，范围 1～65536）、`adjustment`（默认 0，范围 -65536～65536）。总长度为 `原值 × unit_bytes + adjustment`，包含帧头和长度字段本身。多字节数值必须显式声明 `endian: "little"` 或 `"big"`；单字节无需字节序。计算采用有界宽整数，不发生截断或溢出。

无效长度输出带 `invalid_length` 错误的首字节，然后重新搜索帧头。帧头外噪声逐字节输出 `noise`，使任意拆包方式的结果一致；所有噪声仍可追溯原始事件。完整帧、噪声及残留均保留原始字节。对于无帧头的固定长度模式，调用者应确保数据从帧边界开始。

## 字段

`fields` 为数组，每个方向最多 256 个字段，字段必须具有唯一非空 `name`、`type`。所有字段的原始字节长度预算最多 1 MiB，文本字段按 `max_frame_bytes` 计入预算，重叠字段也分别计入，避免解析结果无限展开。二进制字段通过 `offset` 定位；文本字段通过零起始 `index` 定位，两者不能混用。`split_separator` 默认为逗号，可配置任意非空字符串；拆分保留空字段，不进行 CSV 引号或反斜杠转义。

| `type` | 二进制配置 | 输出 |
| --- | --- | --- |
| `u8`、`i8`、`u16`、`i16`、`u32`、`i32` | `offset`，多字节需 `endian` | JSON 整数 |
| `u64`、`i64` | `offset`、`endian` | 精确十进制字符串，避免 JSON 使用者丢失精度 |
| `float32`、`float64` | `offset`、`endian` | 有限 JSON 数值；NaN、Infinity 为错误 |
| `bytes` | `offset`、正整数 `length` | 大写十六进制字符串 |
| `ascii`、`utf8` | `offset`、正整数 `length` | 经严格编码验证的字符串 |

文本 `index` 支持整数、浮点数、`ascii`、`utf8`；整数检查类型范围并拒绝负无符号数、溢出和尾随字符。数值必须完整匹配，不修剪空白，浮点数使用与地区设置无关的转换。文本 `u64`、`i64` 同样输出精确十进制字符串。字段超出实际帧长度、编码错误或转换失败均添加字段错误，其他字段继续解析。

数值字段可配置有限 `scale`（默认 1）、`bias`（默认 0）、字符串 `unit`，以及 `enum` 对象。枚举键是原值的十进制/JSON 表示，值必须为字符串；命中时输出标签，未命中时按 `原值 × scale + bias` 输出。64 位整数字段禁止浮点缩放，保留精度。`emit_raw_fields: true` 输出变换前的 `raw_fields` 对象；字段的单位单独输出到 `units`。字段错误始终保留整帧 `raw_hex`。

## 校验

`checksum.algorithm` 支持 `sum8`（累加低 8 位）、`xor8`、`crc16-modbus`（反射多项式 0xA001，初值 0xFFFF）、`crc16-ccitt-false`（多项式 0x1021，初值 0xFFFF，不反射）、`crc32`（IEEE 反射多项式 0xEDB88320，初值和最终异或均为 0xFFFFFFFF）。

`coverage: {"start": 0, "end_from_end": 2}` 指定半开覆盖区间 `[start, 帧内容长度 - end_from_end)`。`offset_from_end` 是校验结果首字节距帧内容末尾的距离，默认校验宽度；不能小于校验宽度。`coverage.start` 默认 0，`coverage.end_from_end` 默认 `offset_from_end`。多字节结果必须指定 `result_endian`，单字节无需指定。覆盖区间或结果位置超出实际帧范围时产生 `checksum_range` 错误；不匹配产生 `checksum`，并输出计算值和线上值。

## 事件与结果

离线和实时路径均把按时间顺序的原始事件传入同一 `Protocol` 实例。`data` 事件只能为 `tx` 或 `rx`；两个方向各自缓存，不混合。所有事件推进组帧超时：从该方向最后收到字节算起，在到达配置时限时输出 `incomplete` 和 `assembly_timeout`，清空旧残留。`disconnect` 完成并重置两个方向；之后新连接数据不会与旧残留拼接。`finish(reason)` 输出并清空残留，错误码携带调用者给出的原因。

每条输出具有 `schema_version`、`kind: "frame"`、`direction`、`status`（`ok`、`error`、`noise`、`incomplete`）、`raw_hex`、`fields`、`errors`、`sources`、`wall_ns`、`mono_ns`。时间来自该输出最后一个原始字节，不生成新的采集时间。`errors` 为包含 `code`、`message` 等上下文的数组。字段或校验错误将帧标记为 `error`，不会丢弃整帧。

`sources` 按线上顺序列出 `{event, offset, length}`，`event` 引用原始事件序号，`offset` 和 `length` 为该事件中的字节偏移与长度。相邻同事件连续范围合并；重建这些范围必须与 `raw_hex` 完全一致。改变串口读包大小会改变来源引用，但不改变帧内容、解码值、错误或噪声序列。
