# Windows 真实显示比例扩展

本扩展改变 Sunshine 捕获显示器的 Windows 系统 DPI，因此会影响该显示器中的文字、图标和遵循 DPI 的应用界面；它不是 Moonlight 的图像放大。与本地 Windows 设置相同，部分旧应用可能不会立即重绘。接口不重启系统、不注销、不写注册表、不安装显示驱动，也不修改客户端缩放偏好。

## 请求与响应

仅在现有 GameStream HTTPS 服务注册，复用已配对且启用的客户端证书校验。无证书、未知、禁用及已解除配对身份不能进入处理函数。明文 HTTP 不注册此路由，不使用 Web 管理密码或额外凭据。Windows 构建的 `serverinfo` 返回 `OfficeDisplayScale=1`，表示协议存在；具体显示器与当前桌面是否可操作必须查询确认。非 Windows 构建不宣告该能力，接口返回 501。

- 读取：`GET /office/display-scale`。查询和 body 都必须为空；不得附带 GameStream 常规的 `uniqueid`、`uuid`。
- 写入：`POST /office/display-scale?scale=150&displayToken=<GET返回的令牌>`。body 为空，query 必须恰有一个 `scale` 和一个 `displayToken`，拒绝未知及重复参数。
- `scale` 是规范十进制整数，如 `150` 或 `200`；不接受前导零、正号、空白、单位及小数。不支持的整数返回 422，不做 clamp。
- `displayToken` 必须为读取返回的 32 位小写十六进制字符串。它由随机值生成，不包含主机、显示器或账户身份，不是授权令牌；授权始终由已配对 TLS 证书完成。进程重启、查询失败或目标/活动拓扑变化后须重新读取。

成功读取或写入的示例（比例列表仅为示例，不是所有显示器的固定能力）：

```xml
<root status_code="200">
  <scale>150</scale>
  <recommended>100</recommended>
  <supportedScales>100,125,150,175,200</supportedScales>
  <displayToken>0123456789abcdef0123456789abcdef</displayToken>
</root>
```

客户端必须读取 XML 的 `status_code`；HTTP 200 不代表操作成功。只有实际系统读数有效且写后再次查询确认目标和比例一致才返回 XML 200。成功响应中的 `scale` 永远来自 backend 查询结果。错误不返回猜测的比例或旧快照，可返回 `message` 说明原因。

| XML status_code | 含义 |
| --- | --- |
| 200 | 查询成功，或写后系统读回已确认 |
| 400 | 参数、令牌格式、GET query 或 body 不符合协议 |
| 401 | 配对证书未授权；缺证书也可能在 TLS 层直接失败 |
| 409 | 捕获目标不唯一、克隆显示、目标令牌失效、拓扑改变或写后不一致 |
| 422 | 指定比例不在该显示器明确支持的比例列表内 |
| 501 | 非 Windows 平台不支持 |
| 503 | Windows API、控制台访问、WDDM、DPI awareness 或有效 DPI 核验失败 |

多客户端共享同一个 Windows 显示器状态；服务端用局部 mutex 串行执行查询/写入/读回，客户端也应串行发送。服务端不保存用户比例偏好；原先取得的比例只用于显示，不能在重新连接时自动覆盖系统状态。错误后客户端可再 GET 一次确认系统实际值，不能盲目重复写。

## 捕获目标与系统实现

1. 读取 `config::video.output_name`。配置非空时通过 `display_device::map_output_name` 映射至 GDI `\\.\DISPLAYn`，再与 `QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` 中的 source 名称匹配。映射失败或不是恰好一个匹配则拒绝。
2. 配置为空时仅在活动拓扑中恰有一个 source 才允许；多个活动 source 不猜测主显示器。一个 source 克隆至多个 target 也拒绝，避免一次请求影响其他显示器。
3. 读取每条活动路径的 adapter/source/target 和 target 设备路径，在服务端形成私有目标与活动拓扑指纹。原始标识不会返回、打印或写入配置；对外只给随机进程内令牌。拓扑、捕获配置或显示器身份变化使旧令牌失效，路径枚举顺序变化不会单独使其失效。
4. 使用 `DisplayConfigGetDeviceInfo` 的私有 type `-3`（32 字节 header+min/current/max relative packet）读取推荐、当前及支持边界，再以标准表 `100,125,150,175,200,225,250,300,350,400,450,500` 解码。先以 64 位数校验相对边界和索引；越界、自定义哨兵及未知值直接拒绝。
5. 用 `GetProcessDpiAwareness` 确认进程已处于 per-monitor awareness，再按选定 GDI source 找到唯一 HMONITOR，通过 `GetDpiForMonitor(MDT_EFFECTIVE_DPI)` 核验实际 x/y DPI 与解码比例精确一致。不改变进程或线程 DPI awareness，不将系统感知不足返回的 96 DPI 当作真实 100%。Sunshine 原有采集初始化负责设置 per-monitor v2，因此应在串流建立后查询。
6. 写入前重新查询和核验目标、当前支持比例及活动拓扑，然后仅对该 source 调用 `DisplayConfigSetDeviceInfo` 私有 type `-4`（24 字节 packet）。不广播，不改所有显示器，不采用注册表替代，不 clamp。API 返回成功后再独立查询，只有目标相同且当前比例等于请求才报告成功；幂等写也再次查询。

