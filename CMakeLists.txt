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

# 导入项目自带的 RKNN Runtime
add_library(rknnrt SHARED IMPORTED)

set_target_properties(rknnrt PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/lib/librknnrt.so"
)

# 摄像头、日志等基础模块
add_library(parking_lib STATIC
    src/camera/camera.c
    src/log/log.c
)

target_include_directories(parking_lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
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

# 主程序
add_executable(parking
    src/main.c
)

target_link_libraries(parking
    PRIVATE
        parking_lib
        plate_recognizer_lib
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

set_target_properties(test_recognizer PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

# 让构建目录里的程序找到项目中的 librknnrt.so
set_target_properties(parking PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)
