# 架构与上游合并维护

## 基线

- 上游仓库：`SunshineStream/Sunshine`（原 `LizardByte/Sunshine`，GitHub 自动重定向，`upstream` remote 使用旧 URL 仍可用）。
- 当前基线 tag：`v2026.1118.30014`，提交 `37ee7d549391938ab5dc468783fc6dadc23dbc7e`（2026-11 合并）。
- 初始 tag：`v2026.914.233613`，提交 `63d35f702ee9e362e43263742981836ec0710384`。
- 独立服务端：`linrong123/sunshine-desk`；配套主控端：`linrong123/moonlight-desk`。
- 保留完整历史；办公新增代码与文档放在 `office/`，不复制一套编码器、网络协议或驱动栈。

## 边界与补丁位置

上游文件只保留下表中的适配点，其余办公逻辑放在 `office/`。合并冲突时以上游版本为底，再按“办公差异”列重新落适配点。

| 位置 | 办公差异 | 合并时检查 |
| --- | --- | --- |
| `src/nvhttp.cpp` | 3 处：`#include "office/nvhttp_routes.h"`；`serverinfo` 中调用 `office::add_serverinfo_capabilities(tree)`；`start()` 注册完上游 HTTPS 路由后调用 `office::register_https_routes(https_server)`；另在 `test_support` 末尾有仅测试用的 `make_verified_https_server`（其 verify 回调同样记录 `last_verified_client_cert/name`）与 `launch_for_test`。直连授权另加：`SunshineHTTPSServer::handle_handshake` 在握手校验成功后调用 `office_remember_connection(...)`，以客户端 TCP 端点为键记录该连接（`weak_ptr`）与证书；`office_verified_client(peer)` 按请求的远端端点取回证书并重查启用状态；`launch` 与 `resume` 在 `make_launch_session` 之后调用 `office_bind_launch_identity(...)` 用按连接身份覆盖会话证书与名称，在上游参数检查之后、建立会话之前调用 `office::reject_unapproved_stream(...)` 闸门；`/cancel` 调用 `office::quit_current_client(request)` 取消调用者自己的宽限。上游 `last_verified_client_cert/name` 全局变量与其写入点不动 | 只注册到 HTTPS 服务器，绝不注册到明文 HTTP；测试工厂的校验逻辑须与 `start()` 中上游 verify lambda 保持一致（证书链校验 + 客户端启用检查 + 记录验证身份 + 401 响应）；闸门须在任何会话/应用启动副作用之前 |
| `office/nvhttp_routes.*` | 办公 HTTPS 扩展：路由表（cursor、display-scale GET/POST、connect-request）与 serverinfo capability（`OfficeCursorSet`、`OfficeConnectConfirm`，Windows 上 `OfficeDisplayScale`） | 新增已认证扩展只改这里的路由表；处理函数签名跟随 `SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>` |
| `office/policy.h` | 固定 Desktop JSON、严格 cursor 参数校验、`desktop_catalog_only` 开关、`default_encryption_mode = 2` | 保持独立，不引入主机私有配置 |
| `office/connect_confirm.*`、`office/authorization*`、`office/windows/authorization_ui.cpp`、`office/tray_devices.h` | 直连授权：确认状态机、connect-request 处理与闸门、配对/连接弹窗线程、托盘设备管理，见 [CONNECTION.md](CONNECTION.md) | 依赖上游 `nvhttp::get_pending_pairings`/`pin`/`cancel_pairing`/`set_client_enabled`/`unpair_client`/`get_cert_by_uuid` 与 `rtsp_stream::terminate_sessions_by_cert`；上游改这些签名或语义时同步调整 |
| `src/main.cpp` | 首次 `proc::refresh` 前置 `office::desktop_catalog_only = true`；构造 `office::authorization_ui_t`（Windows 启动弹窗线程，其他平台空实现） | 确认仍在首次 refresh 之前；弹窗对象须在 `nvhttp::start` 线程之前构造（否则启动瞬间确认未生效），因而在其 join 之后析构 |
| `src/stream.cpp` | `session::start` 先调用 `office::stream_started`，再以 `memory_order_release` 置为 RUNNING（上游原为 relaxed；保证并发的 `stop()` 看到 RUNNING 时计数已加上，`stream_stopped` 不会先于 `stream_started`，避免计数泄漏一直显示「串流中」），`session::stop` 首次置为 STOPPING 后调用 `office::stream_stopped`（开始断线宽限）。自适应 FEC 另加 7 处：`#include "office/adaptive_fec.h"`；`session_t` 成员 `office::adaptive_fec_session_t adaptive_fec`；控制流注册 `0x5502`（`office::frame_fec_status_type`）处理函数调用 `office::adaptive_fec_feedback`；控制循环客户端已连接分支末尾调用 `office::adaptive_fec_tick`；`videoBroadcastThread` 的 `fecPercentage` 改取 `office::adaptive_fec_percentage(session->adaptive_fec, config::stream.fec_percentage)`；同函数的分块数改由 `office::fit_fec_blocks` 计算（超出 4 块上限时先退回配置值，仍超出才走上游关闭 FEC 的分支）；`session::start` 开头调用 `office::adaptive_fec_start` | 两处须分别位于会话真正启动与结束的唯一路径上；`adaptive_fec_start` 须在控制/视频线程启动前；上游若自行处理 `0x5502` 或改动 FEC 分块（`fec::max_data_shards`、4 块上限），按 [ADAPTIVE_FEC.md](ADAPTIVE_FEC.md) 复核估计公式 |
| `src/config.cpp` | `lan/wan_encryption_mode` 默认值取 `office::default_encryption_mode`；`apply_config` 在解析 flags 前调用 `office::apply_config(vars)`，读取并移除 `desk_confirm_connections` | 上游新增加密默认值或改 apply_config 顺序时复核；该键须在上游「未知配置项」告警前被移除 |
| `src/system_tray.cpp/.h` | Windows 菜单在上游 Windows 项前插入「已授权设备」子菜单、「每次连接需本机确认」勾选项和分隔线；新增 `refresh_office_menu()`（持托盘锁重建子菜单，刷新时暂存通知以免重复弹出）。原生主窗口另加 3 处：`_WIN32` office 头文件块里 `#include "office/main_window.h"`；静态 `tray` 结构 `#elif defined(_WIN32)` 分支设 `.cb = office::tray_open_main_window_cb`（左键打开主窗口，macOS 分支保持上游）；配对通知回调 `notification_cb` 在 `_WIN32` 下改调 `office::open_pin_ui()`，其他平台仍 `launch_ui("/pin")` | `tests/unit/test_system_tray.cpp` 的 Windows 菜单下标因此后移 3：6 已授权设备、7 勾选项、8 分隔线、9 Reset Display Device Config、10 Restart、11 Quit、12 结束符；上游增删菜单项时同步改测试下标，不删断言。`.cb` 须按 `struct tray` 成员顺序放在 `notification_cb` 之后、`menu` 之前（指定初始化器顺序）；`LeftClickOpensSunshineDeskMainWindow` 断言该回调 |
| `src/process.cpp` | `parse` 在开关打开时读取固定 Desktop 目录而非 apps 文件；函数签名与上游一致 | 检查所有生产加载入口仍经过 `parse`；单元测试中开关默认关闭，上游 apps 文件测试照常运行 |
| `src/confighttp.cpp` | `getApps` 在开关打开时返回固定目录；不注册 `POST /api/apps` 与 `DELETE /api/apps/{id}` | 防止上游新增其他应用导入/创建入口；处理函数本身保留，上游测试直接调用它们。另：办公键 `desk_confirm_connections` 依赖网页「保存配置」保留未知键（已核实：`getConfig` 返回文件全部键，`Config.vue` `save()` 只删除等于标签页默认值的已知选项，`saveConfig` 写回请求中全部非空键），合并上游时若这三处改为白名单或丢弃未知键，须把该键加入网页配置 |
| `src/config.cpp`、`docs/configuration.md`、Web `config_tabs.json` | `dd_configuration_option` 默认 `ensure_active`；`lan_encryption_mode`/`wan_encryption_mode` 默认 `2`；`docs/configuration.md` 新增 `desk_confirm_connections` 条目 | 三处默认一致；不覆盖已配置的 disabled/manual 和显式加密模式 |
| `Navbar.vue`、`router.js` | 隐藏“应用”“精选”导航，两条路由重定向到首页；未使用的 `Layers`/`Star` 图标导入已删除 | 上游新增应用相关页面时同样隐藏 |
| `tests/unit/test_nvhttp_client_auth.cpp` | `OfficeCursorTlsTest` 在注册生产路由后调用虚函数 `register_test_routes(server)`（默认空），子类 `OfficeConnectRequestTlsTest` 借此在 `server->start()` 前挂上 `launch_for_test` | Simple-Web-Server 必须在 start 前注册资源；上游改 fixture 时保留该钩子 |
| 三个平台 `assets/apps.json` | 新安装 Desktop-only | 上游 config.cpp 仍仅在用户文件不存在时复制模板 |
| `src/globals.*`、`src/platform/common.h`、各平台 capture 签名及测试替身 | `display_cursor` 与 capture 的 `cursor` 参数为 `std::atomic_bool` | 上游新增的 capture 实现（本次合并新增 macOS ScreenCaptureKit 路径）和测试里的 `display_t` 替身（本次为 `tests/unit/platform/linux/test_display_geometry.cpp`、`tests/unit/platform/macos/test_display_helpers.mm`）须同步改签名，只改签名、不删测试；合并后 `grep -rn "capture(.*, bool \*" src tests` 应无结果；见下文取舍 |
| `office/display_scale.*`、`office/windows/display_scale.cpp` | 捕获 source 的真实系统 DPI、严格参数、随机目标令牌、串行读写与读回 | Windows 非公开 -3/-4 布局和 effective DPI 核验必须同时有效；未知值拒绝，不能 clamp 或改其他显示器；保留 mock/TLS 测试 |
| `office/adaptive_fec.*` | 自适应视频 FEC：`0x5502` 解析、丢包估计、纯控制器、超大帧退回下限分块、会话状态与 `desk_adaptive_fec`/`desk_adaptive_fec_max` 配置，见 [ADAPTIVE_FEC.md](ADAPTIVE_FEC.md) | 依赖 moonlight-common-c `RtpVideoQueue.c` 的报告时机与字段语义（大端）；客户端改报告条件时同步改 `observe_frame_fec_status` 与模拟器测试 |
| `office/authorization.cpp` | `office::apply_config` 末尾依次调用 `apply_clipboard_config`、`apply_adaptive_fec_config`，在上游未知键告警前消费办公配置键 | 新增办公配置键都挂在这里 |
| `office/main_window.*`、`office/windows/main_window.*` | 原生主窗口：纯逻辑（地址、设备行、名称校验、重启规则）与 Win32 窗口分离，窗口在弹窗线程运行，见 [MAIN_WINDOW.md](MAIN_WINDOW.md) | 依赖上游 `launch_ui`、`platf::restart`、`nvhttp::get_all_clients`/`get_cert_by_uuid`、`config::nvhttp.sunshine_name`、`net::map_port(nvhttp::PORT_HTTP)`；上游改这些签名或 `sunshine_name` 只在启动时读取的语义时同步调整 |
| `cmake/compile_definitions/common.cmake` | 把 office 源码加入公共构建清单（含直连授权的 8 个文件、主窗口的 4 个文件与自适应 FEC 的 2 个文件；`office/windows/*.cpp` 整体 `#ifdef _WIN32`） | Windows 以外不宣告 DPI 能力、不调用 Windows API；新符号必须完整 Doxygen 文档化 |
| `dockle.toml` | `[targets.doxygen] inputs` 逐个列入 office 源文件 | 上游已用 dockle 取代 `docs/Doxyfile`；`dockle_add_docs(docs TARGETS docs)` 未加 `ALL`，`ninja -C build` 不构建文档，上游只在 Read the Docs（`.readthedocs.yaml`）跑严格文档构建，因此 office CI 目前**不**执行 Doxygen 检查，需手动构建 `docs` 目标核对；新增 office 源文件要同时加到这里，不整目录加入以免中文 Markdown 进入文档构建 |
| `office/windows` | 复用 MSI 的干净安装包装、只写候选的配置工具 | 不擅自安装 VDD、覆盖现有配置或重启 |
| `.github/workflows/ci-windows.yml` | 去掉 ARM64 矩阵项；测试步骤 `set -euo pipefail`；上传测试 XML | 其余上游 workflow 保持原文件，见下文 |
| `.github/workflows/office-*.yml` | 办公自有 workflow | 不传发布或签名密钥，只读权限 |

