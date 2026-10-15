#include "demo_stubs/demo_runtime.c"
#include "../main/demo_ble.c"

int main(void) {
    test_sem_fails = true;
    assert(demo_ble_start() == ESP_ERR_NO_MEM);
    assert(!s_initialized && !s_host_stopped && !s_host_task);
    assert(test_nimble_stops == 0);
    test_sem_fails = false;
    test_nimble_name_result = 1;
    assert(demo_ble_start() == ESP_FAIL);
    assert(!s_initialized && !s_host_stopped && !s_host_task);
    assert(test_nimble_stops == 0);
    test_nimble_name_result = 0;
    test_nimble_deinits = 0;
    test_create_fails = true;
    assert(demo_ble_start() == ESP_ERR_NO_MEM);
    assert(!s_initialized && !s_host_task && !s_host_stopped);
    assert(test_nimble_stops == 0 && test_nimble_deinits == 1);
    assert(demo_ble_stop() == ESP_OK); // Creation failure must not trap the page.

    // Failed cleanup retains ownership so a later exit can retry it.
    test_nimble_deinit_result = ESP_FAIL;
    assert(demo_ble_start() == ESP_ERR_NO_MEM);
    assert(s_initialized && !s_host_task);
    assert(demo_ble_stop() == ESP_FAIL);
    assert(test_nimble_stops == 0);
    test_nimble_deinit_result = ESP_OK;
    assert(demo_ble_stop() == ESP_OK);
    assert(!s_initialized && !s_host_stopped);

    test_create_fails = false;
    assert(demo_ble_start() == ESP_OK);
    TaskHandle_t task = s_host_task;
    assert(demo_ble_stop() == ESP_ERR_TIMEOUT);
    assert(s_host_task == task && !task->deleted);
    assert(test_nimble_stops == 1);
    test_run_worker(task);
    assert(s_host_task == task && s_host_stopped->available);
    test_nimble_deinit_result = ESP_FAIL;
    assert(demo_ble_stop() == ESP_FAIL);
    assert(task->deleted && !s_host_task && s_initialized);
    assert(test_nimble_stops == 1); // Do not restart an already stopped host.
    test_nimble_deinit_result = ESP_OK;
    assert(demo_ble_stop() == ESP_OK);
    assert(!s_initialized && !s_host_stopped);
    test_auto_ack = true;
    assert(demo_ble_start() == ESP_OK);
    assert(s_host_task != task);
    assert(demo_ble_stop() == ESP_OK);
    assert(test_task_deletes == 2);
    puts("BLE allocation, cleanup retry and host handoff tests: PASS");
    return 0;
}
