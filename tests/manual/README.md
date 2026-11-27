# 手工集成验证（不注册 CTest）

这两个程序要在**装有系统 OBS** 的机器上跑：它们用纯 libobs 起一个无头 OBS，
加载 `obs-outputs` + `rtmp-services` + 本插件 `.so`，验证真实环境下的行为。
先 `cmake --build` 出插件，再按下例编译运行（路径按本机调整）：

```bash
SRC=<repo>/src
g++ -std=c++17 -g -O1 -fsanitize=address,undefined \
    -I"$SRC" -I/usr/include/obs \
    tests/manual/srt_reload_test.cpp "$SRC/srt/srt_catalog.cpp" "$SRC/json11/json11.cpp" \
    -o /tmp/srt_reload_test -l:libobs.so.30 -lpthread
/tmp/srt_reload_test
```

- `obs_integration_test.cpp`：协议 `bvc_srt` 注册命中 `bvc_srt_output`、
  服务下拉含本插件条目、`rtmpsrt://` 地址与推流码经 `rtmp_common` 原样透传给
  `obs_service_get_connect_info`。
- `srt_reload_test.cpp`：写入 services.json 后**同进程**重建 properties 即可见
  新条目（= 关掉再打开「设置→直播」就生效，无需重启 OBS）。

注意：
- 必须调 `obs_shutdown()` 再退出。不调的话 rtmp-services 的联网更新线程会在
  进程拆台时跑在已析构的 libcrypto 上，ASan 报 UAF——那是 harness 假象。
- 两者用 `/tmp/opencode/obs-*` 做独立 OBS 配置目录，互不污染真实用户配置。
