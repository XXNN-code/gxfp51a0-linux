# GXFP51A0 Linux 进展与驱动编写指南

最后更新：2026-11-24 傍晚
状态：**M3 验收通过（10/10 帧，range 1639–1799）；M4 已构建通过，libfprint 能枚举到 `Goodix GXFP51A0`，待 `install` 进 /usr/local**。已经采到真实指纹图（`run-capture-155646.log`：背景 mean 2728、手指 1400、差分范围 1804、脊线周期 9 px）。链路 复位 → 版本 → OTP/DAC → 配置 → TLS → D4 → 采图 → 解码 全部真机打通。

---

## 1. 一句话结论

在这台 MateBook 13（WRTB-WXX9）上，Goodix `GXFP51A0`（固件 `GF_ST411SEC_APP_14108`）已经可以由 Linux 用户态完整驱动到：

```
复位 → SPI 帧协议 → DriverState → 版本 → OTP 校准 → 配置上传
→ F2 读工厂记录 → 解出 PMK → TLS1.2 PSK 握手 → 取到图像帧（加密）
```

**唯一还没做完的是"图像内容本身"**：此前拿到的帧全为 0，根因是采图前少了 `FDT boot → NAV → FDT background → 重放 DAC` 这一段；解码器（88×80 → 80×64，12 bit）已就位并通过离线测试。

---

## 2. 硬件与协议事实（真机实测，可直接依赖）

| 项 | 实测值 |
|---|---|
| ACPI 节点 | `GXFP51A0:00`，SPI 从设备 bound 到 `spidev1.0` |
| 复位 | GPIO264：HIGH 300 ms → LOW 600 ms |
| 中断 | GPIO48，**电平高 = 有回复**（不是边沿） |
| SPI | mode 0 + `SPI_CS_HIGH`，1 MHz，8 bit |
| 芯片 ID | `0x2504` |
| 固件 | `GF_ST411SEC_APP_14108` |
| 帧格式 | `[type][len16 LE][body][ck]`，`type` = `0xa0` 明文 / `0xb0` TLS 或 ACK |
| 校验 | `ck = 0xaa - sum(body)`，`len16` **包含** ck |
| 有效区 | 64×80（待图像解码确认，见 §5） |

### 2.1 地址与密钥

| 项 | 值 |
|---|---|
| 工厂记录 | F2 读 `0x08008000`，记录头 8 字节，salt 在 `base + 8` |
| F2 长度字段 | **4 字节小端 u32**，不是 u16 |
| F2 返回 | 比请求少 8 字节；长度 256 时数据 248 字节 |
| PMK 解密 | `key = SHA256(salt‖48×00‖seed)[:16]`，AES-128-CBC，IV = salt，无 padding |
| seed | `5cba6e25819518de2d53e96dc0347ab0` |
| 明文格式 | `00 0d 00 00 00 30` + 48 字节 PMK |
| PMK 指纹 | SHA-256 前 8 字节 = `30176ca18f9472ed`（真机与离线回放一致） |

PMK **只在内存**：不落盘、不打日志、不进配置文件。探针只打印指纹。

### 2.2 TLS 参数（实测，和参考实现文档不一致，以实测为准）

| 项 | 实测 |
|---|---|
| 角色 | 传感器 = client，主机 = **server** |
| 版本 | TLS 1.2（ClientHello `03 03`，record 版本 `03 01`） |
| 套件 | **`0x00ae` = `PSK-AES128-CBC-SHA256`**，仅此一种 + `00ff` SCSV |
| 扩展 | **零扩展**（无 SNI、无 EMS、无 PSK identity hint） |
| PSK identity | `Client_identity`，在 ClientKeyExchange 里 |
| 触发 | `D0 03 00 00 00 D7`（REQTLS） |

> 踩坑记录：`0x00ae` 常被误抄成 GCM。**GCM 是 `0x00a8`**。用 GCM 时 OpenSSL 回
> `no shared cipher` + alert 0x28，看起来像配置问题，其实是套件码错。

### 2.3 加密的方向性（本轮新结论）

**命令走明文，只有图像方向加密。**

- 在 TLS 隧道内发 `NOP` / `GET_IMAGE` → 传感器**完全不应答**（`tunnelled -> 0 read(s)`）
- 同样命令用明文 `0xa0` 帧发 → 立刻回明文 ACK + 加密图像记录

所以主机 → 传感器方向永远是 `0xa0` 明文命令帧；传感器 → 主机方向，命令回复是明文 ACK，
图像数据是 `0xb0` TLS 记录。

### 2.4 命令回复计数（不遵守会把下一条命令噎住）

| 命令 | 明文回复数 |
|---|---|
| `0x32` / `0x36`（FDT） | 2 |
| `0xae`（MCU 状态） | 2 |
| `0x82`（读寄存器） | 2 |
| `0x80`（写寄存器） | 1 |
| NOP `0x00` | 0（**不能等**） |

只读一条回复会留下高电平，下一条命令被吞 —— 这是早期"GET_IMAGE 完全无输出"的真因。

---

## 3. 已打通的链路（按时间顺序，含证据）

| 里程碑 | 内容 | 证据 |
|---|---|---|
| M1 | 复位 + SPI + 帧协议 + 版本 | `GXFP51A0 firmware: GF_ST411SEC_APP_14108` |
| M2a | OTP 校准 + 配置上传 | `config response status 0x01` |
| M2c | 明文采集旁路失败 | GET_IMAGE 只回 `d0 03 00 04 00 d3`（REQTLS） |
| M2e | 工厂 PMK 恢复 | `pmk: recovered, 48 bytes, sha256 30176ca18f9472ed` |
| M2b | TLS-PSK 握手 | `handshake ok: TLSv1.2 PSK-AES128-CBC-SHA256` |
| M3a | 取到图像帧（加密） | 明文 `20 4a 29` + 10569 字节，来自 10629 字节 TLS 记录 |

### 3.1 完整基线序列

```text
GPIO264 复位
A0 明文: 96 03 00 01 00 10              DriverState install
A0 明文: a8 03 00 00 00 ff              版本 → 35 字节字符串
A0 明文: a2 … / 82 / a6 …               软复位 + 芯片 ID + OTP
A0 明文: 70 03 00 14 00 23              idle
A0 明文: 80 … ×4                        DAC 写入 0x0220/0x0236/0x0238/0x023a
A0 明文: 90 …                           配置上传 → status 0x01
A0 明文: f2 … 0x08008000                工厂记录 → 解 PMK
A0 明文: d0 03 00 00 00 d7              REQTLS
← B0: 52B ClientHello (16 03 01 00 2f)
→ B0: 95B ServerHello / 91B CCS+Finished
← B0: 26B ClientKeyExchange, 6B CCS, 85B Finished
A0 明文: 20 03 00 01 00 86              GET_IMAGE
← A0: 6B ACK
← B0: 10629B TLS record → 明文 10573B
```

