idf_component_register(SRCS "src/tiny_watch_ui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES tiny_watch
                       PRIV_REQUIRES freertos)
