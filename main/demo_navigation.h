#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DEMO_NAV_INPUT_OTHER = 0,
    DEMO_NAV_INPUT_UP_CLICK,
    DEMO_NAV_INPUT_DOWN_CLICK,
    DEMO_NAV_INPUT_OK_CLICK,
    DEMO_NAV_INPUT_OK_LONG,
} demo_nav_input_t;

typedef enum {
    DEMO_NAV_ACTION_NONE = 0,
    DEMO_NAV_ACTION_REFRESH,
    DEMO_NAV_ACTION_ENTER,
    DEMO_NAV_ACTION_EXIT,
    DEMO_NAV_ACTION_FORWARD,
} demo_nav_action_t;

typedef struct {
    size_t selected;
    int active;
    size_t count;
} demo_navigation_t;

typedef struct {
    demo_nav_action_t action;
    size_t index;
} demo_nav_result_t;

void demo_navigation_init(demo_navigation_t *navigation, size_t count);
demo_nav_result_t demo_navigation_handle(demo_navigation_t *navigation,
                                         demo_nav_input_t input,
                                         bool selected_available);
void demo_navigation_complete_exit(demo_navigation_t *navigation);