---

## 4. 关键 bug 与教训

这几条都是"看起来像固件问题，其实是自己代码错"，重走会浪费整晚。

| 现象 | 真因 | 教训 |
|---|---|---|
| DAC 写失败 | 写前没等 GPIO48 回低，ACK 窗口只给 100 ms | 电平型中断，写前必须 `wait_idle` |
| GET_IMAGE 毫无输出 | 每条命令只读一条回复，残留帧把后续全噎住 | 按回复计数排空 |
| "14108 封锁 F2 内存读" | F2 长度字段写成 1 字节，256 被截成 0 | **先怀疑自己的组帧，再怀疑固件** |
| PMK 解不出 | 初始 seed 抄错一字节；记录头 8 字节未跳过 | 用指纹核对，别用"看起来对" |
| `no shared cipher` | 套件码 `0x00ae` 记成 GCM | 以 `openssl ciphers -V` 和设备通告为准 |
| TLS 握手报 `buffer 5` | BIO 一次塞整条 record，OpenSSL 先只要 5 字节头 | BIO 必须实现成流式 |

---

## 5. 空帧的两个原因：先缺背景配方，补上后仍缺 D4 确认

`tls-image-plain.bin` 实测结构，现在已完全对得上厂商标定过的布局：

```text
总长 10573 = 8 字节头 + 10560 打包像素 + 5 字节尾
  0..2    20 4a 29            帧头 cmd 0x20, len16 = 10570
  3..7    00 <4 字节计数器>    图像头（状态 + 帧计数，每次自增）
  8..     10560 字节          6 字节承载 4 个 12-bit 采样
  -5..    c3 d0 ba 33 88      固定尾
```

`10560 / 6 * 4 = 7040 = 88 * 80`，即传感器按 **88 列 × 80 行**输出，可见区是转置后的
**80 × 64**。尾部 5 字节不是 `0xaa-sum`，因为它根本不属于普通命令帧，无需再猜。

空帧的判据不只是“像素全 0”，同一轮日志里 FDT 报告也是 `touch=0x3f, zones=6, mean=0 live=0`：
触摸标志位全亮、六区模拟值全为 0。**模拟前端根本没启动**，图像自然是零。

对比已验证的采集路径，我们的序列缺了整整一段前置：

```text
我们之前：FDT finger-mode → NOP → MCU state → FDT capture-down → GET_IMAGE
参考路径：NOP → MCU state → FDT boot → NAV(0x50) → FDT background
         → 读 0x0082 → 重写四个 DAC 寄存器（OTP 标定值） → GET_IMAGE
```

也就是说,`--configure` 里那次 DAC 写入只够让配置上传被接受，**采图前还必须在
`FDT boot + NAV + FDT background` 之后重放一次 DAC trim**，传感器才会输出真实采样。

### 5.1 配方补齐后仍然全零 → 真正的缺口是 D4 确认

补齐背景配方后（`capture-2.log`）现象没变：`fdt: touch=0x3f zones=6 mean=0 live=0`
在每一轮都一模一样，图像仍是 10573 字节全零。这说明缺的不是配方，而是**安全状态**。

参考实现 `gx_tls_handshake()` 在握手成功后还有一步，我们之前完全漏掉：

```text
握手完成 → 明文发 D4 03 00 00 00 D3 → 等 ACK 0xd4（status 0x01）→ 才允许采图
```

Windows 与已验证的 Stage3C 路径都这么做。缺这一步，MCU 仍让**所有命令 ACK**、
FDT 报告仍返回 16 字节（所以看起来"通了"），但采图通路没被打开，回报的就是
固定模板（touchflag 0x3f、六区全 0）和全零图像缓冲。这类"全 ACK 但内容清零"
正是安全门未解锁的典型表现。

已实现：`gx51_tls_start()` 握手成功后自动发 D4 并校验 ACK（`src/tls.c`
的 `tls_confirm_up()`，失败重放 3 次），日志会打印
`tls: MCU confirmed tunnel (D4 ack 0x01)`。

**判据**：`fdt: ... mean=` 从 0 变成几百（空手约 335，按手指约 230）。

### 5.2 D4 确认后仍全零 → 还缺 WakeupMCU

`fdt-poll-1.log` 实测：`tls: MCU confirmed tunnel (D4 ack 0x01)` 已出现，但 213 轮
FDT 报告的 20 字节**完全一致**（含校验和 `0x23`），`touch=0x3f zones=6 mean=0`。
固定模板 + 零 ADC 说明采图引擎没被唤醒，命令通道只是"能用"。

对照参考实现 `gx_cold_prepare()` / `gx_session_start()`，还有一步我们从未发过：

```text
原始 4 字节 SPI 写  0f 00 00 0e   （无帧头、无 ACK、随后 5 ms 静默）
```

这是 Windows `gfspi.dll` 的 WakeupMCU。它在 `DriverState` 之后、固件读取之前发一次，
**在采图上下文准备好之后再发一次** —— 第二次正是把 MCU 从 `0x70 idle` 里拉出来的动作。
缺它时所有命令照常 ACK，但模拟前端不开：FDT 六区为 0，图像是尺寸正确的全零缓冲。

已实现：`gx51_wakeup_mcu()`（`src/transport.c`，原样 4 字节），`--fdt-poll` 与
`--tls-image` 在握手/D4 之后自动调用，日志打印 `WakeupMCU (0f 00 00 0e) sent`。

真机复核（`run-fdtpoll-154017.log`）：WakeupMCU 已发出，D4 已确认，**213 轮 FDT 仍完全一致**
（20 字节含校验和 `0x23` 逐字节相同，`touch=0x3f zones=6 mean=0`）。固定模板 + 零 ADC 说明
问题不在采图配方，而在更上游的初始化。

### 5.3 真正的顺序错误：F2 取 PMK 之后必须硬复位

对照参考实现 `gx_upload_config_and_reqtls()`（`driver/goodix51a0/goodix51a0.c`）：

```text
F2/E4 工厂 staging 取 PMK
→ 注释原文：the reviewed full MCU reset is essential AFTER actual F2/E4
  factory boot-staging extraction: its RAM state is transient and must not
  reach target initialization
→ GPIO264 硬复位 + A8 读固件 + DriverState install
→ gx_target_configure()：软复位 / chip-id / OTP / 软复位 / 0x70 / 4×DAC / 0x90
→ D0 REQTLS
```