`src/crypto.cpp` 的 OpenSSL 4.1 兼容补丁已由上游自行修复，本次合并后完全采用上游版本。

### cursor 为什么保持 `std::atomic_bool`

cursor 可见性由 HTTPS 处理线程写入，各平台 capture 循环在另一个线程持续读取 `*cursor`。若恢复上游 `bool *` 签名，只在写入端用 `std::atomic_ref` 或加锁并不能消除数据竞争（读端仍是普通读），而改造所有读端等于改动同样多的 capture 代码。因此保留 `std::atomic_bool` 签名，代价是每次合并要检查上游新增的 capture 实现。

### workflow 处理

上游 workflow 文件保持原样（`ci-windows.yml` 除外），通过 GitHub 的 workflow 禁用开关阻止触发型入口在本仓库运行，避免自动发布、文档站、翻译或包仓库同步。禁用状态保存在 GitHub 仓库侧，不在文件中：

- 已禁用：CodeQL、common lint、CI、CI-Release、localize、Publish Artifacts、Publish Screenshots、Build GH-Pages、Release Notifications、Top issues、Update changelog、Update docs、Update Flathub repo、Update Homebrew repo、Update pacman repo、Update Winget repo。
- 保持启用：仅 `workflow_call` 的可复用构建（CI-Alpine、Archlinux、Bundle、Cloudsmith、Copr、Flatpak、FreeBSD、Homebrew、Linux、macOS、PowerShell、Windows，以及本次合并新增的 CI-Web (called)），它们不能被独立触发；办公 workflow（Desk Windows validation、Desk upstream drift）；Dependabot Updates。
- 风险：上游新增的触发型 workflow 在合并并推送到 main 后会立即以“启用”状态生效。合并前必须审查新增 workflow（drift CI 的 summary 会列出），推送前或推送后立即用 `gh workflow disable "<名称>" -R linrong123/sunshine-desk` 禁用不需要的入口。本仓库默认分支是 `main`，上游 `push: master` 触发不会命中，但 `schedule`、`pull_request`、`release` 等会。

