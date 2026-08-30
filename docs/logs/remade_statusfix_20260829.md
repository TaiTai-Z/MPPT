# v2.3.0 续检附录（2026-08-29）

## 本轮发现

旧串口记录中的 `MPPT ARM LIMITED 150` 虽返回 OK，但随后 `CONTROL mode=OFF duty=0`、`HRTIM counters=0`、`sampling=0`、`power_outputs=0`。根因是 ARM 切换前关闭了 HRTIM/快速 ADC，而模式切换没有恢复 PWM 同步采样；5 ms 采样停滞监督随后执行功率级关断。

## 已完成修正

- `power_control_set_mode(CV/MPPT)` 在模式切换后恢复快速 ADC 采样 epoch；恢复失败保持 OFF、置 HRTIM 锁定并返回错误。
- `STATUS` 不再把 `gate=LOCKED` 写死，改为根据 `outputs_enabled`、PA2 inhibit 和 OE3 实际状态报告 `ARMED` 或 `LOCKED`。该修改只改善可观测性，不放宽保护条件。
- 外部保护配置保持有效；PA5 AUX 请求与功率级关断继续分离。

## 验证证据

- 静态检查：37 个工程条目、118 个向量、128-KiB Flash、外部保护配置、12 路 PWM/跨板网络检查通过。
- Keil ArmClang V6.24：`0 Error(s), 0 Warning(s)`；本次镜像 `Code=34134 RO-data=8442 RW-data=4740 ZI-data=9852`。
- ST-Link：`Erase Done. Programming Done. Verify OK. Application running ...`，20:45:00 完成。
- 本轮未占用 COM5，尚未进行修正后串口回读和示波器 PWM 物理验证。

## 上电前验证顺序

1. 手动复位后发送 `STATUS`，确认 `latch=0`、功率输出关闭。
2. 发送 `AUX ON`，随后 `AUX STATUS`，实测 MP9486 输出 15 V。
3. 发送 `MPPT ARM LIMITED 150`。
4. 立即读取 `CONTROL` 和 `HRTIMDIAG`：应看到 `mode=MPPT`，`cycle_sampling=1`，`fast_fault=0`，软启动后的 `duty_q15>0`，HRTIM `TA/TB` 计数器运行，`power_outputs=1`，且 `gate=ARMED`。
5. 首次功率验证仍需低压、限流、预充条件；先示波器确认 PA9/PA10 和驱动器输入，再逐级升压。
