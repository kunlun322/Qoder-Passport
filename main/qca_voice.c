// main/qca_voice.c —— 见 qca_voice.h。
//
// 协议（voice.realtime.v1）要点：
//   上行  audio.append  payload.audio = base64(PCM16 16 kHz mono)，每 80 ms 一包
//         interrupt     打断助手
//         playback.started / playback.ended / playback.cancelled  播放回执
//   下行  voice.ready   可以开始说话
//         audio.delta   payload.audio = base64(PCM16 24 kHz mono)，单条约 25 KB
//         audio.done    本段音频结束
//         voice.state   payload.state（thinking / interrupted …）
//         transcript.final  payload.role + payload.text
//         error / connection.closed / voice.replaced
// 打断后服务端仍可能补发一段残留 delta：打开“丢弃窗口”，直到 voice.state=interrupted、
// audio.done 或 2 s 兜底。
#include "qca_voice.h"

#include <stdio.h>
#include <string.h>

#include "app_fonts.h"
#include "audio_dsp.h"
#include "b64_stream.h"
#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "qca_config.h"
#include "qca_pat.h"
#include "qjson.h"
#include "utf8_util.h"

static const char *TAG = "qca_voice";

#define SR                 16000
#define FRAME_SAMPLES      (SR * VOICE_FRAME_MS / 1000)   // 320
#define FRAME_BYTES        (FRAME_SAMPLES * 2)            // 640
// 80 ms 一包：JSON ≈ 3.5 KB，小于 WebSocket 发送缓冲（WS_BUF_BYTES），不会被拆成续帧
// （协议文档没有承诺支持分片帧）。WebSocket 收发缓冲各占 WS_BUF_BYTES 堆，整个会话常驻。
// 曾试过 40 ms/2KB 省 4KB 堆(boot18)：包数翻倍后上行几乎每 100~200 ms 就因发送队列未
// 回落到可写水位而丢一包，服务端随即中断回答。堆的余量已由 Wi-Fi 动态收发缓冲上限
// (sdkconfig.defaults)解决(boot18 历史最低 12.7KB、分配失败 0 次)，因此恢复 80 ms/4KB。
// 下行按块流式解析，与缓冲大小无关。
#define UP_FRAMES          4
#define UP_PCM_BYTES       (FRAME_BYTES * UP_FRAMES)       // 2560
#define UP_JSON_CAP        (64 + 4 * ((UP_PCM_BYTES + 2) / 3) + 16)
#define WS_BUF_BYTES       4096
_Static_assert(UP_JSON_CAP < WS_BUF_BYTES, "audio.append 必须装进一个 WebSocket 帧");
#define RING_FRAMES        10                              // 插话前 200 ms 回补
// C3 无 PSRAM。早期动态 TLS 缓冲下 12KB 播放缓冲会挤掉 TLS 的大块分配；现在 TLS 记录缓冲
// 是握手时固定分配的(IN 8KB/OUT 4KB，见 sdkconfig.defaults)，会话中不再有大块分配，
// 实测音频任务启动后仍剩约 34KB 堆，因此把播放缓冲恢复到 12KB。
// 下行是 24kHz PCM 的 base64(≈64KB/s)，8KB TCP 窗口在 ~80ms RTT 下只有约 1.5 倍余量，
// Wi-Fi 抖动时很容易短暂慢于实时；更深的预缓冲用来吸收这类抖动，代价是起播多等约 0.1 秒。
// 缓冲在握手后分配，会话结束即释放，重试的 HTTPS 不与它争抢内存。
#define PLAY_SB_BYTES      (12 * 1024)                     // ≈ 384 ms @16k
#define PREROLL_BYTES      (SR * 2 * 220 / 1000)           // 220 ms
#define REBUF_BYTES        (SR * 2 * 160 / 1000)           // 断流后再缓冲 160 ms
_Static_assert(PREROLL_BYTES <= PLAY_SB_BYTES, "播放预缓冲不能超过队列容量");
_Static_assert(REBUF_BYTES <= PLAY_SB_BYTES, "断流预缓冲不能超过队列容量");
#define PREROLL_WAIT_MS    800
#define REBUF_WAIT_MS      500
// 插话后丢弃旧回答音频，正常由 voice.state=interrupted / audio.done 关闭。
// 实测下行积压时 interrupted 可能晚到 4~5 秒，窗口过早超时会把旧回答的残音放出来并再次误判插话，
// 所以超时只作兜底。
#define DROP_WINDOW_MS     8000
#define DUCK_MS            300
#define DUCK_GAIN_Q15      8192                            // -12 dB
#define CHIME_GUARD_MS     250
#define DEFAULT_VOLUME     80

#define EV_CTL_EXIT   BIT0
#define EV_MIC_EXIT   BIT1
#define EV_PLAY_EXIT  BIT2

typedef enum { CHIME_NONE = 0, CHIME_START, CHIME_RESUME, CHIME_PAUSE } chime_t;

// ---------------- 共享状态 ----------------
static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_evq;            // vs_event_t
static StreamBufferHandle_t s_sb;      // 16 kHz PCM，ws → play
static EventGroupHandle_t s_eg;
static SemaphoreHandle_t s_view_mtx;
static SemaphoreHandle_t s_send_mtx;   // 保护 s_small_buf 发送
static TaskHandle_t s_ctl_task;

static volatile bool s_running;        // 会话进行中（任务循环条件）
static volatile bool s_stop_req;
static volatile bool s_connected;
static volatile bool s_ready;
static volatile vs_state_t s_state = VS_ENDED;
static volatile bool s_uplink;          // 由 ctl 计算
static volatile bool s_send_ring;       // 插话确认后先回补环形缓冲
static volatile int s_volume = DEFAULT_VOLUME;
// 音量持久化：会话开始时读取，会话结束时才写入，避免播放中写 Flash 暂停缓存造成卡顿。
#define VOL_NVS_NS   "qca_voice"
#define VOL_NVS_KEY  "vol"
#define VOL_MIN_SAVED 10            // 存成 0 会让下次开机像“没声音”，与参考固件一样兜底到 10
static bool s_vol_loaded;
static int s_vol_saved = -1;

