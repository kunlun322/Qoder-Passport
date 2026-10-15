// main/app_list.h —— 列表光标与可视窗口（纯逻辑，可 host 测试）。
//
// ▲/▼ 移动光标，首尾循环；窗口跟随光标滚动，保证光标总在可视行内。
#pragma once

typedef struct {
    int count;    // 条目总数
    int visible;  // 一屏可见行数
    int cursor;   // 当前选中（0..count-1；count==0 时为 0）
    int top;      // 窗口首行
} app_list_t;

static inline void app_list_fix_window(app_list_t *l) {
    if (l->count <= l->visible) { l->top = 0; return; }
    if (l->cursor < l->top) l->top = l->cursor;
    if (l->cursor >= l->top + l->visible) l->top = l->cursor - l->visible + 1;
    if (l->top > l->count - l->visible) l->top = l->count - l->visible;
    if (l->top < 0) l->top = 0;
}

static inline void app_list_init(app_list_t *l, int count, int visible) {
    l->count = count > 0 ? count : 0;
    l->visible = visible > 0 ? visible : 1;
    l->cursor = 0;
    l->top = 0;
}

static inline void app_list_move(app_list_t *l, int delta) {
    if (l->count <= 0) return;
    int c = (l->cursor + delta) % l->count;
    if (c < 0) c += l->count;
    l->cursor = c;
    app_list_fix_window(l);
}
