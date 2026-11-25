# GXFP51A0 Linux 驱动移植计划（最小可用版）

最后更新：2026-11-24　状态：M1–M4 已完成并真机通过；M5 未通过（异指可匹配），
原因已定位到 MINDTCT 伪细节点，详见 [SUMMARY.md](SUMMARY.md)

---

## 1. 目标

只做到一件事：**让这台笔记本的指纹传感器能被 Linux 使用**。

```text
设备应答 → 建立加密会话 → 采到一张指纹图像 → 接入 libfprint → 录入并验证一根手指
```

明确不做：固件升级、图形界面、多机型支持、SGX 模拟、自建指纹数据库、自定义 PAM 模块、电源管理优化。

---

## 2. 已验证的硬件与协议事实

运行环境实测，非推断。

| 项目 | 值 | 来源 |
|---|---|---|
| 机型 | HUAWEI `WRTB-WXX9`，MateBook 13 | `/sys/class/dmi/id` |
| 传感器 | ACPI `GXFP51A0`，Goodix GF3658 / ST411 | ACPI、Windows INF |
| 固件 | `GF_ST411SEC_APP_14108` | A8 查询实测 |
| 芯片 ID | `0x2504` | 82 读取实测（**需先 A2 软复位**，否则返回 0） |
| 软复位返回码 | `0x010008` | A2 实测 |
| SPI | `/dev/spidev1.0`，mode 0 + `SPI_CS_HIGH`，1 MHz | 实测 |
| 复位脚 | GPIO264，HIGH 300 ms → LOW 600 ms | 实测 |
| 中断脚 | GPIO48，**电平高有效** | 实测 |
| ACK 窗口 | 100 ms；写前必须等该线回低 | 参考实现 + 实测 |
| OTP | 64 字节，6 个 CRC 字段全部通过 | 实测 |
| 标定值 | tcode 256，fdt_delta 31，DAC 0x0b78/0xb9/0xb7/0xb6 | 实测 |
| 配置上传 | `0x90` 回应状态 `0x01`（接受） | 实测 |

**社区参考机对比（固件 14115）**：芯片 ID 相同，tcode 相同，fdt_delta 33、DAC 0x0b68/0xb8/0xb6/0xb6 不同。差异来自逐片 OTP 标定，属正常。OTP 格式与本机一致。

### 参考实现

- 主实现：`GodsQuantum/Huawei-Matebook-13-Linux`（本地克隆在 `~/Downloads/gxfp51a0-linux`）
- 协议研究：`szlukabence/goodix-fingerprint-spi-linux`
- 局部副本：`~/Downloads/gxfp51a0-min`（本项目，只保留指纹最小集）

**注意**：参考实现在多处硬中止 `fw != "GF_ST411SEC_APP_14115"`。本项目不复制该限制，改为记录版本并按层验证。

---

## 3. 帧格式（已确认）

```text
outer: type(A0 plain / B0 见下) + len(LE16) + checksum(sum of first 3 bytes)
       ... 2 ms 间隔 ...
inner: cmd + (payload_len+1)(LE16) + payload + checksum(0xAA - sum)
```

已确认命令：

| 命令 | 内容 | 用途 |
|---|---|---|
| `00 05 00 00 00 00 00 A5` | NOP | 前置节奏 |
| `96 03 00 01 00 10` | DriverState:Install | 建立驱动状态 |
| `A8 03 00 00 00 FF` | GetEvkVersion | 固件版本 |
| `A2 03 00 01 14 F0` | soft reset | 返回 3 字节码 |
| `82 06 00 00 00 00 04 00 1E` | reg read addr 0 len 4 | 芯片 ID |
| `A6 03 00 00 00 01` | OTP read | 64 字节 |
| `70 03 00 14 00 23` | idle | 配置前 |
| `80 06 00 addr(2) value(2) cksum` | reg write | DAC |
| `90 01 01 + 256B config + cksum` | upload config | 回应状态必须 0x01 |
| `AE 02 00 55 A5` | MCU state | 只读诊断 |
| `F2 09 00 addr(4) len(4) cksum` | mem read | 供 PMK 读取，未启用 |

`0xF0` 是固件升级命令，**禁止实现**。

