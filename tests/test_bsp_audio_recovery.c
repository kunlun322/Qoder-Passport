// Exercise the real BSP, not a duplicate recovery state machine. The stubs below
// model esp_codec_dev 1.6.2's public-interface failure semantics: early opened
// flags, swallowed set_fmt/enable/I2C errors, and failed suspend retaining enabled.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../components/bsp/src/bsp_audio.c"

struct test_channel { bool running; uint32_t hz; } tx_channel, rx_channel;
struct test_codec {
    audio_codec_if_t base;
    const audio_codec_ctrl_if_t *ctrl;
    bool enabled;
};
struct test_codec_dev {
    const audio_codec_if_t *codec;
    const audio_codec_data_if_t *data;
    bool opened;
};
static uint8_t registers[256];
static int fail_reg = -1, fail_value = -1, fail_writes;
static int fail_tx_fmt, fail_rx_fmt, fail_tx_enable, fail_rx_enable;
static int fail_codec_delete;
static bool tx_data_enabled, rx_data_enabled;
static unsigned codec_creations, codec_starts, ctrl_creations, ctrl_deletions;
static bool read_reg0e_reserved_zero;

static int reg_write(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len,
                     void *value, int size) {
    (void)ctrl;
    assert(reg_len == 1 && size == 1);
    uint8_t byte = *(uint8_t *)value;
    if (reg == fail_reg && (fail_value < 0 || byte == fail_value) && fail_writes) {
        --fail_writes;
        return ESP_FAIL;
    }
    registers[reg] = byte;
    return ESP_OK;
}

static int reg_read(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len,
                    void *value, int size) {
    (void)ctrl;
    assert(reg_len == 1 && size == 1);
    *(uint8_t *)value = registers[reg];
    if (reg == 0x0e && read_reg0e_reserved_zero) *(uint8_t *)value &= 0x7f;
    return ESP_OK;
}

static bool ctrl_open(const audio_codec_ctrl_if_t *ctrl) { return ctrl != NULL; }
static int ctrl_info(const audio_codec_ctrl_if_t *ctrl, audio_codec_ctrl_info_t *info) {
    (void)ctrl;
    *info = (audio_codec_ctrl_info_t){ .i2c = { .addr = BSP_I2C_ES8311_ADDR << 1 } };
    return ESP_OK;
}
esp_err_t bsp_i2c_init(void) { return ESP_OK; }
void *bsp_i2c_bus(void) { return registers; }
esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }

const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *cfg) {
    assert(cfg->bus_handle == bsp_i2c_bus());
    audio_codec_ctrl_if_t *ctrl = calloc(1, sizeof(*ctrl));
    assert(ctrl);
    *ctrl = (audio_codec_ctrl_if_t){ .is_open = ctrl_open, .read_reg = reg_read,
                                   .write_reg = reg_write, .get_info = ctrl_info };
    ++ctrl_creations;
    return ctrl;
}
int audio_codec_delete_ctrl_if(const audio_codec_ctrl_if_t *ctrl) {
    ++ctrl_deletions;
    free((void *)ctrl);
    return ESP_OK;
}
const audio_codec_gpio_if_t *audio_codec_new_gpio(void) {
    return calloc(1, sizeof(audio_codec_gpio_if_t));
}
int audio_codec_delete_gpio_if(const audio_codec_gpio_if_t *gpio) {
    free((void *)gpio);
    return ESP_OK;
}
esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx,
                          i2s_chan_handle_t *rx) {
    (void)cfg;
    *tx = &tx_channel; *rx = &rx_channel;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t channel, const i2s_std_config_t *cfg) {
    channel->hz = cfg->clk_cfg.sample_rate_hz;
    return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t channel) {
    if (channel->running) return ESP_ERR_INVALID_STATE;
    channel->running = true;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t channel) {
    if (!channel->running) return ESP_ERR_INVALID_STATE;
    channel->running = false;
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t channel) {
    assert(!channel->running);
    return ESP_OK;
}

