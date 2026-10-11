cmake_minimum_required(VERSION 3.14)

project(Parking LANGUAGES C CXX)

# C 代码标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# plate_recognizer.cpp 使用了 std::clamp，需要 C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找 OpenCV
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
find_package(Threads REQUIRED)

option(PARKING_BUILD_LVGL_TEST
    "Build the standalone LVGL DRM/KMS display test"
    OFF
)

set(PARKING_LVGL_SOURCE_DIR "" CACHE PATH
    "Optional local LVGL v9.6.0 source directory"
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(CMAKE_CROSSCOMPILING AND DEFINED RK3576_SDK_ROOT)
    set(_parking_libdrm_package_dir
        "${RK3576_SDK_ROOT}/ubuntu22.04/packages/arm64/libdrm")
    set(PARKING_LIBDRM_DEV_DEB
        "${_parking_libdrm_package_dir}/libdrm-dev_2.4.114-1_arm64.deb"
        CACHE FILEPATH "ARM64 libdrm development package")
    set(PARKING_LIBDRM_RUNTIME_DEB
        "${_parking_libdrm_package_dir}/libdrm2_2.4.114-1_arm64.deb"
        CACHE FILEPATH "ARM64 libdrm runtime package")
    unset(_parking_libdrm_package_dir)
endif()

include(FetchContent)
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps"
    CACHE PATH "Downloaded dependency directory" FORCE)

if(PARKING_LVGL_SOURCE_DIR)
    FetchContent_Declare(lvgl SOURCE_DIR "${PARKING_LVGL_SOURCE_DIR}")
else()
    FetchContent_Declare(lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG v9.6.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
endif()

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h"
    CACHE FILEPATH "LVGL configuration" FORCE)
