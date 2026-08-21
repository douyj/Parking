 cmake_minimum_required(VERSION 3.10)

  project(Parking C)

  set(CMAKE_C_STANDARD 99)
  set(CMAKE_C_STANDARD_REQUIRED ON)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

  add_library(parking_lib STATIC
      src/camera/camera.c
      src/log/log.c
  )

  target_include_directories(parking_lib PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
  )

  # 主程序
  add_executable(parking
      src/main.c
  )

  target_link_libraries(parking PRIVATE parking_lib)

  # 测试程序
  add_executable(test_camera
      tests/test_camera.c
  )

  target_link_libraries(test_camera PRIVATE parking_lib)