static bool data_open(const audio_codec_data_if_t *data) { return data != NULL; }
static int enable_one(bool output, bool enable) {
    int *fail = output ? &fail_tx_enable : &fail_rx_enable;
    i2s_chan_handle_t channel = output ? &tx_channel : &rx_channel;
    int result;
    if (enable && *fail) { --*fail; result = ESP_FAIL; }
    else result = enable ? i2s_channel_enable(channel) : i2s_channel_disable(channel);
    // The dependency updates its flags even when the driver rejects the change.
    if (output) tx_data_enabled = enable;
    else rx_data_enabled = enable;
    return result;
}
static int data_enable(const audio_codec_data_if_t *data, esp_codec_dev_type_t type, bool enable) {
    (void)data;
    int result = ESP_OK;
    if (type & ESP_CODEC_DEV_TYPE_OUT) result = enable_one(true, enable);
    if (type & ESP_CODEC_DEV_TYPE_IN) result = enable_one(false, enable);
    return result; // Deliberately reproduce the dependency's overwritten TX error.
}
static int format_one(bool output, esp_codec_dev_sample_info_t *fs) {
    int *fail = output ? &fail_tx_fmt : &fail_rx_fmt;
    i2s_chan_handle_t channel = output ? &tx_channel : &rx_channel;
    (void)i2s_channel_disable(channel);
    if (*fail) { --*fail; return ESP_FAIL; }
    channel->hz = fs->sample_rate;
    // Mirrors the data driver's full-duplex RX path when TX is not enabled.
    if (!output && !tx_data_enabled) {
        (void)i2s_channel_disable(&tx_channel);
        tx_channel.hz = fs->sample_rate;
        (void)i2s_channel_enable(&tx_channel);
    }
    return ESP_OK;
}
static int data_format(const audio_codec_data_if_t *data, esp_codec_dev_type_t type,
                       esp_codec_dev_sample_info_t *fs) {
    (void)data;
    // Reproduce check_fs_compatible(): stopped hardware alone is insufficient;
    // stale dependency enable flags would reject a new rate against the peer.
    if (type == ESP_CODEC_DEV_TYPE_OUT && rx_data_enabled && rx_channel.hz != fs->sample_rate)
        return ESP_FAIL;
    if (type == ESP_CODEC_DEV_TYPE_IN && tx_data_enabled && tx_channel.hz != fs->sample_rate)
        return ESP_FAIL;
    int result = ESP_OK;
    if (type & ESP_CODEC_DEV_TYPE_OUT) result = format_one(true, fs);
    if (type & ESP_CODEC_DEV_TYPE_IN) result = format_one(false, fs);
    return result; // Same swallowed first-direction error as 1.6.2.
}
static int data_read(const audio_codec_data_if_t *data, uint8_t *pcm, int size) {
    (void)data;
    memset(pcm, 1, (size_t)size);
    return rx_channel.running ? ESP_OK : ESP_FAIL;
}
static int data_write(const audio_codec_data_if_t *data, uint8_t *pcm, int size) {
    (void)data; (void)pcm; (void)size;
    return tx_channel.running ? ESP_OK : ESP_FAIL;
}
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *cfg) {
    assert(cfg->tx_handle == &tx_channel && cfg->rx_handle == &rx_channel);
    audio_codec_data_if_t *data = calloc(1, sizeof(*data));
    assert(data);
    *data = (audio_codec_data_if_t){ .is_open = data_open, .enable = data_enable,
        .set_fmt = data_format, .read = data_read, .write = data_write };
    return data;
}
int audio_codec_delete_data_if(const audio_codec_data_if_t *data) {
    tx_data_enabled = rx_data_enabled = false;
    free((void *)data);
    return ESP_OK;
}