static void volume_load(void) {
    if (s_vol_loaded) return;
    s_vol_loaded = true;
    nvs_handle_t h;
    if (nvs_open(VOL_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // 首次使用：命名空间尚不存在
    uint8_t v = 0;
    if (nvs_get_u8(h, VOL_NVS_KEY, &v) == ESP_OK && v <= 100) {
        s_volume = v < VOL_MIN_SAVED ? VOL_MIN_SAVED : v;
        s_vol_saved = v;
    }
    nvs_close(h);
}

static void volume_save(void) {
    const int v = s_volume;
    if (!s_vol_loaded || v == s_vol_saved) return;
    nvs_handle_t h;
    if (nvs_open(VOL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, VOL_NVS_KEY, (uint8_t)v) == ESP_OK && nvs_commit(h) == ESP_OK) s_vol_saved = v;
    nvs_close(h);
    ESP_LOGI(TAG, "音量 %d%% 已保存", v);
}

// 下行段落
static volatile bool s_drop;            // 丢弃窗口
static volatile int64_t s_drop_until_ms;
static volatile bool s_seg_active;      // 有一段助手音频正在缓冲/播放
static volatile bool s_seg_done;        // 收到本段 audio.done
static volatile bool s_seg_started_sent;
static volatile bool s_seg_cancelled;   // 暂停中收到的段：丢弃
static volatile int64_t s_seg_first_ms;
// 每段播放的诊断统计（仅用于串口日志判断是否断流）。
static struct {
    uint32_t played_bytes;
    uint32_t underruns;
    uint32_t stall_ms;
    uint32_t peak_bytes;
    int32_t start_ms;    // 首包到开始出声，-1 = 尚未出声
} s_ps;
// 服务端 ID 长度不固定；截断后的 ID 会让回执被判无效，因此留足余量。
#define QCA_ID_MAX 96
static char s_seg_work[QCA_ID_MAX], s_seg_ann[QCA_ID_MAX];

// 播放侧
static volatile bool s_flush_req;       // 淡出并清空
static volatile chime_t s_chime_req;
static volatile bool s_chime_active;
static volatile int64_t s_chime_guard_until;
static volatile int s_out_level;        // 最近一帧输出电平
static volatile int64_t s_duck_until;
static volatile bool s_playing;

// 回声标定（开场提示音）
static volatile bool s_calib;
static volatile int s_calib_out_sum, s_calib_out_n;
static volatile int s_calib_mic_sum, s_calib_mic_n;
static volatile int s_echo_gain_q8 = 256;
// 标定时的音量。输出电平在 codec 音量之前测得，会话中调音量后回声按 0.5 dB/级变化，
// 插话判定要用按当前音量换算的增益，否则调大音量后回声被误判为插话。
static volatile int s_echo_calib_vol = -1;
// 界面快照
static qca_voice_view_t s_view;
static volatile int s_mic_level;

static char s_conv[64];
static char s_ws_uri[160];
static char s_ws_hdr[QCA_PAT_MAX + 32];

// 最近一次 WebSocket 错误（ws 任务写，断开时用于生成界面提示）。
static volatile int s_ws_http_status;     // 升级握手的 HTTP 状态码（0=未拿到）
static volatile esp_err_t s_ws_tls_err;   // 只读取 TCP_TRANSPORT 类型的有效字段
static volatile int s_ws_close_code;     // 服务端关闭帧状态码
static volatile bool s_ws_failed;
// 只由 CONNECTED 回调创建，销毁 ws（回调已停止）后 ctl 才读取退出等待集。
static bool s_mic_up, s_play_up;
static bool media_start(bool *mic_up, bool *play_up);

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void post(vs_event_t ev) {
    if (s_evq) (void)xQueueSend(s_evq, &ev, 0);
}

static void view_set_line(const char *text, bool is_user) {
    if (!s_view_mtx) return;
    xSemaphoreTake(s_view_mtx, portMAX_DELAY);
    snprintf(s_view.line, sizeof(s_view.line), "%s", text);
    app_text_sanitize(s_view.line, sizeof(s_view.line) - 1);
    s_view.line_is_user = is_user;
    s_view.line_seq++;
    xSemaphoreGive(s_view_mtx);
}

// ---------------- 上行发送 ----------------
// 发送超时即被 WebSocket 客户端判为断线（1.8.0 写超时会 abort 连接，且写期间持锁、
// 收不了下行）。实机曾在回答结束、上行恢复后约 1.5 s 内遇到 TCP 发送区满 2 s，整个
// 会话因此报错。所以：语音帧先无阻塞检查套接字可写，不可写就丢掉这一帧（实时语音
// 丢 80 ms 好过断线）；控制/回执消息很小，给更长的超时。
#define CTRL_SEND_TIMEOUT_MS  4000
#define AUDIO_SEND_TIMEOUT_MS 2000
static volatile int s_ws_fd = -1;
static uint32_t s_up_dropped;            // 本轮拥塞丢弃的上行帧（仅 mic 任务读写）

static bool ws_send(const char *data, int len, int timeout_ms) {
    if (!s_ws || !s_connected || !s_running || s_stop_req) return false;
    return esp_websocket_client_send_text(s_ws, data, len, pdMS_TO_TICKS(timeout_ms)) == len;
}

// 在 CONNECTED 回调中调用：语音会话期间设备上只有这一条已连接的 TCP 连接。
// 不唯一或找不到时返回 -1，上行退回原来的阻塞发送，不做猜测。
static int find_ws_socket(void) {
    int found = -1;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        int type = 0;
        socklen_t tl = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM) continue;
        struct sockaddr_storage peer;
        socklen_t pl = sizeof(peer);
        if (getpeername(fd, (struct sockaddr *)&peer, &pl) != 0) continue;
        if (found >= 0) return -1;
        found = fd;
    }
    return found;
}

// lwIP 在发送区空闲超过一半(≈5.7KB)时才报可写，足够放下一帧(≈3.5KB)，写入不会阻塞。
// 连接出错时同样报可写，随后的发送会正常走断线流程。
static bool uplink_writable(void) {
    const int fd = s_ws_fd;
    if (fd < 0) return true;
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {0, 0};
    return select(fd + 1, NULL, &wfds, NULL, &tv) > 0;
}

// 回执只携带非空的 work_id / announcement_id：空字符串会被服务端判为无效回执。
static void send_control(const char *type, bool with_ids) {
    char buf[384];
    const bool w = with_ids && s_seg_work[0];
    const bool a = with_ids && s_seg_ann[0];
    const int n = snprintf(buf, sizeof(buf),
                           "{\"version\":\"" QCA_PROTO_VERSION "\",\"type\":\"%s\",\"payload\":{"
                           "%s%s%s%s%s%s%s}}",
                           type,
                           w ? "\"work_id\":\"" : "", w ? s_seg_work : "", w ? "\"" : "",
                           (w && a) ? "," : "",
                           a ? "\"announcement_id\":\"" : "", a ? s_seg_ann : "", a ? "\"" : "");
    if (n <= 0 || n >= (int)sizeof(buf)) {
        ESP_LOGW(TAG, "控制消息过长，未发送: %s", type);
        return;
    }
    xSemaphoreTake(s_send_mtx, portMAX_DELAY);
    const bool ok = ws_send(buf, n, CTRL_SEND_TIMEOUT_MS);
    xSemaphoreGive(s_send_mtx);
    // 只记录 ID 是否存在及长度，不把 ID 本身写进日志。
    if (with_ids) {
        ESP_LOGI(TAG, "→ %s [work:%u ann:%u]%s", type, (unsigned)strlen(s_seg_work),
                 (unsigned)strlen(s_seg_ann), ok ? "" : "（发送失败）");
    } else {
        ESP_LOGI(TAG, "→ %s%s", type, ok ? "" : "（发送失败）");
    }
}

