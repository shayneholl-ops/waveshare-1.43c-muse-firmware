# wupsbr 板文件对照 — 1.43C（2026-10-26）

对照双方（均为 ESP-IDF v6.0.1，同一上游 SDK 结构）：

- 对方：`wupsbr/waveshare-muse-gadget-sdk` @ `2c64881`
  - `esp32/components/muse/boards/board_waveshare_s3_143c.c`
    （GitHub 文件 SHA `4494e33c64c01e8e1280e2ce6e7dfcb34ab4ff1b`）
  - `esp32/components/muse/boards/muse_lcd_bands.c`
    （GitHub 文件 SHA `17ac3cc800308516477588675f167ac681f1c2dc`）
- 我方：`~/workspace/firmware-rebuild/board_waveshare_s3_143c.c`（616 行）
  + SDK checkout `~/firmware-build/muse-gadget-sdk`（上游 main @ `c9354b7`，
  含本次 Edge TTS 改动）

## 完全一致的部分（不用动）

- 全部引脚：显示 QSPI D0–D3 GPIO9–12、CS15、SCK14、RST13、TE8；
  触摸 SDA47/SCL48、0x15、RST16、400 kHz、glitch 7、内部上拉；
  音频 MCLK38/SCLK39/LRCK40/DSDIN41/DSOUT42、功放 46、LDO 18；
  电池 ADC1_CH3/GPIO4、STAT GPIO7；BOOT GPIO0；LED GPIO5。
- SH8601 初始化命令 12 条中 11 条逐字节相同（FE 00、C4 80、3A 55、
  35 00、53 20、36 C0、63 FF、2A 0006 01D7、2B 0000 01D1、11、29）。
  唯一差别是亮度初值 0x51：我方 0xFF，对方 0x00（暗启动，UI 接管后
  无差别；可顺手改 0x00，省开机一闪）。
- 触摸：双轴镜像（LCD_RES − 原始值）、抬起事件（0x01）过滤、
  8 次采样平均、12 dB 衰减、曲线拟合校准、mic_slot −1（双麦混音）、
  ES7210 增益 3 dB 步进 + 33→34.5 特殊处理、panel_sleep 的 SLPIN/SLPOUT
  （0x10/0x11）+ 120 ms  settling，全部相同。
- 电池毫伏数：ADC ×2（200k/200k 分压）、STAT LOW=充电、100k 上拉，
  全部相同。毫伏是实测 ground truth，双方无分歧。

## 5 个 TODO 的结论

### 1. 屏 gap 6 vs 8 → 改成 8（对方赢）

- 我方：`set_gap(6, 0)` + `bands_set_gap(6, 0)`，注释里自己写了
  “BSP 用 8，图像偏 2 px 就换 8”。
- 对方：`LCD_GAP_X 8`，注释“as Waveshare's examples and factory
  firmware set it”，并解释：2A 窗口起点 6 是面板 GRAM 窗口，
  gap 是驱动另加的偏移，两者不是一回事。
- 结论：我方把“窗口起点 6”误当成了 gap。改两处 gap 调用 6→8。
  注意：圆屏上 2 px 偏移很难肉眼察觉，没发现偏不等于没偏。

### 2. Codec LDO GPIO18 极性 → 有源高，确认（我方原假设正确）

- 对方：`CODEC_EN` 注释“pulled up, driven high too”，init 里直接
  置 1。
- 我方 init 置 0、codec_power(true) 时置 1，最终状态一致，且实测
  codec 本来就能 probe（语音输入正常）。
- 结论：不用改。TODO 关闭。

### 3. NS4150B 供电轨 → 5.0 V（对方赢；既不是 3.3 也不是 VBAT 4.2）

- 我方：`pa_voltage = 3.3`，TODO 猜“VBAT 则约 4.2”。
- 对方：`pa_voltage = 5.0, codec_dac_voltage = 3.3`（对方头注释称
  引脚与驱动来自 Waveshare 例程与原理图）。
- 结论：改成 5.0。`pa_voltage` 只影响音量映射计算，不输出电压，
  改错不伤硬件；对方的原理图来源强于我方的猜测。若改后音量
  与 UI 百分比明显不符，再回头查原理图实测。

### 4. CST820 休眠命令 → 找到了（对方赢）

- 对方：`TP_REG_MODE 0xA5`，`0x03` = hibernate until reset；
  power_off 里 `reg_write(s_tp, 0xA5, 0x03)`。