---

## 4. 进度

| 阶段 | 内容 | 状态 | 证据 |
|---|---|---|---|
| M1 | 复位 + SPI + 帧协议 + 版本查询 | **完成** | `GXFP51A0 firmware: GF_ST411SEC_APP_14108` |
| M2a | OTP 校准 + 配置上传 | **完成** | `config response status 0x01` |
| M2b | 读设备侧 PMK + TLS-PSK 握手 | **完成** | `tls: handshake ok: TLSv1.2 PSK-AES128-CBC-SHA256` |
| M3 | 单张指纹图像采集与解码 | **完成（单帧）** | `run-capture-155646.log`：背景 mean 2728 / 手指 1400 / 差分范围 1804 / 脊线周期 9 px。根因链＝①缺 background/NAV 配方 ②缺 `0xD4` 确认 ③缺 WakeupMCU ④F2 后未硬复位 ⑤`0x80` 写寄存器少前导 `0x00` |
| M4 | 接入 libfprint，设备可发现 | **完成** | `fprintd-list` 列出 `Goodix GXFP51A0 (GF3658 / ST411)`；`tools/build-libfprint.sh install` 可重复安装（库进 `/usr/local`，udev 规则进 `/etc`） |
| M3.5 | **新增：信息量闸门** | **未通过** | 判据＝同指 best-of-template 最小值 > 异指最大值，各 ≥20 帧。实测两者重叠，见 SUMMARY.md §4 |
| M5 | 录入 / 正例 / 反例 | **失败** | 同指可 match，**异指同样 match**；根因＝MINDTCT 检出的点全在填充边框上，真实区内为 0 |
| M6 | fprintd 与系统认证（最后） | 阻塞 | 等 M5 通过 |

M2a 的一个实测细节：`0x80` 第一次会被吞（`no ACK`），重放一次即通过。正常现象，不是故障。

但连续两次被吞会直接让 M2a 失败（2026-11-23 夜实测过一次），因此 `send_wait_ack` 已加固：

| 项 | 改前 | 改后 |
|---|---|---|
| ACK 等待 | 100 ms | **250 ms**（idle 转换后 MCU 应答明显变慢） |
| 重放次数 | 2 | **3** |
| 进入命令前 | 直接发 | **先排空 2 帧残留**（避免迟到的上一帧被当成当前 ACK） |
| `0x70` 之后 | 立即写 `0x80` | **等 30 ms** 再写寄存器 |

---

## 5. 后续阶段计划

### M2b：TLS-PSK 会话

从参考实现挖出的精确事实（`goodix51a0.c` / `goodix_tls.c` / `gx51_factory_pmk.c`）：

**PMK 来源**：设备不直接给出 PMK，靠 `F2` 读 staging
- selector `0x08004000`，请求长度 256
- MCU "拒绝"该读，但仍回吐一段开机残留记录，长度 248（=256-8）
- 记录首字节在传输中被破坏，需逐字节爆破
- 解密：`key = SHA256(salt[16] || seed[16] || 48 字节 0)[:16]`，AES-128-CBC，无 padding
- 明文校验：`00 0D` + BE32 = `0x30`（48 字节 PMK）
- `seed` = `5cba6e25819518de2d53e96dc0347ab0`（16 字节常量）
- 读完必须紧跟 `E4` 一致性检查，否则 staging 会被后续命令覆盖
- PMK 只驻内存；参考实现缓存到 `/var/lib/fprint`，本项目**不落盘**

**TLS 参数（2026-11-23 真机实测修正）**：
- 传感器是 **client**，主机是 **server**
- 套件 **`0x00ae` = `PSK-AES128-CBC-SHA256`**（CBC，不是 GCM！`0x00a8` 才是 GCM），TLS 1.2，仅此一种 + `00ff` SCSV
- 实测 ClientHello：52 字节，`16 03 01 00 2f`，**零扩展**，无 PSK identity hint
- 用 GCM 套件会让 OpenSSL 回 `no shared cipher` + `handshake_failure`（alert 0x28）
- PSK identity 字符串 `Client_identity`（在 ClientKeyExchange 里）
- 图像记录会超过 TLS 1.2 的 16 KiB 明文上限，需直接按 client_write_key/IV 解记录

