#pragma once
#include "esp_adc/adc_cali.h"
typedef struct { int unit_id; int chan; int atten; int bitwidth; } adc_cali_curve_fitting_config_t;
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *, adc_cali_handle_t *);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t);