我们原来的顺序是 **configure 在前、F2 取 PMK 在后**，然后直接在 staging 后的 RAM 上
发 REQTLS。于是出现典型症状：`0x96 / a8 / a2 / 82 / a6 / 80 / 90 / d0` 全部 ACK，
TLS 握手也能成（PMK 本身正确、D4 也确认），但**模拟前端从未被 `0x90` 真正装载** ——
FDT 六区恒 0、图像尺寸正确但全零。

这也解释了为什么补 D4、补 WakeupMCU、补背景配方都无效：三者都在错误的状态之后执行。

已改为参考顺序（`--fdt-poll` / `--tls-image` / `--tls` 现在都在 `tls_open()` 内完成）：

```text
F2 取 PMK（内存驻留）
→ 关 SPI/IRQ → GPIO264 复位 → 重开 → install → A8 校验（hard_reset_cycle）
→ gx51_configure_ex()：软复位 / chip-id / OTP / 软复位 / 0x70 / 4×DAC / 0x90
→ D0 REQTLS → 握手 → D4
→ 一次裸 FDT（fdt pre-arm）→ WakeupMCU → 再一次 FDT → 背景配方 → 轮询
```

日志新增三行判据：

```text
tls: post-staging reset before target configure
tls: firmware after post-staging reset: GF_ST411SEC_APP_14108
capture: fdt pre-arm -> ...  fdt: touch=.. zones=.. mean=..
```

### 5.4 最后一块拼图：`0x80` 写寄存器少了一个前导 0x00

`run-fdtpoll-154937.log` 里 `reg read 0x0082` 终于打出原始帧：

```text
frame (6): 82 03 00 80 1f 86      → 寄存器 0x0082 = 0x1f80
```

`0x1f80 = (fdt_delta 0x1f << 8) | 0x80`，正是 `gxfp_patch_config()` 写进去的值，
说明 `0x90` 配置上传确实生效了。可 FDT 仍然 216 轮逐字节相同。于是逐条对比 vendor 的
组帧函数，发现唯一不一致的就是 DAC 写入：

```c
/* vendor gxfp_build_reg_write(): {0x80,0x06,0x00, 0x00, addr_lo, addr_hi, val_lo, val_hi, ck} */
/* ours (wrong):                    {0x80,0x06,0x00, addr_lo, addr_hi, val_lo, val_hi, 0x00, ck} */
```

`0x80` 的载荷是 **`{0x00, addr_lo, addr_hi, val_lo, val_hi}`**，第一个字节是选择位。
我们漏掉它、又在末尾多补了一个 0，地址和值整体左移一字节 —— 写入落到了错误地址。
MCU 依然回 ACK（ACK 只回显命令码），所以日志看着"全部 ack"，而 OTP 标定的 DAC trim
从未真正写进去，模拟前端因此一直没通电。

同类错误最容易复发，因此三条命令现在都按 vendor 字节级核对过：

| 命令 | body | 状态 |
|---|---|---|
| `0x82` chip-id / 读寄存器 | `82 06 00 00 00 00 04 00 1e` | 本来一致 |
| `0xa6` OTP | `a6 03 00 00 00 01` | 本来一致 |
| `0x70` idle | `70 03 00 14 00 23` | 本来一致 |
| `0x90` 配置上传 | `90 01 01` + 256B + ck | 本来一致 |
| `0x80` 写寄存器 | `80 06 00 00 addr_lo addr_hi val_lo val_hi ck` | **已修** |

同时加了写后回读：`configure: reg 0x0220 readback 0x0b78 (wrote 0x0b78) match`，
以后"ACK 但没写进去"这类问题一眼可见。

### 5.5 结果：模拟前端活了（2026-11-24 15:54 真机）

`run-fdtpoll-155404.log`：

```text
configure: reg 0x0220 readback 0x0b78 (wrote 0x0b78) match      ← DAC trim 真的进去了
configure: reg 0x0236 readback 0x00b9 (wrote 0x00b9) match
configure: reg 0x0238 readback 0x00b7 (wrote 0x00b7) match
configure: reg 0x023a readback 0x00b6 (wrote 0x00b6) match
frame (20): 36 11 00 00 01 00 00 61 01 82 01 44 01 60 01 4a 01 63 01 28
    fdt: touch=0x00 zones=0 mean=350 live=6                        ← 空闲
frame (20): 36 11 00 00 01 00 00 ... （按压时）
    fdt: touch=0x3f zones=6 mean=187 live=6                        ← 手指
```

217 轮统计：空闲 175 轮 `mean=350..351`，按压 42 轮 `mean=186..220`，**降幅 130~165**，
`touchflag` 在 `0x00` / `0x3f` 之间干净翻转。参考实现的真机基线是空闲 353、降幅约 100，
数值同量级，判定标准（`mean ≥ 340` 为空闲、六区 ≥5 为手指）可以直接沿用。

至此 M3 检测链路完成：`mean` 从 0 变成 350，是整条链路第一次拿到真实模拟量。
剩下一件事就是图像本身。

### 5.6 采图不再靠固定延时

`--tls-image` 原来提示后 `sleep 800ms` 就抓，无法区分"手指没放"和"手指晚放 100ms"。
现在用 FDT 做门控（`wait_for_touch` / `wait_for_clear`，`src/probe.c`）：

```text
背景 pass：提示"保持传感器干净" → 等 touch=0x00 且 mean≥300 → 配方 → GET_IMAGE → bg.pgm
手指 pass：提示"按住手指" → 轮询 FDT 直到 6 区 ≥5 或 12 s 超时 → 配方 → GET_IMAGE
           → 提示"可以抬起" → 范围 <64 则重试，最多 5 轮
```

另外 `--tls-image` 不再要求先跑 `--configure`：`tls_open()` 里已经包含
post-staging 复位 + 完整 configure，`device_cal` 在那里填充。

### 5.7 第一张真实指纹图（2026-11-24 15:56 真机）

`run-capture-155646.log` 全链路一次通过，没有任何重试：

```text
capture: sensor clear (touch=0x00 mean=350)
...
capture: background min=1540 max=3239 mean=2728 p1=3124 p99=3124 live=5119
capture: wrote bg.pgm
capture: waiting for finger... touch=0x3f ... → wait_for_touch 命中
    fdt: touch=0x3f zones=6 mean=184 live=6
tls: app data 10573 bytes: 20 4a 29 00 5c 41 d5 22 ...
capture: finger min=539 max=2343 mean=1400 p1=2136 p99=2136 live=5119
capture: wrote finger.pgm
capture: wrote finger-diff.pgm
capture: verdict = image data present (range 1804)
```

**图像内容是真的**，不是噪声：

