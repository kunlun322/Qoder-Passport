// Host tests for the QCA companion's pure-logic modules (no ESP-IDF needed).
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "app_list.h"
#include "audio_dsp.h"
#include "b64_stream.h"
#include "qca_list_parse.h"
#include "qjson.h"
#include "portal_form.h"
#include "utf8_util.h"
#include "voice_logic.h"

// ---------------------------------------------------------------- qjson
typedef struct {
    char name[64];
    size_t name_len;
    int prim_count;
    int obj_begin;
    int arr_begin;
    int id_depth_ok;
    char emoji[8];
    size_t emoji_len;
} qj_collect_t;

static void qj_cb(void *user, qj_parser_t *p, qj_event_t ev, const char *data, size_t len)
{
    qj_collect_t *c = user;
    switch (ev) {
    case QJ_EV_OBJ_BEGIN: c->obj_begin++; break;
    case QJ_EV_ARR_BEGIN: c->arr_begin++; break;
    case QJ_EV_STR_CHUNK:
        if (qj_key_is(p, "name")) {
            assert(c->name_len + len < sizeof(c->name));
            memcpy(c->name + c->name_len, data, len);
            c->name_len += len;
        } else if (qj_key_is(p, "e")) {
            assert(c->emoji_len + len < sizeof(c->emoji));
            memcpy(c->emoji + c->emoji_len, data, len);
            c->emoji_len += len;
        } else if (qj_key_is(p, "id")) {
            if (qj_depth(p) == 3 && qj_in_array(p, 2) && strcmp(qj_key(p, 1), "data") == 0) {
                c->id_depth_ok++;
            }
        }
        break;
    case QJ_EV_PRIM:
        if (qj_key_is(p, "n")) assert(len == 2 && memcmp(data, "12", 2) == 0);
        if (qj_key_is(p, "ok")) assert(len == 4 && memcmp(data, "true", 4) == 0);
        c->prim_count++;
        break;
    default: break;
    }
}

static void test_qjson(void)
{
    static const char doc[] =
        "{\"data\":[{\"id\":\"t1\",\"name\":\"\xE4\xB8\xAD\\u6587 \\\"x\\\"\"},"
        "{\"id\":\"t2\",\"n\":12,\"e\":\"\\ud83d\\ude00\"}],\"ok\":true}";
    qj_parser_t p;
    qj_collect_t c;

    // Whole document at once, then byte-by-byte: results must match.
    for (int pass = 0; pass < 2; pass++) {
        memset(&c, 0, sizeof(c));
        qj_init(&p, qj_cb, &c);
        if (pass == 0) {
            assert(qj_feed(&p, doc, sizeof(doc) - 1));
        } else {
            for (size_t i = 0; i < sizeof(doc) - 1; i++) assert(qj_feed(&p, doc + i, 1));
        }
        assert(qj_done(&p));
        c.name[c.name_len] = '\0';
        assert(strcmp(c.name, "\xE4\xB8\xAD\xE6\x96\x87 \"x\"") == 0);
        assert(c.emoji_len == 4 && memcmp(c.emoji, "\xF0\x9F\x98\x80", 4) == 0);
        assert(c.prim_count == 2);
        assert(c.obj_begin == 3);
        assert(c.arr_begin == 1);
        assert(c.id_depth_ok == 2);
    }

    // Syntax errors are sticky.
    qj_init(&p, qj_cb, &c);
    assert(!qj_feed(&p, "{]", 2));
    assert(!qj_feed(&p, "}", 1));

    // Nesting deeper than QJ_MAX_DEPTH is rejected.
    char deep[QJ_MAX_DEPTH + 2];
    memset(deep, '[', sizeof(deep));
    qj_init(&p, qj_cb, &c);
    assert(!qj_feed(&p, deep, sizeof(deep)));

    // An incomplete document is not done.
    qj_init(&p, qj_cb, &c);
    assert(qj_feed(&p, "{\"a\":1", 6));
    assert(!qj_done(&p));
}

