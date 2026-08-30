#include "stm32g474_bare.h"
#include "power_control.h"
#include "board_config.h"
#include "board_hrtim.h"
#include "board_safety.h"
#include "board_clock.h"
#include "board_fast_adc.h"
#include "board_uart.h"
#include "board_cal_store.h"
#include <stdint.h>
#include <string.h>
#if defined(APP_USE_RTTHREAD)
#include <rtthread.h>
#define RT_FAULT_GUARD_PRIORITY 3U
#define RT_TELEMETRY_PRIORITY 20U
#define RT_FAULT_GUARD_STACK_SIZE 512U
#define RT_TELEMETRY_STACK_SIZE 384U
#endif

#define LINE_SIZE 96U
#define LOG_CAPACITY 32U
#define UART_FRAME_CAPACITY 1024U
#define HEARTBEAT_PERIOD_MS 1000UL
#define HEARTBEAT_DEFAULT_ENABLED 1UL

typedef enum { APP_STATE_BOOT = 0U, APP_STATE_SAFE_OFF, APP_STATE_READY,
               APP_STATE_FAULT, APP_STATE_RGB_MANUAL } app_state_t;
typedef struct {
    uint32_t code;
    uint32_t arg;
    uint32_t sequence;
    uint32_t uptime_ms;
    uint32_t state;
    uint32_t fault_bits;
    uint32_t raw_inputs;
    int32_t pv_mv;
    int32_t vbus_mv;
    int32_t current_ma;
    uint32_t sample_sequence;
} log_entry_t;

static uint8_t rgb[3] = {0U, 0U, 0U};
static uint32_t dwt_ok;
static app_state_t app_state = APP_STATE_BOOT;
static uint32_t fault_latched, fault_reasons, command_count, command_errors;
static uint32_t uart_errors, fault_count, log_sequence, event_log_head, log_count;
static uint32_t last_error_code, last_error_arg;
static log_entry_t event_log[LOG_CAPACITY]
    __attribute__((section(".ccmram"), aligned(8)));
static uint32_t control_fault_seen;
static uint32_t fault_input_seen;
static uint32_t reset_cause_flags;
static uint32_t heartbeat_enabled;
static uint32_t next_heartbeat;
static uint32_t calibration_dirty;
static uint32_t calibration_loaded;
static char uart_frame[UART_FRAME_CAPACITY];
static uint32_t uart_frame_length;
static uint32_t uart_frame_discard;
#if defined(APP_USE_RTTHREAD)
static struct rt_thread rt_fault_guard_thread;
static struct rt_thread rt_telemetry_thread;
static rt_uint8_t rt_fault_guard_stack[RT_FAULT_GUARD_STACK_SIZE]
    RT_SECTION(".ccmram") ALIGN(RT_ALIGN_SIZE);
static rt_uint8_t rt_telemetry_stack[RT_TELEMETRY_STACK_SIZE]
    RT_SECTION(".ccmram") ALIGN(RT_ALIGN_SIZE);
static volatile uint32_t rt_workers_started;
static volatile uint32_t rt_heartbeat_due;
static volatile uint32_t rt_fault_guard_raw;
static volatile uint32_t rt_fault_guard_fast;
extern struct rt_thread main_thread;
#endif
#if !defined(APP_USE_RTTHREAD)
static volatile uint32_t bare_tick_ms;
#endif

static void log_event(uint32_t code, uint32_t arg);

static uint32_t fault_code_is_persistent(uint32_t code)
{
    switch (code) {
    case POWER_ERROR_15V_UVLO:
    case POWER_ERROR_PV_OVP:
    case POWER_ERROR_VBUS_OVP:
    case POWER_ERROR_PHASE_A_OCP:
    case POWER_ERROR_PHASE_B_OCP:
    case POWER_ERROR_OTP_NTC1:
    case POWER_ERROR_OTP_NTC2:
    case POWER_ERROR_AUX_DROP:
    case POWER_ERROR_ADC_INIT:
    case POWER_ERROR_ADC_READ:
    case POWER_ERROR_CURRENT_SENSOR:
    case POWER_ERROR_NTC_SENSOR:
    case POWER_ERROR_HRTIM_FLT:
    case POWER_ERROR_HRTIM_EEV:
    case POWER_ERROR_PV_UVLO:
    case POWER_ERROR_CURRENT_IMBALANCE:
    case POWER_ERROR_CURRENT_STUCK:
    case POWER_ERROR_VBUS_BUILD_TIMEOUT:
    case POWER_ERROR_DUTY_SATURATION:
    case POWER_ERROR_POWER_LIMIT:
    case POWER_ERROR_SAMPLE_STALE:
        return 1UL;
    default:
        return 0UL;
    }
}

#if !defined(APP_USE_RTTHREAD)
void SysTick_Handler(void)
{
    ++bare_tick_ms;
}

static uint32_t app_time_init(void)
{
    bare_tick_ms = 0UL;
    return (SysTick_Config(board_clock_get_hz() / BOARD_SYSTICK_HZ) == 0UL) ?
           1UL : 0UL;
}

static uint32_t app_now_ms(void)
{
    return bare_tick_ms;
}
#else
static uint32_t app_time_init(void)
{
    return 1UL;
}

static uint32_t app_now_ms(void)
{
    return (uint32_t)rt_tick_get();
}
#endif

static void delay_cycles_fallback(uint32_t cycles)
{
    volatile uint32_t loops = (cycles + 2UL) / 3UL;
    while (loops-- != 0UL) __asm volatile ("nop");
}
static void delay_cycles(uint32_t cycles)
{
    uint32_t deadline;
    if (dwt_ok == 0UL) { delay_cycles_fallback(cycles); return; }
    deadline = DWT_CYCCNT + cycles;
    while ((int32_t)(DWT_CYCCNT - deadline) < 0) {}
}
static void dwt_init(void)
{
    volatile uint32_t i; uint32_t first;
    COREDEBUG_DEMCR |= (1UL << 24); DWT_CYCCNT = 0UL; DWT_CTRL |= 1UL;
    first = DWT_CYCCNT;
    for (i = 0UL; i < 64UL; ++i) __asm volatile ("nop");
    dwt_ok = (DWT_CYCCNT != first) ? 1UL : 0UL;
}
static void gpio_output(GPIO_TypeDef *port, uint32_t pin, uint32_t high)
{
    uint32_t shift = pin * 2U;
    port->BSRR = high ? (1UL << pin) : (1UL << (pin + 16U));
    port->MODER = (port->MODER & ~(3UL << shift)) | (1UL << shift);
    port->OTYPER &= ~(1UL << pin);
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << shift)) | (2UL << shift);
    port->PUPDR &= ~(3UL << shift);
}
static uint32_t gpio_read(GPIO_TypeDef *port, uint32_t pin)
{ return ((port->IDR & (1UL << pin)) != 0UL) ? 1UL : 0UL; }
static void uart_putc(char value)
{
    if (uart_frame_discard != 0UL) {
        if (value == '\n') {
            uart_frame_discard = 0UL;
            uart_frame_length = 0UL;
        }
        return;
    }
    if (uart_frame_length >= sizeof(uart_frame)) {
        uart_frame_discard = 1UL;
        uart_frame_length = 0UL;
        ++uart_errors;
        return;
    }
    uart_frame[uart_frame_length++] = value;
    if (value == '\n') {
        if (board_uart_write_atomic(uart_frame, uart_frame_length) !=
            uart_frame_length) ++uart_errors;
        uart_frame_length = 0UL;
    }
}

static void watchdog_init(void)
{
    uint32_t timeout = 100000UL;
    IWDG_KR = 0x5555UL;
    IWDG_PR = 3UL;       /* LSI / 32 */
    IWDG_RLR = 1999UL;   /* approximately 2 s at nominal 32 kHz LSI */
    while ((IWDG_SR & 7UL) != 0UL && timeout-- != 0UL) {}
    IWDG_KR = 0xCCCCUL;
    IWDG_KR = 0xAAAAUL;
}

static void watchdog_feed(void)
{
    IWDG_KR = 0xAAAAUL;
}

void power_control_watchdog_kick(void)
{
    watchdog_feed();
}
static void uart_puts(const char *text)
{ while (*text != '\0') uart_putc(*text++); }
static void put_u32(uint32_t value)
{
    char buffer[11]; uint32_t i = sizeof(buffer) - 1U;
    buffer[i] = '\0';
    do { buffer[--i] = (char)('0' + (value % 10UL)); value /= 10UL; } while (value != 0UL);
    uart_puts(&buffer[i]);
}
static void put_hex32(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF"; uint32_t shift;
    uart_puts("0x");
    for (shift = 28UL; ; shift -= 4UL)
    { uart_putc(digits[(value >> shift) & 0xFUL]); if (shift == 0UL) break; }
}
static void put_i32(int32_t value)
{
    if (value < 0) { uart_putc('-'); put_u32((uint32_t)(-(value + 1)) + 1UL); }
    else put_u32((uint32_t)value);
}

static void put_fixed_i32(int32_t value, uint32_t scale, uint32_t decimals)
{
    uint32_t magnitude, divisor;
    if (value < 0) { uart_putc('-'); magnitude = (uint32_t)(-(value + 1)) + 1UL; }
    else magnitude = (uint32_t)value;
    put_u32(magnitude / scale); uart_putc('.'); divisor = scale / 10UL;
    while (decimals-- != 0UL) { uart_putc((char)('0' + ((magnitude / divisor) % 10UL))); divisor /= 10UL; }
}
static int parse_u32(const char *text, uint32_t *value)
{
    uint32_t result = 0UL;
    if (*text < '0' || *text > '9') return 0;
    while (*text >= '0' && *text <= '9')
    {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (result > 400UL || (result * 10UL + digit) > 400UL) return 0;
        result = result * 10UL + digit;
    }
    if (*text != '\0' || value == (uint32_t *)0) return 0;
    *value = result;
    return 1;
}
static int parse_u32_limit(const char *text, uint32_t limit, uint32_t *value)
{
    uint32_t result = 0UL;
    if (*text < '0' || *text > '9') return 0;
    while (*text >= '0' && *text <= '9')
    {
        uint32_t digit = (uint32_t)(*text++ - '0');
        if (digit > limit || result > (limit - digit) / 10UL) return 0;
        result = result * 10UL + digit;
    }
    if (*text != '\0' || value == (uint32_t *)0) return 0;
    *value = result;
    return 1;
}
static int parse_four_u32(const char *text, const uint32_t limits[4],
                          uint32_t values[4])
{
    uint32_t i;
    for (i = 0UL; i < 4UL; ++i) {
        char field[12];
        uint32_t length = 0UL, j;
        while (*text == ' ') ++text;
        while (text[length] >= '0' && text[length] <= '9') ++length;
        if (length == 0UL || length >= sizeof(field)) return 0;
        for (j = 0UL; j < length; ++j) field[j] = text[j];
        field[length] = '\0';
        if (!parse_u32_limit(field, limits[i], &values[i])) return 0;
        text += length;
        if (i != 3UL) {
            if (*text != ' ') return 0;
            while (*text == ' ') ++text;
        }
    }
    return *text == '\0';
}

static int parse_two_u32(const char *text, uint32_t limit,
                         uint32_t values[2])
{
    uint32_t i;
    for (i = 0UL; i < 2UL; ++i)
    {
        char field[12];
        uint32_t length = 0UL;
        while (*text == ' ') ++text;
        while (text[length] >= '0' && text[length] <= '9') ++length;
        if (length == 0UL || length >= sizeof(field)) return 0;
        {
            uint32_t j;
            for (j = 0UL; j < length; ++j) field[j] = text[j];
            field[length] = '\0';
        }
        if (!parse_u32_limit(field, limit, &values[i])) return 0;
        text += length;
        if (i == 0UL) {
            if (*text != ' ') return 0;
            while (*text == ' ') ++text;
        }
    }
    return *text == '\0';
}