static int codec_write(struct test_codec *codec, uint8_t reg, uint8_t value) {
    return codec->ctrl->write_reg(codec->ctrl, reg, 1, &value, 1);
}
static int codec_enable(const audio_codec_if_t *interface, bool enable) {
    struct test_codec *codec = (struct test_codec *)interface;
    if (codec->enabled == enable) return ESP_OK;
    if (enable) ++codec_starts;
    int result = codec_write(codec, 0x0d, enable ? 0x01 : 0xfc);
    result |= codec_write(codec, 0x0e, enable ? 0x02 : 0xff);
    if (result == ESP_OK) codec->enabled = enable;
    return result;
}
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *cfg) {
    struct test_codec *codec = calloc(1, sizeof(*codec));
    assert(codec);
    codec->base.enable = codec_enable;
    codec->ctrl = cfg->ctrl_if;
    ++codec_creations;
    // Initialization has intermediate write errors overwritten in 1.6.2.
    (void)codec_write(codec, 0x0d, 0xfa);
    (void)codec_write(codec, 0x44, 0x08);
    return &codec->base;
}
int audio_codec_delete_codec_if(const audio_codec_if_t *interface) {
    struct test_codec *codec = (struct test_codec *)interface;
    // close() runs another suspend, discards its result, then releases the object.
    (void)codec_write(codec, 0x0d, 0xfc);
    (void)codec_write(codec, 0x45, 0x00);
    free(codec);
    if (fail_codec_delete) { --fail_codec_delete; return ESP_FAIL; }
    return ESP_OK;
}
esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *cfg) {
    struct test_codec_dev *dev = calloc(1, sizeof(*dev));
    assert(dev);
    dev->codec = cfg->codec_if; dev->data = cfg->data_if;
    return dev;
}
int esp_codec_dev_open(esp_codec_dev_handle_t dev, esp_codec_dev_sample_info_t *fs) {
    if (dev->opened) return ESP_OK;
    dev->opened = true; // Real dependency sets this before configuration succeeds.
    (void)dev->data->set_fmt(dev->data, ESP_CODEC_DEV_TYPE_IN_OUT, fs);
    (void)dev->data->enable(dev->data, ESP_CODEC_DEV_TYPE_IN_OUT, true);
    (void)codec_write((struct test_codec *)dev->codec, 0x02, 0x12); // set_fs ignores I2C failure.
    return dev->codec->enable(dev->codec, true);
}
int esp_codec_dev_close(esp_codec_dev_handle_t dev) {
    if (dev->opened) {
        (void)dev->codec->enable(dev->codec, false);
        (void)dev->data->enable(dev->data, ESP_CODEC_DEV_TYPE_IN_OUT, false);
    }
    dev->opened = false;
    return ESP_OK;
}
void esp_codec_dev_delete(esp_codec_dev_handle_t dev) {
    (void)esp_codec_dev_close(dev);
    free(dev);
}
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t dev, float gain) {
    assert(gain == 30.0f);
    (void)codec_write((struct test_codec *)dev->codec, 0x16, 0x0a);
    return ESP_OK;
}
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t dev, int volume) {
    (void)codec_write((struct test_codec *)dev->codec, 0x32, (uint8_t)volume);
    return ESP_OK;
}
int esp_codec_dev_write(esp_codec_dev_handle_t dev, void *pcm, int bytes) {
    return dev->data->write(dev->data, pcm, bytes);
}
int esp_codec_dev_read(esp_codec_dev_handle_t dev, void *pcm, int bytes) {
    return dev->data->read(dev->data, pcm, bytes);
}

