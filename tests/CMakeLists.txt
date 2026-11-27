# SRT 输出离线回归测试（CTest）。无外网、不需要 OBS 运行；
# tests/manual/ 下另有需要系统 OBS 的手工集成验证程序（不注册进 CTest）。
find_package(libobs REQUIRED)
find_package(Threads REQUIRED)

# 1) SRT 核心：URL 解析 / AMF / FLV tag / RTMP 字节级口径（E-25）/ SRT 传输。
add_executable(
  srt_core_test
  srt_core_test.cpp
  ${CMAKE_SOURCE_DIR}/src/srt/amf.cpp
  ${CMAKE_SOURCE_DIR}/src/srt/flv_mux.cpp
  ${CMAKE_SOURCE_DIR}/src/srt/rtmp_client.cpp
  ${CMAKE_SOURCE_DIR}/src/srt/srt_target.cpp
  ${CMAKE_SOURCE_DIR}/src/srt/srt_transport.cpp
)
target_include_directories(srt_core_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(TARGET PkgConfig::SRT)
  target_link_libraries(srt_core_test PRIVATE PkgConfig::SRT)
else()
  target_include_directories(srt_core_test PRIVATE ${SRT_INCLUDE_DIR})
  target_link_libraries(srt_core_test PRIVATE ${SRT_LIBRARY})
endif()
target_link_libraries(srt_core_test PRIVATE Threads::Threads)
add_test(NAME srt_core_test COMMAND srt_core_test)

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
