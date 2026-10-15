#include "demo_navigation.h"

void demo_navigation_init(demo_navigation_t *navigation, size_t count) {
    navigation->selected = 0;
    navigation->active = -1;
    navigation->count = count;
}

demo_nav_result_t demo_navigation_handle(demo_navigation_t *navigation,
                                         demo_nav_input_t input,
                                         bool selected_available) {
    demo_nav_result_t result = { .action = DEMO_NAV_ACTION_NONE, .index = 0 };
    if (!navigation || navigation->count == 0) return result;

    if (navigation->active >= 0) {
        result.index = (size_t)navigation->active;
        result.action = input == DEMO_NAV_INPUT_OK_LONG
                      ? DEMO_NAV_ACTION_EXIT
                      : DEMO_NAV_ACTION_FORWARD;
        return result;
    }

    if (input == DEMO_NAV_INPUT_UP_CLICK) {
        navigation->selected = (navigation->selected + navigation->count - 1)
                             % navigation->count;
        result.action = DEMO_NAV_ACTION_REFRESH;
    } else if (input == DEMO_NAV_INPUT_DOWN_CLICK) {
        navigation->selected = (navigation->selected + 1) % navigation->count;
        result.action = DEMO_NAV_ACTION_REFRESH;
    } else if (input == DEMO_NAV_INPUT_OK_CLICK && selected_available) {
        navigation->active = (int)navigation->selected;
        result.action = DEMO_NAV_ACTION_ENTER;
    }
    result.index = navigation->selected;
    return result;
}

void demo_navigation_complete_exit(demo_navigation_t *navigation) {
    if (navigation) navigation->active = -1;
}
