# Inter-processor sound link for the C2 (FRANK Core 2) board.
#
# Built into both halves: the master (frank-genesis) and the slave
# (frank-genesis-slave). Keeping one library shared by both is what
# stops the wire protocol from drifting between the two images.

add_library(link STATIC
    ${CMAKE_CURRENT_LIST_DIR}/link_bus.c
    ${CMAKE_CURRENT_LIST_DIR}/link_proto.c
    ${CMAKE_CURRENT_LIST_DIR}/link_session.c
)

pico_generate_pio_header(link ${CMAKE_CURRENT_LIST_DIR}/link_bus.pio)

target_include_directories(link PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(link PUBLIC
    pico_stdlib
    hardware_dma
    hardware_pio
    hardware_clocks
)