Windows 没有提供本扩展使用的原子“按拓扑指纹写 DPI”操作。服务端 mutex 只串行本扩展自己的请求，无法阻止操作系统或用户在最后核验与 Windows 写调用之间拔插/重排显示器。实现尽量贴近写调用再次核验，并在写后检测变化；不能将该措施描述为与外部拓扑变化原子互斥。

## 来源与兼容性限制

packet 布局、相对索引含义与标准表参考 [imniko/SetDPI 的 DpiHelper.h](https://github.com/imniko/SetDPI/blob/master/DpiHelper.h) 和 [DpiHelper.cpp](https://github.com/imniko/SetDPI/blob/master/DpiHelper.cpp)，来源许可为 [Unlicense](https://github.com/linrong123/sunshine-desk/blob/main/office/licenses/SetDPI-Unlicense.txt)。本项目独立补充目标约束、严格校验、有效 DPI 核验、序列化与读回，并去掉参考实现中的 clamp。

`-3` / `-4` 不是 Microsoft 公布和保证兼容的 DPI 子类型；Windows 更新、驱动、VDD、会话和自定义 DPI 可能使它失效。实际 DPI 与私有索引不能相互确认时拒绝，自定义比例没有另一个写入路径。Windows 全局自定义缩放中恰好等于标准百分比的行为仍需实机验证，不能承诺所有自定义模式都能独立辨识。

Microsoft 的 [DisplayConfigGetDeviceInfo 文档](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-displayconfiggetdeviceinfo) 明确列出无 WDDM 的 `ERROR_NOT_SUPPORTED`，以及无当前桌面/控制台访问（含远程会话）的 `ERROR_ACCESS_DENIED`。本扩展直接报告失败，不提升会话权限、不切换登录会话。有效 DPI 查询约束参见 [GetDpiForMonitor](https://learn.microsoft.com/en-us/windows/win32/api/shellscalingapi/nf-shellscalingapi-getdpiformonitor)。

## 验证与后续验收

新增 `tests/unit/test_office_display_scale.cpp` 检查相对索引上下界/整数溢出、严格参数、实际读数、精确支持列表、目标令牌失效、写前 TOCTOU、写失败及写后读数/目标/查询失败。`tests/display_scale_test_utils.h` 的 synthetic backend 不调用任何 Windows 显示 API，测试前安装、结束时恢复并清除令牌。

`OfficeDisplayScaleTlsTest` 通过生产 TLS server、配对验证和真实 GET/POST 路由，验证成功 XML、严格 body/query，以及未知/禁用/解除配对/缺证书/明文无法接触 backend。完整测试筛选：

```text
cmake-build-<配置>/tests/test_sunshine --gtest_filter="OfficeDisplayScale*:OfficeCursorTlsTest.*"
```

Windows CI 入口为 `.github/workflows/office-windows.yml`，调用现有 Windows AMD64 完整构建和 GoogleTest；不会执行真实显示比例写入。公共编译清单包含 core 与 Windows 实现，`docs/Doxyfile` 纳入 `office/`，严格 Doxygen 文档检查覆盖新符号。

本轮本地独立 mock 测试通过 14 项，启用 `-Wall -Wextra -Werror`；完整生产 TLS 新用例须等待 Windows CI。本轮没有连接远端运行此接口，没有部署、重启/注销，也没有实际更改任何显示器比例。

真实验收尚未完成：实体显示器/VDD 分别读取；150%/200%及恢复；显示器重排/断开时旧令牌拒绝；独立多屏只改捕获屏；克隆/自定义DPI/无console权限拒绝；低 DPI awareness、Windows版本和驱动兼容；应用文字图标实际变化及既有串流连续性。需在用户授权的合适时机执行，不能用 mock 或编译通过替代。