**命令**：
- `D0 03 00 00 00 D7` REQTLS，期望 ACK `0xd0` 且状态位 0 置位
- 握手完成后发 NOP → `D4`，期望 ACK `0xd4`

**顺序**（与参考实现一致）：

```text
GPIO 复位
→ F2 staging 读（必须在配置之前）
→ E4 一致性检查
→ GPIO 复位
→ DriverState install → A8 版本
→ 82 / A6 / 70 / 4×80 / 90（同 M2a）
→ D0 REQTLS
→ TLS1.2 PSK-AES128-CBC-SHA256 握手（主机为 server）
→ NOP → D4 确认
```

**实现要点（`src/tls.c`）**：OpenSSL 3.2，只有 server 端；record 走自定义 BIO，一个 TLS record = 一个 `0xb0` 帧。BIO 必须实现成**流式**：OpenSSL 先要 5 字节记录头再要正文，一次塞整条 record 会报 `record of 52 bytes exceeds buffer 5`。`0xb0` 同时是 ACK 操作码，所以按“6 字节帧 + 首字节 `0xb0`”识别并丢弃 ACK。

要求：

- PMK 只在内存，不落盘、不打日志、不进配置文件
- 只接受传感器实际通告的 PSK-AES128-CBC-SHA256；GCM 只作后备，不主动要求
- 大记录（超过单帧上限）按既有分片规则处理
- 握手失败只重试一次，之后停下，不复位重刷
- 用 OpenSSL 的 PSK 回调，不自己实现 TLS

**验收**：握手完成，且握手后命令仍得到有效 ACK。

**失败分类**：

| 现象 | 判断 |
|---|---|
| `F2` 返回全 FF | PMK 读取地址/长度不对，查协议记录，不要试固件升级 |
| 握手 alert（bad record mac） | PSK 取错或身份提示串不对 |
| 握手超时在第一个 flight | SPI 分片或 IRQ 节奏问题 |

**M2b 实测（2026-11-23 夜，真机通过）**

`--configure --tls` 的完整帧序列，可作为回归基准：

```text
d0 03 00 00 00 d7  REQTLS            → b0 03 00 d0 01 26  (ack 0x01)
                                     → b0 frame, 52 B, record 16 03 01 00 2f  ClientHello
← tx 95 B ServerHello (record 0x16)
                                     → 26 B, record 0x16 (ClientKeyExchange, identity Client_identity)
                                     → 6 B, record 0x14 (CCS)
                                     → 85 B, record 0x16 (Finished)
← tx 91 B (record 0x14 CCS) + Finished
tls: handshake ok: TLSv1.2 PSK-AES128-CBC-SHA256
```

定位过程值得记一笔：`no shared cipher` 一开始被误判为 ctx 配置问题，
用本地 `s_server` 对拍、再把传感器那条 52 字节 hello 直接喂给同参数 server
ctx，才确认是**套件码记错**（0x00ae=CBC 而非 GCM）。协议事实以 `openssl
ciphers -V` 与设备实际通告为准，参考实现文档里的套件名不可信。

### M2c：明文采集旁路（在 M2b 之前验证）

目的：14108 比 14115 老，可能不强制 TLS 就回图。若成立，PSK 整块可砍。

**回复数量表（参考实现实证，必须遵守）**：

| 命令 | 明文回复数 | 说明 |
|---|---|---|
| `0x00` NOP | 0 | 发出后**不要等**，等了必超时 |
| `0x32` FDT-down | 2 | ACK + 20 字节 FDT/touch 报告；只读 1 条会让下一条命令被吞 |
| `0x36` FDT-manual | 2 | 同上 |
| `0x50` NAV | 2 |  |
| `0x80` 寄存器写 | 1 |  |
| `0x82` 寄存器读 | 2 |  |
| `0xae` MCU 状态 | 2 |  |
| `0x20` GET_IMAGE | ACK 可能 + TLS 记录 | ACK 窗口 1000 ms，无 ACK 且无 TLS 才可重放一次；已收到 TLS 记录不得重放 |