set(LV_BUILD_SET_CONFIG_OPTS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(LV_USE_FIND_PACKAGE_LIBDRM ON CACHE BOOL "" FORCE)
set(LV_USE_PKG_CONFIG_LIBDRM OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_LINUX_DRM ON CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_LINUX_DRM_GBM_BUFFERS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_LINUX_DRM_USE_EGL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(lvgl)

execute_process(
    COMMAND ${CMAKE_COMMAND}
        -DLVGL_SOURCE_DIR=${lvgl_SOURCE_DIR}
        -DLVGL_DRM_PATCH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/lvgl-v9.6.0-drm-zpos.patch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyLvglDrmPatch.cmake
    RESULT_VARIABLE _parking_lvgl_patch_result
)
if(NOT _parking_lvgl_patch_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply the LVGL DRM plane zpos patch")
endif()
unset(_parking_lvgl_patch_result)

# 导入项目自带的 RKNN Runtime
add_library(rknnrt SHARED IMPORTED)

set_target_properties(rknnrt PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/lib/librknnrt.so"
)

# 摄像头、日志等基础模块
add_library(parking_lib STATIC
    src/camera/camera.c
    src/display/display.c
    src/image/image_convert.c
    src/gate/gate.c
    src/gate/pwm_sysfs.c
    src/log/log.c
    src/pipeline/pipeline_frame.c
    src/pipeline/latest_frame_slot.c
    src/pipeline/plate_confirmation.c
)

target_include_directories(parking_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_link_libraries(parking_lib
    PUBLIC
        m
)

# 停车业务状态机（不直接依赖 Camera、Gate、SQLite 或网络）
add_library(parking_business_lib STATIC
    src/business/parking_controller.c
    src/business/parking_fee.c
    src/business/parking_plate_policy.c
)

target_include_directories(parking_business_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# SQLite 停车记录与车位存储层。实现运行时加载 libsqlite3，避免
# RK3576 工具链 sysroot 缺少 SQLite 开发包时阻塞交叉编译。
add_library(parking_storage_lib STATIC
    src/storage/parking_store.c
)

target_include_directories(parking_storage_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(parking_storage_lib
    PUBLIC
        ${CMAKE_DL_LIBS}
)

# 正式 LVGL/DRM 停车界面，由主线程驱动，不直接访问业务或硬件模块。
add_library(parking_ui_lib STATIC
    src/ui/parking_ui.c
)

target_include_directories(parking_ui_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(parking_ui_lib
    PUBLIC
        lvgl::lvgl
)

target_compile_options(parking_ui_lib
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
)

# 车牌识别模块
add_library(plate_recognizer_lib STATIC
    src/recognizer/rknn_model.cpp
    src/recognizer/plate_preprocess.cpp
    src/recognizer/plate_postprocess.cpp
    src/recognizer/plate_visualizer.cpp
    src/recognizer/plate_recognizer.cpp
)

target_include_directories(plate_recognizer_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_link_libraries(plate_recognizer_lib
    PUBLIC
        ${OpenCV_LIBS}
        rknnrt
)

# 多线程车牌识别管线
add_library(plate_pipeline_lib STATIC
    src/pipeline/plate_event_queue.c
    src/pipeline/plate_pipeline.c
)

target_include_directories(plate_pipeline_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/pipeline"
)

target_link_libraries(plate_pipeline_lib
    PUBLIC
        parking_lib
        plate_recognizer_lib
        Threads::Threads
)

# 主程序
add_executable(parking
    src/main.c
)

target_link_libraries(parking
    PRIVATE
        plate_pipeline_lib
        parking_business_lib
        parking_storage_lib
        parking_ui_lib
)

target_compile_options(parking
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
)

if(PARKING_BUILD_LVGL_TEST)
    add_library(parking_lvgl_demo_lib STATIC
        src/ui/parking_lvgl_demo.c
    )

    target_include_directories(parking_lvgl_demo_lib
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )

    target_link_libraries(parking_lvgl_demo_lib
        PUBLIC
            lvgl::lvgl
    )

    target_compile_options(parking_lvgl_demo_lib
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
    )

    add_executable(test_lvgl_display
        tests/test_lvgl_display.c
    )

    target_link_libraries(test_lvgl_display
        PRIVATE
            parking_lvgl_demo_lib
    )

    target_compile_options(test_lvgl_display
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
    )

    set_target_properties(test_lvgl_display PROPERTIES
        SKIP_BUILD_RPATH TRUE
    )
endif()

# 纯 C 车牌业务入口策略测试（不访问摄像头、NPU、Display 或 Gate）
add_executable(test_parking_plate_policy
    tests/test_parking_plate_policy.c
)

target_link_libraries(test_parking_plate_policy
    PRIVATE
        parking_business_lib
)

# 纯 C 停车计费测试（不访问数据库或硬件）
add_executable(test_parking_fee
    tests/test_parking_fee.c
)

target_link_libraries(test_parking_fee
    PRIVATE
        parking_business_lib
)

# 纯 C SQLite 停车存储测试（使用临时数据库，不访问硬件）
add_executable(test_parking_store
    tests/test_parking_store.c
)

target_link_libraries(test_parking_store
    PRIVATE
        parking_storage_lib
        parking_business_lib
)

# 摄像头测试
add_executable(test_recognizer
    tests/test_recognizer.cpp
)

target_include_directories(test_recognizer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)

target_link_libraries(test_recognizer
    PRIVATE
        plate_recognizer_lib
)

# USB 摄像头到 framebuffer 的真实硬件显示测试（不注册到 CTest）
add_executable(test_camera
    tests/test_camera.c
)

target_link_libraries(test_camera
    PRIVATE
        parking_lib
)

# USB 摄像头实时车牌识别测试（不注册到 CTest）
add_executable(test_realtime_recognizer
    tests/test_realtime_recognizer.c
)

target_link_libraries(test_realtime_recognizer
    PRIVATE
        parking_lib
        plate_recognizer_lib
)

set_target_properties(test_realtime_recognizer PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# USB 摄像头多线程实时车牌识别测试（不注册到 CTest）
add_executable(test_realtime_recognizer_threaded
    tests/test_realtime_recognizer_threaded.c
)

target_link_libraries(test_realtime_recognizer_threaded
    PRIVATE
        plate_pipeline_lib
)

set_target_properties(test_realtime_recognizer_threaded PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# 稳定车牌事件到 Gate 的受控联动测试（默认 dry-run，不注册到 CTest）
add_executable(test_realtime_plate_gate
    tests/test_realtime_plate_gate.c
)

target_link_libraries(test_realtime_plate_gate
    PRIVATE
        plate_pipeline_lib
)

set_target_properties(test_realtime_plate_gate PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# Gate 真实硬件交互测试（不会注册到 CTest，避免自动驱动闸门）
add_executable(test_gate
    tests/test_gate.c
)

target_link_libraries(test_gate
    PRIVATE
        parking_lib
)

set_target_properties(test_recognizer PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# 让构建目录里的程序找到项目中的 librknnrt.so
set_target_properties(parking PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/lib"
)