- 我方 display_pause 只停 LVGL，TODO 正是缺这条命令。
- 结论：display_pause 可照此改：暂停时向 0xA5 写 0x03；恢复时
  必须复位触摸（hibernate 只有复位能唤醒），复位时序可沿用
  对方的 20/20/200 ms（我方现为 200/200/200，两者都能用）。
  收益：待机电流下降。

### 5. BOOT 提示位置 → 底部正中（对方赢，待肉眼确认）

- 我方：`{ LV_ALIGN_CENTER, 153, -129 }`，1.75C  rim 布局借来的。
- 对方：`{ LV_ALIGN_CENTER, 0, 190 }`，即底部正中（466 屏上
  y = 233 + 190 = 423，近底边）。
- 结论：改成对方的。提示只是 UI 指示，错了不影响功能；刷机后
  肉眼确认一次：提示应挨着实体 BOOT 键。

## 不建议照搬的两处（我方更好）

### 电池百分比曲线 → 留我方的分段线性表

- 对方是二次曲线（“Watcher's LiPo 曲线”）：
  `pct = (−v² + 9016v − 19189000) / 10000`。
- 实测点 4084 mV：对方给 95%，我方给 88%（与板子实测上报的
  88% 一致）。按标准锂电曲线（4.2 V=100%、4.0 V≈80%、
  3.7 V≈20%、3.6 V≈10%），对方在中段明显偏乐观
  （3.7 V 给 48%，3.6 V 给 31%），我方 12 点表更贴近。
- 结论：百分比只是估算，毫伏数双方一致；留我方曲线。

### USB 推断 → 留我方的简单版

- 我方：`usb = charging`。
- 对方：`usb = charging || 60 秒内充过 || 串口主机 || v ≥ 4080`
  （满电也算 USB，理由是“电池供电时 Muse 会 doze Wi-Fi”）。
- 结论：对方的版本会让满电电池也保持 Wi-Fi 常醒，更耗电。
  我方保守版更省电；除非 Wi-Fi dozing 造成实际问题，否则不改。

## 额外发现（不在原 5 项内）

### A. 防冻结修复：我方缺上游 PR #34，建议下一版带上

- 对方 bands 文件里有 `wait_band`：LVGL 的 flush 等待改成阻塞式，
  注释说明自旋等待会在 LVGL 持有两把互斥锁时饿死发送任务，
  导致屏幕约 20 分钟后冻住（即上游 PR #34）。
- 我方 checkout 的 `boards/muse_lcd_bands.c` 经 grep 确认没有
  `wait_band` / `flush_wait_cb`，仍是旧机制。
- 结论：下次编固件时把对方的 wait_band 机制移植过来。这是
  bands 文件改动，不动板文件；用户至今没报冻屏，但风险真实存在。

### B. power_off：对方有完整实现，我方为 NULL（可选）

- 对方 power_off：亮度 0 → 关屏 → 触摸休眠（0xA5=0x03）→
  功放 0 → 等 BOOT 松开 → 深睡，BOOT 按下唤醒。
- 我方 header 曾写“无已知软件关机路径”，故为 NULL，设置里
  因此没有 Power off。
- 结论：如想要设置里有电源关机，照对方 recipe 加；这是行为
  变化，不随本次修正默认带上。

### C. 小差异（可顺手，不重要）

- 触摸复位时序：对方 20/20/200 ms（称按 Waveshare 例程），
  我方 200/200/200 ms；两者都能用。
- 触摸钳制上限：对方 [0, 465]，我方上限 466（多 1 px），
  可顺手改成 465。
- 对方 init 时不碰 LED GPIO5、不接 TE；我方 LED 初始化为关、
  接 TE。无冲突。

## 建议采纳清单（下次编固件时）

1. 两处 gap 调用 6 → 8。
2. `pa_voltage` 3.3 → 5.0。
3. display_pause：暂停写 0xA5=0x03，恢复复位触摸。
4. talk_hint 改 `{ LV_ALIGN_CENTER, 0, 190 }`。
5. 亮度初值 0x51 改 0x00（可选，省开机一闪）。
6. 移植 wait_band 防冻结修复（bands 文件）。
7. 触摸钳制上限 466 → 465（可选）。

不采纳：对方电池百分比曲线、对方 USB 推断（含满电算 USB）。
暂不默认采纳：power_off 实现（行为变化，另议）。

## 重要提醒

以上任何一处改动都要重新编译，SHA 会变；此前已交付的 TTS
应用包（`muse-1.43c-tts-app.bin`，SHA `48acc84b…ec3d`）将作废，
需重发。不要在用户选定 OTA/esptool 之前悄悄重编。