// ---------------------------------------------------------------- base64
static void test_b64(void)
{
    static const uint8_t raw[] = { 'M', 'a', 'n', 'y', ' ', 0x00, 0xFF, 0x10 };
    char enc[32];
    const size_t n = b64_encode(raw, sizeof(raw), enc, sizeof(enc));
    assert(n == b64_encoded_len(sizeof(raw)));
    assert(n == 12);
    assert(memcmp(enc, "TWFueSAA/xA=", 12) == 0);
    assert(b64_encode(raw, sizeof(raw), enc, 4) == 0);   // capacity too small

    // Decode across every possible split point.
    for (size_t split = 0; split <= n; split++) {
        b64_decoder_t d;
        uint8_t out[16];
        b64_decoder_init(&d);
        size_t w = b64_decode_chunk(&d, enc, split, out, sizeof(out));
        w += b64_decode_chunk(&d, enc + split, n - split, out + w, sizeof(out) - w);
        assert(!d.error);
        assert(w == sizeof(raw));
        assert(memcmp(out, raw, sizeof(raw)) == 0);
    }

    b64_decoder_t d;
    uint8_t out[8];
    b64_decoder_init(&d);
    assert(b64_decode_chunk(&d, "TW\nFu", 5, out, sizeof(out)) == 3);   // whitespace ignored
    assert(!d.error && memcmp(out, "Man", 3) == 0);
    b64_decoder_init(&d);
    (void)b64_decode_chunk(&d, "TW*u", 4, out, sizeof(out));
    assert(d.error);
}

// ---------------------------------------------------------------- DSP
static void test_dsp(void)
{
    int16_t in[300];
    int16_t out[(300 + 2) * 2 / 3 + 1];
    dsp_rs_t rs;

    for (size_t i = 0; i < 300; i++) in[i] = 1000;
    dsp_rs_init(&rs);
    // Feed in uneven chunks: the 3:2 ratio must hold across calls.
    size_t w = dsp_rs_24to16(&rs, in, 7, out);
    w += dsp_rs_24to16(&rs, in + 7, 293, out + w);
    assert(w == 200);
    for (size_t i = 4; i < w; i++) assert(out[i] == 1000);   // DC passes after warm-up

    const int16_t lv[4] = { 100, -100, 300, -300 };
    assert(dsp_mean_abs(lv, 4) == 200);
    assert(dsp_mean_abs(lv, 0) == 0);

    int16_t g[64];
    for (size_t i = 0; i < 64; i++) g[i] = 10000;
    dsp_gain_ramp(g, 64, 32768, 32768);
    assert(g[0] == 10000 && g[63] == 10000);
    dsp_gain_ramp(g, 64, 0, 0);
    for (size_t i = 0; i < 64; i++) assert(g[i] == 0);

    // Tone is bounded by amp, non-silent, and saturates instead of wrapping.
    int16_t t[160] = { 0 };
    uint32_t phase = 0;
    dsp_tone_add(t, 160, 1000, 16000, 8000, &phase);
    int peak = 0;
    for (size_t i = 0; i < 160; i++) {
        const int a = t[i] < 0 ? -t[i] : t[i];
        if (a > peak) peak = a;
    }
    assert(peak > 6000 && peak <= 8000);
    assert(phase == 0);   // 160 samples = exactly 10 periods of 1 kHz @ 16 kHz
    phase = 0;
    dsp_tone_add(t, 3, 1000, 16000, 0, &phase);
    assert(phase == 3u * 4u * 65536u);   // phase carries across calls
    for (size_t i = 0; i < 160; i++) t[i] = 32000;
    phase = 0;
    dsp_tone_add(t, 160, 1000, 16000, 8000, &phase);
    for (size_t i = 0; i < 160; i++) assert(t[i] >= 24000);   // no wrap to negative
}

