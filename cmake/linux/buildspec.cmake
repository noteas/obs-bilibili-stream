# CMake Linux build dependencies module
#
# CI 专用:OBS 开发文件从 buildspec.json 钉死的源码版本(= fork 基线 32.2.0)构建,
# 与 Windows/macOS 同机制(见 cmake/windows/buildspec.cmake)。
# 动机(2026-11-27 CI 实证):发行版 libobs-dev 版本不可控 —— noble=30.0.2、
# resolute=32.1.0,均缺 fork 需要的 OBS 32.2 头(OBS_ENCODER_CAP_MULTITRACK_DYN_BITRATE
# 为 32.2.0 新增);OBS 官方 PPA 只发 obs-studio 单包,dev 文件布局不可依赖。
# 本地开发不受影响:defaults.cmake 仅在 CI 环境变量存在时 include 本模块,
# 平时仍走系统 OBS 的 libobs cmake 包。

include_guard(GLOBAL)

include(buildspec_common)

# _check_dependencies_linux: Set up Linux slice for _check_dependencies
function(_check_dependencies_linux)
  set(arch ${CMAKE_SYSTEM_PROCESSOR})
  set(platform linux-${arch})

  set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
  # 与 Windows 下载同一个源码 zip ⇒ buildspec 哈希同源复用
  set(obs-studio_filename "VERSION.zip")
  set(obs-studio_destination "obs-studio-VERSION")
  set(dependencies_list obs-studio)

  _check_dependencies()
endfunction()

_check_dependencies_linux()
