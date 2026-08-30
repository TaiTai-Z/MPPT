#ifndef BOARD_CAL_STORE_H
#define BOARD_CAL_STORE_H

#include <stdint.h>
#include "power_control.h"

typedef struct {
    uint32_t ready;
    uint32_t jedec_id;
    uint32_t valid_slot_mask;
    uint32_t active_sequence;
    uint32_t load_count;
    uint32_t save_count;
    uint32_t erase_count;
    int32_t last_result;
    uint32_t fault_valid_slot_mask;
    uint32_t fault_latest_sequence;
    uint32_t fault_append_count;
    uint32_t fault_erase_count;
} board_cal_store_diag_t;

typedef struct {
    uint32_t code;
    uint32_t arg;
    uint32_t uptime_ms;
    uint32_t state;
    uint32_t fault_bits;
    uint32_t raw_inputs;
    int32_t pv_mv;
    int32_t vbus_mv;
    int32_t current_ma;
    uint32_t sample_sequence;
} board_fault_record_t;

uint32_t board_cal_store_init(void);
int board_cal_store_load(power_calibration_t *calibration);
int board_cal_store_save(const power_calibration_t *calibration);
int board_cal_store_erase(void);
int board_fault_store_append(const board_fault_record_t *record);
int board_fault_store_read_recent(uint32_t newest_index,
                                  board_fault_record_t *record,
                                  uint32_t *sequence);
int board_fault_store_erase(void);
void board_cal_store_get_diag(board_cal_store_diag_t *diag);

#endif