static char s_up_json[UP_JSON_CAP];

static void send_audio(const uint8_t *pcm, size_t bytes) {
    static const char PFX[] = "{\"version\":\"" QCA_PROTO_VERSION "\",\"type\":\"audio.append\","
                              "\"payload\":{\"audio\":\"";
    static const char SFX[] = "\"}}";
    if (!uplink_writable()) {
        if (s_up_dropped++ == 0) ESP_LOGW(TAG, "上行拥塞，暂时丢弃语音帧");
        return;
    }
    if (s_up_dropped) {
        ESP_LOGW(TAG, "上行恢复，共丢弃 %u 包(每包 %d ms)", (unsigned)s_up_dropped,
                 UP_FRAMES * VOICE_FRAME_MS);
        s_up_dropped = 0;
    }
    size_t w = sizeof(PFX) - 1;
    memcpy(s_up_json, PFX, w);
    const size_t e = b64_encode(pcm, bytes, s_up_json + w, sizeof(s_up_json) - w - sizeof(SFX));
    if (e == 0) return;
    w += e;
    memcpy(s_up_json + w, SFX, sizeof(SFX) - 1);
    w += sizeof(SFX) - 1;
    (void)ws_send(s_up_json, (int)w, AUDIO_SEND_TIMEOUT_MS);   // 客户端内部有锁，可与回执并发
}

// ---------------- 下行解析 ----------------
typedef struct {
    char type[32];
    char state[24];
    char role[16];
    char text[QCA_VOICE_LINE_MAX];
    char code[32];
    char work[QCA_ID_MAX];
    char ann[QCA_ID_MAX];
    char *dst;
    size_t cap, len;
    bool audio_seen;
    b64_decoder_t b64;
    dsp_rs_t rs;
    uint8_t odd;            // 上一分片残留的半个样本
    bool has_odd;
} rx_ctx_t;

static rx_ctx_t s_rx;
static qj_parser_t s_rxp;
static bool s_rx_in_msg;

static void rx_reset(void) {
    memset(&s_rx, 0, sizeof(s_rx));
    b64_decoder_init(&s_rx.b64);
    dsp_rs_init(&s_rx.rs);
}

// 新一段助手音频开始（第一包 delta）。
static void segment_begin(void) {
    s_seg_active = true;
    s_seg_done = false;
    s_seg_started_sent = false;
    s_seg_cancelled = (s_state == VS_PAUSED);
    s_seg_first_ms = now_ms();
    memset(&s_ps, 0, sizeof(s_ps));
    s_ps.start_ms = -1;
    s_seg_work[0] = s_seg_ann[0] = '\0';
    if (!s_seg_cancelled) post(VE_AUDIO_FIRST);
}

static void push_pcm24(const uint8_t *bytes, size_t n) {
    // 拼成完整的 int16 样本（分片可能把一个样本切成两半）。
    int16_t in[48];
    int16_t out[34];
    size_t i = 0;
    while (i < n) {
        size_t k = 0;
        if (s_rx.has_odd) {
            in[k++] = (int16_t)(s_rx.odd | ((uint16_t)bytes[i++] << 8));
            s_rx.has_odd = false;
        }
        while (k < 48 && i + 1 < n) {
            in[k++] = (int16_t)(bytes[i] | ((uint16_t)bytes[i + 1] << 8));
            i += 2;
        }
        if (k < 48 && i < n && i + 1 >= n) {
            s_rx.odd = bytes[i++];
            s_rx.has_odd = true;
        }
        const size_t m = dsp_rs_24to16(&s_rx.rs, in, k, out);
        const uint8_t *p = (const uint8_t *)out;
        size_t left = m * 2;
        while (left > 0) {
            if (!s_sb || s_drop || s_stop_req || s_seg_cancelled || !s_running) return;
            const size_t sent = xStreamBufferSend(s_sb, p, left, pdMS_TO_TICKS(20));
            p += sent;
            left -= sent;
        }
    }
}

static bool discarding(void) {
    return s_drop || s_seg_cancelled || s_state == VS_PAUSED || s_state == VS_ENDED ||
           s_state == VS_ERROR;
}

static void rx_cb(void *user, qj_parser_t *p, qj_event_t ev, const char *data, size_t len) {
    (void)user;
    const int d = qj_depth(p);
    if (ev != QJ_EV_STR_CHUNK && ev != QJ_EV_STR_END) return;
    const char *k = qj_key(p, d);
    const bool in_payload = d == 2 && strcmp(qj_key(p, 1), "payload") == 0;

    // payload.audio：边解码边写入播放缓冲。
    if (in_payload && strcmp(k, "audio") == 0) {
        if (ev == QJ_EV_STR_END) return;
        if (!s_rx.audio_seen) {
            s_rx.audio_seen = true;
            b64_decoder_init(&s_rx.b64);
            s_rx.has_odd = false;
            if (!s_drop && !s_seg_active) {
                segment_begin();
                dsp_rs_init(&s_rx.rs);
            }
        }
        if (discarding()) return;
        uint8_t pcm[80];
        const size_t n = b64_decode_chunk(&s_rx.b64, data, len, pcm, sizeof(pcm));
        if (n) push_pcm24(pcm, n);
        return;
    }

    if (ev == QJ_EV_STR_END) { s_rx.dst = NULL; return; }
    if (!s_rx.dst) {
        char *dst = NULL;
        size_t cap = 0;
        if (d == 1 && strcmp(k, "type") == 0) { dst = s_rx.type; cap = sizeof(s_rx.type); }
        // ID 以根级字段为准，payload 内的同名字段只在根级缺失时补位。
        else if ((d == 1 || (in_payload && !s_rx.work[0])) && strcmp(k, "work_id") == 0) { dst = s_rx.work; cap = sizeof(s_rx.work); }
        else if ((d == 1 || (in_payload && !s_rx.ann[0])) && strcmp(k, "announcement_id") == 0) { dst = s_rx.ann; cap = sizeof(s_rx.ann); }
        else if (in_payload && strcmp(k, "state") == 0) { dst = s_rx.state; cap = sizeof(s_rx.state); }
        else if (in_payload && strcmp(k, "role") == 0) { dst = s_rx.role; cap = sizeof(s_rx.role); }
        else if (in_payload && (strcmp(k, "text") == 0 || strcmp(k, "message") == 0)) { dst = s_rx.text; cap = sizeof(s_rx.text); }
        else if (in_payload && strcmp(k, "code") == 0) { dst = s_rx.code; cap = sizeof(s_rx.code); }
        if (!dst) return;
        s_rx.dst = dst;
        s_rx.cap = cap;
        s_rx.len = 0;
        dst[0] = '\0';
    }
    size_t room = s_rx.cap - 1 - s_rx.len;
    if (len > room) len = room;
    memcpy(s_rx.dst + s_rx.len, data, len);
    s_rx.len += len;
    s_rx.dst[s_rx.len] = '\0';
}

