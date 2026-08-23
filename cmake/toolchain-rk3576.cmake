# 目标系统
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 使用 LubanCat SDK 自带的工具链和 sysroot。
# 不要使用宿主 Ubuntu 的 /usr/bin/aarch64-linux-gnu-*，否则会链接
# 宿主机的新版 glibc，产物无法在板端的旧版 glibc 上运行。
set(RK3576_TOOLCHAIN_ROOT
    /home/dyj/SDK/LubanCat_Linux_Generic_SDK_20260729/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu
    CACHE PATH
    "LubanCat RK3576 cross toolchain root"
)

set(CMAKE_C_COMPILER
    "${RK3576_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-gcc"
)

set(CMAKE_CXX_COMPILER
    "${RK3576_TOOLCHAIN_ROOT}/bin/aarch64-none-linux-gnu-g++"
)

set(CMAKE_SYSROOT
    "${RK3576_TOOLCHAIN_ROOT}/aarch64-none-linux-gnu/libc"
)

# ARM64 OpenCV 配置目录
set(OpenCV_DIR
    /home/dyj/studying/RK3576_RKNN_Learn/rknn-toolkit2/rknpu2/examples/3rdparty/opencv/opencv-linux-aarch64/share/OpenCV
    CACHE PATH
    "ARM64 OpenCV configuration directory"
)

# 查找宿主机上的构建工具
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# 库、头文件和包允许从指定的 ARM64 SDK 路径查找
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