| 指标 | 背景 | 手指 | 判据 |
|---|---|---|---|
| 全局均值 | 2728 | 1400 | 手指压暗整幅（手指比传感面大） |
| 动态范围 | 1699 | 1804 | ≫ 64 阈值 |
| 差分图行剖面自相关 | — | 峰值 0.654 @ lag 9 | 周期性结构，即脊线 |

差分图沿输出行方向的周期为 **9 px**、自相关峰 0.65，说明是稳定的平行脊线而非随机噪声；
`80×64` 输出、脊线周期 9 px 反推像素间距约 55 µm（≈460 dpi），与 `500 dpi` 量级吻合。
图里能看到脊线弯曲与分叉点，图像上下是同一根手指的连续纹路。

查看方式（新增 `tools/pgm2png.py`，纯标准库，无需 netpbm）：

```bash
./tools/pgm2png.py --scale 8 bg.pgm finger.pgm finger-diff.pgm
# 或用任意看图工具直接打开 .pgm
```

同时修了两处观测缺陷：

- `gx51_image_stats()` 的 `p1` 原来一直等于 `p99`（同一循环里被反复覆盖），现在两个百分位各自独立扫描。
- `write_pgm()` 改成按 `p1..p99` 拉伸，而不是按 `max` 缩放 —— 否则均匀背景会被拉成纯白、脊线调制全被压掉。
- 每次 `GET_IMAGE` 都把解密后的 10573 字节落盘为 `plain-<phase>.bin`，离线可重新解码，
  单元测试也能拿真实数据跑（`tls-image-plain.bin`）。

**至此 M3 完成：Linux 用户态拿到了第一张可用的指纹图。**


---

## 6. 下一步（按顺序，每步都有判据）

### 步骤 0：先确认模拟前端活着（`--fdt-poll`，15 秒）

```bash
sudo ./tools/capture.sh --configure --fdt-poll 12
```

脚本把完整输出写进 `run-fdtpoll-HHMMSS.log`，屏幕只在需要按手指时打印
`>>> press a finger on the sensor now`（走 `/dev/tty`，不受重定向影响）。

握手（含 D4）、背景配方、然后连续打 FDT，每轮打印 `touch / zones / mean / live`。
看到提示后按上手指再抬起。这一步不做图像、不解码，只回答"ADC 有没有值"。

现在日志里能直接分辨三种状态：

| 日志 | 含义 |
|---|---|
| `tls: post-staging reset ...` 后紧跟固件串 | staging 后复位成功，初始化边界干净 |
| `fdt pre-arm` 就有 `mean≈340` | 前端在 configure 之后已经活了，与配方无关 |
| `fdt post-wakeup` 仍 `mean=0` | 复位/configure 本身没生效，`0x90` 未装载 |

**判据**：`mean` 非 0，且按手指时六区数值下降约 100。

### 步骤 1：背景 pass + 手指 pass（已实现，M3 验收用 `--frames 10`）

`--tls-image` 现在跑两段配方，中间等你按手指：

```text
背景（手不要碰传感器）:
  NOP → 0xae 0x55 → FDT 0x0d boot → NAV 0x50 → FDT 0x0d background
  → 读 0x0082 → 写 0x0220/0x0236/0x0238/0x023a → GET_IMAGE      → bg.pgm

手指（最多 5 轮，每轮重新 arm）:
  FDT 0x0d finger-mode → FDT 0x0c finger-mode → NOP → 0xae 0x55
  → FDT 0x0c capture-down → GET_IMAGE                            → finger.pgm
```

**判据**：`capture: verdict = image data present`，且 `finger min/max` 的差值 ≥ 64。
FDT 行会同时打印 `touch=.. zones=.. mean=..`：`mean` 从 0 变成几百，就说明前置步骤生效了。

### 步骤 2：解码（已实现）

`src/image.c`：8 字节头 + 10560 字节打包（6→4，12 bit）+ 5 字节尾，转置 88×80 → 80×64，
落 `bg.pgm` / `finger.pgm`，并额外写一张 `finger-diff.pgm`（手指帧减背景，中值 2048）。

**判据**：`feh finger-diff.pgm` 看到纹线；同指连拍纹线位置稳定。

### 步骤 3：固化成驱动

解码、配方、TLS 都已就位，剩下的只是搬进 libfprint 回调，见 §7。

**判据**：`fprintd-list` 列出设备；`fprintd-enroll` 成功。

---

## 7. 驱动怎么写

### 7.1 形态选择

**推荐：用户态 libfprint 驱动，不写内核模块。**

理由：

- SPI 走 `spidev`、GPIO 走 `gpiod`（`/dev/gpiochipN`），用户态全够用
- 图像解码、TLS、模板匹配都是应用层活，没必要进内核
- 出问题不会把系统带崩，调试成本低一个数量级

模块划分（与现有 `src/` 一一对应，可直接演进）：

| 文件 | 职责 | 现状 |
|---|---|---|
| `src/transport.c` | spidev ioctl、GPIO 请求、电平轮询、帧读写 | 已有 |
| `src/protocol.c` | 组帧、校验、ACK / 版本 / F2 解析 | 已有 |
| `src/target.c` | 复位、install、OTP 校准、配置上传 | 已有 |
| `src/pmk.c` | F2 读工厂记录 + AES 解密取 PMK | 已有 |
| `src/tls.c` | OpenSSL PSK server + 自定义 BIO | 已有 |
| `src/image.c` | **待写**：FDT 序列、取图、解码 | — |
| `src/fp-device.c` | **待写**：libfprint 回调 | — |

### 7.2 分层与数据流

```text
libfprint (FpDevice)
   │  capture / enroll / verify
   ▼
fp-device.c     状态机、超时、取消
   │  gx51_capture(frames)
   ▼
image.c         FDT 序列 → GET_IMAGE → 收 TLS 记录 → 12bpp 解码
   │  pixels
   ▼
tls.c           SSL_read（图像方向）
   │  record
   ▼
protocol.c      帧解析 / 组帧
   │  bytes
   ▼
transport.c     SPI + GPIO
```

关键约束：**一层只认下一层的接口**。`image.c` 不该知道 SPI 细节，`tls.c` 不该知道命令语义。

### 7.3 状态机（必须显式建模）

```text
UNINIT ──> INIT (reset, install, version)
             │
             ▼
          CONFIGURE (OTP, DAC, config upload)   ← 开机只做一次，结果可缓存
             │
             ▼
          PMK_LOAD (F2 读 + 解密，内存驻留)
             │
             ▼
          TLS_UP (REQTLS + 握手)
             │
             ├──> IDLE ──FDT 检测到手指──> CAPTURE ──> DECODE ──> IDLE
             │
             └──> ERROR ──reset──> UNINIT
```