static void close_drop_window(const char *why) {
    if (!s_drop) return;
    s_drop = false;
    ESP_LOGI(TAG, "丢弃窗口关闭（%s）", why);
}

// 一条完整消息解析完毕。
static void rx_message_done(void) {
    const char *t = s_rx.type;
    if (strcmp(t, "audio.delta") == 0) {
        // 两个 ID 可能分别出现在不同的分片里，各自在首次出现时记录。
        if (s_seg_active && !s_seg_work[0] && s_rx.work[0])
            snprintf(s_seg_work, sizeof(s_seg_work), "%s", s_rx.work);
        if (s_seg_active && !s_seg_ann[0] && s_rx.ann[0])
            snprintf(s_seg_ann, sizeof(s_seg_ann), "%s", s_rx.ann);
        if (s_seg_active && s_seg_cancelled && !s_seg_started_sent) {
            s_seg_started_sent = true;   // 只回一次 cancelled
            send_control("playback.cancelled", true);
        } else if (s_seg_active && !s_seg_started_sent && !s_drop) {
            s_seg_started_sent = true;
            send_control("playback.started", true);
        }
        post(VE_ACTIVITY);
    } else if (strcmp(t, "audio.done") == 0) {
        if (s_drop) {
            close_drop_window("audio.done");
        } else if (s_seg_active) {
            s_seg_done = true;
            if (s_seg_cancelled) { s_seg_active = false; s_seg_cancelled = false; }
        }
    } else if (strcmp(t, "voice.ready") == 0) {
        s_ready = true;
        post(VE_READY);
    } else if (strcmp(t, "voice.state") == 0) {
        ESP_LOGI(TAG, "voice.state=%s", s_rx.state);
        if (strcmp(s_rx.state, "interrupted") == 0) close_drop_window("interrupted");
        else if (strcmp(s_rx.state, "thinking") == 0 || strcmp(s_rx.state, "processing") == 0)
            post(VE_SERVER_THINKING);
    } else if (strcmp(t, "transcript.final") == 0) {
        const bool user = strcmp(s_rx.role, "user") == 0;
        if (s_rx.text[0]) {
            view_set_line(s_rx.text, user);   // “你：/助手：”前缀由界面添加
            ESP_LOGI(TAG, "转写[%s] %u 字", s_rx.role, (unsigned)utf8_count(s_rx.text));
        }
        if (user && voice_is_pause_phrase(s_rx.text)) post(VE_PAUSE_PHRASE);
        post(VE_ACTIVITY);
    } else if (strcmp(t, "playback.interrupt") == 0) {
        // 服务端要求停播（例如服务端 VAD 判定用户插话）。服务端此后进入 idle 等用户说话，
        // 所以本地也必须离开“回答中”重新开上行，否则双方互等(boot18 静默 105 s)。
        ESP_LOGI(TAG, "服务端要求停播");
        if (s_seg_active) {
            s_flush_req = true;
            send_control("playback.cancelled", true);
            s_seg_active = false;
            s_seg_done = false;
        }
        post(VE_SERVER_INTERRUPT);
    } else if (strcmp(t, "error") == 0) {
        // message 仅写本地串口日志，便于定位回执等协议错误。
        ESP_LOGW(TAG, "服务端错误 code=%s msg=%s", s_rx.code, s_rx.text);
        char line[QCA_VOICE_LINE_MAX];
        snprintf(line, sizeof(line), "服务提示：%s", s_rx.code[0] ? s_rx.code : "未知错误");
        view_set_line(line, false);
        if (!s_ready) {
            xSemaphoreTake(s_view_mtx, portMAX_DELAY);
            snprintf(s_view.error, sizeof(s_view.error), "%s", s_rx.code[0] ? s_rx.code : "连接被拒绝");
            app_text_sanitize(s_view.error, sizeof(s_view.error) - 1);
            xSemaphoreGive(s_view_mtx);
            post(VE_ERROR);
        }
    } else if (strcmp(t, "connection.closed") == 0 || strcmp(t, "voice.replaced") == 0) {
        ESP_LOGW(TAG, "服务端关闭会话: %s", t);
        post(VE_CLOSED);
    } else if (strcmp(t, "work.completed") == 0 || strcmp(t, "work.failed") == 0) {
        post(VE_ACTIVITY);
    }
}

// 按最近一次有效错误给出提示；连接后的传输异常也需要走可重试错误路径。
static void set_connect_error(void) {
    char m[sizeof(s_view.error)];
    const int st = s_ws_http_status;
    if (st == 401 || st == 403) snprintf(m, sizeof(m), "PAT 无效或无权限（%d）", st);
    else if (st == 404) snprintf(m, sizeof(m), "会话不存在（404）");
    else if (st == 409) snprintf(m, sizeof(m), "会话未就绪（409）");
    else if (st > 0 && st != 101) snprintf(m, sizeof(m), "握手被拒绝（HTTP %d）", st);
    else if (s_ws_tls_err != ESP_OK) snprintf(m, sizeof(m), "加密连接失败，请重试");
    else if (s_ws_close_code) snprintf(m, sizeof(m), "服务端关闭连接（%d）", s_ws_close_code);
    else if (s_ready) snprintf(m, sizeof(m), "语音连接中断，请重试");
    else snprintf(m, sizeof(m), "无法建立语音连接");
    xSemaphoreTake(s_view_mtx, portMAX_DELAY);
    if (!s_view.error[0]) snprintf(s_view.error, sizeof(s_view.error), "%s", m);
    xSemaphoreGive(s_view_mtx);
}