// ---------------------------------------------------------------- barge-in / VAD
static void test_barge(void)
{
    barge_det_t d;
    barge_init(&d, 256);
    for (int i = 0; i < BARGE_HIST_FRAMES; i++) barge_push_output(&d, 2000);
    assert(barge_echo_estimate(&d) == 2000);

    // Echo-level mic input never triggers.
    for (int i = 0; i < 20; i++) assert(barge_feed_mic(&d, 2500) == BARGE_NONE);

    // A clearly louder voice: suspect (duck) first, then confirm.
    assert(barge_feed_mic(&d, 5000) == BARGE_NONE);
    assert(barge_feed_mic(&d, 5000) == BARGE_NONE);
    assert(barge_feed_mic(&d, 5000) == BARGE_SUSPECT);
    assert(barge_feed_mic(&d, 5000) == BARGE_CONFIRM);

    // Quiet room, silent output: low mic level is not speech.
    barge_init(&d, 256);
    for (int i = 0; i < 10; i++) assert(barge_feed_mic(&d, 1000) == BARGE_NONE);

    assert(barge_calibrate_gain_q8(1000, 500) == 256);
    assert(barge_calibrate_gain_q8(0, 500) == 256);
    assert(barge_calibrate_gain_q8(100, 30000) == 1024);
    assert(barge_calibrate_gain_q8(1000, 0) == 32);

    // Volume after calibration scales the echo gain by 0.5 dB per step.
    assert(barge_gain_for_volume(105, 80, 80) == 105);
    assert(barge_gain_for_volume(105, 80, 100) == 332);   // +10 dB ≈ x3.16
    assert(barge_gain_for_volume(105, 80, 70) == 59);     // -5 dB
    assert(barge_gain_for_volume(256, 80, 90) == 455);
    assert(barge_gain_for_volume(256, 100, 0) == 16);     // clamped low
    assert(barge_gain_for_volume(1024, 0, 100) == 4096);  // clamped high
    assert(barge_gain_for_volume(0, 80, 80) == 256);
    assert(barge_gain_for_volume(200, -1, 50) == 200);    // invalid input unchanged
    // At +10 dB the echo that matched the old estimate no longer confirms.
    barge_init(&d, barge_gain_for_volume(105, 80, 100));
    for (int i = 0; i < BARGE_HIST_FRAMES; i++) barge_push_output(&d, 3000);
    for (int i = 0; i < 20; i++) assert(barge_feed_mic(&d, 3000 * 105 / 256 * 3) != BARGE_CONFIRM);
}

static void test_vad(void)
{
    vad_t v;
    vad_init(&v, 3, 35);
    for (int i = 0; i < 200; i++) assert(vad_feed(&v, 100) == VAD_NONE);
    assert(vad_threshold(&v) == 800);
    assert(vad_feed(&v, 2000) == VAD_NONE);
    assert(vad_feed(&v, 2000) == VAD_NONE);
    assert(vad_feed(&v, 2000) == VAD_SPEECH_START);
    for (int i = 0; i < 34; i++) assert(vad_feed(&v, 100) == VAD_NONE);
    assert(vad_feed(&v, 100) == VAD_SPEECH_END);

    // A single loud click does not start speech.
    assert(vad_feed(&v, 5000) == VAD_NONE);
    assert(vad_feed(&v, 100) == VAD_NONE);
    assert(vad_feed(&v, 5000) == VAD_NONE);
}

static void test_pause_phrase(void)
{
    assert(voice_is_pause_phrase("\xE6\x9A\x82\xE5\x81\x9C"));                         // 暂停
    assert(voice_is_pause_phrase("\xE5\x81\x9C\xE4\xB8\x80\xE4\xB8\x8B\xE5\x90\xA7")); // 停一下吧
    assert(voice_is_pause_phrase("\xE4\xBC\x91\xE6\x81\xAF\xE4\xB8\x80\xE4\xB8\x8B")); // 休息一下
    assert(!voice_is_pause_phrase("\xE4\xBD\xA0\xE5\xA5\xBD"));                         // 你好
    assert(!voice_is_pause_phrase(NULL));
    // A long sentence that merely mentions 暂停 is not a command.
    assert(!voice_is_pause_phrase(
        "\xE8\xAF\xB7\xE8\xA7\xA3\xE9\x87\x8A\xE4\xB8\x80\xE4\xB8\x8B\xE8\xA7\x86\xE9\xA2\x91"
        "\xE6\x92\xAD\xE6\x94\xBE\xE5\x99\xA8\xE9\x87\x8C\xE7\x9A\x84\xE6\x9A\x82\xE5\x81\x9C"
        "\xE6\x8C\x89\xE9\x92\xAE\xE6\x98\xAF\xE6\x80\x8E\xE4\xB9\x88\xE5\xB7\xA5\xE4\xBD\x9C"));
}