static void ws_wait_until(uint32_t deadline)
{
    while ((int32_t)(DWT_CYCCNT - deadline) < 0) {}
}
static void ws_byte(uint32_t value)
{
    uint32_t bit;
    for (bit = 0UL; bit < 8UL; ++bit) {
        uint32_t start = DWT_CYCCNT;
        uint32_t high_cycles = ((value & 0x80UL) != 0UL) ? 119UL : 60UL;
        GPIOC->BSRR = 1UL << WS_PIN;
        ws_wait_until(start + high_cycles);
        GPIOC->BSRR = 1UL << (WS_PIN + 16U);
        ws_wait_until(start + 213UL);
        value <<= 1;
    }
}
static uint32_t percent_to_byte(uint32_t percent)
{ return (percent * 255UL + 50UL) / 100UL; }
static void ws_show(void)
{
    uint32_t irq_state;
    /* Do not mask interrupts for a WS2812 frame while the 10-us injected
     * sampling period is active; doing so would overflow the ADC queue. */
    if (dwt_ok == 0UL || board_clock_is_170mhz() == 0UL ||
        board_hrtim_sampling_is_running() != 0UL) return;
    irq_state = __get_PRIMASK();
    __disable_irq();
    GPIOC->BSRR = 1UL << (WS_PIN + 16U); delay_cycles(17000UL);
    ws_byte(percent_to_byte(rgb[1])); ws_byte(percent_to_byte(rgb[0])); ws_byte(percent_to_byte(rgb[2]));
    GPIOC->BSRR = 1UL << (WS_PIN + 16U); delay_cycles(17000UL);
    if (irq_state == 0UL) __enable_irq();
}
static void safe_outputs_off(void)
{
    board_safety_force_off();
}
static void safe_power_outputs_off(void)
{
    board_safety_force_power_off();
}
static uint32_t read_fault_inputs(void)
{
    return board_safety_read_fault_inputs();
}
#if defined(APP_USE_RTTHREAD)
static void rt_fault_guard_entry(void *parameter)
{
    (void)parameter;
    for (;;) {
        uint32_t raw = read_fault_inputs();
        uint32_t fast = board_fast_adc_fault_flags();
        if (raw != 0UL || fast != 0UL) {
            /* No RT object is touched by the ADC ISR.  This thread adds a
             * 1-ms physical-input guard and leaves all application-state
             * mutation to the priority-6 owner thread. */
            safe_power_outputs_off();
            rt_fault_guard_raw = raw;
            rt_fault_guard_fast = fast;
        } else {
            rt_fault_guard_raw = 0UL;
            rt_fault_guard_fast = 0UL;
        }
        rt_thread_mdelay(1);
    }
}

static void rt_telemetry_entry(void *parameter)
{
    (void)parameter;
    for (;;) {
        rt_thread_mdelay(1000);
        rt_heartbeat_due = 1UL;
    }
}

static uint32_t rt_start_workers(void)
{
    rt_err_t result;
    result = rt_thread_init(&rt_fault_guard_thread, "fault_guard",
                            rt_fault_guard_entry, RT_NULL,
                            rt_fault_guard_stack,
                            sizeof(rt_fault_guard_stack),
                            RT_FAULT_GUARD_PRIORITY, 1U);
    if (result != RT_EOK) return 0UL;
    result = rt_thread_init(&rt_telemetry_thread, "telemetry",
                            rt_telemetry_entry, RT_NULL,
                            rt_telemetry_stack,
                            sizeof(rt_telemetry_stack),
                            RT_TELEMETRY_PRIORITY, 1U);
    if (result != RT_EOK) return 0UL;
    if (rt_thread_startup(&rt_fault_guard_thread) != RT_EOK) return 0UL;
    if (rt_thread_startup(&rt_telemetry_thread) != RT_EOK) return 0UL;
    rt_workers_started = 1UL;
    return 1UL;
}
#endif
static void log_event(uint32_t code, uint32_t arg)
{
    log_entry_t *entry = &event_log[event_log_head];
    power_control_status_t status;
    power_control_get_status(&status);
    entry->code = code;
    entry->arg = arg;
    entry->sequence = ++log_sequence;
    entry->uptime_ms = app_now_ms();
    entry->state = (uint32_t)app_state;
    entry->fault_bits = status.fault_bits;
    entry->raw_inputs = read_fault_inputs();
    entry->pv_mv = status.pv_mv;
    entry->vbus_mv = status.vbus_mv;
    entry->current_ma = status.pv_current_ma;
    entry->sample_sequence = status.sample_sequence;
    if (fault_code_is_persistent(code) != 0UL) {
        board_fault_record_t persistent;
        persistent.code = code;
        persistent.arg = arg;
        persistent.uptime_ms = entry->uptime_ms;
        persistent.state = entry->state;
        persistent.fault_bits = entry->fault_bits;
        persistent.raw_inputs = entry->raw_inputs;
        persistent.pv_mv = entry->pv_mv;
        persistent.vbus_mv = entry->vbus_mv;
        persistent.current_ma = entry->current_ma;
        persistent.sample_sequence = entry->sample_sequence;
        (void)board_fault_store_append(&persistent);
    }
    event_log_head = (event_log_head + 1UL) % LOG_CAPACITY;
    if (log_count < LOG_CAPACITY) ++log_count;
}
static const char *state_name(void)
{
    switch (app_state) { case APP_STATE_BOOT: return "BOOT"; case APP_STATE_SAFE_OFF: return "SAFE_OFF";
    case APP_STATE_READY: return "READY"; case APP_STATE_FAULT: return "FAULT";
    case APP_STATE_RGB_MANUAL: return "RGB_MANUAL"; default: return "UNKNOWN"; }
}
static void show_state_rgb(app_state_t state)
{
    if (state == APP_STATE_BOOT) { rgb[0] = 0U; rgb[1] = 0U; rgb[2] = 12U; }
    else if (state == APP_STATE_SAFE_OFF) { rgb[0] = 0U; rgb[1] = 4U; rgb[2] = 12U; }
    else if (state == APP_STATE_READY) { rgb[0] = 0U; rgb[1] = 12U; rgb[2] = 0U; }
    else if (state == APP_STATE_FAULT) { rgb[0] = 40U; rgb[1] = 0U; rgb[2] = 0U; }
    else return;
    ws_show();
}
static void latch_fault(uint32_t raw)
{
    uint32_t newly = raw & ~fault_input_seen;
    fault_input_seen = raw;
    if (newly == 0UL) return;
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    safe_outputs_off(); fault_latched = 1UL; fault_reasons |= newly; ++fault_count;
    power_control_set_external_fault_latched(1UL);
    app_state = APP_STATE_FAULT; show_state_rgb(APP_STATE_FAULT);
    log_event((newly & 0xC0UL) ? POWER_ERROR_HRTIM_EEV : POWER_ERROR_HRTIM_FLT, newly);
}
static char ascii_upper(char value)
{ return (value >= 'a' && value <= 'z') ? (char)(value - ('a' - 'A')) : value; }
static int text_equal_ci(const char *left, const char *right)
{
    while (*left && *right) { if (ascii_upper(*left++) != ascii_upper(*right++)) return 0; }
    return (*left == '\0' && *right == '\0') ? 1 : 0;
}
static int text_starts_ci(const char *text, const char *prefix)
{
    while (*prefix) { if (!*text || ascii_upper(*text++) != ascii_upper(*prefix++)) return 0; }
    return 1;
}
static void trim_command(char *text)
{
    char *begin = text;
    char *end;
    if (text == (char *)0) return;
    while (*begin == ' ' || *begin == '\t') ++begin;
    if (begin != text) memmove(text, begin, strlen(begin) + 1UL);
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) --end;
    *end = '\0';
}
static uint32_t command_hash(const char *text)
{
    uint32_t hash = 2166136261UL;
    while (*text != '\0') { hash ^= (uint8_t)*text++; hash *= 16777619UL; }
    return hash;
}
static void record_command_error(uint32_t code, uint32_t arg)
{
    ++command_errors; last_error_code = code; last_error_arg = arg; log_event(code, arg);
}
static int parse_rgb(const char *text, uint8_t output[3])
{
    uint32_t i;
    for (i = 0UL; i < 3UL; ++i)
    {
        uint32_t value = 0UL;
        if (*text < '0' || *text > '9') return 0;
        while (*text >= '0' && *text <= '9') { value = value * 10UL + (uint32_t)(*text++ - '0'); if (value > 100UL) return 0; }
        output[i] = (uint8_t)value; if (i != 2UL && *text++ != ',') return 0;
    }
    return *text == '\0';
}
static void print_rgb(void)
{ uart_puts("RGB "); put_u32(rgb[0]); uart_putc(','); put_u32(rgb[1]); uart_putc(','); put_u32(rgb[2]); uart_puts("\r\n"); }
static void print_status(void)
{
    power_control_status_t status;
    power_control_get_status(&status);
    uart_puts("STATUS state="); uart_puts(state_name()); uart_puts(" latch="); put_u32(fault_latched);
    uart_puts(" reason="); put_hex32(fault_reasons); uart_puts(" raw="); put_hex32(read_fault_inputs());
    uart_puts(" cmd="); put_u32(command_count); uart_puts(" err="); put_u32(command_errors);
    uart_puts(" uart_err="); put_u32(uart_errors); uart_puts(" faults="); put_u32(fault_count);
    uart_puts(" log_count="); put_u32(log_count);
    uart_puts(" cal_loaded="); put_u32(calibration_loaded);
    uart_puts(" cal_dirty="); put_u32(calibration_dirty);
    uart_puts(" last_err="); put_hex32(last_error_code); uart_puts(" last_arg="); put_hex32(last_error_arg);
    uart_puts(" reset="); put_hex32(reset_cause_flags);
    uart_puts(" hb="); put_u32(heartbeat_enabled);
    uart_puts(" gate=");
    uart_puts((status.outputs_enabled != 0UL &&
               gpio_read(GPIOA, GATE_INHIBIT_PIN) == 0UL &&
               gpio_read(GPIOC, OE3_PIN) == 0UL) ? "ARMED" : "LOCKED");
    uart_puts(" aux15v=");
    uart_puts(board_aux_get_request() != 0UL ? "REQUESTED_EXTERNAL_CONFIRM" :
             "OFF_EXTERNAL_CONFIRM");
    uart_puts(" ext_latch=");
    {
        power_control_status_t latch_status;
        power_control_get_status(&latch_status);
        put_u32(latch_status.external_fault_latched);
    }
    uart_puts(" pa5_odr="); put_u32((GPIOA->ODR >> AUX_ENABLE_PIN) & 1UL);
    uart_puts(" pa5_idr="); put_u32(gpio_read(GPIOA, AUX_ENABLE_PIN)); uart_puts("\r\n"); print_rgb();
    {
        uart_puts("CONTROL mode="); uart_puts(power_control_mode_name(status.mode));
        uart_puts(" adc="); put_u32(status.adc_backend_ready);
        uart_puts(" cal="); put_u32(status.calibration_valid);
        uart_puts(" vcal="); put_u32(status.voltage_calibration_valid);
        uart_puts(" hrtim="); put_u32(status.hrtim_backend_ready);
        uart_puts(" duty="); put_u32(status.duty_q15);
        uart_puts(" I1_mA="); put_i32(status.i1_ma);
        uart_puts(" I2_mA="); put_i32(status.i2_ma);
        uart_puts(" faults="); put_hex32(status.fault_bits); uart_puts("\r\n");
    }
}
static void print_state_frame(void)
{
    power_control_status_t status;
    uint32_t raw = read_fault_inputs();
    power_control_get_status(&status);
    uart_puts("STATE state="); uart_puts(state_name());
    uart_puts(" mode="); uart_puts(power_control_mode_name(status.mode));
    uart_puts(" target_mV="); put_u32(status.target_mv);
    uart_puts(" pv_ref_mV="); put_u32(status.pv_reference_mv);
    uart_puts(" duty_q15="); put_u32(status.duty_q15);
    uart_puts(" proposed_q15="); put_u32(status.proposed_duty_q15);
    uart_puts(" outputs="); put_u32(status.outputs_enabled);
    uart_puts(" gate_inhibit="); put_u32(gpio_read(GPIOA, GATE_INHIBIT_PIN));
    uart_puts(" aux_enable="); put_u32(gpio_read(GPIOA, AUX_ENABLE_PIN));
    uart_puts(" oe1="); put_u32(gpio_read(GPIOB, OE1_PIN));
    uart_puts(" oe2="); put_u32(gpio_read(GPIOC, OE2_PIN));
    uart_puts(" oe3="); put_u32(gpio_read(GPIOC, OE3_PIN));
    uart_puts(" flt="); put_hex32(raw & 0x3FUL);
    uart_puts(" eev="); put_hex32((raw >> 6) & 0x3UL);
    uart_puts(" latch="); put_u32(fault_latched);
    uart_puts(" reason="); put_hex32(fault_reasons);
    uart_puts(" control_fault="); put_hex32(status.fault_bits);
    uart_puts(" adc_ready="); put_u32(status.adc_backend_ready);
    uart_puts(" adc_mask="); put_hex32(status.adc_ready_mask);
    uart_puts(" adc_cal="); put_u32(status.calibration_valid);
    uart_puts(" current_cal="); put_u32(status.current_calibration_valid);
    uart_puts(" polarity_cal="); put_u32(status.current_polarity_valid);
    uart_puts(" voltage_cal="); put_u32(status.voltage_calibration_valid);
    uart_puts(" zero_I1_mV="); put_u32(status.current_zero_i1_mv);
    uart_puts(" zero_I2_mV="); put_u32(status.current_zero_i2_mv);
    uart_puts(" gain1_mV_per_A="); put_u32(status.current_gain_i1_mv_per_a);
    uart_puts(" gain2_mV_per_A="); put_u32(status.current_gain_i2_mv_per_a);
    uart_puts(" polarity1="); put_i32(status.current_polarity_i1);
    uart_puts(" polarity2="); put_i32(status.current_polarity_i2);
    uart_puts(" pv_gain_ppm="); put_u32(status.pv_gain_ppm);
    uart_puts(" vbus_gain_ppm="); put_u32(status.vbus_gain_ppm);
    uart_puts(" v15_sense="); put_u32(status.v15_sense_valid);
    uart_puts(" hrtim_ready="); put_u32(status.hrtim_backend_ready);
    uart_puts(" protect_ready="); put_u32(status.protection_backend_ready);
    uart_puts(" sample_seq="); put_u32(status.sample_sequence);
    uart_puts("\r\n");
}
static void print_samples(void)
{
    static const char *names[POWER_ADC_CHANNEL_COUNT] = {
        "PA0_I1", "PA1_I2", "PA3_NTC1", "PA6_NTC2", "PA7_SIG",
        "PC4_SIG", "PC0_VIN", "PC1_VO", "PB1_SIG"
    };
    power_sample_t sample;
    uint32_t i;
    power_control_get_sample(&sample);
    uart_puts("SAMPLES seq="); put_u32(sample.sequence);
    uart_puts(" valid="); put_hex32(sample.valid_mask);
    uart_puts(" faults="); put_hex32(sample.fault_bits);
    uart_puts(" conversions="); put_u32(sample.conversion_count); uart_puts("\r\n");
    for (i = 0UL; i < POWER_ADC_CHANNEL_COUNT; ++i)
    {
        uart_puts("ADC "); uart_puts(names[i]); uart_puts(" raw="); put_u32(sample.raw[i]);
        uart_puts(" pin_mV="); put_u32(sample.pin_mv[i]); uart_puts("\r\n");
    }
    uart_puts("ADC NOTE=Vin/Vo/current/temp are nominal estimates until end-to-end calibration\r\n");
}
static void print_control(void)
{
    power_control_status_t status;
    power_control_get_status(&status);
    uart_puts("CONTROL mode="); uart_puts(power_control_mode_name(status.mode));
    uart_puts(" target_mV="); put_u32(status.target_mv);
    uart_puts(" pv_ref_mV="); put_u32(status.pv_reference_mv);
    uart_puts(" I1_pin_mV="); put_i32(status.ia_pin_mv);
    uart_puts(" I2_pin_mV="); put_i32(status.ib_pin_mv);
    uart_puts(" VPV_pin_mV="); put_i32(status.pv_pin_mv);
    uart_puts(" VBUS_pin_mV="); put_i32(status.vbus_pin_mv); uart_puts("\r\n");
    uart_puts("CONTROL duty_q15="); put_u32(status.duty_q15);
    uart_puts(" proposed_q15="); put_u32(status.proposed_duty_q15);
    uart_puts(" adc_ready_mask="); put_hex32(status.adc_ready_mask);
    uart_puts(" cal_valid="); put_u32(status.calibration_valid);
    uart_puts(" current_cal="); put_u32(status.current_calibration_valid);
    uart_puts(" polarity_cal="); put_u32(status.current_polarity_valid);
    uart_puts(" voltage_cal="); put_u32(status.voltage_calibration_valid);
    uart_puts(" v15_sense="); put_u32(status.v15_sense_valid);
    uart_puts(" hrtim_ready="); put_u32(status.hrtim_backend_ready);
    uart_puts(" protection_ready="); put_u32(status.protection_backend_ready);
    uart_puts(" ext_latch="); put_u32(status.external_fault_latched);
    uart_puts(" outputs="); put_u32(status.outputs_enabled);
    uart_puts(" faults="); put_hex32(status.fault_bits); uart_puts("\r\n");
    uart_puts("CONTROL pv_mV="); put_i32(status.pv_mv);
    uart_puts(" vbus_mV="); put_i32(status.vbus_mv);
    uart_puts(" vbus_ovp_mV="); put_u32(status.vbus_ovp_limit_mv);
    uart_puts(" dynamic_ovp_mV="); put_u32(status.vbus_dynamic_ovp_limit_mv);
    uart_puts(" vbus_ovp_raw="); put_u32(status.vbus_ovp_raw_threshold);
    uart_puts(" pv_ovp_mV="); put_u32(status.pv_ovp_limit_mv);
    uart_puts(" ocp_mA="); put_u32(status.ocp_limit_ma);
    uart_puts(" uvlo15_mV="); put_u32(status.uvlo_15v_limit_mv); uart_puts("\r\n");
    uart_puts("CONTROL I1_zero_mV="); put_u32(status.current_zero_i1_mv);
    uart_puts(" I2_zero_mV="); put_u32(status.current_zero_i2_mv);
    uart_puts(" gain1_mV_per_A="); put_u32(status.current_gain_i1_mv_per_a);
    uart_puts(" gain2_mV_per_A="); put_u32(status.current_gain_i2_mv_per_a);
    uart_puts(" polarity1="); put_i32(status.current_polarity_i1);
    uart_puts(" polarity2="); put_i32(status.current_polarity_i2);
    uart_puts(" pv_gain_ppm="); put_u32(status.pv_gain_ppm);
    uart_puts(" vbus_gain_ppm="); put_u32(status.vbus_gain_ppm);
    uart_puts(" I1_mA="); put_i32(status.i1_ma);
    uart_puts(" I2_mA="); put_i32(status.i2_ma);
    uart_puts(" I1_est_mA="); put_i32(status.i1_est_ma);
    uart_puts(" I2_est_mA="); put_i32(status.i2_est_ma);
    uart_puts(" current_mA="); put_i32(status.pv_current_ma);
    uart_puts(" power_mW="); put_i32(status.pv_power_mw); uart_puts("\r\n");
    uart_puts("CONTROL filtered_pv_mV="); put_i32(status.pv_filtered_mv);
    uart_puts(" filtered_iin_mA="); put_i32(status.pv_current_filtered_ma);
    uart_puts(" filtered_power_mW="); put_i32(status.pv_power_filtered_mw);
    uart_puts(" soft_q15="); put_u32(status.softstart_limit_q15);
    uart_puts(" vbus_limit="); put_u32(status.vbus_limit_active);
    uart_puts(" boost_reachable="); put_u32(status.boost_reachable);
    uart_puts(" gains_validated="); put_u32(status.controller_gains_validated);
    uart_puts(" cmp_write="); put_u32(status.compare_write_ok); uart_puts("\r\n");
    uart_puts("CONTROL fast_seq="); put_u32(status.fast_sample_sequence);
    uart_puts(" adc_irq="); put_u32(status.fast_adc_irq_count);
    uart_puts(" incomplete="); put_u32(status.fast_adc_incomplete_count);
    uart_puts(" fast_fault="); put_hex32(status.fast_adc_fault_flags);
    uart_puts(" cycle_sampling="); put_u32(status.per_cycle_sampling_running);
    uart_puts(" measured_hz="); put_u32(status.fast_sample_rate_hz);
    uart_puts(" requested_hz=100000\r\n");
    uart_puts("CONTROL Tvalid="); put_hex32(status.temperature_valid_mask);
    uart_puts(" T1_cdeg="); put_i32(status.ntc1_cdeg);
    uart_puts(" T2_cdeg="); put_i32(status.ntc2_cdeg);
    uart_puts(" trip_cdeg="); put_i32(status.ntc_otp_trip_cdeg);
    uart_puts(" recover_cdeg="); put_i32(status.ntc_otp_recover_cdeg);
    uart_puts(" ntc1_bad="); put_u32(status.ntc1_invalid_count);
    uart_puts(" ntc2_bad="); put_u32(status.ntc2_invalid_count);
    uart_puts(" temp_model=10k_B3950_4k42\r\n");
    uart_puts("CONTROL limiter="); put_u32(status.current_limit_active);
    uart_puts(" imbalance_ms="); put_u32(status.current_imbalance_ms);
    uart_puts(" stuck_ms="); put_u32(status.current_stuck_ms);
    uart_puts(" build_ms="); put_u32(status.vbus_build_elapsed_ms);
    uart_puts(" saturation_ms="); put_u32(status.duty_saturation_ms);
    uart_puts(" power_limit_ms="); put_u32(status.power_limit_ms);
    uart_puts(" mppt_perturb="); put_u32(status.mppt_perturb_count);
    uart_puts(" mppt_invalid="); put_u32(status.mppt_invalid_sample_count);
    uart_puts(" last_stop_fault="); put_hex32(status.last_stop_fault_bits);
    uart_puts(" last_stop_fast="); put_hex32(status.last_stop_fast_fault_flags);
    uart_puts(" last_stop_duty="); put_u32(status.last_stop_duty_q15);
    uart_puts(" last_stop_ms="); put_u32(status.last_stop_time_ms);
    uart_puts("\r\n");
}