static void ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket 已连接，等待 voice.ready（剩余堆 %u，最大块 %u）",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        // 回调返回后客户端才继续收消息，保证 ready/audio.delta 前媒体已就绪。
        // ctl 的 50ms 轮询可能输给第一段 audio.delta，不能在轮询里启动媒体。
        if (!s_stop_req && s_running) {
            s_ws_fd = find_ws_socket();
            if (s_ws_fd < 0) ESP_LOGW(TAG, "未能唯一定位语音连接套接字，上行不做拥塞丢帧");
            if (!media_start(&s_mic_up, &s_play_up)) {
                xSemaphoreTake(s_view_mtx, portMAX_DELAY);
                snprintf(s_view.error, sizeof(s_view.error), "内存不足，音频无法启动");
                xSemaphoreGive(s_view_mtx);
                post(VE_ERROR);
            } else {
                s_connected = true;
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_connected) ESP_LOGW(TAG, "WebSocket 断开");
        s_connected = false;
        s_ws_fd = -1;
        if (e->close_status_code) s_ws_close_code = e->close_status_code;
        if (!s_stop_req) {
            const vs_event_t ev = voice_disconnect_event(s_ready, s_ws_failed, s_ws_close_code);
            if (ev == VE_ERROR) {
                ESP_LOGW(TAG, "异常断开：HTTP=%d tls=0x%x close=%d 剩余堆 %u 最大块 %u",
                         s_ws_http_status, (unsigned)s_ws_tls_err, s_ws_close_code,
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
                set_connect_error();
            }
            post(ev);
        }
        break;
    case WEBSOCKET_EVENT_ERROR: {
        const esp_websocket_error_codes_t *h = &e->error_handle;
        s_ws_failed = true;
        if (h->esp_ws_handshake_status_code) s_ws_http_status = h->esp_ws_handshake_status_code;
        // 1.8.0 的 event_data 未整体初始化：只有 TCP_TRANSPORT 的 TLS/errno 字段有效。
        if (h->error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            if (h->esp_tls_last_esp_err != ESP_OK) s_ws_tls_err = h->esp_tls_last_esp_err;
            ESP_LOGW(TAG, "WebSocket 传输错误 tls=%s(0x%x) stack=%d verify=0x%x errno=%d",
                     esp_err_to_name(h->esp_tls_last_esp_err), (unsigned)h->esp_tls_last_esp_err,
                     h->esp_tls_stack_err, (unsigned)h->esp_tls_cert_verify_flags,
                     h->esp_transport_sock_errno);
        } else {
            ESP_LOGW(TAG, "WebSocket 错误 type=%d HTTP=%d", (int)h->error_type,
                     h->esp_ws_handshake_status_code);
        }
        if (!s_stop_req) {
            set_connect_error();
            post(VE_ERROR);
        }
        break;
    }
    case WEBSOCKET_EVENT_DATA: {
        if (e->op_code == 0x8) {   // 关闭帧：前 2 字节状态码 + 原因
            if (e->data_len >= 2) {
                s_ws_close_code = ((uint8_t)e->data_ptr[0] << 8) | (uint8_t)e->data_ptr[1];
                ESP_LOGW(TAG, "服务端关闭帧 code=%d", s_ws_close_code);
            }
            break;
        }
        if (e->op_code == 0x1 && e->payload_offset == 0) {
            rx_reset();
            qj_init(&s_rxp, rx_cb, NULL);
            s_rx_in_msg = true;
        } else if (e->op_code != 0x0 && e->op_code != 0x1) {
            break;   // ping/pong/close/binary
        }
        if (!s_rx_in_msg || e->data_len <= 0) break;
        if (!qj_feed(&s_rxp, e->data_ptr, (size_t)e->data_len)) {
            ESP_LOGW(TAG, "下行 JSON 解析失败，丢弃本条");
            s_rx_in_msg = false;
            break;
        }
        if (e->payload_offset + e->data_len >= e->payload_len && qj_done(&s_rxp)) {
            s_rx_in_msg = false;
            rx_message_done();
        }
        break;
    }
    default:
        break;
    }
}

// ---------------- 打断 ----------------
static void do_interrupt(void) {
    s_drop = true;                       // 先开窗口，解除 ws 任务的背压等待
    s_drop_until_ms = now_ms() + DROP_WINDOW_MS;
    if (s_seg_active) {
        s_flush_req = true;
        send_control("playback.cancelled", true);
        s_seg_active = false;
        s_seg_done = false;
    }
    send_control("interrupt", false);
}

// ---------------- mic 任务 ----------------
static void mic_task(void *arg) {
    (void)arg;
    static int16_t frame[FRAME_SAMPLES];
    static uint8_t up[UP_PCM_BYTES];
    static int16_t ring[RING_FRAMES][FRAME_SAMPLES];
    int up_frames = 0;
    int ring_head = 0, ring_count = 0;
    barge_det_t barge;
    vad_t vad_listen, vad_resume;
    int g_base = -1, g_cal = -2, g_vol = -1;   // 换算增益的缓存键
    barge_init(&barge, s_echo_gain_q8);
    vad_init(&vad_listen, 5, 35);    // 100 ms 起，700 ms 静音止
    vad_init(&vad_resume, 18, 25);   // 暂停中：持续 ≈0.36 s 的人声才唤回
    vs_state_t last = VS_CONNECTING;

    while (s_running) {
        if (bsp_audio_read(frame, sizeof(frame)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(VOICE_FRAME_MS));
            continue;
        }
        const int64_t t = now_ms();
        const int lvl = dsp_mean_abs(frame, FRAME_SAMPLES);
        s_mic_level = lvl;
        if (g_base != s_echo_gain_q8 || g_cal != s_echo_calib_vol || g_vol != s_volume) {
            g_base = s_echo_gain_q8;
            g_cal = s_echo_calib_vol;
            g_vol = s_volume;
            barge.gain_q8 = barge_gain_for_volume(g_base, g_cal, g_vol);
        }
        barge_push_output(&barge, s_out_level);

        if (s_calib) { s_calib_mic_sum += lvl; s_calib_mic_n++; }

        const vs_state_t st = s_state;
        if (st != last) {
            vad_reset_run(&vad_listen);
            vad_reset_run(&vad_resume);
            last = st;
        }
        const bool chime_quiet = !s_chime_active && t >= s_chime_guard_until;

        if (st == VS_SPEAKING && s_playing) {
            const barge_res_t r = barge_feed_mic(&barge, lvl);
            if (r == BARGE_SUSPECT) s_duck_until = t + DUCK_MS;
            if (r == BARGE_CONFIRM) {
                ESP_LOGI(TAG, "插话确认 mic=%d est=%d 音量=%d 增益=%d/256", lvl,
                         barge_echo_estimate(&barge), g_vol, barge.gain_q8);
                post(VE_BARGE);
            }
        } else if ((st == VS_LISTENING || st == VS_THINKING) && chime_quiet && s_uplink) {
            const vad_ev_t v = vad_feed(&vad_listen, lvl);
            if (v == VAD_SPEECH_START) post(VE_SPEECH_START);
            if (v == VAD_SPEECH_END) post(VE_SPEECH_END);
        } else if (st == VS_PAUSED && chime_quiet && !s_playing) {
            if (vad_feed(&vad_resume, lvl) == VAD_SPEECH_START) {
                ESP_LOGI(TAG, "暂停中检测到人声，恢复聆听");
                post(VE_SPEECH_START);
            }
        }

        // 环形缓冲：插话确认前的 200 ms 不能丢，否则用户第一句话会少半个词。
        memcpy(ring[ring_head], frame, FRAME_BYTES);
        ring_head = (ring_head + 1) % RING_FRAMES;
        if (ring_count < RING_FRAMES) ring_count++;

        const bool open = s_uplink && chime_quiet && s_connected && s_ready;
        if (open && s_send_ring) {
            s_send_ring = false;
            // 环里已含本帧；按时间顺序拼包发送。
            int idx = (ring_head - ring_count + RING_FRAMES) % RING_FRAMES;
            int n = 0;
            for (int i = 0; i < ring_count; i++) {
                memcpy(up + n * FRAME_BYTES, ring[idx], FRAME_BYTES);
                idx = (idx + 1) % RING_FRAMES;
                if (++n == UP_FRAMES) { send_audio(up, UP_PCM_BYTES); n = 0; }
            }
            up_frames = n;
            continue;
        }
        if (open) {
            memcpy(up + up_frames * FRAME_BYTES, frame, FRAME_BYTES);
            if (++up_frames == UP_FRAMES) {
                send_audio(up, UP_PCM_BYTES);
                up_frames = 0;
            }
        } else {
            up_frames = 0;
        }
    }
    xEventGroupSetBits(s_eg, EV_MIC_EXIT);
    vTaskDelete(NULL);
}