`office-windows.yml` 仅调用 Windows 构建，`publish_release=false`，`release_version` 跟随当前基线 tag，不传发布或签名密钥，只有 `contents: read`，工件留在本项目 Actions。首期 Windows 矩阵仅保留 AMD64；重新启用 ARM64 时恢复原矩阵项，并先验证依赖与安装包。

`office-upstream-drift.yml` 每周一和手动触发时运行：用 `git ls-remote --tags --sort=-v:refname` 取上游最新 release tag，`git merge-tree --write-tree` 模拟合并，把冲突文件和合并基线以来上游新增的 workflow 写入 job summary；有冲突则 job 失败。它只读，不推送、不写任何外部仓库。

完整 CI、测试 XML 统计与下载文件哈希见[验证记录](VALIDATION.md)。构建通过与真实主机/双端验收分开记录；不要把硬件跳过项当作已通过。

## 合并步骤

1. 查看最近一次 Desk upstream drift 的 job summary（或 `gh workflow run office-upstream-drift.yml -R linrong123/sunshine-desk` 手动运行），了解冲突文件和上游新增 workflow。
2. 工作树干净后获取上游 tag，从最新 main 创建 `chore/upstream-<tag>` 维护分支，记录新 tag 和提交。
3. `git merge --no-ff <tag>`。冲突文件以上游版本为底，按上表重新落适配点；检查认证、应用启动目录和所有新增 API 写入口，不要用大面积替换抹掉上游安全修复。子模块指针取上游，不提交本地子模块状态。
4. 审查上游新增 workflow 的触发条件，需要时在推送 main 前后立即禁用。更新 `office-windows.yml` 的 `release_version`。
5. 运行独立策略检查、Windows 配置脚本检查、完整 Windows 构建和 GoogleTest（`gh workflow run office-windows.yml --ref <分支>`）。检查 Doxygen 和 clang-format，不删除上游测试来迎合办公限制。
6. 需要时在干净 Windows VM 中完成 README 中的安装与串流验收。使用合成账户、临时证书和隔离数据，不把真实凭据作为测试夹具。
7. 更新本文件基线与适配点表、[验证记录](VALIDATION.md) 与未解决项，CI 通过后 `--no-ff` 合回 main。