static void print_hrtim_diag(void)
{
    board_hrtim_diag_t diag;
    board_fast_adc_snapshot_t fast = {0};
    board_hrtim_get_diag(&diag);
    uart_puts("HRTIM profile="); put_u32(diag.profile);
    uart_puts(" backend_ready="); put_u32(diag.backend_ready);
    uart_puts(" timing="); put_u32(diag.timing_configured);
    uart_puts(" clock_validated="); put_u32(diag.clock_validated);
    uart_puts(" requested_hz="); put_u32(diag.requested_pwm_hz);
    uart_puts(" psc_code="); put_u32(diag.prescaler_code);
    uart_puts(" protect="); put_u32(diag.protection_ready);
    uart_puts(" arming_compiled="); put_u32(diag.arming_compiled);
    uart_puts(" counters="); put_hex32(diag.counters_enabled);
    uart_puts(" disabled="); put_hex32(diag.output_disable_status);
    uart_puts(" pwm_high_z="); put_u32(diag.pwm_pins_high_z);
    uart_puts(" period="); put_u32(diag.period_ticks);
    uart_puts(" compare="); put_u32(diag.compare_ticks);
    uart_puts(" adc_trigger="); put_u32(diag.adc_trigger_configured);
    uart_puts(" dll="); put_u32(diag.dll_ready);
    uart_puts(" sampling="); put_u32(diag.sampling_running);
    uart_puts(" hrclk_MHz="); put_u32(diag.effective_hr_clock_mhz);
    uart_puts(" calculated_hz="); put_u32(diag.calculated_pwm_hz);
    uart_puts(" dma="); put_u32(diag.dma_configured);
    uart_puts(" deadtime_applicable="); put_u32(diag.deadtime_applicable);
    uart_puts(" phase_ticks="); put_u32(diag.phase_shift_ticks);
    uart_puts(" power_outputs="); put_u32(diag.power_outputs_enabled);
    uart_puts(" map=CHE2<-PA9/TA2,CHF1<-PA10/TB1\r\n");
    uart_puts("HRTIMARM attempts="); put_u32(diag.arm_attempts);
    uart_puts(" fail_stage="); put_u32(diag.arm_fail_stage);
    uart_puts(" oen="); put_hex32(diag.output_enable_readback);
    uart_puts(" int_hi="); put_hex32(diag.internal_seen_high);
    uart_puts(" int_lo="); put_hex32(diag.internal_seen_low);
    uart_puts(" pad_hi="); put_hex32(diag.pad_seen_high);
    uart_puts(" pad_lo="); put_hex32(diag.pad_seen_low);
    uart_puts(" outa="); put_hex32(diag.timer_a_output_reg);
    uart_puts(" outb="); put_hex32(diag.timer_b_output_reg);
    uart_puts(" gate_fail="); put_hex32(diag.arm_gate_fail_mask);
    uart_puts(" stages=1:gate,2:AF_OEN,3:counter,4:internal_edge,5:pad_edge\r\n");
    (void)board_fast_adc_get_snapshot(&fast);
    uart_puts("FASTADC sequence="); put_u32(fast.sequence);
    uart_puts(" irq="); put_u32(fast.irq_count);
    uart_puts(" starts="); put_u32(fast.start_count);
    uart_puts(" start_fail="); put_u32(fast.start_failure_count);
    uart_puts(" armed="); put_hex32(fast.armed_mask);
    uart_puts(" last_adc1_cr="); put_hex32(fast.last_start_adc1_cr);
    uart_puts(" last_adc2_cr="); put_hex32(fast.last_start_adc2_cr);
    uart_puts(" faults="); put_hex32(fast.fault_flags);
    uart_puts(" incomplete="); put_u32(fast.incomplete_count);
    uart_puts(" ntc1_raw="); put_u32(fast.ntc1_raw);
    uart_puts(" ntc2_raw="); put_u32(fast.ntc2_raw);
    uart_puts("\r\n");
}
static void print_clock_diag(void)
{
    board_clock_diag_t diag;
    board_clock_get_diag(&diag);
    uart_puts("CLOCK source=");
    uart_puts(diag.source == BOARD_CLOCK_SOURCE_HSE_PLL_170M ?
              "HSE8_PLL170" : "HSI16_FALLBACK");
    uart_puts(" sys_hz="); put_u32(diag.system_core_hz);
    uart_puts(" hse_ready="); put_u32(diag.hse_ready);
    uart_puts(" pll_ready="); put_u32(diag.pll_ready);
    uart_puts(" boost="); put_u32(diag.range1_boost);
    uart_puts(" flash_ws="); put_u32(diag.flash_latency);
    uart_puts(" css="); put_u32(diag.css_enabled);
    uart_puts(" fallback="); put_u32(diag.fallback_active);
    uart_puts(" error="); put_u32(diag.error); uart_puts("\r\n");
}
#if defined(APP_USE_RTTHREAD)
static uint32_t rt_stack_unused_bytes(rt_thread_t thread)
{
    const rt_uint8_t *bytes;
    uint32_t unused = 0UL;
    if (thread == RT_NULL || thread->stack_addr == RT_NULL) return 0UL;
    bytes = (const rt_uint8_t *)thread->stack_addr;
    while (unused < thread->stack_size && bytes[unused] == (rt_uint8_t)'#')
        ++unused;
    return unused;
}

