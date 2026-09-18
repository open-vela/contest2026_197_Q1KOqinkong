if(CONFIG_LVX_USE_DEMO_CONTEST2026_197_FOODLOOP)
  nuttx_add_application(
    NAME foodloop
    SRCS foodloop_main.c foodloop_ui.c
    STACKSIZE 8192)
endif()
