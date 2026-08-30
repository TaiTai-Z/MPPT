# Release notes — RT-Thread Safe v2.1.0

- 同步裸机 v2.1.0 的 170-MHz HSE/PLL、CSS、完整时钟读回、100-kHz HRTIM、ADC1/2逐周期注入采样和安全硬锁。
- RT main优先级从19调整到6、stack调整到2048 B。
- 新增优先级3的 `fault_guard` 与优先级20的 `telemetry`；main保持控制状态单写者。
- ADC1_2 ISR为 NVIC优先级2且不调用 RT API；快速采样与线程调度解耦。
- ADC双序列增加JQOVF/OVR、同半序列重复和跨周期拼接检查，异常原子锁存并立即关断；快采率按真实RT tick间隔换算。
- main增加5-ms快采无进展看门狗，两个ADC中断同时静默时也不再继续使用旧快照。
- 快采运行期间跳过会屏蔽中断约230 us的WS2812位带刷新，避免RT优先级无法补救的ADC队列溢出。
- 新增 `RTDIAG`、`CLOCKDIAG`、实测快采序列频率与追加式 `remade_master.md`。
- 当前交付证据为主机语法、内核单元和结构检查；未在本环境进行 ARMClang、下载或实板运行。
