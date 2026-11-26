# 请求指纹全套原生 LiveHime 画像，无指纹开关

插件采用与官方直播姬一致的原生形态：`LiveHime/{ver} os/Windows pc_app/livehime build/{build}` UA、不发 Origin/Referer、Content-Type 无 charset、全请求携带 buvid3（缺失经 `x/frontend/finger/spi` 自动获取并持久化）、client_traceid 每会话 UUID、nav 与 stopLive 同带 appsign。

不提供“浏览器画像”开关：信令层本就在扮原生客户端，画像与认证方式必须自洽，半浏览器混搭是最脆弱的组合。
