#include "bsp_es8311_sleep_check.h"

#include <assert.h>

int main(void)
{
    const uint8_t registers[] = {0x00, 0x01, 0x0D, 0x0E, 0x12, 0x45};
    const uint8_t values[] = {0x1F, 0x00, 0xFC, 0x7F, 0x02, 0x01};
    assert(bsp_es8311_sleep_check_count == sizeof(registers));

    for (size_t i = 0; i < bsp_es8311_sleep_check_count; i++) {
        const bsp_es8311_reg_check_t *check = &bsp_es8311_sleep_checks[i];
        assert(check->reg == registers[i]);

        // Exercise the production table/predicate over every possible readback.
        // Only REG0E may accept two values; all other registers stay exact.
        for (unsigned actual = 0; actual <= UINT8_MAX; actual++) {
            bool expected = actual == values[i] ||
                            (registers[i] == 0x0E && actual == 0xFF);
            assert(bsp_es8311_sleep_check_matches(check, (uint8_t)actual) == expected);
        }
    }

    // A set bit outside the mask in an expected value is ignored as well.
    const bsp_es8311_reg_check_t unmasked_expected = {0x0E, 0xFF, 0x7F};
    assert(bsp_es8311_sleep_check_matches(&unmasked_expected, 0x7F));
    assert(bsp_es8311_sleep_check_matches(&unmasked_expected, 0xFF));
    assert(!bsp_es8311_sleep_check_matches(&unmasked_expected, 0x7E));
    return 0;
}