ACK 状态位：bit0 = 命令被接受，bit1 = pre-TLS 门标志。`0x01` 与 `0x03` 都是成功。

**2026-11-23 实测（14108）**：`0x36`、`0x32`、`0xae` 都有 `status 0x01` 的 ACK，说明明文命令通道正常、pre-TLS 门标志已清（bit1=0）。但旧探针每条命令只读 1 帧，`0x32` 的第二帧残留把 IRQ 线拉高，`GET_IMAGE` 在写入前就被 `wait_idle` 静默放弃 —— **`20 03 00 01 00 86` 之后没有任何输出，图上没有发出**。

已修：`gx51_read_frame_to()`（可配 IRQ 超时）、`drain_cmd()` 按上表排空、NOP 不等、GET_IMAGE 1000 ms 窗口 + 一次安全重放、TLS 记录存 `tls-record.bin`、超过 1024 字节的明文帧存 `image.bin`。

**判定**：

| 输出 | 含义 |
|---|---|
| `TLS record ... bytes` | 图像走 TLS，必须回去解决 PSK |
| `plain frame #1 ... (payload)` + `image.bin` | 明文出图，PSK 可跳过 |
| `ack status 0x01` 后 1000 ms 无记录 | 命令被接受但没出图：手指未放/未按压，或还需触发 |
| `line busy, not sent` | 上一帧没排空，仍有时序问题 |

**2026-11-23 第二次实测（排空修复后）**：整条配方全部打通，每条命令都拿到 ACK `0x01` 与 20 字节 FDT 报告：

```text
fdt 0x0d finger-mode  ack 0x01 + 20B
fdt 0x0c finger-mode  ack 0x01 + 20B
nop                   (无回复，符合预期)
mcu state 0x55        ack 0x01 + 20B  (payload 00 00 30 00 ... 30 01)
fdt 0x0c capture-down ack 0x01 + 20B
get-image             ack 0x01  then  d0 03 00 04 00 d3
```

`d0 03 00 04 00 d3` 是**设备主动发起的 TLS 请求帧**（校验和 `0xaa - sum` 正确，形态与 REQTLS 同命令号）。含义：**图像被 TLS 门挡住，明文旁路失败**。所以 M2b 的 PSK 不能砍，必须解决。

### M2d：PSK 获取（F2 staging）

**更正 2026-11-23 夜**：此前"F2 被固件封死"的结论是**我们自己的 bug**，不是固件行为。

`gxfp_build_mem_read` 的长度字段是 **4 字节小端**（body[7..10]）。探针原来写成 `body[7] = (uint8_t)len`，对 `len=256` 会截断成 `0`，所以每次都在请求"读 0 字节"，传感器自然只回一条空 echo。地址对不对根本没被测到。

正确请求（`0x08004000`，256 字节）：

```text
f2 09 00 00 40 00 00 00 01 00 00 6e
                  └addr┘  └─len─┘ ck
```

F2 响应的两种合法形态（`body_matches(body, len, 0xf2, payload)`）：

| 形态 | 长度 | 数据位置 |
|---|---|---|
| 直接 | `requested + 4` | `body[3..]` |
| 回显 + 数据 | `requested + 12` | `body[11..]`（前 8 字节是 addr32‖len32 回显） |
| 纯 echo | `12`（请求 8 字节时） | 无数据 |

**14115 的实测行为**：读工厂区（`0x08004000`、`0x08008000`、`0x0800a000`）被"拒绝"，但拒绝帧里塞着一段 **248 字节**的开机残留 staging 记录。解析方式：`stale_len = len - 12`，数据在 `body[11..]`。app flash 窗口 `0x08020000–0x08040000` 是正常读，不走这条路。

**PMK 解密**（`gx51_factory_pmk.c`，已核对）：

```text
key   = SHA256( salt[16] || 48×00 || seed[16] )[0:16]
        seed = 5cba6e25819518de2d53e96d6c0347ab0
iv    = salt（即 body[0:16]）
aes   = AES-128-CBC, 无 padding
明文  = 00 0d || BE32(0x30) || 48 字节 PMK
首字节在 14115 上会被破坏 → 爆破 256 种，只允许唯一解
```