static void print_rt_diag(void)
{
    rt_thread_t idle = rt_thread_idle_gethandler();
    uart_puts("RT tick_hz="); put_u32(RT_TICK_PER_SECOND);
    uart_puts(" adc_isr_prio="); put_u32(NVIC_GetPriority(ADC1_2_IRQn));
    uart_puts(" uart_isr_prio="); put_u32(NVIC_GetPriority(USART1_IRQn));
    uart_puts(" systick_prio="); put_u32(NVIC_GetPriority(SysTick_IRQn));
    uart_puts(" pendsv_prio="); put_u32(NVIC_GetPriority(PendSV_IRQn));
    uart_puts(" fault_guard_prio="); put_u32(RT_FAULT_GUARD_PRIORITY);
    uart_puts(" main_prio="); put_u32(RT_MAIN_THREAD_PRIORITY);
    uart_puts(" telemetry_prio="); put_u32(RT_TELEMETRY_PRIORITY);
    uart_puts(" idle_prio="); put_u32(RT_THREAD_PRIORITY_MAX - 1U);
    uart_puts(" workers="); put_u32(rt_workers_started);
    uart_puts(" raw_pending="); put_hex32(rt_fault_guard_raw);
    uart_puts(" fast_pending="); put_hex32(rt_fault_guard_fast);
    uart_puts(" owner=main single_writer=1\r\n");
    uart_puts("RTSTACK main_free=");
    put_u32(rt_stack_unused_bytes(&main_thread));
    uart_puts("/2048 fault_guard_free=");
    put_u32(rt_stack_unused_bytes(&rt_fault_guard_thread));
    uart_puts("/512 telemetry_free=");
    put_u32(rt_stack_unused_bytes(&rt_telemetry_thread));
    uart_puts("/384 idle_free=");
    put_u32(rt_stack_unused_bytes(idle));
    uart_puts("/256 unit=bytes\r\n");
}
#endif
static const char *fault_code_name(uint32_t code)
{
    switch (code)
    {
    case POWER_ERROR_MANUAL_STOP: return "E_MANUAL_STOP";
    case POWER_ERROR_15V_UVLO: return "E_15V_UVLO";
    case POWER_ERROR_PV_OVP: return "E_PV_OVP";
    case POWER_ERROR_VBUS_OVP: return "E_415V_OVP";
    case POWER_ERROR_PHASE_A_OCP: return "E_2P5A_OCP_I1";
    case POWER_ERROR_PHASE_B_OCP: return "E_2P5A_OCP_I2";
    case POWER_ERROR_OTP_NTC1: return "E_OTP_NTC1";
    case POWER_ERROR_OTP_NTC2: return "E_OTP_NTC2";
    case POWER_ERROR_AUX_DROP: return "E_AUX_DROP";
    case POWER_ERROR_ADC_INIT: return "E_ADC_INIT";
    case POWER_ERROR_ADC_READ: return "E_ADC_READ";
    case POWER_ERROR_CURRENT_SENSOR: return "E_CURRENT_SENSOR";
    case POWER_ERROR_NTC_SENSOR: return "E_NTC_SENSOR";
    case POWER_ERROR_CALIBRATION: return "E_CALIBRATION";
    case POWER_ERROR_HRTIM_LOCK: return "E_HRTIM_LOCK";
    case POWER_ERROR_HRTIM_FLT: return "E_HRTIM_FLT";
    case POWER_ERROR_HRTIM_EEV: return "E_HRTIM_EEV";
    case POWER_ERROR_GATE_READBACK: return "E_GATE_READBACK";
    case POWER_ERROR_INTERLOCK: return "E_INTERLOCK";
    case POWER_ERROR_CONFIG: return "E_CONFIG";
    case POWER_ERROR_CAL_STORE: return "E_CAL_STORE";
    case POWER_ERROR_BOOST_UNREACHABLE: return "E_BOOST_UNREACHABLE";
    case POWER_ERROR_PV_UVLO: return "E_PV_UVLO";
    case POWER_ERROR_CURRENT_IMBALANCE: return "E_CURRENT_IMBALANCE";
    case POWER_ERROR_CURRENT_STUCK: return "E_CURRENT_STUCK";
    case POWER_ERROR_VBUS_BUILD_TIMEOUT: return "E_VBUS_BUILD_TIMEOUT";
    case POWER_ERROR_DUTY_SATURATION: return "E_DUTY_SATURATION";
    case POWER_ERROR_POWER_LIMIT: return "E_POWER_LIMIT";
    case POWER_ERROR_SAMPLE_STALE: return "E_SAMPLE_STALE";
    case 0x2001UL: return "E_UART";
    case 0x3001UL: return "E_COMMAND";
    case 0x4001UL: return "E_FLT_INPUT";
    case 0x5002UL: return "E_GATE_LOCK";
    case 0x7001UL: return "E_CONTROL_LOCK";
    default: return "E_EVENT";
    }
}
static void print_protect(void)
{
    power_control_status_t status;
    uint32_t raw = read_fault_inputs();
    power_control_get_status(&status);
    uart_puts("PROTECT FLT="); put_hex32(raw & 0x3FUL);
    uart_puts(" EEV="); put_hex32((raw >> 6) & 0x3UL);
    uart_puts(" active_high_config=1 polarity_validated=");
    put_u32(BOARD_FLT_RUNTIME_POLARITY_VALIDATED); uart_puts("\r\n");
    uart_puts("PROTECT OE1=PB2="); put_u32(gpio_read(GPIOB, OE1_PIN));
    uart_puts(" OE2=PC15="); put_u32(gpio_read(GPIOC, OE2_PIN));
    uart_puts(" OE3=PC14="); put_u32(gpio_read(GPIOC, OE3_PIN));
    uart_puts(" PA2_GATE_INHIBIT="); put_u32(gpio_read(GPIOA, GATE_INHIBIT_PIN));
    uart_puts(" PA5_AUX_ENABLE="); put_u32(gpio_read(GPIOA, AUX_ENABLE_PIN));
    uart_puts(" PA5_POLICY=LOCKED_OFF_NO_SENSE\r\n");
    uart_puts("PROTECT VBUS_HARD_OVP=415V dynamic_mV=");
    put_u32(status.vbus_dynamic_ovp_limit_mv);
    uart_puts(" source=PC1 raw=");
    put_u32(status.vbus_ovp_raw_threshold);
    uart_puts(" state=");
    uart_puts((status.fault_bits & POWER_FAULT_VBUS_OVP) ? "TRIPPED" : "ARMED_DIAG");
    uart_puts("\r\n");
    uart_puts("PROTECT PV_OVP_mV="); put_u32(status.pv_ovp_limit_mv);
    uart_puts(" source=PC0 state=");
    uart_puts(status.pv_ovp_limit_mv ?
              ((status.fault_bits & POWER_FAULT_PV_OVP) ? "TRIPPED" : "ARMED_DIAG") :
              "LIMIT_REQUIRED");
    uart_puts("\r\n");
    uart_puts("PROTECT OCP=2.5A_PER_CHANNEL source=PA0/PA1 state=");
    uart_puts(status.current_calibration_valid ? "ARMED_DIAG" : "CAL_REQUIRED");
    uart_puts(" zero1_mV="); put_u32(status.current_zero_i1_mv);
    uart_puts(" zero2_mV="); put_u32(status.current_zero_i2_mv);
    uart_puts(" gain1_mV_per_A="); put_u32(status.current_gain_i1_mv_per_a);
    uart_puts(" gain2_mV_per_A="); put_u32(status.current_gain_i2_mv_per_a);
    uart_puts(" polarity1="); put_i32(status.current_polarity_i1);
    uart_puts(" polarity2="); put_i32(status.current_polarity_i2); uart_puts("\r\n");
    uart_puts("PROTECT OTP trip_cdeg="); put_i32(status.ntc_otp_trip_cdeg);
    uart_puts(" recover_cdeg="); put_i32(status.ntc_otp_recover_cdeg);
    uart_puts(" sensor_open_short=ACTIVE state=");
    uart_puts((status.fault_bits & (POWER_FAULT_OTP_NTC1 |
              POWER_FAULT_OTP_NTC2 | POWER_FAULT_NTC_SENSOR)) ?
              "TRIPPED" : "ARMED_DIAG"); uart_puts("\r\n");
    uart_puts("PROTECT 15V_UVLO=15V source=UNMAPPED state=");
    uart_puts(status.v15_sense_valid ? "ARMED_DIAG" : "SENSE_UNAVAILABLE");
    uart_puts(" hardware_driver_uvlo=REQUIRED\r\n");
    uart_puts("PROTECT hardware_pwm=DISABLED gate=LOCKED aux_feedback=UNWIRED flt_routes=CONNECTOR_ONLY\r\n");
}
static void print_heartbeat_status(void)
{
    uart_puts("HB enabled="); put_u32(heartbeat_enabled);
    uart_puts(" period_ms=1000 default=ON units=V,A,C\r\n");
}
static void print_heartbeat(void)
{
    power_control_status_t status;
    power_sample_t sample;
    uint32_t vin_valid, vo_valid, i1_valid, i2_valid, actual_fault;
    power_control_get_status(&status);
    power_control_get_sample(&sample);
    vin_valid = (sample.valid_mask >> POWER_ADC_VPV) & 1UL;
    vo_valid = (sample.valid_mask >> POWER_ADC_VBUS) & 1UL;
    i1_valid = (sample.valid_mask >> POWER_ADC_I1) & 1UL;
    i2_valid = (sample.valid_mask >> POWER_ADC_I2) & 1UL;
    actual_fault = fault_latched ||
        ((status.fault_bits & (POWER_FAULT_ADC_INIT | POWER_FAULT_ADC_TIMEOUT |
                               POWER_FAULT_ADC_RAIL | POWER_FAULT_PV_OVP |
                               POWER_FAULT_VBUS_OVP | POWER_FAULT_OCP |
                               POWER_FAULT_15V_UVLO | POWER_FAULT_AUX_DROP |
                               POWER_FAULT_FLT_INPUT | POWER_FAULT_EEV_INPUT |
                               POWER_FAULT_OTP_NTC1 | POWER_FAULT_OTP_NTC2 |
                               POWER_FAULT_NTC_SENSOR |
                               POWER_FAULT_CURRENT_SENSOR |
                               POWER_FAULT_BOOST_UNREACHABLE |
                               POWER_FAULT_PV_UVLO |
                               POWER_FAULT_CURRENT_IMBALANCE |
                               POWER_FAULT_CURRENT_STUCK |
                               POWER_FAULT_VBUS_BUILD_TIMEOUT |
                               POWER_FAULT_DUTY_SATURATION |
                               POWER_FAULT_POWER_LIMIT |
                               POWER_FAULT_SAMPLE_STALE)) != 0UL);
    if (status.mode == POWER_CONTROL_OFF && status.outputs_enabled == 0UL &&
        fault_latched == 0UL) {
        actual_fault = (status.fault_bits &
            (POWER_FAULT_ADC_INIT | POWER_FAULT_ADC_RAIL |
             POWER_FAULT_PV_OVP | POWER_FAULT_VBUS_OVP |
             POWER_FAULT_OTP_NTC1 | POWER_FAULT_OTP_NTC2 |
             POWER_FAULT_NTC_SENSOR)) != 0UL;
    }
    {
        int32_t current1 = (i1_valid && i2_valid && status.calibration_valid) ?
                           status.pv_current_ma : 0;
        int32_t current2 = 0; /* No output-current sensor exists in this netlist. */
        int32_t duty_percent = (int32_t)((status.duty_q15 * 10000UL + 16384UL) / 32768UL);
        /* FireWater: name:value pairs separated by commas; all values numeric. */
        uart_puts("Vin:"); put_fixed_i32(vin_valid ? status.pv_mv : 0, 1000UL, 3UL);
        uart_puts(",Vo:"); put_fixed_i32(vo_valid ? status.vbus_mv : 0, 1000UL, 3UL);
        uart_puts(",Iin:"); put_fixed_i32(current1, 1000UL, 3UL);
        uart_puts(",Iout:"); put_fixed_i32(current2, 1000UL, 3UL);
        uart_puts(",Duty:"); put_fixed_i32(duty_percent, 100UL, 2UL);
        uart_puts(",T1:"); put_fixed_i32((status.temperature_valid_mask & 1UL) ? status.ntc1_cdeg : 0, 100UL, 2UL);
        uart_puts(",T2:"); put_fixed_i32((status.temperature_valid_mask & 2UL) ? status.ntc2_cdeg : 0, 100UL, 2UL);
        uart_puts(",State:"); put_u32((uint32_t)app_state);
        uart_puts(",Fault:"); put_u32(actual_fault); uart_puts("\r\n");
    }
}
static void print_fault_codes(void)
{
    uart_puts("FAULTCODES E_15V_UVLO=0x0301 E_PV_OVP=0x0302 E_415V_OVP=0x0303\r\n");
    uart_puts("FAULTCODES E_2P5A_OCP_I1=0x0304 E_2P5A_OCP_I2=0x0305 E_AUX_DROP=0x0308\r\n");
    uart_puts("FAULTCODES E_OTP_NTC1=0x0306 E_OTP_NTC2=0x0307 E_CURRENT_SENSOR=0x0203 E_NTC_SENSOR=0x0204\r\n");
    uart_puts("FAULTCODES E_ADC_INIT=0x0201 E_ADC_READ=0x0202 E_CALIBRATION=0x0701\r\n");
    uart_puts("FAULTCODES E_HRTIM_LOCK=0x0501 E_HRTIM_FLT=0x0503 E_HRTIM_EEV=0x0504 E_GATE_READBACK=0x0505\r\n");
    uart_puts("FAULTCODES E_INTERLOCK=0x0702 E_CONFIG=0x0703 E_CAL_STORE=0x0704 E_BOOST_UNREACHABLE=0x0705\r\n");
    uart_puts("FAULTCODES E_PV_UVLO=0x0309 E_CURRENT_IMBALANCE=0x030A E_CURRENT_STUCK=0x0205\r\n");
    uart_puts("FAULTCODES E_VBUS_BUILD_TIMEOUT=0x030B E_DUTY_SATURATION=0x0506 E_POWER_LIMIT=0x030C E_SAMPLE_STALE=0x0206\r\n");
}
static void service_control_faults(void)
{
    power_control_status_t status;
    uint32_t critical, effective, newly;
    power_control_get_status(&status);
    critical = POWER_FAULT_ADC_INIT | POWER_FAULT_ADC_TIMEOUT |
               POWER_FAULT_HRTIM_LOCK |
               POWER_FAULT_ADC_RAIL | POWER_FAULT_PV_OVP |
               POWER_FAULT_VBUS_OVP | POWER_FAULT_OCP |
               POWER_FAULT_15V_UVLO | POWER_FAULT_AUX_DROP |
               POWER_FAULT_OTP_NTC1 | POWER_FAULT_OTP_NTC2 |
               POWER_FAULT_NTC_SENSOR | POWER_FAULT_CURRENT_SENSOR |
               POWER_FAULT_BOOST_UNREACHABLE | POWER_FAULT_PV_UVLO |
               POWER_FAULT_CURRENT_IMBALANCE | POWER_FAULT_CURRENT_STUCK |
               POWER_FAULT_VBUS_BUILD_TIMEOUT | POWER_FAULT_DUTY_SATURATION |
               POWER_FAULT_POWER_LIMIT | POWER_FAULT_SAMPLE_STALE;
    if (status.mode == POWER_CONTROL_OFF && status.outputs_enabled == 0UL &&
        status.last_stop_fault_bits == 0UL) {
        /* SAFE_OFF is a diagnostic/commissioning state.  ADC startup timeout,
         * uncalibrated Hall plausibility and run-only plant conditions are
         * reported by CONTROL but must not latch FAULT or cancel AUX ON. */
        critical &= ~(POWER_FAULT_ADC_TIMEOUT | POWER_FAULT_OCP |
                      POWER_FAULT_15V_UVLO | POWER_FAULT_AUX_DROP |
                      POWER_FAULT_CURRENT_SENSOR |
                      POWER_FAULT_BOOST_UNREACHABLE | POWER_FAULT_PV_UVLO |
                      POWER_FAULT_CURRENT_IMBALANCE |
                      POWER_FAULT_CURRENT_STUCK |
                      POWER_FAULT_VBUS_BUILD_TIMEOUT |
                      POWER_FAULT_DUTY_SATURATION |
                      POWER_FAULT_POWER_LIMIT |
                      POWER_FAULT_SAMPLE_STALE);
    }
    /* contain_critical_faults() can physically stop the plant before this
     * owner thread runs.  Include its persistent snapshot so the following
     * SAFE_OFF scan cannot erase the reason and leave READY/Fault=0 behind. */
    effective = status.fault_bits | status.last_stop_fault_bits;
    newly = (effective & critical) & ~control_fault_seen;
    if (newly != 0UL)
    {
        if (newly & POWER_FAULT_ADC_INIT) log_event(POWER_ERROR_ADC_INIT, status.adc_ready_mask);
        if (newly & (POWER_FAULT_ADC_TIMEOUT | POWER_FAULT_ADC_RAIL))
            log_event(POWER_ERROR_ADC_READ, newly);
        if (newly & POWER_FAULT_PV_OVP) log_event(POWER_ERROR_PV_OVP, (uint32_t)status.pv_mv);
        if (newly & POWER_FAULT_VBUS_OVP) log_event(POWER_ERROR_VBUS_OVP, (uint32_t)status.vbus_mv);
        if ((newly & POWER_FAULT_OCP) && (status.fault_bits & POWER_FAULT_OCP_I1)) {
            log_event(POWER_ERROR_PHASE_A_OCP, (uint32_t)status.i1_ma);
        }
        if ((newly & POWER_FAULT_OCP) && (status.fault_bits & POWER_FAULT_OCP_I2)) {
            log_event(POWER_ERROR_PHASE_B_OCP, (uint32_t)status.i2_ma);
        }
        if ((newly & POWER_FAULT_OCP) &&
            !(status.fault_bits & (POWER_FAULT_OCP_I1 | POWER_FAULT_OCP_I2))) {
            log_event(POWER_ERROR_PHASE_A_OCP, (uint32_t)status.pv_current_ma);
        }
        if (newly & POWER_FAULT_15V_UVLO) log_event(POWER_ERROR_15V_UVLO, 0UL);
        if (newly & POWER_FAULT_AUX_DROP) log_event(POWER_ERROR_AUX_DROP, 0UL);
        if (newly & POWER_FAULT_OTP_NTC1)
            log_event(POWER_ERROR_OTP_NTC1, (uint32_t)status.ntc1_cdeg);
        if (newly & POWER_FAULT_OTP_NTC2)
            log_event(POWER_ERROR_OTP_NTC2, (uint32_t)status.ntc2_cdeg);
        if (newly & POWER_FAULT_NTC_SENSOR)
            log_event(POWER_ERROR_NTC_SENSOR, status.temperature_valid_mask);
        if (newly & POWER_FAULT_CURRENT_SENSOR)
            log_event(POWER_ERROR_CURRENT_SENSOR, status.fast_adc_fault_flags);
        if (newly & POWER_FAULT_BOOST_UNREACHABLE)
            log_event(POWER_ERROR_BOOST_UNREACHABLE, (uint32_t)status.pv_mv);
        if (newly & POWER_FAULT_PV_UVLO)
            log_event(POWER_ERROR_PV_UVLO, (uint32_t)status.pv_mv);
        if (newly & POWER_FAULT_CURRENT_IMBALANCE)
            log_event(POWER_ERROR_CURRENT_IMBALANCE,
                      (uint32_t)(status.i1_ma - status.i2_ma));
        if (newly & POWER_FAULT_CURRENT_STUCK)
            log_event(POWER_ERROR_CURRENT_STUCK, status.current_stuck_ms);
        if (newly & POWER_FAULT_VBUS_BUILD_TIMEOUT)
            log_event(POWER_ERROR_VBUS_BUILD_TIMEOUT,
                      status.vbus_build_elapsed_ms);
        if (newly & POWER_FAULT_DUTY_SATURATION)
            log_event(POWER_ERROR_DUTY_SATURATION, status.duty_q15);
        if (newly & POWER_FAULT_POWER_LIMIT)
            log_event(POWER_ERROR_POWER_LIMIT, (uint32_t)status.pv_power_mw);
        if (newly & POWER_FAULT_SAMPLE_STALE)
            log_event(POWER_ERROR_SAMPLE_STALE, status.fast_sample_sequence);
        if (newly & POWER_FAULT_HRTIM_LOCK)
            log_event(POWER_ERROR_HRTIM_LOCK, status.compare_write_ok);
        (void)power_control_set_mode(POWER_CONTROL_OFF);
        safe_power_outputs_off(); fault_latched = 1UL; fault_reasons |= newly;
        power_control_set_external_fault_latched(1UL);
        ++fault_count; app_state = APP_STATE_FAULT; show_state_rgb(APP_STATE_FAULT);
    }
    control_fault_seen = effective & critical;
}
static void print_log(void)
{
    uint32_t i;
    uint32_t first = (event_log_head + LOG_CAPACITY - log_count) % LOG_CAPACITY;
    uart_puts("LOG storage=RAM count="); put_u32(log_count);
    uart_puts(" capacity="); put_u32(LOG_CAPACITY); uart_puts(" volatile=1\r\n");
    for (i = 0UL; i < log_count; ++i)
    {
        const log_entry_t *entry = &event_log[(first + i) % LOG_CAPACITY];
        if (!entry->sequence) continue;
        uart_puts("LOG seq="); put_u32(entry->sequence); uart_puts(" code="); put_hex32(entry->code);
        uart_puts(" name="); uart_puts(fault_code_name(entry->code));
        uart_puts(" arg="); put_hex32(entry->arg);
        uart_puts(" t_ms="); put_u32(entry->uptime_ms);
        uart_puts(" state="); put_u32(entry->state);
        uart_puts(" faults="); put_hex32(entry->fault_bits);
        uart_puts(" raw="); put_hex32(entry->raw_inputs);
        uart_puts(" pv_mV="); put_i32(entry->pv_mv);
        uart_puts(" vbus_mV="); put_i32(entry->vbus_mv);
        uart_puts(" current_mA="); put_i32(entry->current_ma);
        uart_puts(" sample="); put_u32(entry->sample_sequence); uart_puts("\r\n");
    }
    {
        board_cal_store_diag_t diag;
        board_cal_store_get_diag(&diag);
        uart_puts("PFAULT storage=W25Q128 slots=");
        put_hex32(diag.fault_valid_slot_mask);
        uart_puts(" latest="); put_u32(diag.fault_latest_sequence);
        uart_puts(" depth=4\r\n");
        for (i = 0UL; i < 4UL; ++i) {
            board_fault_record_t fault;
            uint32_t sequence;
            if (board_fault_store_read_recent(i, &fault, &sequence) != 0) continue;
            uart_puts("PFAULT seq="); put_u32(sequence);
            uart_puts(" code="); put_hex32(fault.code);
            uart_puts(" name="); uart_puts(fault_code_name(fault.code));
            uart_puts(" arg="); put_hex32(fault.arg);
            uart_puts(" t_ms="); put_u32(fault.uptime_ms);
            uart_puts(" faults="); put_hex32(fault.fault_bits);
            uart_puts(" raw="); put_hex32(fault.raw_inputs);
            uart_puts(" vbus_mV="); put_i32(fault.vbus_mv);
            uart_puts(" current_mA="); put_i32(fault.current_ma);
            uart_puts("\r\n");
        }
    }
}
static void clear_event_log(void)
{
    uint32_t i;
    for (i = 0UL; i < LOG_CAPACITY; ++i) {
        event_log[i].code = 0UL;
        event_log[i].arg = 0UL;
        event_log[i].sequence = 0UL;
    }
    event_log_head = 0UL;
    log_count = 0UL;
}
static void print_reset_cause(void)
{
    uart_puts("RESET_FLAGS="); put_hex32(reset_cause_flags);
    uart_puts(" PIN="); put_u32((reset_cause_flags >> 26) & 1UL);
    uart_puts(" POR="); put_u32((reset_cause_flags >> 27) & 1UL);
    uart_puts(" SW="); put_u32((reset_cause_flags >> 28) & 1UL);
    uart_puts(" IWDG="); put_u32((reset_cause_flags >> 29) & 1UL);
    uart_puts(" WWDG="); put_u32((reset_cause_flags >> 30) & 1UL);
    uart_puts(" LPWR="); put_u32((reset_cause_flags >> 31) & 1UL); uart_puts("\r\n");
}
static void print_cal_store(void)
{
    board_cal_store_diag_t diag;
    board_cal_store_get_diag(&diag);
    uart_puts("CALSTORE ready="); put_u32(diag.ready);
    uart_puts(" jedec="); put_hex32(diag.jedec_id);
    uart_puts(" slots="); put_hex32(diag.valid_slot_mask);
    uart_puts(" sequence="); put_u32(diag.active_sequence);
    uart_puts(" loaded="); put_u32(calibration_loaded);
    uart_puts(" dirty="); put_u32(calibration_dirty);
    uart_puts(" loads="); put_u32(diag.load_count);
    uart_puts(" saves="); put_u32(diag.save_count);
    uart_puts(" erases="); put_u32(diag.erase_count);
    uart_puts(" result="); put_i32(diag.last_result); uart_puts("\r\n");
}
static int calibration_is_safe_off(void)
{
    power_control_status_t status;
    power_control_get_status(&status);
    return (fault_latched == 0UL && read_fault_inputs() == 0UL &&
            status.mode == POWER_CONTROL_OFF && status.outputs_enabled == 0UL &&
            board_safety_outputs_are_off() != 0UL) ? 1 : 0;
}
static void load_calibration_at_boot(void)
{
    power_calibration_t calibration;
    if (board_cal_store_load(&calibration) == 0 &&
        power_control_import_calibration(&calibration) == 0) {
        calibration_loaded = 1UL;
        calibration_dirty = 0UL;
    } else {
        calibration_loaded = 0UL;
        calibration_dirty = 0UL;
    }
}
static int save_calibration(void)
{
    power_calibration_t calibration;
    if (!calibration_is_safe_off()) return -1;
    if (power_control_export_calibration(&calibration) != 0) return -2;
    if (board_cal_store_save(&calibration) != 0) return -3;
    calibration_loaded = 1UL;
    calibration_dirty = 0UL;
    return 0;
}
static int clear_fault_to_safe(void)
{
    if (read_fault_inputs() != 0UL) return 0;
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    if (power_control_clear_faults() != 0) return 0;
    fault_latched = 0UL;
    fault_reasons = 0UL;
    control_fault_seen = 0UL;
    fault_input_seen = 0UL;
    power_control_set_external_fault_latched(0UL);
#if defined(APP_USE_RTTHREAD)
    rt_fault_guard_raw = 0UL;
    rt_fault_guard_fast = 0UL;
    __DMB();
#endif
    safe_outputs_off();
    app_state = APP_STATE_SAFE_OFF;
    show_state_rgb(APP_STATE_SAFE_OFF);
    log_event(POWER_ERROR_MANUAL_STOP, 0UL);
    return 1;
}
static void print_pinmap(void)
{
    uart_puts("PINMAP CHE2<-PA9/TA2 CHF1<-PA10/TB1 OE3<-PC14 PA2=INHIBIT PA5=AUX_EN\r\n");
    uart_puts("PINMAP U11 CHA1<-PB12/TC1 CHA2<-PB13/TC2 CHB1<-PB14/TD1 CHB2<-PB15/TD2\r\n");
    uart_puts("PINMAP U11 CHC1<-PC6/TF1 CHC2<-PC7/TF2 CHD1<-PC8/TE1 CHD2<-PC9/TE2\r\n");
    uart_puts("PINMAP U11 CHE1<-PA8/TA1 CHE2<-PA9/TA2 CHF1<-PA10/TB1 CHF2<-PA11/TB2\r\n");
}
static int prepare_mppt_calibration(void)
{
    power_control_status_t status;
    power_sample_t sample;
    uint32_t zero1 = 0UL, zero2 = 0UL;

    power_control_get_status(&status);
    if (status.calibration_valid != 0UL) return 1;
    if (fault_latched != 0UL || read_fault_inputs() != 0UL ||
        status.mode != POWER_CONTROL_OFF || status.outputs_enabled != 0UL ||
        board_safety_outputs_are_off() == 0UL) return 0;
    power_control_get_sample(&sample);
    if ((sample.valid_mask & ((1UL << POWER_ADC_I1) |
                              (1UL << POWER_ADC_I2) |
                              (1UL << POWER_ADC_VPV) |
                              (1UL << POWER_ADC_VBUS))) !=
        ((1UL << POWER_ADC_I1) | (1UL << POWER_ADC_I2) |
         (1UL << POWER_ADC_VPV) | (1UL << POWER_ADC_VBUS))) return 0;

    /* Auto-zero is performed only while all physical power outputs are off. */
    if (status.current_calibration_valid == 0UL &&
        power_control_auto_zero_current(64UL, &zero1, &zero2) != 0) return 0;
    power_control_get_status(&status);
    if (status.current_polarity_valid == 0UL &&
        power_control_set_current_polarity(1, 1) != 0) return 0;
    power_control_get_status(&status);
    if (status.voltage_calibration_valid == 0UL &&
        /* 1,000,000 ppm is the nominal divider gain from the supplied
         * netlist; an end-to-end measured calibration can overwrite it. */
        power_control_set_voltage_calibration(1000000UL, 1000000UL) != 0) return 0;
    power_control_get_status(&status);
    return status.calibration_valid != 0UL ? 1 : 0;
}
static void command_mppt_disarm(void)
{
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    safe_outputs_off();
    if (!fault_latched) {
        app_state = APP_STATE_SAFE_OFF;
        show_state_rgb(APP_STATE_SAFE_OFF);
    }
    uart_puts("OK STOP outputs=DISABLED gate=LOCKED aux15v=OFF\r\n");
}
static void command_mppt_arm(uint32_t target_v)
{
    int target_result;
    int arm_result = -99;
    power_control_status_t before;
    power_control_get_status(&before);
    if (before.mode == POWER_CONTROL_MPPT && before.outputs_enabled != 0UL &&
        before.target_mv == target_v * 1000UL) {
        uart_puts("OK START already=RUNNING target_V="); put_u32(target_v);
        uart_puts(" outputs=1 duty_q15="); put_u32(before.duty_q15);
        uart_puts(" aux15v=REQUESTED\r\n");
        return;
    }
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    safe_outputs_off();
    if (fault_latched || read_fault_inputs() != 0UL)
    {
        record_command_error(0x3111UL, read_fault_inputs());
        uart_puts("ERR E_MPPT_FAULT_INTERLOCK outputs=DISABLED\r\n");
        return;
    }
    if (!prepare_mppt_calibration())
    {
        power_control_status_t cal_status;
        power_control_get_status(&cal_status);
        record_command_error(0x3113UL, cal_status.fault_bits);
        uart_puts("ERR E_MPPT_CALIBRATION outputs=DISABLED current=");
        put_u32(cal_status.current_calibration_valid);
        uart_puts(" polarity="); put_u32(cal_status.current_polarity_valid);
        uart_puts(" voltage="); put_u32(cal_status.voltage_calibration_valid);
        uart_puts(" faults="); put_hex32(cal_status.fault_bits); uart_puts("\r\n");
        return;
    }
    target_result = power_control_set_target_mv(target_v * 1000UL);
    if (target_result == 0)
        arm_result = power_control_set_mode(POWER_CONTROL_MPPT);
    if (target_result != 0 || arm_result != 0)
    {
        power_control_status_t arm_status;
        power_control_get_status(&arm_status);
        record_command_error(0x3112UL, target_v);
        uart_puts("ERR E_MPPT_ARM_LOCKED outputs=DISABLED reason=");
        if (arm_status.calibration_valid == 0UL) {
            uart_puts("CAL_REQUIRED");
            if (arm_status.current_calibration_valid == 0UL) uart_puts("|CURRENT");
            if (arm_status.current_polarity_valid == 0UL) uart_puts("|POLARITY");
            if (arm_status.voltage_calibration_valid == 0UL) uart_puts("|VOLTAGE");
        } else if (arm_result == -6) {
            uart_puts("FAST_ADC_FIRST_CYCLE");
        } else if (arm_result == -7) {
            board_hrtim_diag_t hrtim_diag;
            board_hrtim_get_diag(&hrtim_diag);
            uart_puts("POWER_ARM_READBACK(stage=");
            put_u32(hrtim_diag.arm_fail_stage);
            uart_puts(",gate="); put_hex32(hrtim_diag.arm_gate_fail_mask);
            uart_puts(",backend="); put_u32(hrtim_diag.backend_ready);
            uart_puts(",timing="); put_u32(hrtim_diag.timing_configured);
            uart_puts(",dll="); put_u32(hrtim_diag.dll_ready);
            uart_puts(",clock="); put_u32(hrtim_diag.clock_validated);
            uart_puts(",int_hi="); put_hex32(hrtim_diag.internal_seen_high);
            uart_puts(",int_lo="); put_hex32(hrtim_diag.internal_seen_low);
            uart_puts(",pad_hi="); put_hex32(hrtim_diag.pad_seen_high);
            uart_puts(",pad_lo="); put_hex32(hrtim_diag.pad_seen_low);
            uart_putc(')');
        } else if (arm_status.boost_reachable == 0UL) {
            uart_puts("BOOST_UNREACHABLE");
        } else if (arm_status.fault_bits != 0UL) {
            uart_puts("FAULT");
        } else if (arm_status.hrtim_backend_ready == 0UL) {
            uart_puts("HRTIM");
        } else if (arm_status.protection_backend_ready == 0UL) {
            uart_puts("PROTECTION");
        } else {
            uart_puts("INTERLOCK");
        }
        uart_puts(" cal="); put_u32(arm_status.calibration_valid);
        uart_puts(" current="); put_u32(arm_status.current_calibration_valid);
        uart_puts(" polarity="); put_u32(arm_status.current_polarity_valid);
        uart_puts(" voltage="); put_u32(arm_status.voltage_calibration_valid);
        uart_puts(" faults="); put_hex32(arm_status.fault_bits); uart_puts("\r\n");
        return;
    }
    app_state = APP_STATE_READY;
    show_state_rgb(APP_STATE_READY);
    {
        power_control_status_t started;
        power_control_get_status(&started);
        uart_puts("OK START target_V="); put_u32(target_v);
        uart_puts(" mode=MPPT sampling=1 outputs="); put_u32(started.outputs_enabled);
        uart_puts(" duty_q15="); put_u32(started.duty_q15);
        uart_puts(" aux15v="); put_u32(board_aux_get_request());
        uart_puts(" gate=ARMED\r\n");
    }
}
static void command_cal_current_auto(void)
{
    uint32_t zero1 = 0UL, zero2 = 0UL;
    uint32_t raw_inputs;
    power_control_status_t control_status;
    int result;
    (void)power_control_set_mode(POWER_CONTROL_OFF);
    safe_outputs_off();
    if (!fault_latched) app_state = APP_STATE_SAFE_OFF;
    power_control_get_status(&control_status);
    raw_inputs = read_fault_inputs();
    if (app_state != APP_STATE_SAFE_OFF || fault_latched != 0UL ||
        raw_inputs != 0UL || control_status.mode != POWER_CONTROL_OFF ||
        control_status.outputs_enabled != 0UL ||
        gpio_read(GPIOB, OE1_PIN) == 0UL ||
        gpio_read(GPIOC, OE2_PIN) == 0UL ||
        gpio_read(GPIOC, OE3_PIN) == 0UL ||
        gpio_read(GPIOA, GATE_INHIBIT_PIN) == 0UL ||
        gpio_read(GPIOA, AUX_ENABLE_PIN) != 0UL)
    {
        record_command_error(0x3201UL, raw_inputs);
        uart_puts("ERR E_CAL_REQUIRES_FULL_DISABLE use=STOP_then_CAL\r\n");
        return;
    }
    result = power_control_auto_zero_current(64UL, &zero1, &zero2);
    if (result != 0)
    {
        record_command_error(0x3202UL, (uint32_t)(-result));
        uart_puts("ERR E_CAL_AUTO code="); put_i32(result); uart_puts("\r\n");
    }
    else
    {
        calibration_dirty = 1UL;
        uart_puts("OK CAL zero1_mV="); put_u32(zero1);
        uart_puts(" zero2_mV="); put_u32(zero2);
        uart_puts(" gain1_mV_A=44 gain2_mV_A=44 polarity=REQUIRED\r\n");
    }
}
static void print_help(void)
{
  /* ASCII-only output keeps the command guide readable on terminals whose
   * code page is not UTF-8. */
  uart_puts("CMD PING=link; VERSION=firmware; STATUS=all; CONTROL=control; PROTECT=protection; SAMPLES=ADC\r\n");
  uart_puts("START [100..400]=auto-calibrate and start MPPT (default 150V); STOP=disable PWM and AUX\r\n");
  uart_puts("MPPT ARM LIMITED 100..400=legacy START; MPPT DISARM=legacy STOP; MPPT STATUS=STATUS+CONTROL+PROTECT\r\n");
  uart_puts("AUX ON/OFF/STATUS=15V request and status (AUX does not start PWM); TARGET volts=set bus target\r\n");
  uart_puts("CAL=auto current zero in full disable; CAL SAVE/LOAD/STATUS/CLEAR=calibration storage\r\n");
  uart_puts("MODE OFF|CV|MPPT; FAULTS/CLEAR; LOG/FAULTLOG; HRTIMDIAG; UARTDIAG; HB ON/OFF/NOW; HELP\r\n");
}
static void command(char *line)
{
    uint8_t parsed[3]; uint32_t value; ++command_count;
    trim_command(line);
    if (*line == '\0') return;
    if (text_equal_ci(line, "PING")) { uart_puts("PONG "); uart_puts(FW_VERSION); uart_puts("\r\n"); return; }
    if (text_equal_ci(line, "VERSION")) { uart_puts("FW "); uart_puts(FW_VERSION); uart_puts("\r\n"); return; }
    if (text_equal_ci(line, "HB") || text_equal_ci(line, "HB STATUS")) { print_heartbeat_status(); return; }
    if (text_equal_ci(line, "HB NOW") || text_equal_ci(line, "MEAS")) { print_heartbeat(); return; }
    if (text_equal_ci(line, "HB ON"))
    {
        heartbeat_enabled = 1UL;
        next_heartbeat = app_now_ms() + HEARTBEAT_PERIOD_MS;
        uart_puts("OK HB_ON\r\n"); print_heartbeat_status(); return;
    }
    if (text_equal_ci(line, "HB OFF"))
    {
        heartbeat_enabled = 0UL;
        uart_puts("OK HB_OFF\r\n"); print_heartbeat_status(); return;
    }
    if (text_equal_ci(line, "WHO")) { uart_puts("STM32G474RBT3 OE_LATCH PB2/PC15/PC14 PA2_INHIBIT PA5_AUX PC13_RGB\r\n"); return; }
    if (text_equal_ci(line, "PINMAP")) { print_pinmap(); return; }
    if (text_starts_ci(line, "PRINT ")) { uart_puts("ECHO "); uart_puts(line + 6); uart_puts("\r\n"); return; }
    if (text_equal_ci(line, "HELP")) { print_help(); return; }
    if (text_equal_ci(line, "HRTIMDIAG")) { print_hrtim_diag(); return; }
    if (text_equal_ci(line, "CLOCKDIAG")) { print_clock_diag(); return; }
#if defined(APP_USE_RTTHREAD)
    if (text_equal_ci(line, "RTDIAG")) { print_rt_diag(); return; }
#endif
    if (text_equal_ci(line, "START")) { command_mppt_arm(150UL); return; }
    if (text_starts_ci(line, "START "))
    {
        if (!parse_u32(line + 6, &value) || value < 100UL || value > 400UL) {
            record_command_error(0x3110UL, value);
            uart_puts("ERR E_START_TARGET_RANGE use=START 100..400\r\n");
        } else command_mppt_arm(value);
        return;
    }
    if (text_equal_ci(line, "STOP") || text_equal_ci(line, "HALT") ||
        text_equal_ci(line, "MPPT DISARM")) { command_mppt_disarm(); return; }
    if (text_equal_ci(line, "CAL") || text_equal_ci(line, "CAL CURRENT")) {
        command_cal_current_auto(); return;
    }
    /* MPPT startup/stop command vocabulary.  AUTO is the short form of the
     * same qualified START path; keeping one implementation prevents the
     * command parser from reporting a hard-coded V15-sense error on a board
     * whose protection is intentionally external. */
    if (text_equal_ci(line, "MPPT AUTO"))
    {
        command_mppt_arm(150UL);
        return;
    }
    if (text_equal_ci(line, "MPPT STATUS")) { print_status(); print_control(); print_protect(); return; }
    if (text_equal_ci(line, "MPPT SAMPLES")) { print_samples(); return; }
    if (text_equal_ci(line, "AUX ON"))
    {
        uint32_t state = board_aux_set_request(1UL);
        uart_puts(state != 0UL ? "OK AUX_ON request=1 pwm=UNCHANGED pgood=UNAVAILABLE\r\n" :
                  "ERR E_AUX_WRITE request=1\r\n");
        return;
    }
    if (text_equal_ci(line, "AUX OFF"))
    {
        board_safety_force_off();
        uart_puts("OK AUX_OFF request=0 outputs=DISABLED\r\n");
        return;
    }
    if (text_equal_ci(line, "AUX STATUS"))
    {
        uart_puts("AUX request="); put_u32(board_aux_get_request());
        uart_puts(" pgood=UNAVAILABLE pwm=");
        uart_puts(board_safety_power_outputs_are_off() != 0UL ? "DISABLED\r\n" : "CHECK\r\n");
        return;
    }
    if (text_equal_ci(line, "MPPT DISARM")) { command_mppt_disarm(); return; }
    if (text_starts_ci(line, "MPPT ARM LIMITED "))
    {
        uint32_t target_v = 0UL;
        if (!parse_u32(line + 17, &target_v) || target_v < 100UL || target_v > 400UL)
        {
            record_command_error(0x3110UL, target_v); uart_puts("ERR E_MPPT_TARGET_RANGE use=MPPT ARM LIMITED 100..400\r\n");
            return;
        }
        command_mppt_arm(target_v);
        return;
    }
    if (text_equal_ci(line, "STATUS")) { print_status(); return; }
    if (text_equal_ci(line, "CONTROL")) { print_control(); return; }
    if (text_equal_ci(line, "PROTECT")) { print_protect(); return; }
    if (text_equal_ci(line, "FAULTCODES")) { print_fault_codes(); return; }
    if (text_equal_ci(line, "PROTECT CAL CLEAR"))
    {
        int result;
        safe_outputs_off();
        power_control_clear_all_calibration();
        result = board_cal_store_erase();
        calibration_loaded = 0UL;
        calibration_dirty = (result == 0) ? 0UL : 1UL;
        if (result == 0)
            uart_puts("OK ALL_CAL_CLEARED storage=ERASED outputs=DISABLED\r\n");
        else {
            record_command_error(POWER_ERROR_CAL_STORE, (uint32_t)(-result));
            uart_puts("ERR E_CAL_STORE runtime_cleared storage_not_erased outputs=DISABLED\r\n");
        }
        return;
    }
    if (text_equal_ci(line, "PROTECT CAL STATUS")) { print_cal_store(); return; }
    if (text_equal_ci(line, "PROTECT CAL SAVE"))
    {
        int result;
        safe_outputs_off();
        result = save_calibration();
        if (result == 0) uart_puts("OK CAL_SAVED storage=W25Q128 slot=A_OR_B outputs=DISABLED\r\n");
        else {
            record_command_error(POWER_ERROR_CAL_STORE, (uint32_t)(-result));
            uart_puts("ERR E_CAL_STORE save_requires_complete_calibration_safe_off_and_W25Q\r\n");
        }
        return;
    }
    if (text_equal_ci(line, "PROTECT CAL LOAD"))
    {
        power_calibration_t calibration;
        int result = -1;
        safe_outputs_off();
        if (calibration_is_safe_off() && board_cal_store_load(&calibration) == 0)
            result = power_control_import_calibration(&calibration);
        if (result == 0) {
            calibration_loaded = 1UL; calibration_dirty = 0UL;
            uart_puts("OK CAL_LOADED storage=W25Q128 outputs=DISABLED\r\n");
        } else {
            record_command_error(POWER_ERROR_CAL_STORE, (uint32_t)(-result));
            uart_puts("ERR E_CAL_STORE load_requires_safe_off_and_valid_record\r\n");
        }
        return;
    }
    if (text_equal_ci(line, "PROTECT CAL CURRENT AUTO"))
    { command_cal_current_auto(); return; }
    if (text_starts_ci(line, "PROTECT CAL CURRENT "))
    {
        const uint32_t limits[4] = {3300UL, 3300UL,
                                    BOARD_CURRENT_GAIN_MAX_MV_PER_A,
                                    BOARD_CURRENT_GAIN_MAX_MV_PER_A};
        uint32_t values[4];
        (void)power_control_set_mode(POWER_CONTROL_OFF);
        safe_outputs_off();
        if (!fault_latched) app_state = APP_STATE_SAFE_OFF;
        if (!parse_four_u32(line + 20, limits, values) ||
            power_control_set_current_calibration(values[0], values[1],
                                                  values[2], values[3]) != 0)
        { ++command_errors; uart_puts("ERR E_CURRENT_CAL_RANGE zero=0..ADC-20mV gain=35..53mV_A and OCP_headroom_required\r\n"); }
        else { calibration_dirty = 1UL; uart_puts("OK CURRENT_CAL zero1_mV="); put_u32(values[0]); uart_puts(" zero2_mV="); put_u32(values[1]); uart_puts(" gain1_mV_A="); put_u32(values[2]); uart_puts(" gain2_mV_A="); put_u32(values[3]); uart_puts(" polarity=REQUIRED\r\n"); }
        return;
    }
    if (text_starts_ci(line, "PROTECT CAL POLARITY "))
    {
        const char *args = line + 21;
        int32_t p1 = 0, p2 = 0;
        (void)power_control_set_mode(POWER_CONTROL_OFF);
        safe_outputs_off();
        if (text_equal_ci(args, "1 1") || text_equal_ci(args, "+1 +1"))
            { p1 = 1; p2 = 1; }
        else if (text_equal_ci(args, "1 -1") || text_equal_ci(args, "+1 -1"))
            { p1 = 1; p2 = -1; }
        else if (text_equal_ci(args, "-1 1") || text_equal_ci(args, "-1 +1"))
            { p1 = -1; p2 = 1; }
        else if (text_equal_ci(args, "-1 -1")) { p1 = -1; p2 = -1; }
        if (p1 == 0 || power_control_set_current_polarity(p1, p2) != 0) {
            ++command_errors;
            uart_puts("ERR E_POLARITY_FORMAT use=PROTECT CAL POLARITY +1|-1 +1|-1\r\n");
        } else {
            calibration_dirty = 1UL;
            uart_puts("OK CURRENT_POLARITY I1="); put_i32(p1);
            uart_puts(" I2="); put_i32(p2);
            uart_puts(" verify_with_known_forward_current=REQUIRED\r\n");
        }
        return;
    }
    if (text_starts_ci(line, "PROTECT CAL VOLTAGE "))
    {
        uint32_t values[2];
        (void)power_control_set_mode(POWER_CONTROL_OFF);
        safe_outputs_off();
        if (!fault_latched) app_state = APP_STATE_SAFE_OFF;
        if (!parse_two_u32(line + 20, BOARD_VOLTAGE_GAIN_MAX_PPM, values) ||
            power_control_set_voltage_calibration(values[0], values[1]) != 0)
        {
            ++command_errors;
            uart_puts("ERR E_VOLTAGE_CAL_RANGE use=PROTECT CAL VOLTAGE pv_gain_ppm vbus_gain_ppm range=900000..1100000\r\n");
        }
        else
        {
            calibration_dirty = 1UL;
            uart_puts("OK VOLTAGE_CAL pv_gain_ppm="); put_u32(values[0]);
            uart_puts(" vbus_gain_ppm="); put_u32(values[1]);
            uart_puts(" outputs=DISABLED\r\n");
        }
        return;
    }
    if (text_equal_ci(line, "SAMPLES")) { print_samples(); return; }
    if (text_equal_ci(line, "SAMPLES NOW")) { (void)power_control_scan_now(); print_samples(); return; }
    if (text_equal_ci(line, "ADC")) { print_samples(); return; }
    if (text_starts_ci(line, "TARGET "))
    {
        if (!parse_u32(line + 7, &value) || value == 0UL ||
            power_control_set_target_mv(value * 1000UL) != 0)
        { ++command_errors; uart_puts("ERR E_TARGET_RANGE use=1..400V below_415V_OVP\r\n"); }
        else { uart_puts("OK TARGET_V="); put_u32(value); uart_puts("\r\n"); }
        return;
    }
    if (text_starts_ci(line, "MODE "))
    {
        power_control_mode_t mode;
        if (text_equal_ci(line + 5, "OFF")) mode = POWER_CONTROL_OFF;
        else if (text_equal_ci(line + 5, "CV")) mode = POWER_CONTROL_CV;
        else if (text_equal_ci(line + 5, "MPPT")) mode = POWER_CONTROL_MPPT;
        else { ++command_errors; uart_puts("ERR E_MODE\r\n"); return; }
        value = (uint32_t)power_control_set_mode(mode);
        if (value == 0UL) {
            if (mode == POWER_CONTROL_OFF && !fault_latched) {
                safe_outputs_off();
                app_state = APP_STATE_SAFE_OFF;
                show_state_rgb(APP_STATE_SAFE_OFF);
            }
            uart_puts("OK MODE "); uart_puts(power_control_mode_name(mode));
            uart_puts(mode == POWER_CONTROL_OFF ? " outputs=DISABLED\r\n" : "\r\n");
        }
        else { ++command_errors; log_event(POWER_ERROR_CONFIG, mode); uart_puts("ERR E_CONTROL_LOCK boost_reachability_arming_calibration_or_backend_not_ready\r\n"); }
        return;
    }
    if (text_equal_ci(line, "CONTROL RESET"))
    {
        safe_outputs_off();
        if (power_control_clear_faults() == 0) { control_fault_seen = 0UL; uart_puts("OK CONTROL_RESET\r\n"); }
        else { ++command_errors; uart_puts("ERR E_CONTROL_ACTIVE\r\n"); }
        return;
    }
    if (text_equal_ci(line, "RGB")) { print_rgb(); return; }
    if (text_equal_ci(line, "UARTDIAG"))
    {
        board_uart_diag_t diag;
        board_uart_get_diag(&diag);
        uart_puts("UART baud=115200 mode=IRQ_ASYNC rx_errors=");
        put_u32(diag.error_count); uart_puts(" rx_drop="); put_u32(diag.rx_drop);
        uart_puts(" rx_queued="); put_u32(diag.rx_queued);
        uart_puts(" tx_queued="); put_u32(diag.tx_queued);
        uart_puts(" tx_drop="); put_u32(diag.tx_drop);
        uart_puts(" tx_frame_drop="); put_u32(diag.tx_frame_drop);
        uart_puts(" irq="); put_u32(diag.irq_count);
        uart_puts(" commands="); put_u32(command_count);
        uart_puts(" BRR="); put_u32(diag.brr);
        uart_puts(" PRESC="); put_u32(diag.presc);
        uart_puts("\r\n"); return;
    }
    if (text_equal_ci(line, "FAULTS")) { uart_puts("FAULT latch="); put_u32(fault_latched); uart_puts(" reason="); put_hex32(fault_reasons); uart_puts(" raw="); put_hex32(read_fault_inputs()); uart_puts(" control_seen="); put_hex32(control_fault_seen); uart_puts("\r\n"); return; }
    if (text_equal_ci(line, "FAULT") || text_equal_ci(line, "FAULT LATCH"))
    {
        (void)power_control_set_mode(POWER_CONTROL_OFF); safe_outputs_off();
        fault_latched = 1UL; fault_reasons |= POWER_FAULT_INTERLOCK;
        power_control_set_external_fault_latched(1UL);
        ++fault_count; app_state = APP_STATE_FAULT; show_state_rgb(APP_STATE_FAULT);
        log_event(POWER_ERROR_MANUAL_STOP, 0UL); uart_puts("ACK FAULT_LATCHED outputs=OFF\r\n"); return;
    }
    if (text_equal_ci(line, "FAULT CLEAR") || text_equal_ci(line, "CLEAR"))
    {
        uint32_t raw = read_fault_inputs();
        if (raw) { uart_puts("ERR E_ACTIVE_FAULT raw="); put_hex32(raw); uart_puts("\r\n"); }
        else if (clear_fault_to_safe()) { uart_puts("OK FAULT_CLEARED outputs=OFF\r\n"); }
        else { uart_puts("ERR E_CONTROL_FAULT_ACTIVE check=CONTROL,PROTECT\r\n"); }
        return;
    }
    if (text_equal_ci(line, "LOG") || text_equal_ci(line, "FAULTLOG") ||
        text_equal_ci(line, "EVENTLOG") || text_equal_ci(line, "HISTORY")) { print_log(); return; }
    if (text_equal_ci(line, "FAULTLOG STATUS"))
    {
        board_cal_store_diag_t diag;
        board_cal_store_get_diag(&diag);
        uart_puts("FAULTLOG storage=RAM+W25Q128 persistent=1 count="); put_u32(log_count);
        uart_puts(" capacity="); put_u32(LOG_CAPACITY); uart_puts(" latest="); put_u32(log_sequence);
        uart_puts(" pslots="); put_hex32(diag.fault_valid_slot_mask);
        uart_puts(" platest="); put_u32(diag.fault_latest_sequence);
        uart_puts(" latch="); put_u32(fault_latched); uart_puts("\r\n"); return;
    }
    if (text_equal_ci(line, "FAULTLOG ERASE") || text_equal_ci(line, "FAULTLOG CLEAR"))
    {
        uint32_t i;
        (void)i;
        if (fault_latched || read_fault_inputs() != 0UL) { ++command_errors; uart_puts("ERR E_ERASE_REQUIRES_SAFE_OFF\r\n"); return; }
        if (board_fault_store_erase() != 0) {
            ++command_errors;
            uart_puts("ERR E_FAULT_STORE_ERASE runtime_log_preserved\r\n");
            return;
        }
        clear_event_log();
        uart_puts("OK FAULTLOG_CLEARED storage=RAM+W25Q128\r\n"); return;
    }
    if (text_equal_ci(line, "GATE OFF") || text_equal_ci(line, "DISARM")) { (void)power_control_set_mode(POWER_CONTROL_OFF); safe_outputs_off(); if (!fault_latched) { app_state = APP_STATE_SAFE_OFF; show_state_rgb(APP_STATE_SAFE_OFF); } log_event(POWER_ERROR_MANUAL_STOP, 0UL); uart_puts("OK GATE_OFF outputs=DISABLED\r\n"); return; }
    if (text_equal_ci(line, "GATE ON")) { (void)power_control_set_mode(POWER_CONTROL_OFF); safe_outputs_off(); fault_latched = 1UL; fault_reasons |= POWER_FAULT_INTERLOCK; power_control_set_external_fault_latched(1UL); app_state = APP_STATE_FAULT; show_state_rgb(APP_STATE_FAULT); log_event(POWER_ERROR_INTERLOCK, 0UL); uart_puts("ERR E_COMMISSION_LOCK gate=LOCKED\r\n"); return; }
    if (text_equal_ci(line, "RESETCAUSE")) { print_reset_cause(); return; }
    if (text_equal_ci(line, "STATE")) { print_state_frame(); return; }
    if (text_equal_ci(line, "OFF")) { rgb[0] = 0U; rgb[1] = 0U; rgb[2] = 0U; app_state = APP_STATE_RGB_MANUAL; ws_show(); (void)power_control_set_mode(POWER_CONTROL_OFF); safe_outputs_off(); uart_puts("ACK OFF\r\n"); print_rgb(); return; }
    if (text_starts_ci(line, "SET ")) line += 4;
    if (parse_rgb(line, parsed)) { rgb[0] = parsed[0]; rgb[1] = parsed[1]; rgb[2] = parsed[2]; app_state = APP_STATE_RGB_MANUAL; ws_show(); log_event(0x6001UL, (rgb[0] << 16) | (rgb[1] << 8) | rgb[2]); uart_puts("ACK "); print_rgb(); return; }
    ++command_errors;
    last_error_code = 0x3001UL; last_error_arg = command_hash(line);
    log_event(last_error_code, last_error_arg);
    uart_puts("ERR E_UNKNOWN_COMMAND code="); put_hex32(last_error_code);
    uart_puts(" hash="); put_hex32(last_error_arg); uart_puts("\r\n");
}
static void uart_init(void)
{
    uart_frame_length = 0UL;
    uart_frame_discard = 0UL;
    board_uart_init();
}

