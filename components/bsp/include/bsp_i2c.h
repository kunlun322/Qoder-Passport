// components/bsp/include/bsp_i2c.h
// ES8311(0x18)与 CW2017(0x63)共用一条 I2C 总线。由本模块独立持有总线句柄,
// 两个驱动都向它索取 —— 避免"谁先初始化谁建总线"的隐式顺序依赖。
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

// 初始化共享总线。幂等:重复调用直接返回 ESP_OK,可在每个驱动的 init 里放心调。
esp_err_t bsp_i2c_init(void);

// 取共享总线句柄。未初始化时返回 NULL。
i2c_master_bus_handle_t bsp_i2c_bus(void);

// 扫描 0x08..0x77 并打印所有应答的设备。排查"芯片是不是没焊好/地址对不对"极有用。
// 直接在正式总线上扫,不要另开临时总线 —— 原因见 bsp_i2c.c 中 bsp_i2c_scan() 的注释。
esp_err_t bsp_i2c_scan(void);

// deep sleep 专用：所有共享外设的最后一笔 I2C 事务完成后，将 SDA/SCL
// 设为无上下拉的高阻输入。调用后禁止再访问总线，必须立即进入 deep sleep
// 或重启。板上外部上拉电阻的静态功耗不受此接口控制。
esp_err_t bsp_i2c_prepare_deep_sleep(void);
