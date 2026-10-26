cmake_minimum_required(VERSION 3.16)
project(lunar_mesh C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(lmesh STATIC
    src/aes.c
    src/frame.c
    src/link.c
    src/flood.c
    src/neighbor.c
    src/hello.c
    src/route.c
    src/store.c
    src/message.c
    src/dedup.c
    src/status.c
    src/host.c
    src/node.c
)
target_include_directories(lmesh PUBLIC include PRIVATE src)

function(lmesh_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion)
    endif()
endfunction()
lmesh_warnings(lmesh)

# Host-only targets: simulator and unit tests. Skipped when this folder is pulled into a firmware build.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()

    add_executable(lmesh_sim sim/sim_net.c sim/sim_main.c)
    target_link_libraries(lmesh_sim PRIVATE lmesh)
    lmesh_warnings(lmesh_sim)

    function(lmesh_add_test name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE lmesh)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        lmesh_warnings(${name})
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    lmesh_add_test(test_smoke tests/unit/test_smoke.c)
    lmesh_add_test(test_frame tests/unit/test_frame.c)
    lmesh_add_test(test_link tests/unit/test_link.c)
    lmesh_add_test(test_flood tests/unit/test_flood.c)
    lmesh_add_test(test_neighbor tests/unit/test_neighbor.c)
    lmesh_add_test(test_hello tests/unit/test_hello.c)
    lmesh_add_test(test_route tests/unit/test_route.c)
    lmesh_add_test(test_store tests/unit/test_store.c)
    lmesh_add_test(test_message tests/unit/test_message.c)
    lmesh_add_test(test_dedup tests/unit/test_dedup.c)
    lmesh_add_test(test_status tests/unit/test_status.c)
    lmesh_add_test(test_host tests/unit/test_host.c)
    add_test(NAME sim_smoke COMMAND lmesh_sim smoke 1)
endif()
