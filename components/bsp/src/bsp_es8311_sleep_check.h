// Hardware-independent ES8311 suspend readback policy; private to the BSP.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t reg;
    uint8_t value;
    uint8_t mask;
} bsp_es8311_reg_check_t;

extern const bsp_es8311_reg_check_t bsp_es8311_sleep_checks[];
extern const size_t bsp_es8311_sleep_check_count;

// Call only after a successful register read; an I2C error is never a match.
bool bsp_es8311_sleep_check_matches(const bsp_es8311_reg_check_t *check,
                                  uint8_t actual);
