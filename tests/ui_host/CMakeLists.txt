cmake_minimum_required(VERSION 3.16)
project(monitor_ui_host C CXX ASM)
get_filename_component(MONITOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(STRINGS "${MONITOR_ROOT}/version.txt" MONITOR_VERSION)
file(STRINGS "${MONITOR_ROOT}/sdkconfig.defaults" MONITOR_LV_KIB REGEX "^CONFIG_LV_MEM_SIZE_KILOBYTES=")
string(REPLACE "CONFIG_LV_MEM_SIZE_KILOBYTES=" "" MONITOR_LV_KIB "${MONITOR_LV_KIB}")
configure_file(lv_conf.h.in lv_conf.h @ONLY)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_BINARY_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory("${MONITOR_ROOT}/managed_components/lvgl__lvgl" lvgl)
target_include_directories(lvgl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(render render.c "${MONITOR_ROOT}/main/monitor_ui.c"
    "${MONITOR_ROOT}/main/monitor/monitor_logic.c" "${MONITOR_ROOT}/main/server_state.c"
    "$ENV{IDF_PATH}/components/json/cJSON/cJSON.c")
target_compile_definitions(render PRIVATE MONITOR_TEST_VERSION="${MONITOR_VERSION}")
target_include_directories(render PRIVATE stubs "${MONITOR_ROOT}/tests/audio_stubs"
    "${MONITOR_ROOT}/components/bsp/include" "${MONITOR_ROOT}/main"
    "${MONITOR_ROOT}/main/events" "${MONITOR_ROOT}/main/audio" "${MONITOR_ROOT}/main/monitor"
    "${MONITOR_ROOT}/main/ota" "$ENV{IDF_PATH}/components/json/cJSON")
target_link_libraries(render PRIVATE lvgl m)
