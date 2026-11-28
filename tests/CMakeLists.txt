# SRT 线路离线回归测试（CTest）。无外网、不需要 OBS 运行，也不依赖 libsrt；
# tests/manual/ 下另有需要系统 OBS 的手工集成验证程序（不注册进 CTest）。
find_package(libobs REQUIRED)
find_package(Threads REQUIRED)

# 1) rtmpsrt:// 目标解析（srt_target.cpp，纯函数）。
add_executable(srt_target_test srt_target_test.cpp ${CMAKE_SOURCE_DIR}/src/srt/srt_target.cpp)
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

# 3) 端点响应解析(bili_parsers.cpp,纯函数,只依赖 json11):nav 三态、room/GetInfo、
# startLive 解析序(protocols[].code / live_key 回落 / rtmp.new_link 回落 / notice /
# v_voucher / 人脸 / 60043)。fixtures = 契约钉死的形状。
add_executable(
  bili_parsers_test
  bili_parsers_test.cpp
  ${CMAKE_SOURCE_DIR}/src/bili_parsers.cpp
  ${CMAKE_SOURCE_DIR}/src/json11/json11.cpp
)
target_include_directories(bili_parsers_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME bili_parsers_test COMMAND bili_parsers_test)

# 4) 无头集成 harness(默认关):真实 libobs + 真实 Qt(offscreen)+ 假 B站服务端,把编译出的
# 插件 .so 按 OBS 的方式装进来并从外部驱动侧栏。不入 CTest(需要起服务端),入口 =
# tests/harness/run_headless.sh。可执行体导出 obs_frontend_* 替身(ENABLE_EXPORTS)。
option(BUILD_HARNESS "Build the headless integration harness (needs Qt6)" OFF)
if(BUILD_HARNESS)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
  add_executable(bili_harness harness/headless_main.cpp ${CMAKE_SOURCE_DIR}/src/json11/json11.cpp)
  target_include_directories(bili_harness PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(bili_harness PRIVATE OBS::libobs Qt6::Core Qt6::Gui Qt6::Widgets)
  set_target_properties(bili_harness PROPERTIES ENABLE_EXPORTS ON)
  # LD_PRELOAD 垫片:让被测插件的 libcurl 信任假服务端的测试 CA(插件代码零改动)。
  find_package(OpenSSL REQUIRED)
  add_library(bili_test_ca_shim MODULE harness/libcurl_test_ca_shim.c)
  set_target_properties(bili_test_ca_shim PROPERTIES PREFIX "")
  target_link_libraries(bili_test_ca_shim PRIVATE OpenSSL::SSL OpenSSL::Crypto ${CMAKE_DL_LIBS})
endif()
