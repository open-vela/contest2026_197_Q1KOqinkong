#ifndef __CONTEST2026_197_Q1KOQINKONG_APP_FOODLOOP_UI_H
#define __CONTEST2026_197_Q1KOQINKONG_APP_FOODLOOP_UI_H

#include <stdbool.h>
#include <stdint.h>

enum foodloop_ui_state_e
{
  FOODLOOP_UI_HOME,
  FOODLOOP_UI_SCANNING,
  FOODLOOP_UI_ANALYZING,
  FOODLOOP_UI_PREVIEW,
  FOODLOOP_UI_CAPTURED,
  FOODLOOP_UI_DRAFT_READY,
  FOODLOOP_UI_CONFIRMED,
  FOODLOOP_UI_ERROR
};

int foodloop_ui_show(enum foodloop_ui_state_e state, bool uses_mimo);
int foodloop_ui_show_demo(enum foodloop_ui_state_e state);
int foodloop_ui_show_preview(uint16_t *frame, int width, int height);
int foodloop_ui_show_live_frame(uint16_t *frame, int width, int height);
int foodloop_ui_show_draft_result(FAR const char *const *lines,
                                  int line_count);
int foodloop_ui_show_list_result(FAR const char *title,
                                 FAR const char *const *lines,
                                 int line_count);

#endif
