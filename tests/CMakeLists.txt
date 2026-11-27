# SRT 线路离线回归测试（CTest）。无外网、不需要 OBS 运行，也不依赖 libsrt；
# tests/manual/ 下另有需要系统 OBS 的手工集成验证程序（不注册进 CTest）。
find_package(libobs REQUIRED)
find_package(Threads REQUIRED)

# 1) rtmpsrt:// 目标解析（srt_target.cpp，纯函数）。
add_executable(
  srt_target_test
  srt_target_test.cpp
  ${CMAKE_SOURCE_DIR}/src/srt/srt_target.cpp
)
target_include_directories(srt_target_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME srt_target_test COMMAND srt_target_test)

# 2) 服务目录合并（srt_catalog.cpp 带 libobs 符号，纯函数路径不触 obs API）。
add_executable(
  srt_catalog_test
  srt_catalog_test.cpp
  ${CMAKE_SOURCE_DIR}/src/srt/srt_catalog.cpp
  ${CMAKE_SOURCE_DIR}/src/json11/json11.cpp
)
target_include_directories(srt_catalog_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(srt_catalog_test PRIVATE OBS::libobs Threads::Threads)
add_test(NAME srt_catalog_test COMMAND srt_catalog_test)