错误恢复规则：

- 单条命令无 ACK → 重放 2 次（`0x80` 在 idle 转换后尤其容易吞）
- SPI 读写错 → 不复位，直接返回错误给上层
- 握手失败 → 只重试一次，之后整机复位重来
- 任何 `0xF0`、固件写入、eFuse 写入 → **永不执行**

### 7.4 libfprint 接入要点

最小回调集合：

```c
probe()    // 检查 spidev 与 gpiochip 可访问，不做复位
open()     // 复位 + install + 版本校验（固件不符直接失败）
init()     // configure（OTP/DAC/config），失败即 FAIL
capture()  // 异步：注册 GPIO48 可读回调，FDT 等手指，取图解码，ssm 完成
verify()   // 复用 libfprint 的 partial-print matcher，不自己写算法
enroll()
cancel()   // 必须能中断等待中的 FDT/图像读取
close()    // 释放 SPI/GPIO/TLS，清零 PMK
```

实现细节：

1. **异步而非轮询**：libfprint 用 GLib 主循环。给 `irq_fd` 加 `g_unix_fd_add()`，数据就绪时再读，别 `usleep` 死等。
2. **超时表**（实测值，照抄即可）：

   | 场景 | 超时 |
   |---|---|
   | 复位后首条命令 | 250 ms |
   | 普通命令 ACK | 250 ms |
   | FDT 等手指 | 10 s（可取消） |
   | 单条 TLS 记录 | 1500 ms |
   | 握手 | 8 s |

3. **PMK 生命周期**：`open()` 时解密一次，`close()` 时 `explicit_bzero`。不要写缓存文件。若担心被换出，可 `mlock()` 那 48 字节。
4. **取消语义**：`cancel()` 要能让正在等 IRQ 的线程在 100 ms 内退出，靠 `poll()` + 事件 fd，不靠信号。
5. **不做图像美化**：只做必要的 12→8 bit 归一化，别加滤波/锐化，matcher 对预处理很敏感。

### 7.5 测试策略

| 层 | 测法 |
|---|---|
| 协议/校验 | 离线向量（已有 `tests/`） |
| PMK 解密 | 真机抓包回放 + SHA-256 指纹比对（已有） |
| TLS | 本地 `s_server` 对拍 + 传感器原始 ClientHello 回放（做过） |
| 图像 | 连拍 10 张，比对纹线稳定性 |
| 驱动 | `fprintd-verify` 正例/反例各 10 次 |

### 7.6 M4 代码已就位（2026-11-24 傍晚）

驱动不再只是设计，已经落成三个文件：

| 文件 | 作用 |
|---|---|
| `src/session.c` + `include/session.h` | 真正的采图序列：复位 → DriverState → F2 取 PMK → post-staging 复位 → configure → REQTLS → 握手 → D4 → WakeupMCU → 背景配方 → FDT 门控 → 取图解码 |
| `driver/goodix51a0/goodix51a0.c` | libfprint `FpImageDevice` 驱动：`FP_DEVICE_TYPE_UDEV` + `spi_acpi_id = "GXFP51A0"`，靠 libfprint 自带的 spidev 发现逻辑匹配设备 |
| `tools/build-libfprint.sh` + `tools/patch-libfprint.py` | 拉取 libfprint 1.94.9（哈希固定）、拷入驱动源码、补 meson 表、构建、落 `build/stage` |

线程模型：libfprint 的 image-device vfunc 跑在主上下文，而一次会话是好几秒的阻塞
SPI 事务，所以 `activate()` 只开一个 worker 线程，主线程只看 `g_main_context_invoke()`
投递回来的 `report_finger_status` / `image_captured` / `activate_complete`。
`activate()` 期间用 `fpi_device_critical_enter()` 挡住取消，否则 deactivate 会在
ACTIVATING 状态下进来，状态机不允许。

`session.c` 里新增了离线可测的部分：`gx51_session_fdt_decode()` 用真机抓到的两帧
（空闲 `36 11 00 …mean 350`、按压 `32 11 00 …mean 184`）做回归，
`tests/test_session.c` 15 项断言，含坏校验和/错 tag/短帧的拒绝路径。

构建与安装分两步，因为安装要写 `/usr/local`：

```bash
sudo ./tools/install-deps.sh        # 只装 glib/gusb/gudev/pixman/meson 等构建依赖
./tools/build-libfprint.sh build    # 普通用户即可，产物落 build/stage
sudo ./tools/build-libfprint.sh install   # 拷进 /usr/local + ldconfig
```

`/usr/local/lib/x86_64-linux-gnu` 在 `ld.so.conf` 里排在 `/lib/x86_64-linux-gnu` 之前，
所以 fprintd 会加载我们的库，发行版包保持原样、随时可回退。

### 7.7 构建通过，设备已被 libfprint 发现（2026-11-24 16:20）

依赖装完后构建一次跑通（110 个目标，3 分 20 秒），产物落 `build/stage`：

```text
staged: build/stage/usr/local/lib/x86_64-linux-gnu/libfprint-2.so.2.0.0
driver present: GXFP51A0 id table found
```

在**没安装、没 root** 的情况下，用 stage 里的库直接枚举设备，确认发现链路成立：

```text
$ LD_LIBRARY_PATH=build/stage/usr/local/lib/x86_64-linux-gnu ./build/list-devices
devices: 1
  [0] Goodix GXFP51A0 (GF3658 / ST411)
      id: 0
      driver: goodix51a0
```

这说明 `FpIdEntry.spi_acpi_id = "GXFP51A0"` 与
`/sys/bus/spi/devices/spi-GXFP51A0:00` 的匹配生效，不需要额外 hwdb 条目。

构建过程中遇到并修掉的三处环境问题（都写进了脚本，换机器不会再踩）：

| 现象 | 原因 | 处理 |
|---|---|---|
| `tar` 退出码 2 | 沙箱拒绝 `utimensat()`，tar 恢复 mtime 失败即报错 | 解压加 `-m`（不恢复时间戳） |
| 大批文件 `rm -rf` 被拦 | 安全策略对 >50 文件删除要确认 | 原地覆盖解压；`meson` 用 `--reconfigure` 而非 `--wipe` |
| `Unknown compiler(s): g++` | libfprint 无条件声明 `['c','cpp']`，实际只有 `examples/cpp-test.cpp` 用 | `patch-libfprint.py` 在无 C++ 编译器时降级为纯 C 并移除该示例 |

另外补了一条 udev 规则 `driver/goodix51a0/70-goodix51a0.rules`，随库一起安装：

