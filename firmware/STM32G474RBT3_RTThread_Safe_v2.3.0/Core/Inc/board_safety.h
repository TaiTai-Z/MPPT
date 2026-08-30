#ifndef BOARD_SAFETY_H
#define BOARD_SAFETY_H

#include <stdint.h>

typedef struct {
    uint32_t raw_fault_inputs;
    uint32_t outputs_are_off;
    uint32_t hardware_fault_routing_confirmed;
    uint32_t v15_feedback_confirmed;
    uint32_t v15_pgood_runtime;
    uint32_t protection_backend_ready;
} board_safety_diag_t;

/* Initializes every physical containment signal before application services. */
void board_safety_init(void);

/* Idempotent emergency path: stops HRTIM, disables all U11 banks, asserts the
 * shared UCC27511 inhibit and turns the auxiliary 15-V request off. */
void board_safety_force_off(void);
/* Stops only the power stage (HRTIM/OE/gate inhibit) and preserves the
 * independent PA5 auxiliary-rail request. */
void board_safety_force_power_off(void);

uint32_t board_safety_read_fault_inputs(void);
uint32_t board_safety_outputs_are_off(void);
uint32_t board_safety_power_outputs_are_off(void);
uint32_t board_safety_protection_ready(void);
uint32_t board_safety_request_power_on(void);
/* Independent auxiliary 15-V request; never arms PWM by itself. */
uint32_t board_aux_set_request(uint32_t enabled);
uint32_t board_aux_get_request(void);
/* Hardware-specific builds must override this weak fail-closed function with
 * a measured MCU input.  Merely changing a confirmation macro cannot arm. */
uint32_t board_v15_pgood_read(void);
void board_safety_get_diag(board_safety_diag_t *diag);

#endif