## 当前不实现的功能

自建虚拟显示驱动、静默导入账户或已配对身份、禁用系统锁屏、替换控制台会话迁移、多个独立观看者的独立光标显示、远端光标形状/坐标协议均不在本轮实现范围。VDD 安装与签名验证由可信驱动发行方和 Windows 负责，服务端仅复用上游显示模式管理。

## 显示比例扩展的来源与状态

Windows 私有 DPI packet 布局与标准比例表参考 [imniko/SetDPI](https://github.com/imniko/SetDPI) 的 `DpiHelper.h` / `DpiHelper.cpp`，其 Unlicense 原文保存于 `office/licenses/SetDPI-Unlicense.txt`。此实现保留来源说明，独立补充严格边界校验、捕获目标选择、无身份令牌、effective DPI 交叉核验、配对 HTTPS 授权和失败读回策略。它没有继承参考实现的 clamp 行为。

本轮仅完成源码、独立本地 mock 测试与文档验证；完整 Windows 构建、生产 TLS 新用例和真实 DPI 双端验收在交付时仍需单独记录，不能以 mock 成功代替实机成功。禁止为了验收自动重启、注销或安装驱动，正在串流的主机应先保留当前会话。完整协议与限制见 [DISPLAY_SCALE.md](DISPLAY_SCALE.md)。
