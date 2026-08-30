#ifndef RT_CONFIG_H__
#define RT_CONFIG_H__

/* RT-Thread Nano 4.1.1, STM32G474RBT3 / Cortex-M4F. */
#define RT_NAME_MAX                    16
#define RT_ALIGN_SIZE                  4
#define RT_THREAD_PRIORITY_MAX         32
#define RT_TICK_PER_SECOND             1000

#define RT_USING_OVERFLOW_CHECK
#define RT_USING_CPU_FFS
#define RT_USING_OBJECT_NAME

/*
 * Required by RT-Thread 4.1.1 when building with ARM Compiler 6.
 * rtdef.h obtains va_list/va_start/va_arg from <stdarg.h> only when this
 * option is enabled.  GCC has a private fallback, ARMClang does not.
 */
#define RT_USING_LIBC

#define RT_USING_SEMAPHORE
#define RT_USING_MUTEX
#define RT_USING_EVENT

#define RT_USING_CONSOLE
#define RT_CONSOLEBUF_SIZE             128
#define RT_CONSOLE_DEVICE_NAME         "uart1"

#define RT_USING_USER_MAIN
#define RT_MAIN_THREAD_STACK_SIZE      4096
/* Main is the sole application-state/control writer.  START performs the
 * bounded calibration/readback path synchronously; the ArmClang call-chain
 * report is about 2.3 KiB, so 2 KiB was insufficient and caused a HardFault
 * followed by IWDG reset.  Keep this stack in CCM SRAM with margin. */
#define RT_MAIN_THREAD_PRIORITY        6

#define IDLE_THREAD_STACK_SIZE         256

#define ARCH_ARM
#define ARCH_ARM_CORTEX_M
#define ARCH_ARM_CORTEX_M4

#endif /* RT_CONFIG_H__ */