// ---------------------------------------------------------------- session FSM
static void test_fsm(void)
{
    voice_fsm_t f;
    vs_actions_t a;
    int64_t t = 1000;

    voice_fsm_init(&f, t);
    assert(f.st == VS_CONNECTING);
    assert(!voice_fsm_uplink_open(&f, t));

    // Start: server ready → listening, rising chime, mic open.
    a = voice_fsm_event(&f, VE_READY, t);
    assert(f.st == VS_LISTENING && a.changed && a.resume_chime);
    assert(voice_fsm_uplink_open(&f, t));

    // User finishes → thinking → assistant audio → speaking (mic gated).
    a = voice_fsm_event(&f, VE_SPEECH_END, t += 100);
    assert(f.st == VS_THINKING && a.changed);
    a = voice_fsm_event(&f, VE_AUDIO_FIRST, t += 500);
    assert(f.st == VS_SPEAKING);
    assert(!voice_fsm_uplink_open(&f, t));

    // Playback drained → listening after the speaker tail.
    a = voice_fsm_event(&f, VE_PLAYBACK_DRAINED, t += 2000);
    assert(f.st == VS_LISTENING);
    assert(!voice_fsm_uplink_open(&f, t + VS_TAIL_MS - 1));
    assert(voice_fsm_uplink_open(&f, t + VS_TAIL_MS));

    // Barge-in while speaking: interrupt, mic open immediately.
    voice_fsm_event(&f, VE_SPEECH_END, t += 1000);
    voice_fsm_event(&f, VE_AUDIO_FIRST, t += 100);
    a = voice_fsm_event(&f, VE_BARGE, t += 300);
    assert(f.st == VS_LISTENING && a.send_interrupt);
    assert(voice_fsm_uplink_open(&f, t));

    // Server-side playback.interrupt while speaking: back to listening without
    // echoing an interrupt, mic reopens after the speaker tail (boot18 stuck case).
    voice_fsm_event(&f, VE_SPEECH_END, t += 1000);
    voice_fsm_event(&f, VE_AUDIO_FIRST, t += 100);
    assert(f.st == VS_SPEAKING);
    a = voice_fsm_event(&f, VE_SERVER_INTERRUPT, t += 400);
    assert(f.st == VS_LISTENING && a.changed && !a.send_interrupt);
    assert(!voice_fsm_uplink_open(&f, t + VS_TAIL_MS - 1));
    assert(voice_fsm_uplink_open(&f, t + VS_TAIL_MS));
    t += VS_TAIL_MS;
    // Outside SPEAKING it is a no-op.
    a = voice_fsm_event(&f, VE_SERVER_INTERRUPT, t += 10);
    assert(f.st == VS_LISTENING && !a.changed);

    // Pause phrase → paused, interrupt, falling chime; mic closed.
    a = voice_fsm_event(&f, VE_PAUSE_PHRASE, t += 100);
    assert(f.st == VS_PAUSED && a.send_interrupt && a.pause_chime);
    assert(!voice_fsm_uplink_open(&f, t));

    // Speaking resumes from pause.
    a = voice_fsm_event(&f, VE_SPEECH_START, t += 5000);
    assert(f.st == VS_LISTENING && a.resume_chime);

    // Short OK toggles pause both ways.
    a = voice_fsm_event(&f, VE_OK_SHORT, t += 100);
    assert(f.st == VS_PAUSED && a.pause_chime);
    a = voice_fsm_event(&f, VE_OK_SHORT, t += 100);
    assert(f.st == VS_LISTENING && a.resume_chime);
    assert(voice_fsm_uplink_open(&f, t));

    // Thinking with no answer for 15 s falls back to listening.
    voice_fsm_event(&f, VE_SPEECH_END, t += 100);
    a = voice_fsm_event(&f, VE_TICK, t + VS_THINK_TIMEOUT_MS - 1);
    assert(f.st == VS_THINKING && !a.changed);
    a = voice_fsm_event(&f, VE_TICK, t += VS_THINK_TIMEOUT_MS);
    assert(f.st == VS_LISTENING && a.changed);

    // 60 s of silence on both sides → auto pause.
    a = voice_fsm_event(&f, VE_TICK, t + VS_IDLE_PAUSE_MS - 1);
    assert(f.st == VS_LISTENING);
    voice_fsm_event(&f, VE_ACTIVITY, t += VS_IDLE_PAUSE_MS - 1);   // activity resets the timer
    a = voice_fsm_event(&f, VE_TICK, t + VS_IDLE_PAUSE_MS - 1);
    assert(f.st == VS_LISTENING);
    a = voice_fsm_event(&f, VE_TICK, t += VS_IDLE_PAUSE_MS);
    assert(f.st == VS_PAUSED && a.pause_chime && !a.send_interrupt);

    // Paused for 5 minutes → session ends.
    a = voice_fsm_event(&f, VE_TICK, t + VS_PAUSE_END_MS - 1);
    assert(f.st == VS_PAUSED);
    a = voice_fsm_event(&f, VE_TICK, t += VS_PAUSE_END_MS);
    assert(f.st == VS_ENDED && a.disconnect);

    // Terminal states ignore further events.
    a = voice_fsm_event(&f, VE_OK_SHORT, t += 10);
    assert(f.st == VS_ENDED && !a.changed);

    voice_fsm_init(&f, t);
    voice_fsm_event(&f, VE_READY, t);
    a = voice_fsm_event(&f, VE_ERROR, t += 10);
    assert(f.st == VS_ERROR && a.disconnect);
    a = voice_fsm_event(&f, VE_READY, t += 10);
    assert(f.st == VS_ERROR && !a.changed);

    // TLS allocation/write/read failure AFTER voice.ready must still offer retry.
    assert(voice_disconnect_event(false, false, 1000) == VE_ERROR);
    assert(voice_disconnect_event(true, true, 1000) == VE_ERROR);
    assert(voice_disconnect_event(true, false, 0) == VE_ERROR);
    assert(voice_disconnect_event(true, false, 1006) == VE_ERROR);
    assert(voice_disconnect_event(true, false, 1000) == VE_CLOSED);
    for (int s = VS_LISTENING; s <= VS_PAUSED; s++) {
        voice_fsm_init(&f, t);
        f.st = (vs_state_t)s;
        a = voice_fsm_event(&f, voice_disconnect_event(true, true, 0), t + 1);
        assert(f.st == VS_ERROR && a.disconnect);
        assert(!voice_fsm_uplink_open(&f, t + 1));
    }
    // A fresh session is allowed only after the adapter's teardown barrier;
    // the restarted FSM is not stuck in the old terminal state.
    voice_fsm_init(&f, t);
    a = voice_fsm_event(&f, VE_READY, t + 1);
    assert(f.st == VS_LISTENING && a.changed);

    for (int s = VS_CONNECTING; s <= VS_ERROR; s++) {
        assert(voice_state_word((vs_state_t)s)[0] != '\0');
        assert(voice_state_guide((vs_state_t)s)[0] != '\0');
    }
}

