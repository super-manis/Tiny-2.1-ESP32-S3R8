set(embed "")
if(CONFIG_TINY_EMBED_MODEL)
    set(embed "../../model/TINY.BIN")
endif()

idf_component_register(SRCS "src/tiny_watch.c" "src/tiny_voice.c"
                       INCLUDE_DIRS "include"
                       REQUIRES tiny_engine
                       PRIV_REQUIRES esp_partition spi_flash esp_pm log freertos
                       EMBED_FILES ${embed})
