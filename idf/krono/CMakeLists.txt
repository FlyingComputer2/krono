# ESP-IDF component: krono synch (Synch.h) and its GPIO pulse / edge-capture
# glue (SynchGpio.h). Add <krono>/idf/krono to EXTRA_COMPONENT_DIRS.
set(KRONO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

idf_component_register(
    SRCS ${KRONO_ROOT}/src/synch/Synch.cpp
         ${KRONO_ROOT}/src/synch/SynchGpio.cpp
    INCLUDE_DIRS ${KRONO_ROOT}/src
    PRIV_REQUIRES lwip pthread esp_driver_gpio esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
)