```udev
ACTION=="add|change", SUBSYSTEM=="spi", ENV{MODALIAS}=="acpi:GXFP51A0:*", \
  RUN{builtin}+="kmod load spi:spidev", \
  RUN+="/bin/sh -c 'echo spidev > %S%p/driver_override && echo %k > %S%p/subsystem/drivers/spidev/bind'"
```

内核里没有 claim 这个 ACPI 设备的驱动，所以必须像 libfprint 给 Elan 做的那样强制绑到
spidev。没有这条规则，`/dev/spidev*` 只在手工 `driver_override` 之后才出现，重启后
fprintd 就找不到设备。

### 7.8 安装时才发现：本机 `/usr` 是只读 overlay

Deepin 用的是 ostree/inmutable 布局：

```text
usr-overlay on /usr type overlay (ro,...)          ← 只读
etc-overlay on /etc type overlay (rw,...)          ← 可写
/dev/nvme0n1p10 on /usr/local type ext4 (rw,...)   ← 独立分区，可写
```

所以 `install -D ... /usr/lib/udev/rules.d/` 直接报 `只读文件系统`。库本身没问题
（`/usr/local` 是单独分区），只有 udev 规则落不下去。规则改放
`/etc/udev/rules.d/70-goodix51a0.rules` —— 这本来就是覆盖包自带规则的标准位置，
udev 读取优先级也高于 `/usr/lib`。安装步骤里同时加了 `udevadm control --reload-rules`
和 `udevadm trigger --subsystem-match=spi --action=change`，装完立刻重新绑定，不必等重启。

### 7.9 enroll 打通的真正关卡：MINDTCT 要背景边距，不是要放大（2026-11-24 17:05 真机）

第一次真机 enroll 全程 `Failed to detect minutiae: No minutiae found`，随后 fprintd
`Device reported an error during identify for enroll`。日志里还有一条误导性提示：

```text
Libfprint compiled without pixman support, impossible to resize
```

**坑一：`fpi_image_resize()` 在无 pixman 时是空操作，不是失败。**
libfprint 只在驱动列表里带 `aes3k` helper 时才 `set10('HAVE_PIXMAN', true)`。
我们的驱动列表只有 `goodix51a0`，所以 `#else` 分支走的是
`return g_object_ref (orig_img)` —— 返回原图，不报错。上一轮"改成 2× 放大 160×128"
的改动因此从未生效，帧还是 80×64。教训：看到这句 `fp_err` 不能只看返回值判空。

**坑二：放大本身就是错的方向。** MINDTCT 的方向图窗口固定 24 px，原生 80×64 的
脊线周期是 9 px（≈2.7 周期/窗口，正好在 DFT 能测的范围）；2× 之后周期 18 px，
只剩 1.3 周期，方向场反而测不出来。离线实测 10 张存量帧：

| 预处理 | 出点成功率 | 平均细节点 |
|---|---|---|
| 原生 80×64 | 0/10 | 0 |
| 最近邻 ×2 | 8/10 | 2.5 |
| 双线性 ×2 | 7/10 | 2.0 |
| 双线性 ×2 + 高斯 | 5/10 | 2.0 |
| 双线性 ×3 | 6/10 | 2.1 |

**真正的根因：没有背景边距。** MINDTCT 在 8 px 块网格上建图，会剔除接触图像边界的块
（`RMV_VALID_NBR_MIN = 3`）。80×64 帧被指纹铺满，没有"背景"可剔，剔完有效块不够，
`minutiae->num == 0`。把帧居中贴到灰色画布上就有东西可剔了：

| 画布 | 出点成功率 | 平均细节点 |
|---|---|---|
| pad 96 | 10/10 | 1.9 |
| **pad 128** | **10/10** | **7.9** |
| pad 144 / 192 / 256 | 10/10 | 8.1 |

`push_image()` 现在建 128×128 的 `FpImage`，80×64 帧贴到 (24, 32)（正好 8 px 块对齐），
四周填全图均值，`ppmm` 保持真实值 19.69 不乘倍数。`img_class->img_width/height = 128`。

真机结果：8 帧全部通过检测，7 个 enroll stage 一次走完，

```text
$ fprintd-list $USER
Fingerprints for user Xiang on Goodix GXFP51A0 (GF3658 / ST411) (press):
 - #0: right-index-finger
```

### 7.10 重激活后的 TLS 静默（真机 17:07 抓到）

verify 第一次抓帧成功，紧接着第二次 `activate()` 时：

```text
configure: target configured
tls: transport: no TLS record within 8000 ms (0 frames read)
Device reported an error during verify: could not establish the TLS session
```

`configure` 全部 ACK 通过，唯独 REQTLS 之后 MCU 一个字节都不回。原因是上一帧
10573 字节的 GET_IMAGE 响应没被完全消费，残留字节留在 SPI FIFO 里，把 REQTLS 交换
整体错位，MCU 认为收到的是垃圾帧因而不应答。

处理（`gx51_session_start_tls()`）：

1. `spi_drain()`：REQTLS 之前把 FIFO 读空（`gx51_read_frame_to(…, 5ms, …)` 读到超时为止）
2. REQTLS 后延时 20 ms → 50 ms
3. 握手全静默时重放一次完整序列（`hard_reset_cycle → configure → drain → REQTLS`），
   仅在失败路径多花 2 秒

另外把 `capture_worker` 里的 `report_finger(FALSE)` 提到 `wait_for_lift()` 之前 ——
image device 要看到这个信号才离开 `await-finger-off`，否则验证方要空等满 5 秒轮询窗口，
表现为每帧之间莫名其妙的停顿。

### 7.11 阈值标定：离线复现打分矩阵

真机 verify 一次只能拿到一个分数，还要 root + fprintd，根本无法用来定阈值。于是把整条
匹配链路搬到本地：`tools/test-match.c` + `tools/build-test-match.sh`（`./build/test-match`）。

做法与驱动完全一致：读 PGM → 贴到 128×128 均值画布 → `fp_image_detect_minutiae()` →
按 `lfs2nist_minutia_XYT()` 的规则转 xyt（`t = (270 − 11.25·dir) mod 360`，超过 180 折回
负数，按 x 再按 y 排序）→ 调 libfprint 树里真实的 Bozorth3 源码打分。之所以直接编译
`nbis/bozorth3/*.c` 而不是 dlsym：Bozorth3 的符号在 libfprint 里是 hidden 的，只有
`fp_image_*` 这几个公开 API 导出；这样顺带保证用的是打了 `MIN_COMPUTABLE_BOZORTH_MINUTIAE=4`
补丁的那份代码。

