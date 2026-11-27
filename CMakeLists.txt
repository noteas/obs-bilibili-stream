cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_SRT_OUTPUT "Enable Bilibili private SRT (RTMP over SRT) output" ON)

include(compilerconfig)
include(defaults)
include(helpers)

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

set(SRT_SOURCES)
if(ENABLE_SRT_OUTPUT)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SRT QUIET IMPORTED_TARGET srt)
  endif()
  if(NOT SRT_FOUND)
    find_path(SRT_INCLUDE_DIR NAMES srt/srt.h)
    find_library(SRT_LIBRARY NAMES srt srt-gnutls)
    if(SRT_INCLUDE_DIR AND SRT_LIBRARY)
      set(SRT_FOUND TRUE)
    endif()
  endif()
  if(SRT_FOUND)
    set(SRT_SOURCES
      src/srt/amf.hpp
      src/srt/amf.cpp
      src/srt/byte_io.hpp
      src/srt/byte_stream.hpp
      src/srt/av1_parse.hpp
      src/srt/av1_parse.cpp
      src/srt/flv_mux.hpp
      src/srt/flv_mux.cpp
      src/srt/hevc_parse.hpp
      src/srt/hevc_parse.cpp
      src/srt/rtmp_client.hpp
      src/srt/rtmp_client.cpp
      src/srt/srt_catalog.hpp
      src/srt/srt_catalog.cpp
      src/srt/srt_module.hpp
      src/srt/srt_module.cpp
      src/srt/srt_output.hpp
      src/srt/srt_output.cpp
      src/srt/srt_target.hpp
      src/srt/srt_target.cpp
      src/srt/srt_transport.hpp
      src/srt/srt_transport.cpp
    )
  else()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # Linux 是本插件的主平台：SRT 输出静默缺失会让人以为功能已生效（服务下拉里有
      # 条目但推流会报「未注册协议」），所以这里直接失败，除非显式 -DENABLE_SRT_OUTPUT=OFF。
      message(
        FATAL_ERROR
          "libsrt not found: 私有 SRT 输出是主功能，不默认降级。安装 libsrt-gnutls-dev（或 apt install libsrt-dev），"
          "或显式关闭：-DENABLE_SRT_OUTPUT=OFF")
    else()
      # 其他平台尚未验证 SRT 路径（CI 上也没装 libsrt），保持降级但把它说清楚。
      message(
        WARNING "libsrt not found on ${CMAKE_SYSTEM_NAME}: 私有 SRT 输出已禁用（本平台未验证，构建其余部分照常）")
      set(ENABLE_SRT_OUTPUT OFF)
    endif()
  endif()
endif()

add_library(
  ${CMAKE_PROJECT_NAME}
  MODULE
  src/bilibili_api.hpp
  src/http_client.hpp
  src/plugin_utils.hpp
  src/md5.hpp
  src/qrcodegen/qrcodegen.hpp
  src/json11/json11.hpp
  src/core/config_manager.hpp
  src/core/qr_generator.hpp
  src/core/platform_util.hpp
  src/ui/console.hpp
  src/plugin-main.cpp
  src/bilibili_api.cpp
  src/http_client.cpp
  src/plugin_utils.cpp
  src/md5.cpp
  src/qrcodegen/qrcodegen.cpp
  src/json11/json11.cpp
  src/core/config_manager.cpp
  src/core/qr_generator.cpp
  src/core/platform_util.cpp
  src/ui/console.cpp
  ${SRT_SOURCES}
)

find_package(libobs REQUIRED)
find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs CURL::libcurl)

if(ENABLE_SRT_OUTPUT)
  if(TARGET PkgConfig::SRT)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::SRT)
  else()
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${SRT_INCLUDE_DIR})
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${SRT_LIBRARY})
  endif()
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE BILI_ENABLE_SRT=1)
endif()

# 离线回归测试（tests/，CTest）：跟随 SRT 输出开关；-DBUILD_TESTS=OFF 可关。
option(BUILD_TESTS "Build offline SRT regression tests (ctest)" ON)
if(ENABLE_SRT_OUTPUT AND BUILD_TESTS)
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