// ---------------- play 任务 ----------------
static void chime_render(chime_t c) {
    static int16_t buf[FRAME_SAMPLES];
    // 两个音：开始/继续 = 上行（660→880 Hz）；暂停 = 下行（880→523 Hz）。
    uint32_t f1 = 660, f2 = 880;
    int ms1 = 120, ms2 = 160;
    if (c == CHIME_PAUSE) { f1 = 880; f2 = 523; ms1 = 120; ms2 = 200; }
    const int16_t amp = 7000;
    uint32_t ph = 0;
    const bool calib = (c == CHIME_START);
    if (calib) {
        s_calib_out_sum = s_calib_out_n = s_calib_mic_sum = s_calib_mic_n = 0;
        s_calib = true;
    }
    s_chime_active = true;
    for (int part = 0; part < 2; part++) {
        const uint32_t f = part ? f2 : f1;
        const int frames = (part ? ms2 : ms1) / VOICE_FRAME_MS;
        for (int i = 0; i < frames && s_running; i++) {
            memset(buf, 0, sizeof(buf));
            dsp_tone_add(buf, FRAME_SAMPLES, f, SR, amp, &ph);
            if (i == 0) dsp_gain_ramp(buf, FRAME_SAMPLES, 0, 32768);
            if (i == frames - 1) dsp_gain_ramp(buf, FRAME_SAMPLES, 32768, 0);
            const int lvl = dsp_mean_abs(buf, FRAME_SAMPLES);
            s_out_level = lvl;
            if (calib) { s_calib_out_sum += lvl; s_calib_out_n++; }
            bsp_audio_write(buf, sizeof(buf));
        }
    }
    // 尾音：等喇叭余响和 DMA 排空（约 90 ms）后再结束标定。
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 5 && s_running; i++) {
        s_out_level = 0;
        bsp_audio_write(buf, sizeof(buf));
    }
    s_chime_active = false;
    s_chime_guard_until = now_ms() + CHIME_GUARD_MS;
    if (calib) {
        s_calib = false;
        if (s_calib_out_n > 0 && s_calib_mic_n > 0) {
            const int out = s_calib_out_sum / s_calib_out_n;
            const int mic = s_calib_mic_sum / s_calib_mic_n;
            s_echo_gain_q8 = barge_calibrate_gain_q8(out, mic);
            s_echo_calib_vol = s_volume;
            ESP_LOGI(TAG, "回声标定：输出 %d，麦克风 %d，增益 %d/256（音量 %d%%）", out, mic,
                     s_echo_gain_q8, s_echo_calib_vol);
        }
    }
}

// 一段播放结束时输出统计：音频时长与墙钟用时之差主要来自断流等待。
// 分配失败计数来自堆的失败钩子（Wi-Fi/lwIP 收包等任意任务），钩子里只计数不打日志。
static volatile uint32_t s_alloc_fail_n, s_alloc_fail_last;
static void alloc_failed_hook(size_t size, uint32_t caps, const char *fn) {
    (void)caps; (void)fn;
    s_alloc_fail_n++;
    s_alloc_fail_last = (uint32_t)size;
}

static void play_stats_log(const char *how) {
    const uint32_t audio_ms = s_ps.played_bytes / (SR * 2 / 1000);
    const uint32_t wall_ms = (uint32_t)(now_ms() - s_seg_first_ms);
    ESP_LOGI(TAG, "播放统计(%s)：音频 %u ms，用时 %u ms，起播 %d ms，断流 %u 次共 %u ms，缓冲峰值 %u/%u B",
             how, (unsigned)audio_ms, (unsigned)wall_ms, (int)s_ps.start_ms, (unsigned)s_ps.underruns,
             (unsigned)s_ps.stall_ms, (unsigned)s_ps.peak_bytes, (unsigned)PLAY_SB_BYTES);
    ESP_LOGI(TAG, "内存：剩余 %u，最大块 %u，历史最低 %u，分配失败 %u 次(最近 %u B)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)esp_get_minimum_free_heap_size(), (unsigned)s_alloc_fail_n,
             (unsigned)s_alloc_fail_last);
}

