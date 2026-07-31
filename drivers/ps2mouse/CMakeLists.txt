add_library(ps2mouse STATIC)

target_sources(ps2mouse PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ps2mouse.c
    ${CMAKE_CURRENT_LIST_DIR}/ps2mouse.h
    ${CMAKE_CURRENT_LIST_DIR}/ps2mouse_wrapper.c
    ${CMAKE_CURRENT_LIST_DIR}/ps2mouse_wrapper.h
)

target_link_libraries(ps2mouse PRIVATE 
    hardware_gpio 
    hardware_irq
    pico_stdlib
)

# Add board variant define
if(BOARD_VARIANT STREQUAL "M2")
    target_compile_definitions(ps2mouse PRIVATE BOARD_M2)
elseif(BOARD_VARIANT STREQUAL "C2")
    target_compile_definitions(ps2mouse PRIVATE BOARD_C2)
else()
    target_compile_definitions(ps2mouse PRIVATE BOARD_M1)
endif()

target_include_directories(ps2mouse PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_include_directories(ps2mouse PRIVATE
    ../../src
    ../../src/doomgeneric/doomgeneric
)
