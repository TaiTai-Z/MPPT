# RT-Thread v2.0.0 交付说明

本工程没有继续沿用旧 v1.0.x 的自制最小器件层，而是直接复用裸机 v2.0.0
的官方 CMSIS、已验证 UART 寄存器配置、完整网表映射和统一安全层。

共享修复包括 PA9/Timer A2 与 PA10/Timer B1 映射、统一物理关断、1 ms
ADC/保护服务、完整校准状态、400 V 目标上限、真正的 MPPT 模式选择、单行
`Iin/Iout` FireWater，以及 96 KiB 普通 SRAM + 32 KiB CCM 分区。

RT 版的串口接收由官方 `USART1_IRQHandler` 缓存，用户主线程每 1 ms 消费；
周期保护使用 RT tick，不依赖 DWT。功率输出仍因实际硬件保护源和 15 V 反馈
缺失而锁定，不能通过修改一个宏绕过。HRTIM 的候选 100 kHz 时基也尚未完成
实板时钟/DLL校准和示波器测量，因此 `backend_ready` 保持 0。