static void play_task(void *arg) {
    (void)arg;
    static int16_t frame[FRAME_SAMPLES];
    int32_t gain = 0;   // 当前增益 Q15
    int64_t wait_since = 0;
    int64_t stall_since = 0;
    bool rebuffer = false;

    while (s_running) {
        if (s_flush_req) {
            if (s_playing || s_seg_active) play_stats_log("打断");
            // 淡出最后一帧后清空缓冲。
            if (s_playing) {
                const size_t n = xStreamBufferReceive(s_sb, frame, sizeof(frame), 0);
                if (n > 0) {
                    if (n < sizeof(frame)) memset((uint8_t *)frame + n, 0, sizeof(frame) - n);
                    dsp_gain_ramp(frame, FRAME_SAMPLES, gain, 0);
                    bsp_audio_write(frame, sizeof(frame));
                }
            }
            while (xStreamBufferReceive(s_sb, frame, sizeof(frame), 0) > 0) {}
            s_playing = false;
            s_out_level = 0;
            gain = 0;
            rebuffer = false;
            stall_since = 0;
            s_flush_req = false;
            continue;
        }
        const chime_t c = s_chime_req;
        if (c != CHIME_NONE && !s_playing) {
            s_chime_req = CHIME_NONE;
            chime_render(c);
            continue;
        }

        const size_t avail = xStreamBufferBytesAvailable(s_sb);
        if (avail > s_ps.peak_bytes) s_ps.peak_bytes = avail;
        if (!s_playing) {
            s_out_level = 0;
            if (avail == 0) {
                if (s_seg_active && s_seg_done && !s_seg_cancelled) {
                    // 本段没有任何可播数据（或已播完后才收到 done）。
                    s_seg_active = false;
                    if (s_seg_started_sent) send_control("playback.ended", true);
                    play_stats_log("完成");
                    rebuffer = false;
                    stall_since = 0;
                    post(VE_PLAYBACK_DRAINED);
                }
                wait_since = 0;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const int64_t t = now_ms();
            if (wait_since == 0) wait_since = t;
            const size_t need = rebuffer ? REBUF_BYTES : PREROLL_BYTES;
            const int wait_ms = rebuffer ? REBUF_WAIT_MS : PREROLL_WAIT_MS;
            if (avail >= need || s_seg_done || t - wait_since >= wait_ms) {
                s_playing = true;
                if (stall_since) s_ps.stall_ms += (uint32_t)(t - stall_since);
                if (s_ps.start_ms < 0) s_ps.start_ms = (int32_t)(t - s_seg_first_ms);
                stall_since = 0;
                rebuffer = false;
                wait_since = 0;
                gain = 0;   // 淡入
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        const size_t n = xStreamBufferReceive(s_sb, frame, sizeof(frame), pdMS_TO_TICKS(40));
        if (n == 0) {
            s_playing = false;
            if (s_seg_done) {
                s_seg_active = false;
                if (s_seg_started_sent) send_control("playback.ended", true);
                play_stats_log("完成");
                rebuffer = false;
                stall_since = 0;
                post(VE_PLAYBACK_DRAINED);
            } else {
                rebuffer = true;   // 网络慢于实时：小缓冲后再继续
                s_ps.underruns++;
                stall_since = now_ms();
            }
            continue;
        }
        s_ps.played_bytes += n;
        if (n < sizeof(frame)) memset((uint8_t *)frame + n, 0, sizeof(frame) - n);
        const int32_t target = now_ms() < s_duck_until ? DUCK_GAIN_Q15 : 32768;
        // 每帧最多移动 1/4 满幅，约 80 ms 完成淡入/压低。
        int32_t next = target;
        if (next > gain + 8192) next = gain + 8192;
        if (next < gain - 8192) next = gain - 8192;
        dsp_gain_ramp(frame, FRAME_SAMPLES, gain, next);
        gain = next;
        s_out_level = dsp_mean_abs(frame, FRAME_SAMPLES);
        bsp_audio_write(frame, sizeof(frame));
    }
    xEventGroupSetBits(s_eg, EV_PLAY_EXIT);
    vTaskDelete(NULL);
}

// ---------------- ctl 任务 ----------------
static void apply_state(vs_state_t st) {
    s_state = st;
    xSemaphoreTake(s_view_mtx, portMAX_DELAY);
    s_view.state = st;
    xSemaphoreGive(s_view_mtx);
}

// TLS 握手完成后再分配播放缓冲、启动 mic/play 任务。失败返回 false。
static bool media_start(bool *mic_up, bool *play_up) {
    if (!s_sb) s_sb = xStreamBufferCreate(PLAY_SB_BYTES, 1);
    if (!s_sb) return false;
    if (!*mic_up) {
        if (xTaskCreate(mic_task, "qv_mic", 4096, NULL, 7, NULL) != pdPASS) return false;
        *mic_up = true;
    }
    if (!*play_up) {
        if (xTaskCreate(play_task, "qv_play", 3584, NULL, 8, NULL) != pdPASS) return false;
        *play_up = true;
    }
    ESP_LOGI(TAG, "音频任务已启动，剩余堆 %u，最大块 %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return true;
}

static void ctl_task(void *arg) {
    (void)arg;
    voice_fsm_t fsm;
    voice_fsm_init(&fsm, now_ms());
    apply_state(VS_CONNECTING);

    esp_websocket_client_config_t cfg = {
        .uri = s_ws_uri,
        .headers = s_ws_hdr,
        .buffer_size = WS_BUF_BYTES,
        .task_stack = 6144,
        .task_prio = 6,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 15,
        .disable_auto_reconnect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s_ws = esp_websocket_client_init(&cfg);
    bool ok = s_ws != NULL;
    if (ok) {
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_handler, NULL);
        ok = esp_websocket_client_start(s_ws) == ESP_OK;
    }
    if (!ok) {
        ESP_LOGE(TAG, "WebSocket 客户端创建失败，剩余堆 %u", (unsigned)esp_get_free_heap_size());
        snprintf(s_view.error, sizeof(s_view.error), "内存不足，语音连接初始化失败");
        post(VE_ERROR);
    }

    const int64_t connect_deadline = now_ms() + 15000;
    int64_t spk_orphan_since = 0;
    while (!s_stop_req) {
        vs_event_t ev;
        const bool got = xQueueReceive(s_evq, &ev, pdMS_TO_TICKS(50)) == pdTRUE;
        const int64_t t = now_ms();
        if (!got) ev = VE_TICK;
        if (ev == VE_TICK && fsm.st == VS_CONNECTING && t > connect_deadline) {
            snprintf(s_view.error, sizeof(s_view.error), "等待服务端就绪超时");
            ev = VE_ERROR;
        }
        if (s_drop && t >= s_drop_until_ms) close_drop_window("超时");

        // 兜底：仍显示“回答中”，但本段已结束/被取消且没有待播音频，1.5 s 后强制回到聆听，
        // 避免任何漏发 DRAINED 的路径让界面卡死、上行永远关闭。
        const bool spk_orphan = fsm.st == VS_SPEAKING && !s_seg_active && !s_playing &&
                                (!s_sb || xStreamBufferBytesAvailable(s_sb) == 0);
        if (!spk_orphan) {
            spk_orphan_since = 0;
        } else if (spk_orphan_since == 0) {
            spk_orphan_since = t;
        } else if (ev == VE_TICK && t - spk_orphan_since >= 1500) {
            ESP_LOGW(TAG, "回答中已无音频，回到聆听");
            spk_orphan_since = 0;
            ev = VE_PLAYBACK_DRAINED;
        }

        const vs_state_t before = fsm.st;
        const vs_actions_t a = voice_fsm_event(&fsm, ev, t);
        if (a.changed) {
            ESP_LOGI(TAG, "状态 %s → %s", voice_state_word(before), voice_state_word(fsm.st));
            apply_state(fsm.st);
        }
        if (a.send_interrupt) do_interrupt();
        if (ev == VE_BARGE && a.changed) s_send_ring = true;
        if (a.pause_chime) s_chime_req = CHIME_PAUSE;
        if (a.resume_chime) s_chime_req = (before == VS_CONNECTING) ? CHIME_START : CHIME_RESUME;
        s_uplink = voice_fsm_uplink_open(&fsm, t);
        if (a.disconnect) break;
    }

    // 收尾先停上行与 PCM，再 join ws。ws 回调会创建媒体，只有 ws 真正退出后，
    // 才能确定完整的媒体退出等待集。任何超时都不能假报 finished、放行新会话。
    s_uplink = false;
    s_running = false;
    s_stop_req = true;
    if (s_ws) {
        if (s_connected) esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1500));
        // stop() join ws；已经停止时返回 ESP_FAIL，不影响退出屏障。
        // 此时只停止、不 destroy：mic/play 可能还在 send_text() 内，必须先等其退出。
        esp_websocket_client_stop(s_ws);
    }
    s_connected = false;
    s_ws_fd = -1;
    const EventBits_t need = (s_mic_up ? EV_MIC_EXIT : 0) | (s_play_up ? EV_PLAY_EXIT : 0);
    if (need) {
        const EventBits_t got = xEventGroupWaitBits(s_eg, need, pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(1500));
        if ((got & need) != need) {
            ESP_LOGW(TAG, "等待音频任务退出，暂不允许重试");
            xEventGroupWaitBits(s_eg, need, pdFALSE, pdTRUE, portMAX_DELAY);
        }
    }
    if (s_ws) {
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    if (s_sb) {
        vStreamBufferDelete(s_sb);
        s_sb = NULL;
    }
    if (s_state != VS_ERROR) apply_state(VS_ENDED);
    ESP_LOGI(TAG, "会话结束，剩余堆 %u，最大块 %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    xSemaphoreTake(s_view_mtx, portMAX_DELAY);
    s_view.finished = true;
    s_ctl_task = NULL;
    xEventGroupSetBits(s_eg, EV_CTL_EXIT);
    xSemaphoreGive(s_view_mtx);
    vTaskDelete(NULL);
}

// ---------------- 对外接口 ----------------
esp_err_t qca_voice_start(const char *conv_id) {
    if (s_ctl_task || s_sb) return ESP_ERR_INVALID_STATE;
    if (!s_evq) s_evq = xQueueCreate(16, sizeof(vs_event_t));
    if (!s_eg) s_eg = xEventGroupCreate();
    if (!s_view_mtx) s_view_mtx = xSemaphoreCreateMutex();
    if (!s_send_mtx) s_send_mtx = xSemaphoreCreateMutex();
    if (!s_evq || !s_eg || !s_view_mtx || !s_send_mtx) return ESP_ERR_NO_MEM;

    xQueueReset(s_evq);
    xEventGroupClearBits(s_eg, EV_CTL_EXIT | EV_MIC_EXIT | EV_PLAY_EXIT);
    xSemaphoreTake(s_view_mtx, portMAX_DELAY);
    memset(&s_view, 0, sizeof(s_view));
    s_view.state = VS_CONNECTING;
    xSemaphoreGive(s_view_mtx);
    s_state = VS_CONNECTING;
    s_stop_req = s_connected = s_ready = s_uplink = s_send_ring = false;
    s_drop = s_seg_active = s_seg_done = s_seg_cancelled = s_flush_req = s_playing = false;
    s_chime_req = CHIME_NONE;
    s_chime_active = false;
    s_chime_guard_until = 0;
    s_out_level = s_mic_level = 0;
    s_duck_until = 0;
    s_rx_in_msg = false;
    s_ws_http_status = 0;
    s_ws_tls_err = ESP_OK;
    s_ws_close_code = 0;
    s_ws_failed = false;
    s_mic_up = s_play_up = false;
    s_ws_fd = -1;
    s_up_dropped = 0;

    snprintf(s_conv, sizeof(s_conv), "%s", conv_id);
    snprintf(s_ws_uri, sizeof(s_ws_uri), QCA_WS_BASE "?conversation_id=%s", s_conv);
    if (!qca_pat_present()) return ESP_ERR_INVALID_STATE;
    snprintf(s_ws_hdr, sizeof(s_ws_hdr), "Authorization: Bearer %s\r\n", qca_pat());

    bsp_audio_set_format(SR, 16, 1);
    volume_load();
    bsp_audio_set_volume((uint8_t)s_volume);
    static bool s_hook_registered;
    if (!s_hook_registered) {
        s_hook_registered = heap_caps_register_failed_alloc_callback(alloc_failed_hook) == ESP_OK;
    }

    // 先只起 ctl 任务连 WebSocket；mic/play 与播放缓冲等 TLS 握手完成后再分配（见 media_start）。
    s_running = true;
    // ctl 优先级必须高于 WebSocket 任务(6)：ws 任务在连续下行音频帧之间释放锁后会立即重新加锁，
    // 低优先级的等待者永远抢不到，插话时的 cancel/interrupt 会超时。高优先级等待者在释放锁的瞬间被唤醒并抢占。
    // ctl 平时阻塞在事件队列上，不会占用 CPU。
    if (xTaskCreate(ctl_task, "qv_ctl", 4096, NULL, 7, &s_ctl_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "开始实时语音，剩余堆 %u，最大块 %u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_OK;
}

void qca_voice_toggle_pause(void) { post(VE_OK_SHORT); }

bool qca_voice_stop(void) {
    if (!s_eg) return true;
    if (s_ctl_task) {
        s_stop_req = true;
        post(VE_TICK);
        // 正在发送的控制消息最多阻塞 CTRL_SEND_TIMEOUT_MS，再加关闭握手 1.5 s。
        const EventBits_t got = xEventGroupWaitBits(s_eg, EV_CTL_EXIT, pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(CTRL_SEND_TIMEOUT_MS + 4000));
        if (!(got & EV_CTL_EXIT)) return false;
    }
    volume_save();
    bsp_audio_set_volume(0);
    return true;
}

void qca_voice_get_view(qca_voice_view_t *out) {
    if (!s_view_mtx) { memset(out, 0, sizeof(*out)); out->state = VS_ENDED; return; }
    xSemaphoreTake(s_view_mtx, portMAX_DELAY);
    *out = s_view;
    xSemaphoreGive(s_view_mtx);
    // 电平映射到 0..100：说话/播放时环会“呼吸”。
    const int raw = (s_state == VS_SPEAKING) ? s_out_level : s_mic_level;
    int v = raw / 30;
    if (v > 100) v = 100;
    out->level = v;
}

void qca_voice_set_volume(int percent) {
    volume_load();
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    if (s_running) {
        bsp_audio_set_volume((uint8_t)percent);
        ESP_LOGI(TAG, "音量 %d%%", percent);
    }
}

int qca_voice_get_volume(void) {
    volume_load();
    return s_volume;
}

esp_err_t qca_voice_reset_volume(void) {
    if (s_running) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    // 尚未保存过音量时不创建命名空间，避免无可删数据却因空间耗尽无法重置。
    esp_err_t e = nvs_open(VOL_NVS_NS, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        e = ESP_OK;
    } else if (e == ESP_OK) {
        nvs_close(h);
        e = nvs_open(VOL_NVS_NS, NVS_READWRITE, &h);
        if (e != ESP_OK) return e;
        e = nvs_erase_key(h, VOL_NVS_KEY);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        s_volume = DEFAULT_VOLUME;
        s_vol_saved = -1;
        s_vol_loaded = false;
    }
    return e;
}