只需前 80 字节：16 字节 salt + 4 个 CBC 块，足够 marker(2)+len(4)+PMK(48)。

**待测**（新版探针已修好长度字段，地址表按参考实现补齐，含 `base+8` 的 80 字节读）：

```bash
sudo ./build/gx51-probe --staging            # 复位后 + install 后各一轮
sudo ./build/gx51-probe --no-reset --staging # 若上一轮为空，冷启动后立刻跑这条
```

判定：出现 `f2 0x08004000 len6 len260 DATA248` 即拿到记录，随后写 `src/pmk.c` 解密。

**依赖**：本机没有 OpenSSL 头文件（`/usr/include/openssl` 不存在），所以 SHA-256 与 AES-128-CBC 自带实现，不引入外部库。

### M2e：PMK 已取得（2026-11-23 夜，实测）

F2 读通道在 14108 上**完全开放**，返回的是真实内存内容（`0x08020000` 读出向量表，`0x20000000` 读出 RAM 指针），不是 14115 那种"拒绝但回吐残留"。

| 项 | 实测值 |
|---|---|
| PMK 所在 base | **`0x08008000`**（`0x08004000`、`0x0800a000` 同样能读，但只有这个位置能解密成功） |
| 记录布局 | `salt(16)` + `AES-128-CBC(64)` |
| 首字节 | **未损坏**（`0xb4`），不像 14115 需要爆破 |
| key | `SHA-256(salt ‖ 48×00 ‖ seed)[:16]`，seed 与 14115 相同 |
| 明文 | `00 0d 00 00 00 30` + 48 字节 PMK + 10 字节尾 |
| PMK 指纹 | `SHA-256(PMK)` 前 8 字节 = `30176ca18f9472ed`（用于比对，不存密钥本身） |

请求长度必须写 4 字节小端。

**F2 帧格式（2026-11-23 修正，重要）**：应答体是 `[f2][len16 = payload+1][payload][ck]`，payload 从**第 3 字节**开始；`len16 + 3 == 帧长`。前面可能有一个 6 字节 ACK（`b0 03 00 f2 03 02`）。旧代码按"12 字节头 + 8 字节回显"解析，两个偏移都错，所以 `--pmk` 报 `not recovered`；离线测试当时切 `raw+6+11` 只是**恰好**落在 salt 上（salt 在记录内偏移 8 = 帧内偏移 11）。

**记录偏移**：工厂记录在 base + 8，前 8 字节是记录头（`0f 00 00 00 80 01 00 00`）。所以 `--staging` 里 `0x08008008` 的 payload 与 `0x08008000` 的 payload[8..] 逐字节相同。

代码：`src/sha256.c`、`src/aes128.c`、`src/pmk.c`（`gx51_mem_payload` / `gx51_mem_read` / `gx51_pmk_from_record` / `gx51_fetch_pmk`），探针加 `--pmk`。离线测试含 SHA-256、AES-128 分组、CBC 链、坏记录拒绝、F2 帧解析与坏校验拒绝、真机抓包回放校验 PMK 指纹、记录 8 字节偏移校验，全部通过。

**下一步 M2b-TLS**：`REQTLS` → TLS1.2 `PSK-AES128-GCM-SHA256` 握手（主机为 server，identity `Client_identity`）→ NOP/`D4` 确认 → GET_IMAGE 走 TLS 记录。

握手用 OpenSSL，不手写 TLS。本机缺头文件：

```bash
sudo apt install libssl-dev     # 3.2.4-0deepin17 已在源里
```

### M3：图像采集

- `0x20` 取图，`0x32/0x36/0x34` FDT 相关
- 有效区 64×80，`6 字节 → 4 像素` 解码
- 只做必要归一化，不做美化
- 输出 libfprint 接受的灰度图

**验收**：连续 10 张，尺寸正确、随手指变化、无随机花屏。

单帧已通过，连续帧用 `--frames 10` 一次采完（每帧另存 `finger-NN.pgm`）。

### M4：libfprint 接入

最小回调集合：`probe / open / capture / enroll / verify / cancel / close`。

- 复用现有 partial-print matcher，不重写算法
- 模板交给 libfprint/fprintd 管理
- 取消、超时、关闭必须释放 SPI/GPIO/TLS

