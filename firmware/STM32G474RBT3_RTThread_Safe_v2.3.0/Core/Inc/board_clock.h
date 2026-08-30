#ifndef BOARD_CLOCK_H
#define BOARD_CLOCK_H

#include <stdint.h>

typedef enum {
    BOARD_CLOCK_SOURCE_HSI16 = 0U,
    BOARD_CLOCK_SOURCE_HSE_PLL_170M = 1U
} board_clock_source_t;

typedef enum {
    BOARD_CLOCK_ERROR_NONE = 0U,
    BOARD_CLOCK_ERROR_HSI_READY = 1U,
    BOARD_CLOCK_ERROR_HSI_SWITCH = 2U,
    BOARD_CLOCK_ERROR_VOS = 3U,
    BOARD_CLOCK_ERROR_FLASH = 4U,
    BOARD_CLOCK_ERROR_HSE_READY = 5U,
    BOARD_CLOCK_ERROR_PLL_DISABLE = 6U,
    BOARD_CLOCK_ERROR_PLL_READY = 7U,
    BOARD_CLOCK_ERROR_PLL_SWITCH = 8U,
    BOARD_CLOCK_ERROR_CSS_ENABLE = 9U
} board_clock_error_t;

typedef struct {
    uint32_t source;
    uint32_t system_core_hz;
    uint32_t hse_ready;
    uint32_t pll_ready;
    uint32_t range1_boost;
    uint32_t flash_latency;
    uint32_t css_enabled;
    uint32_t fallback_active;
    uint32_t error;
} board_clock_diag_t;

uint32_t board_clock_init_170mhz(void);
uint32_t board_clock_is_170mhz(void);
uint32_t board_clock_get_hz(void);
void board_clock_get_diag(board_clock_diag_t *diag);

#endif
