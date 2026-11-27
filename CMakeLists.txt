cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

option(ENABLE_SRT_OUTPUT "Bilibili private SRT output (forked OBS RTMP stack, ADR-0008)" ON)
option(ENABLE_HEVC "Enable HEVC in the forked RTMP stack" ON)

configure_file(src/plugin-support.c.in src/plugin-support.c @ONLY)

execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_COMMIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT GIT_COMMIT_HASH)
  set(GIT_COMMIT_HASH "unknown")
endif()

add_library(
  ${CMAKE_PROJECT_NAME}
  MODULE
  src/bilibili_api.hpp
  src/bili_parsers.hpp
  src/http_client.hpp
  src/plugin_utils.hpp
  src/md5.hpp
  src/qrcodegen/qrcodegen.hpp
  src/json11/json11.hpp
  src/core/config_manager.hpp
  src/core/qr_generator.hpp
  src/core/session.hpp
  src/core/platform_util.hpp
  src/ui/console.hpp
  src/plugin-main.cpp
  src/bilibili_api.cpp
  src/bili_parsers.cpp
  src/http_client.cpp
  src/plugin_utils.cpp
  src/md5.cpp
  src/qrcodegen/qrcodegen.cpp
  src/json11/json11.cpp
  src/core/config_manager.cpp
  src/core/qr_generator.cpp
  src/core/platform_util.cpp
  src/core/session.cpp
  src/ui/console.cpp
  src/srt/srt_catalog.hpp
  src/srt/srt_catalog.cpp
  src/srt/srt_target.hpp
  src/srt/srt_target.cpp
  src/srt/srt_module.hpp
  src/srt/srt_module.cpp
)

find_package(libobs REQUIRED)
find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs CURL::libcurl)

# B站私有 SRT 输出 = fork 的上游 RTMP 栈（只换 socket 层，ADR-0008），静态库链入；
# 缺 libsrt 时 fork 的 CMake 硬失败。BILI_ENABLE_SRT 只门控注册路径（srt_module.cpp）。
if(ENABLE_SRT_OUTPUT)
  add_subdirectory(third_party/obs-rtmp-srt)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE obs-rtmp-srt)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE BILI_ENABLE_SRT)
endif()

# 离线回归测试（tests/，CTest）：-DBUILD_TESTS=OFF 可关。
option(BUILD_TESTS "Build offline regression tests (ctest)" ON)
if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

target_compile_definitions(
  ${CMAKE_PROJECT_NAME}
  PRIVATE "PLUGIN_VERSION_STR=\"${_version}\"" "GIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\""
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# Install plugin and dependencies
install(TARGETS ${CMAKE_PROJECT_NAME} LIBRARY DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-plugins/64bit")
# data 文件：OBS 模板 %module%/data。只装 locale（缺 locale 时 obs_module_text 只回显键名）；
# data/config.json 是开发遗留样例，不随插件分发（运行期配置走 obs_module_config_path）。
install(DIRECTORY data/locale/ DESTINATION "${CMAKE_INSTALL_PREFIX}/data/obs-plugins/${CMAKE_PROJECT_NAME}/locale")