void USART1_IRQHandler(void)
{
#if defined(APP_USE_RTTHREAD)
    rt_interrupt_enter();
#endif
    board_uart_irq_service();
#if defined(APP_USE_RTTHREAD)
    rt_interrupt_leave();
#endif
}

static uint32_t uart_getc(void)
{
    uint8_t value;
    return (board_uart_get_byte(&value) != 0UL) ? (uint32_t)value :
                                                 0xFFFFFFFFUL;
}
int main(void)
{
    char line[LINE_SIZE]; uint32_t length = 0UL;
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN;
    RCC_APB2ENR |= RCC_APB2ENR_HRTIM1EN;
    board_hrtim_safe_init();
    board_safety_init();
    safe_outputs_off();
    /* RT-Thread configured HSE/PLL and SysTick in rt_hw_board_init().  Never
     * switch clocks again after the scheduler tick has started. */
    if (board_clock_is_170mhz() == 0UL)
    {
        fault_latched = 1UL;
        fault_reasons |= POWER_FAULT_INTERLOCK;
    }
    if (app_time_init() == 0UL)
    {
        safe_outputs_off();
        fault_latched = 1UL;
        fault_reasons |= POWER_FAULT_INTERLOCK;
    }
    reset_cause_flags = RCC_CSR;
    RCC_CSR |= RCC_CSR_RMVF;
    safe_outputs_off();
    gpio_output(GPIOC, WS_PIN, 0UL);
    dwt_init(); heartbeat_enabled = HEARTBEAT_DEFAULT_ENABLED;
    next_heartbeat = app_now_ms() + HEARTBEAT_PERIOD_MS;
    /* The fast injected sampler is started by power_control_init().  Refresh
     * the status LED before that point, otherwise ws_show() correctly refuses
     * to mask the 100-kHz ADC interrupt and the LED keeps its reset colour. */
    show_state_rgb(APP_STATE_BOOT);
    app_state = APP_STATE_SAFE_OFF;
    show_state_rgb(APP_STATE_SAFE_OFF);
    uart_init();
    power_control_init();
    if (fault_latched != 0UL) power_control_set_external_fault_latched(1UL);
    (void)board_cal_store_init();
    load_calibration_at_boot();
#if defined(APP_USE_RTTHREAD)
    if (rt_start_workers() == 0UL) {
        safe_outputs_off();
        fault_latched = 1UL;
        fault_reasons |= POWER_FAULT_INTERLOCK;
        power_control_set_external_fault_latched(1UL);
    }
#endif
    log_event(0x1001UL, 0UL);
    uart_puts("BOOT "); uart_puts(FW_VERSION);
    uart_puts("\r\n"); print_clock_diag(); print_pinmap();
    print_cal_store();
    uart_puts("SAFE outputs=DISABLED gate=LOCKED protection=UNWIRED\r\n"); print_help();
    app_state = APP_STATE_SAFE_OFF;
    next_heartbeat = app_now_ms() + HEARTBEAT_PERIOD_MS;
    watchdog_init();
    for (;;)
    {
        uint32_t now_ms = app_now_ms();
        uint32_t raw = read_fault_inputs();
        uint32_t rx_budget = BOARD_RX_DRAIN_BUDGET_BYTES;
#if defined(APP_USE_RTTHREAD)
        raw |= rt_fault_guard_raw;
#endif
        power_control_poll_ms(now_ms);
        service_control_faults();
        if (raw) latch_fault(raw);
        while (rx_budget-- != 0UL) {
            uint32_t value = uart_getc();
            if (value == 0xFFFFFFFFUL) break;
            if (value == '\r' || value == '\n') {
                if (length) { line[length] = '\0'; command(line); length = 0UL; }
            } else if (value == '\b' || value == 0x7FUL) {
                if (length) --length;
            } else if (value >= 0x20UL && value <= 0x7EUL) {
                if ((length + 1UL) < sizeof(line)) line[length++] = (char)value;
                else { length = 0UL; ++command_errors; uart_puts("ERR E_LINE_TOO_LONG\r\n"); }
            }
        }
#if defined(APP_USE_RTTHREAD)
        if (heartbeat_enabled && rt_heartbeat_due != 0UL) {
            rt_heartbeat_due = 0UL;
            print_heartbeat();
        }
#else
        if (heartbeat_enabled && (int32_t)(now_ms - next_heartbeat) >= 0) {
            next_heartbeat += HEARTBEAT_PERIOD_MS;
            print_heartbeat();
        }
#endif
        watchdog_feed();
#if defined(APP_USE_RTTHREAD)
        rt_thread_mdelay(1);
#endif
    }
}
