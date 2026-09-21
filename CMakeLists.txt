cmake_minimum_required(VERSION 3.10)

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
)

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
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)
