#include "bsp_es8311_sleep_check.h"

// REG0E bit 7 is not a defined control bit and reads as 0 on reported boards.
// Keep writing the vendor suspend value 0xFF, but verify only bits 6:0.
const bsp_es8311_reg_check_t bsp_es8311_sleep_checks[] = {
    {0x00, 0x1F, 0xFF}, {0x01, 0x00, 0xFF}, {0x0D, 0xFC, 0xFF},
    {0x0E, 0x7F, 0x7F}, {0x12, 0x02, 0xFF}, {0x45, 0x01, 0xFF},
};

const size_t bsp_es8311_sleep_check_count =
    sizeof(bsp_es8311_sleep_checks) / sizeof(bsp_es8311_sleep_checks[0]);

bool bsp_es8311_sleep_check_matches(const bsp_es8311_reg_check_t *check,
                                  uint8_t actual)
{
    return (actual & check->mask) == (check->value & check->mask);
}