文件名即标签：`<finger>-<n>.pgm` 取最后一个 `-` 之前的部分做分组，同组算 genuine，
跨组算 impostor，最后打印矩阵和两个分布的最大值。

同指 10 帧（16:02 那批）实测：

| | 点数 | 说明 |
|---|---|---|
| finger-01/02/05/06/07/08/09/10 | 8–13 | 正常帧 |
| finger-03、finger-04 | 3、2 | 废帧，与任何帧都是 0 分 |

genuine 55 对，最高 20（02×03 除外），分布大致落在 5–20；`finger-diff.pgm` 是
`--tls-image` 顺带写的**背景差分图**，不是另一根手指，别拿它当 impostor 样本。

据此把 `bz3_threshold` 从 7 降到 6，`nr_enroll_stages` 从 7 提到 10：模板是每个 stage
一份 xyt 的集合，verify 取**最好**的那一条，条目越多越容易撞上高质量帧。impostor 分布
目前还没有真实数据，等第二根手指的抓图补上再定。

后续补测（`captures/index` 10 帧同指 + `captures/other` 2 帧异指，共 12 帧）：

```text
best score per capture            finger   other
finger-01..10                     14 20 0 0 20 16 9 18 18 13
other-01                           8        0
```

- genuine 一方按“最佳模板分”算，9–20，例外是 finger-03/04（2–3 个点的废帧，0 分）
- impostor 最高 8（other-01 对 finger-07），other-02 只有 2 个点、0 分
- 因此在 8 和 9 之间没有重叠区，阈值取 **10**：高于所有 impostor，低于 10 帧真指中的 9 帧

顺便排除了“图像处理不够好”这条线。`tools/variant-gen.py` 对 10 帧各生成 8 个变体
（pad128 / pad160、box blur、16×16 局部对比度归一、2×/3× 双线性），逐个跑 MINDTCT：

| 变体 | 平均点数 | 最高 |
|---|---|---|
| pad128-x3 | 8.7 | 24 |
| pad160 | 8.3 | 14 |
| **pad128（现用）** | **8.1** | **13** |
| pad128-x2 | 5.9 | 18 |
| pad128-lc | 5.5 | 8 |

没有任何变体把均值显著推高，说明**对这批 captures** 而言，8–10 个点已经是瓶颈，调图像
处理救不回来。注意这不是传感器的绝对上限：另一根手指的抓图能到 15–16 个点，差别在按压
与皮肤状态，不在流水线（见 §7.12）。

抓图脚本：`tools/capture-group.sh <group> <frames>`，自动停 fprintd、跑探头、把
`finger-NN.pgm` 重命名成 `<group>-NN.pgm`，正好对上 `test-match` 的分组规则。

### 7.12 首次 verify-match，但 genuine/impostor 完全重叠

真机第一次识别成功：

```text
$ timeout 90 fprintd-verify
Verify started!
Verifying: right-index-finger
Verify result: verify-match (done)
```

然而把异指抓图补齐后（`captures/other` 11 帧，确认为另一根未录入的手指），`test-match`
给出的两个分布是重叠的：

```text
best score per capture        finger   other
finger-01..10 (同指)           14 20 0 0 20 16 9 18 18 13
other-01..11 (异指)            11 20 12 14 7 11 17 12 8 0 20 15 12 4 17 16 17 12 6
```

同指最佳模板分 9–20，异指 0–20 —— **阈值取任何值都无法分开**。20 分不是偶然：异指的
`other-03/04` 对同指打分 20/15，与真指的 20/18 完全撞车。

原因可以从点数看出来。`other-03/04` 有 15、16 个细节点，它们互相打分是 51、51；同一个
点数的指纹对同指（8–10 点）只有 15–20 分。Bozorth3 的分数本质是“一致点对的数量”，两边
点数越多分越高，所以

```text
点数 8–10  × 点数 8–10   → 9–20    （真假都在这区间）
点数 15–16 × 点数 15–16  → 28–51
点数 15–16 × 点数 8–10   → 12–20
```

结论：**分离度由点数决定，不是阈值**。要让它变成能用的识别器，只有两条路：

1. 提高抓到的点数（重按压、加增益、等），让录入侧也进到 15+ 区间，真指分数自然抬到
   30–50，而异指仍在 12–20；
2. 在驱动里对每一帧做点数门控（`fp_image_detect_minutiae` 已经是对外 API），少于阈值
   就 `fpi_image_device_retry_scan()`，别把 2–3 点的废帧写进模板。

现状必须说清楚：**链路通了，但这套阈值不能用于解锁或鉴权**。当前 10 的阈值下，任何人都
有一半概率被误判为通过。

真机复核（`tools/two-fingers.sh` 第 5 步，验证 `right-index-finger` 但按右中指）：

```text
目标：right-index-finger
[通过] 匹配成功
=> match
```

两根手指都被错误归为同一人，与离线矩阵一致。**调阈值这条路到此为止。**

### 7.13 两根手指的交互脚本

驱动没法提示"现在按哪根手指"：fprintd 是系统服务，没有 tty，驱动的 `fprintf(stderr, ...)`
只进 journal。所以把交互提示放到用户侧的 `tools/two-fingers.sh`：

```bash
./tools/two-fingers.sh                       # 默认 右食指 + 右中指
FINGER1=right-thumb-finger FINGER2=left-index-finger ./tools/two-fingers.sh
```

5 步，每步前停下等 Enter：录入第 1 根 → 录入第 2 根 → 验证第 1 根 → 验证第 2 根 →
**验证第 1 根但按第 2 根**（安全性检查，应当不通过）。

### 7.14 四条提高分离度的路，全部实测走不通

真机第 5 步误配之后，先后试了四件事，结论都是负的，记下来免得以后再走：

**1. 同一次按压内多帧平均**（`probe --avg N`，`request_image_average()`）。右食指按 6 次、
每次连采 4 帧平均，落盘后点数从 8–10 掉到 **0–2**。同一按压内的多帧并不一致：手指在
按压过程中会微动，几帧叠加后脊线被抹平。逐帧曝光统计已经加进 probe
（`capture: exposure k/n range ...`），但即使修好也只影响画质，不解决点数太少。

**2. 背景差分**。用 `plain-background.bin` 减 `plain-finger.bin`（方向是 bg−fg，fg 更暗），
拉伸后跑 MINDTCT 只有 1 个点，比原始帧的 12 个点更差。

**3. 换个裁切看能不能多拿面积**。把 88×80 原始栅格逐列统计后发现：源列 64–87 恒为 0，
源行 0–79 全部有效，也就是说传感器真实有效阵面就是 64×80，现有解码取的正是这一块，
**没有浪费像素**。想多拿面积只能换传感器。