// ---------------------------------------------------------------- UTF-8
static bool no_emoji(uint32_t cp, void *user)
{
    (void)user;
    return cp < 0x10000;
}

static void test_utf8(void)
{
    char s[32];
    strcpy(s, "a\xE4\xB8\xAD\xE6\x96\x87");   // a中文 (7 bytes)
    assert(utf8_count(s) == 3);
    assert(utf8_truncate(s, 5) == 4);         // never splits 文
    assert(strcmp(s, "a\xE4\xB8\xAD") == 0);
    assert(utf8_truncate(s, 10) == 4);

    strcpy(s, "x\n\xF0\x9F\x98\x80y\xFFz");
    const size_t n = utf8_sanitize(s, no_emoji, NULL);
    assert(strcmp(s, "x ?y?z") == 0);
    assert(n == 6);

    const char *p = "\xC3\xA9";
    assert(utf8_next(&p) == 0xE9 && *p == '\0');
}

// ---------------------------------------------------------------- list cursor
static void test_list(void)
{
    app_list_t l;
    app_list_init(&l, 12, 5);
    assert(l.cursor == 0 && l.top == 0);
    app_list_move(&l, -1);                    // wraps to the last entry
    assert(l.cursor == 11 && l.top == 7);
    app_list_move(&l, 1);
    assert(l.cursor == 0 && l.top == 0);
    app_list_move(&l, 6);
    assert(l.cursor == 6 && l.top == 2);
    app_list_move(&l, -5);
    assert(l.cursor == 1 && l.top == 1);
    app_list_move(&l, 25);                    // large jumps wrap
    assert(l.cursor == 2);

    app_list_init(&l, 3, 5);                  // fewer than one screen
    app_list_move(&l, 2);
    assert(l.cursor == 2 && l.top == 0);

    app_list_init(&l, 0, 5);                  // empty list is inert
    app_list_move(&l, 1);
    assert(l.cursor == 0 && l.top == 0);
}