**验收**：`fprintd-list` 能列出设备。

### M5：录入与验证

**验收**：`fprintd-enroll` 成功；同指通过；异指全部拒绝。

### M6：系统认证

仅在 M5 稳定后：锁屏 / `sudo`，**密码登录始终保留**。

---

## 6. 硬性约束

1. 不执行 `0xF0`、不刷固件、不写 eFuse、不写 PMK
2. 不修改系统 libfprint，直到 M3 验收通过
3. 不动 BIOS、不写 `_DSM`、不碰 GPIO112
4. 不复制 Windows 驱动二进制或密钥文件
5. 密码登录不得被移除
6. 只面向 `GXFP51A0`
7. **不得通过调整匹配阈值让验收"看起来通过"**。阈值只能由离线分布决定，
   且同指/异指分布必须不重叠（见 M3.5）。

---

## 7. 回滚

```bash
# 传感器从 spidev 解绑，交回内核/ACPI 默认状态
sudo sh -c 'echo spi-GXFP51A0:00 > /sys/bus/spi/drivers/spidev/unbind'
sudo sh -c 'echo > /sys/bus/spi/devices/spi-GXFP51A0:00/driver_override'
sudo systemctl start fprintd
```

传感器异常时：`GPIO264` 复位即可恢复；必要时断电重启。

---

## 8. 常用命令

```bash
cd ~/Downloads/gxfp51a0-min
make && make test

sudo modprobe spidev
echo spidev | sudo tee /sys/bus/spi/devices/spi-GXFP51A0:00/driver_override
echo spi-GXFP51A0:00 | sudo tee /sys/bus/spi/drivers/spidev/bind

sudo systemctl stop fprintd
sudo ./build/gx51-probe --verbose              # M1
sudo ./build/gx51-probe --info --verbose       # 只读诊断
sudo ./build/gx51-probe --configure --verbose  # M2a
```

---

## 9. 代码地图

| 文件 | 职责 | 阶段 |
|---|---|---|
| `src/transport.c` | spidev、GPIO264 复位、GPIO48 电平/ACK、帧读写 | M1 |
| `src/protocol.c` | 内外层校验和、ACK / 版本解析 | M1 |
| `src/target.c` | OTP 校准、配置补丁、A2/82/A6/70/80/90 序列 | M2a |
| `src/config_base.c` | 256 字节厂商配置数据（仅数据） | M2a |
| `src/probe.c` | 命令行入口 | M1/M2a |
| `tests/test_protocol.c` | 帧与解析向量 | M1 |
| `tests/test_cal.c` | 真实 OTP 校准回归 | M2a |
| `src/tls.c` | OpenSSL PSK server + record BIO | M2b |
| `src/image.c`（待建） | 取图、FDT、解码 | M3 |
| `src/fp-device.c`（待建） | libfprint 回调 | M4 |

---

## 10. 下一步（2026-11-24 修订）

M1–M4 全部通过；M5 失败，且失败原因已定位（见 SUMMARY.md §4）。原计划里
"取到图就能匹配"这个隐含假设被证伪，因此下一步只有两条支路：

| 支路 | 做法 | 判据 |
|---|---|---|
| A | 读回 `0x90` 配置里的几何字段，确认读取窗口能否大于 80×64 | 帧内有效行数是否变化 |
| B | 单次按压取 4–8 帧，亚像素位移估计 + 超分重建，提升等效分辨率 | 重建后 80×64 上 MINDTCT 是否在**真实区内**出点 |

两条都以 M3.5 的判据收口：同指可分、异指不可分，各 ≥20 帧。
在闸门通过之前，不接匹配器、不开 M6。

### 已废弃的旧结论（保留以免重走）

早期判定“14108 封死 F2 内存读”是自测 bug：`f2` 请求长度字段写成 1 字节，256 被截成 0，所有地址都回 0 字节。
改成 4 字节小端后，`base+8` 立刻返回 80 字节工厂记录。
教训：**先怀疑自己的组帧代码，再怀疑固件**。

同样作废的还有：E4 只能取摘要、E0 需自备 PSK、需走 Windows SGX —— 均未验证或已被 F2 路径取代。