**4. 不用细节点，改用脊线图案相关**（`tools/match-corr.c`：归一化互相关，±12° 旋转、
±8 px 平移，粗到细搜索）。全帧相关和先高通再相关都试了：

```text
            own    cross
index-01..10  191..900   475..770
other-01..11  526..837   475..770
```

真指最差 191、异指最好 770，重叠得更厉害。原因很直接：4 mm 见方的窗口里只有一片近乎
平行的脊线，任何两根手指的纹理互相都相关到 0.6–0.8；而真指之间因为按压位置不同，重叠
区域常常几乎没有，最好的配对反而出现在异指之间。

### 7.15 结论

链路是通的（取图 → TLS → 模板 → 匹配 → `fprintd-enroll` / `-verify` 全部可用），
但**这套硬件在这个尺寸上做不到可靠的一对一验证**：

- 有效窗口 64×80 px ≈ 4.4×3.5 mm，约 10 个细节点，Bozorth3 的最小可算门槛是 10，
  等于一直在算法的临界点上工作；
- 换成图案相关同样分不开，说明不是算法选型问题，是信息量不够；
- 唯一还可能有救的是“按压位置一致性”，而 fprintd CLI 没有图像反馈，没法引导用户。

**不要把它设成解锁或 sudo 认证方式。** 想继续，只能改硬件（更大传感器）或自带引导
（自写前端显示实时图、要求用户对准），两者都超出本驱动的范围。

### 7.16 上一节的结论有误：那些“细节点”全是填充边框上的假点

上节把 8–10 个细节点当成真实特征，据此推了一堆结论。用 `tools/dump-minutiae`
把坐标打出来后，这条链子垮了。

驱动为了绕开 MINDTCT 的边界剔除，把 80×64 居中贴到 128×128 的画布上（偏移 24/32）。
逐帧统计坐标落点：

```text
frame         total  inside  outside      (真实区域 x 24..103, y 32..95)
finger-01..10   2..13      0    全部      other-01..11 同
```

22 帧、每一帧，**落在真实指纹区域内的细节点是 0**。所有点都贴在贴图的方框边界外
4–5 px（x≈20/106、y≈27/100）——MINDTCT 检测到的是“方框的边”，不是脊线端点。

这解释了此前所有反常现象：
- 为什么加了填充就从 0 个点变成 10/10 出点：出的是方框；
- 为什么 `enroll` 能一路 10/10：比的是方框；
- 为什么异指也能拿 20 分：两根手指的方框位置一样，边界点自然对得上。

**所以 §7.10–7.15 里所有基于细节点数的结论（点数上限、stage 数、阈值 10/6/7）全部作废。**

### 7.17 换成纹理匹配也不行（实测）

去掉细节点、直接比脊线纹理（`tools/match-patch.c`：16×16 分块、块内归一化、
全局位移投票，同指多块应投同一位移，异指应散票）：

```text
块大小/阈值      genuine 最差   impostor 最好
16 / 0.45            24            54      (63 块全票，完全饱和)
24 / 0.55             7            40
32 / 0.60             1            30
32 / 0.70             0            25
```

全部重叠，而且异指分数普遍更高。原因是几何的：460 dpi 下脊线周期 9 px，
一个 16×16 块只有 1.8 个周期——一段近乎正弦的条纹，跟任何别的条纹在某个位移上
都能相关到 0.45 以上。块内信息量不足，任何“相关”类算法都会失效。

顺带否掉了两个假设：
- **按压一致性不是瓶颈**：10 帧互相对齐，位移只有 0–14 px（0–0.8 mm）。
  所以“引导式前端”没有价值，别再往那个方向做。
- **同次按压多帧平均不是解法**：按 0–14 px 对齐后平均，细节点从 8–10 掉到 3，
  脊线被亚像素抖动抹平（`tools/align-avg.py`）。

### 7.18 剩下的唯一可信假设

Windows 能用，说明信息确实存在，只是**不在我们这条采集+匹配链路里**。两个待查：

1. **读取窗口**：`0x90` 下发的 256 字节配置里有几何字段。我们拿到的是 80×88 帧中
   的 80×64 有效区（其余 24 行恒 0）。若配置能开更大窗口，问题就解决了。
   查法：`0x80` 写寄存器后逐条回读，对照 vendor 仓库里的其它配置变体。
2. **多帧亚像素重建**：一次按压内取 4–8 帧，用亚像素位移做超分，把等效分辨率从
   460 dpi 抬到 ~700 dpi，让 MINDTCT 的方向窗口（24 px ≈ 2.7 周期）真正够用。
   代价大，但原理上成立。

在拿到其中一项的实测结果之前，不要再动匹配阈值。`fprintd-enroll` 每阶段只输出一句
`enroll-stage-passed`，脚本自己数数并打印 `[3/10] 已接受 —— 抬起，等 2 秒，再用同一根手指
按下去`，因为我们的驱动每次重激活都要重新握手 2–3 秒，不提示的话很容易连按。



---

## 8. 常用命令

```bash
# 编译 + 离线测试
make && make test

# 完整初始化 + 握手（基线回归）
sudo ./build/gx51-probe --configure --tls --verbose

# 加密取图（当前入口，按键时按上手指）
sudo ./build/gx51-probe --configure --tls-image --verbose

# 只读设备信息
sudo ./build/gx51-probe --info --verbose

# 恢复内核状态
echo spi-GXFP51A0:00 | sudo tee /sys/bus/spi/drivers/spidev/unbind
```

环境前提：`fprintd` 停止、`modprobe spidev`、传感器 bind 到 spidev、root 权限。

---

## 9. 硬性约束

1. 不执行 `0xF0`、不刷固件、不写 eFuse、不写 PMK
2. 不修改系统 libfprint，直到图像解码验收通过
3. 不动 BIOS、不写 `_DSM`、不碰 GPIO112
4. 不复制 Windows 驱动二进制或密钥文件
5. 密码登录不得被移除
6. 只面向 `GXFP51A0`

---

## 10. 文件清单

| 路径 | 说明 |
|---|---|
| `PLAN.md` | 原始计划、里程碑、回滚步骤 |
| `STATUS.md` | 本文档：进展、卡点、驱动写法 |
| `src/probe.c` | 真机探针，所有验证都从这里发起 |
| `tls-image-plain.bin` | 最近一次捕获的明文载荷（空帧，仅供结构分析） |
| `tools/decode_image.py` | 自动猜几何 + 解码成 PGM |
| `tls-clienthello.bin` | 传感器 ClientHello 原始记录 |
| `f2-*.bin` | F2 读回的原始帧（含工厂记录，**不要外传**） |
