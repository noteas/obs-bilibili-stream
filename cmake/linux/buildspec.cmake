# CMake Linux 构建依赖模块(CI 专用,门控见 defaults.cmake)
#
# fork 编译基线需要 OBS 32.2 头(OBS_ENCODER_CAP_MULTITRACK_DYN_BITRATE 等
# 为 32.2.0 新增),而发行版 libobs-dev 版本漂移且必旧(noble=30.0.2、
# resolute=32.1.0,CI 实证均缺该宏)。曾按 Windows 同机制"CI 内从钉死源码
# 构建 OBS 出 dev 文件",Linux 上证伪,双重不可控:
#   1) OBS 32.2 的 configure 连带 ECM/X11/XCB/Libdrm/UUID/PulseAudio/
#      FFmpeg/OpenGL 等整棵依赖树(apt 12+ 包),OBS 构建系统每次演进都要
#      重新对账;
#   2) runner 自带 CMake(runner 镜像更新即漂移)与 OBS install-export 的
#      单配置产物不兼容(实证 IMPORTED_LOCATION not set for ... Release,
#      generate 失败)。
# 但插件对 OBS 不需要任何"新库":插件 .so 的符号在装载期由用户真机 OBS
# 解析;链接面只要符号子集存在(30 时代符号全在,32.2 新增宏是编译期常量);
# 真 32.2 链接门槛由 flatpak job(flathub OBS 32.2 运行时,编译+打包)把关。
# ⇒ Linux CI 只取钉死源码包的"头面":解包后用 BEFORE include 覆盖 apt
# libobs-dev 的旧头,全程不 configure、不编译 OBS。本地开发(无 CI 环境
# 变量)不进入本模块,仍走系统 OBS 的 libobs cmake 包,行为不变。

include_guard(GLOBAL)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" _bili_buildspec)
string(JSON _bili_obs_version GET ${_bili_buildspec} dependencies obs-studio version)
string(JSON _bili_obs_base GET ${_bili_buildspec} dependencies obs-studio baseUrl)
string(JSON _bili_obs_hash GET ${_bili_buildspec} dependencies obs-studio hashes "linux-${CMAKE_SYSTEM_PROCESSOR}")

set(_bili_obs_src "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-${_bili_obs_version}")

if(NOT EXISTS "${_bili_obs_src}/libobs/obs.h")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
  message(STATUS "下载 OBS ${_bili_obs_version} 源码包(仅取头文件面)")
  file(
    DOWNLOAD
      "${_bili_obs_base}/${_bili_obs_version}.zip"
      "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-${_bili_obs_version}.zip"
    EXPECTED_HASH SHA256=${_bili_obs_hash}
    INACTIVITY_TIMEOUT 60
  )
  file(
    ARCHIVE_EXTRACT
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-${_bili_obs_version}.zip"
    DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/.deps"
  )
endif()

# 版本护栏:钉死的头必须含 fork 所需宏,防止 buildspec 被误改后退化成
# fork 内的 cryptic 编译错。
file(READ "${_bili_obs_src}/libobs/obs-encoder.h" _bili_obs_encoder_h)
if(NOT _bili_obs_encoder_h MATCHES "OBS_ENCODER_CAP_MULTITRACK_DYN_BITRATE")
  message(FATAL_ERROR "钉死的 OBS ${_bili_obs_version} 头缺少 OBS 32.2 encoder caps —— 检查 buildspec.json obs-studio 条目")
endif()

# BEFORE 覆盖:目录级 include 路径排在链接目标(apt libobs-dev 的
# OBS::* imported target)的 INTERFACE include(/usr/include/obs)之前,
# 对本工程、fork 与 tests 全部生效。
include_directories(BEFORE "${_bili_obs_src}/libobs" "${_bili_obs_src}/frontend/api")
