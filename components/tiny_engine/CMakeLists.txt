set(srcs "src/tiny_engine.c" "src/tiny_kernels.c" "src/tiny_port_esp.c")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "src/tiny_kernels_s3.S")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       LDFRAGMENTS "linker.lf"
                       PRIV_REQUIRES esp_timer esp_hw_support heap freertos)

# -O2 for the engine even in a -Og / -Os build: the inner loops are the
# whole point of this component.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-math-errno)

if(CONFIG_TINY_USE_PIE AND IDF_TARGET STREQUAL "esp32s3")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TINY_HAVE_PIE=1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE TINY_PREFILL_BLOCK=${CONFIG_TINY_PREFILL_BLOCK})