// Shape mirrors a real GET /forward/templates page: each item nests objects and
// arrays whose own "id"/"enabled"/"status" keys must not leak into the item.
static const char k_templates_json[] =
    "{\"data\":["
    "{\"type\":\"template\",\"id\":\"tmpl_a\",\"name\":\"\\u4e2d\\u5b66\\u7269\\u7406\","
    "\"status\":\"active\",\"model\":{\"context_window\":200000,\"id\":\"dfmodel\"},"
    "\"system\":\"long prompt \\\"quoted\\\" text\",\"tools\":[{\"type\":\"agent_toolset\","
    "\"configs\":[{\"name\":\"bash\",\"enabled\":false}]}],\"vaults\":{\"vault_1\":{\"enabled\":true}},"
    "\"multiagent\":null},"
    "{\"type\":\"template\",\"id\":\"tmpl_b\",\"name\":\"SuperStudent\",\"status\":\"archived\","
    "\"model\":\"efficient\"},"
    "{\"type\":\"template\",\"id\":\"tmpl_c\",\"name\":\"\",\"status\":\"active\",\"tools\":[]}"
    "],\"first_id\":\"tmpl_a\",\"last_id\":\"tmpl_c\",\"has_more\":false}";

static const char k_identities_json[] =
    "{\"data\":["
    "{\"id\":\"idt_1\",\"name\":\"Alice\",\"external_id\":\"ext-1\",\"enabled\":true,"
    "\"metadata\":{\"id\":\"nested\",\"enabled\":false}},"
    "{\"id\":\"idt_2\",\"name\":\"Bob\",\"enabled\":false},"
    "{\"id\":\"idt_3\",\"external_id\":\"ext-3\",\"enabled\":true}"
    "],\"has_more\":true}";

static void parse_split(const char *json, size_t step, qca_list_kind_t kind, qca_list_t *out,
                        qca_list_ctx_t *ctx)
{
    qj_parser_t p;
    qca_list_parse_init(ctx, kind, out);
    qj_init(&p, qca_list_parse_cb, ctx);
    const size_t n = strlen(json);
    for (size_t i = 0; i < n; i += step) {
        const size_t len = n - i < step ? n - i : step;
        assert(qj_feed(&p, json + i, len));
    }
    assert(qj_done(&p));
}