static void fresh(void) {
    fail_writes = 0;
    fail_tx_fmt = fail_rx_fmt = fail_tx_enable = fail_rx_enable = 0;
    audio_cleanup();
    memset(registers, 0, sizeof(registers));
    assert(bsp_audio_init() == ESP_OK);
}
static void fault(int reg, int value, int count) {
    fail_reg = reg; fail_value = value; fail_writes = count;
}
static void assert_rollback(void) {
    uint8_t pcm[2];
    assert(!s_dev && !s_codec && !s_opened);
    assert(!tx_channel.running && !rx_channel.running);
    assert(!tx_data_enabled && !rx_data_enabled);
    assert(bsp_audio_read(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE);
    assert(bsp_audio_write(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE);
}
static void assert_active(void) {
    assert(s_opened && !s_sleeping);
    assert(tx_channel.running && rx_channel.running);
    assert(registers[0x0d] == 0x01 && registers[0x0e] == 0x02);
}

int main(void) {
    fresh();
    bsp_audio_set_volume(43);
    assert(bsp_audio_set_format(16000, 16, 1) == ESP_OK);
    assert_active();
    unsigned created = codec_creations;
    assert(bsp_audio_set_format(16000, 16, 1) == ESP_OK);
    assert(codec_creations == created); // Same-format reuse remains cheap.
    assert(bsp_audio_set_format(8000, 16, 1) == ESP_OK);
    assert(tx_channel.hz == 8000 && rx_channel.hz == 8000);
    read_reg0e_reserved_zero = true;
    assert(bsp_audio_sleep() == ESP_OK);
    assert(registers[0x45] == 0x01); // Nothing writes weaker suspend after force-sleep.
    created = codec_creations;
    unsigned controls = ctrl_creations, deleted = ctrl_deletions;
    assert(bsp_audio_sleep() == ESP_OK);
    assert(bsp_audio_wake() == ESP_OK);
    assert(codec_creations == created + 1 && ctrl_creations == controls && ctrl_deletions == deleted);
    assert(tx_channel.hz == 8000 && registers[0x32] == 43);
    assert_active();

    fresh();
    unsigned starts = codec_starts;
    assert(bsp_audio_sleep() == ESP_OK); // Never opened: no silent open/start.
    assert(codec_starts == starts && !tx_channel.running && !rx_channel.running);
    assert(bsp_audio_wake() == ESP_OK);
    assert_active();

    // A failed close during a format change must revoke PCM access, then permit
    // a fresh constructor/open when the transient I2C fault has gone away.
    fault(0x0d, 0xfc, 1);
    assert(bsp_audio_set_format(8000, 16, 1) != ESP_OK);
    assert_rollback();
    assert(bsp_audio_set_format(8000, 16, 1) == ESP_OK);
    assert_active();

    assert(bsp_audio_sleep() == ESP_OK);
    fault(0x0d, 0xfa, 1); // Wake constructor itself encounters a swallowed I2C error.
    assert(bsp_audio_wake() != ESP_OK);
    assert_rollback();
    assert(s_sleeping);
    assert(bsp_audio_wake() == ESP_OK);
    assert_active();

    // Actual start failure and an error swallowed by set_fs are both rejected.
    const int failing_regs[] = { 0x0d, 0x02, 0x16, 0x32 };
    const int failing_values[] = { 0x01, 0x12, 0x0a, 0x00 };
    for (size_t i = 0; i < sizeof(failing_regs) / sizeof(failing_regs[0]); ++i) {
        fresh();
        fault(failing_regs[i], failing_values[i], 1);
        assert(bsp_audio_set_format(16000, 16, 1) != ESP_OK);
        assert_rollback();
        assert(bsp_audio_set_format(16000, 16, 1) == ESP_OK);
        assert_active();
    }

    // Test both data directions: a successful RX must never mask a failed TX.
    int *data_faults[] = { &fail_tx_fmt, &fail_rx_fmt, &fail_tx_enable, &fail_rx_enable };
    for (size_t i = 0; i < sizeof(data_faults) / sizeof(data_faults[0]); ++i) {
        fresh();
        *data_faults[i] = 1;
        assert(bsp_audio_set_format(16000, 16, 1) != ESP_OK);
        assert_rollback();
        assert(bsp_audio_set_format(16000, 16, 1) == ESP_OK);
        assert_active();
    }

    fresh();
    assert(bsp_audio_set_format(16000, 16, 1) == ESP_OK);
    fault(0x0d, 0xfc, 2); // Both dependency suspend attempts fail; force-sleep succeeds.
    assert(bsp_audio_sleep() != ESP_OK);
    assert_rollback();
    assert(registers[0x0d] == 0xfc && registers[0x45] == 0x01);
    assert(bsp_audio_sleep() != ESP_OK); // Repeated call must not hide the failure.
    starts = codec_starts;
    assert(bsp_audio_wake() == ESP_OK);
    assert(codec_starts == starts + 1);
    assert_active();

    assert(bsp_audio_sleep() == ESP_OK);
    fault(0x0d, 0x01, 1);
    assert(bsp_audio_wake() != ESP_OK);
    assert_rollback();
    assert(s_sleeping);
    assert(bsp_audio_wake() == ESP_OK);
    assert_active();

    audio_cleanup();
    fault(0x0d, 0xfa, 1); // Constructor swallows an early I2C error.
    assert(bsp_audio_init() != ESP_OK);
    assert(!s_initialized && !s_ctrl && !s_tx && !s_rx);
    assert(bsp_audio_init() == ESP_OK);
    assert(bsp_audio_set_format(16000, 16, 1) == ESP_OK);
    assert_active();
    // Failure to release a dependency reference is distinct from an I2C fault:
    // public delete has freed the object, so fail closed until reboot instead of
    // constructing against an uncertain reference count and reporting success.
    fail_codec_delete = 1;
    assert(bsp_audio_sleep() != ESP_OK);
    assert(bsp_audio_wake() != ESP_OK);
    assert_rollback();
    assert(bsp_audio_init() == ESP_ERR_INVALID_STATE);
    audio_cleanup();
    assert(ctrl_creations == ctrl_deletions);
    puts("BSP audio failure/retry/sleep/wake tests: PASS");
    return 0;
}