static void test_list_parse(void)
{
    static qca_list_t out;
    static qca_list_ctx_t ctx;
    const size_t tn = strlen(k_templates_json);
    // Every split size, so status/name/id values are cut at every possible byte.
    for (size_t step = 1; step <= tn; step++) {
        parse_split(k_templates_json, step, QCA_LIST_TEMPLATES, &out, &ctx);
        assert(out.count == 2);                       // archived tmpl_b is dropped
        assert(strcmp(out.items[0].id, "tmpl_a") == 0);
        assert(strcmp(out.items[0].name, "\xe4\xb8\xad\xe5\xad\xa6\xe7\x89\xa9\xe7\x90\x86") == 0);
        assert(strcmp(out.items[1].id, "tmpl_c") == 0);
        assert(strcmp(out.items[1].name, "tmpl_c") == 0);   // empty name falls back to id
        assert(!out.truncated);
    }
    const size_t in = strlen(k_identities_json);
    for (size_t step = 1; step <= in; step++) {
        parse_split(k_identities_json, step, QCA_LIST_IDENTITIES, &out, &ctx);
        assert(out.count == 2);                       // disabled idt_2 is dropped
        assert(strcmp(out.items[0].id, "idt_1") == 0);
        assert(strcmp(out.items[0].name, "Alice") == 0);
        assert(strcmp(out.items[1].id, "idt_3") == 0);
        assert(strcmp(out.items[1].name, "ext-3") == 0);    // no name -> external_id
        assert(out.truncated);                        // has_more=true
    }
    // Error envelope keeps the server message for the log.
    parse_split("{\"error\":{\"type\":\"auth\",\"message\":\"invalid token\"}}", 7,
                QCA_LIST_TEMPLATES, &out, &ctx);
    assert(out.count == 0 && strcmp(ctx.msg, "invalid token") == 0);
}

static void test_portal_form(void)
{
    char out[64];
    const char *body = "ssid=WiFi+%26%3D&pass=a%2Bb%25%22%5C&pat=pt-test-key&open=0";
    assert(form_get(body, strlen(body), "ssid", out, sizeof(out)) == 7);
    assert(strcmp(out, "WiFi &=") == 0);
    assert(form_get(body, strlen(body), "pass", out, sizeof(out)) == 6);
    assert(strcmp(out, "a+b%\"\\") == 0);
    assert(form_get("x=", 2, "x", out, sizeof(out)) == 0);
    assert(form_get("x=foo", 5, "missing", out, sizeof(out)) == -1);
    assert(form_get("x=%", 3, "x", out, sizeof(out)) == -1);
    assert(form_get("x=%2", 4, "x", out, sizeof(out)) == -1);
    assert(form_get("x=%zz", 5, "x", out, sizeof(out)) == -1);
    assert(form_get("x=%00", 5, "x", out, sizeof(out)) == -1);
    assert(form_get("x=1234", 6, "x", out, 4) == -1);
    assert(form_get("x=123", 5, "x", out, 4) == 3);
    assert(form_get("x=%E4%B8%AD", 11, "x", out, sizeof(out)) == 3);
    strcpy(out, " \tpt-test-key\r\n "); form_trim(out);
    assert(strcmp(out, "pt-test-key") == 0);
    strcpy(out, " \t\r\n"); form_trim(out); assert(!out[0]);
    assert(form_ssid_valid(" WiFi "));
    assert(!form_ssid_valid(""));
    assert(form_ssid_valid("12345678901234567890123456789012"));
    assert(!form_ssid_valid("123456789012345678901234567890123"));
    assert(form_wifi_pass_valid(""));
    assert(!form_wifi_pass_valid("1234567"));
    assert(form_wifi_pass_valid("12345678"));
    char psk[65]; memset(psk, 'a', 64); psk[64] = 0;
    assert(form_wifi_pass_valid(psk)); psk[0] = 'z'; assert(!form_wifi_pass_valid(psk));
    assert(form_pat_valid("pt-test-key", 255));
    assert(!form_pat_valid("pt-test-key\r\nX:1", 255));
    assert(!form_pat_valid("pt-test key", 255));
    assert(!form_pat_valid("not-a-pat", 255));
    assert(!form_pat_valid("pt-test-key", 8));
    char escaped[64];
    assert(form_json_escape(escaped, sizeof(escaped), "a\"\\\n", 4) == 11);
    assert(strcmp(escaped, "a\\\"\\\\\\u000a") == 0);
    assert(form_json_escape(escaped, 2, "\"", 1) == 0 && !escaped[0]);
    assert(form_json_escape(escaped, 3, "\"a", 2) == 2);
    assert(strcmp(escaped, "\\\"") == 0);
}

int main(void)
{
    test_portal_form();
    test_qjson();
    test_b64();
    test_dsp();
    test_barge();
    test_vad();
    test_pause_phrase();
    test_fsm();
    test_utf8();
    test_list();
    test_list_parse();
    return 0;
}
