# Sunshine Desk 首版验证记录

验证日期：2026-11-19。本记录证明 Windows AMD64 代码可构建、测试及打包通过。后续已通过独立 ZIP 目录部署至 Windows 并验证实际运行路径，详见[实机部署记录](DEPLOYMENT.md)；完整双端功能仍需分别验收。

## 构建与交付

- 仓库：[`linrong123/sunshine-desk`](https://github.com/linrong123/sunshine-desk)。
- [成功 CI：44508528599](https://github.com/linrong123/sunshine-desk/actions/runs/44508528599)，构建来源提交：[`4fa4cf2e6b9e29d398b9eabb7abf7e669ee02f44`](https://github.com/linrong123/sunshine-desk/tree/4fa4cf2e6b9e29d398b9eabb7abf7e669ee02f44)。
- 完整 C++ 编译、Doxygen 文档检查、Windows 依赖链接检查、配置工具测试、GoogleTest、MSI/ZIP 打包均通过；没有关闭 Werror、文档检查或测试失败退出。
- [下载 `build-Windows-AMD64` 工件](https://github.com/linrong123/sunshine-desk/actions/runs/44508528599/artifacts/14522309053)。工件是未签名测试构建，内部文件、MSI 身份与服务仍保持 Sunshine 兼容名称。需要按 GitHub 工件保留期限及时下载保存。

| 文件 | 字节 | SHA256 |
| --- | ---: | --- |
| `Sunshine-Windows-AMD64-installer.msi` | 34,027,943 | `d6951a16a4d2e91fcc418287101e3f7f9b25e4f30561990f5e931d0058d22244` |
| `Sunshine-Windows-AMD64-lite.zip` | 38,425,573 | `039d8244ab25110a85e3498d280d8669fe2f456367eeeeede6d6724972687176` |

实际 ZIP CRC 检查通过，包含 `Sunshine/sunshine.exe`、`Sunshine/tools/sunshinesvc.exe`，其中 `Sunshine/assets/apps.json` 已核实仅 Desktop。已核对 MSI 复合文件头；这不等于执行过安装或验证过卸载。

本轮交付另在忽略目录 `.work/ci-artifacts/44508528599/` 保存原始测试 XML、run/工件元数据、下载文件和 `receipt.json`（来源提交、逐文件哈希及测试计数）。不把二进制、真实账户或配置备份提交到仓库。

## 真实测试 XML

根据下载的 `tests-Windows-AMD64/test_results.xml` 解析：**共 644 例，639 通过、5 跳过、0 失败、0 错误、0 禁用**；测试耗时 267.645 秒。不能把跳过计入通过数量。

| 办公新增测试组 | 数量 | 已验证行为 |
| --- | ---: | --- |
| `OfficePolicy` | 3，全部通过 | 严格 cursor 参数；固定 Desktop 不读取用户应用文件；空配置 ensure_active/auto 与显式 disabled/manual 覆盖 |
| `OfficeCursorTlsTest` | 3，全部通过 | 已配对幂等 set、非法参数不改状态；未知/禁用/解除配对证书拒绝；无客户端证书和明文请求无法到达处理器 |

TLS 组使用生产服务器类型和认证回调、环回临时端口及临时合成证书，各例约 0.12 秒；没有访问真实主机账户或配对状态。

5 个跳过项：

- 1 例仅用于配置 Windows runner 托盘图标显示；此前独立托盘初始化步骤已执行通过。
- 3 例因 CI 不具备可用 NVENC、AMD VCE、Intel Quick Sync 编码器；不构成这三种真实硬件编码器的验收。
- 1 例为上游已标注的 Windows CI 外部命令 XFAIL。

## 必要修复和本地检查

- MSYS2 OpenSSL 4.1 已弃用旧 ASN.1 长度 API；按[官方接口历史](https://docs.openssl.org/4.1/man3/ASN1_STRING_length/)在 4.1 以上使用新 API，旧版保留兼容分支，未降低 Werror。本机 OpenSSL 3 的旧分支表达式也通过严格编译与长度检查。
- 根 README 办公链接改为公开仓库完整 URL，避免上游 Doxygen INPUT 外的相对链接失败。以官方校验过的 Doxygen 1.18.0 在项目隔离目录实际检查 README、源文件和办公策略头，保持文档警告为错误，零警告通过；完整 Windows CI 的原文档流程亦通过。
- 本地独立 C++ 策略测试、PowerShell 配置工具测试及安装脚本语法解析通过。配置工具验证原文件不变、其他配置保留、输出幂等、拒绝覆盖和换行注入。

## 待交接的边界

- 尚未完成真实 MSI 安装/卸载、首次账户设置与配对、真实 Moonlight Desk 串流画面、动态分辨率、锁屏/解锁后恢复、VDD 模式和实体显示器回退验收。
- 首次预检曾遇到管理命令环境无法启动；后续已恢复并验证 SSH 管理入口，按[可回滚试用方案](TRIAL.md)完成独立 ZIP 目录切换。没有解除来宾执行限制或重启 Windows，具体结果见[实机部署记录](DEPLOYMENT.md)。
- 后续测试抗挂死增强：客户端应显式设置 `timeout_connect`，CI 测试步骤宜增加总时限。当前 fixture 的服务端握手和客户端请求有超时，但沿用的同步 TLS 析构及线程 join 没有独立 deadline，不能宣称所有路径均严格有界。这是后续增强，不是本轮运行失败；本轮完整测试已正常退出通过，未改生产 TLS 析构。

## Windows文字和图标缩放扩展（2026-11-19）

- 源码 `b4684b4d5b9919db63a41bbf26f2e5cf3f82c116` 的[完整Windows CI](https://github.com/linrong123/sunshine-desk/actions/runs/44527801261)通过：严格编译、Doxygen、依赖链接、配置测试、打包、GoogleTest及工件上传。
- 下载并解析真实测试XML：**660项，655通过、5跳过、0失败、0错误**，耗时270.812秒。新增比例索引1项、策略12项、配对HTTPS真实路由3项全部通过；这些测试使用合成显示后端，不修改CI机器显示设置。
- [Windows工件](https://github.com/linrong123/sunshine-desk/actions/runs/44527801261/artifacts/14534514222)已下载并检验；使用独立ZIP，不执行MSI。`Sunshine-Windows-AMD64-lite.zip` 为38,468,154字节，SHA256 `f2ac6c7e4f081db13198c54dc47bf0c22974af32e423a88ae7b00399d550bb49`，ZIP完整性检查通过，应用目录仍只有Desktop。
- 临时原生只读探针在测试主机与Sunshine相同的控制台会话内运行，确认唯一活动source、私有DPI查询返回成功、标准比例解码与有效DPI一致。没有调用DPI写入、写注册表、重启系统或服务；临时任务与文件已撤销，系统脚本执行策略保持原值。
- 以上证明Windows原生读取和两端实现可构建、协议授权与逻辑测试通过；**没有代替真实HTTPS写入及文字图标变化的验收**。准备完成后，用户明确同意立即切换，已按[部署记录](DEPLOYMENT.md)更新运行服务并保留回退版本。详见[协议与兼容性边界](DISPLAY_SCALE.md)。
- 新服务与实际子进程路径均已核对为 `b4684b4d`；接口宣告 `OfficeCursorSet=1` 和 `OfficeDisplayScale=1`，配置备份逐文件核对通过，原配置保持不变。配套 Moonlight Desk 0.3.0 保留原有配对并重新建立串流。Windows 开机时间未变，没有注销用户。
- 自动回退任务已撤销，上传临时目录已清理，受保护的配置备份、回退脚本和旧版本仍保留。DPI 实际写入、分辨率切换及视觉效果尚未标为通过。

## 合并上游 v2026.1118.30014（2026-11-19）

分支 `chore/upstream-2026.1118`：先收窄办公差异，再以 merge 方式合入上游 tag `v2026.1118.30014`（`37ee7d549391938ab5dc468783fc6dadc23dbc7e`），子模块指针取上游。适配点与取舍见[上游合并说明](UPSTREAM.md)。

- 冲突数（`git merge-tree --write-tree --name-only` 对最新上游 release tag）：合并前 24 个文件，合并后 0（tag 已是祖先）。
- [成功 CI：44578543983](https://github.com/linrong123/sunshine-desk/actions/runs/44578543983)，来源提交 `924c682f0847199c041b55e8f030d998e2427470`：`office-policy`（独立策略检查）与 Windows AMD64 完整构建（`BUILD_WERROR=ON`）、GoogleTest、MSI/ZIP 打包均通过。
- GoogleTest：964 项，952 通过，12 跳过，0 失败。跳过项为无 GPU 的 nvenc/amdvce/quicksync 编码器、7 项 Flatpak/外部命令、托盘工作流配置和需授权的虚拟手柄，均为测试自身在缺少 GPU、Flatpak、授权或非托盘配置流程时的 `GTEST_SKIP`。
- office 测试 23 项全部通过：`OfficePolicy` 4（含新增 `ServerinfoAdvertisesOfficeCapabilities`）、`OfficeCursorTlsTest` 3、`OfficeDisplayScaleTlsTest` 3、`OfficeDisplayScaleTest` 12、`OfficeDisplayScaleIndices` 1。TLS 测试改为通过 `make_verified_https_server()` 加 `office::register_https_routes()` 挂载生产路由。
- 失败与修复：[44575226342](https://github.com/linrong123/sunshine-desk/actions/runs/44575226342)（`38d7f10a`）因上游新增测试替身 `test_display_geometry.cpp` 仍以 `bool *` 覆盖 `capture()`，在 `-Werror=overloaded-virtual` 下编译失败；`924c682f` 只改替身签名（含 macOS 同类替身），不删测试。更早的 44574570636 因文档输入变更被取消。
- 本地：`office/tests/Initialize-Office.Tests.ps1` 通过；clang-format 23.1.0 对改动的 C++ 文件无修改。本机没有 C++ 编译器，独立策略检查只在 CI 的 `office-policy` 任务中运行。
- 原始测试 XML 保存在忽略目录 `.work/ci-artifacts/44578543983/`，不提交。

未验证（不能由上述 CI 推出）：

- Linux 与 macOS 未编译：cursor 的 `std::atomic_bool` 签名改动覆盖 Linux 抓屏、macOS `display.mm` 和 macOS 测试替身，只做了代码审阅。
- 严格 Doxygen 检查未执行：上游改用 dockle，`docs` 目标不在默认构建中；`dockle.toml` 已列入 office 源文件，但没有在 CI 中构建过。
- 未部署到任何真实主机；合并后没有在实机上重新验证光标开关、DPI 调整、配对与 Desktop-only 目录。Web UI `getApps` 的 Desktop-only 分支没有单元测试。MSI 未执行安装或卸载。

## 直连授权（2026-11-19）

分支 `feat/direct-auth`：首次配对弹窗、每次连接确认（`/office/connect-request`、launch/resume 闸门、`OfficeConnectConfirm`）、托盘授权管理、加密默认 2。设计与限制见[直连授权](CONNECTION.md)。

- [成功 CI：44598281947](https://github.com/linrong123/sunshine-desk/actions/runs/44598281947)，来源提交 `b54a886f14f6011f9973c5f4394b79cea32fa278`：`office-policy` 与 Windows AMD64 完整构建（`BUILD_WERROR=ON`）、GoogleTest、MSI/ZIP 打包均通过。
- GoogleTest：996 项，984 通过，12 跳过，0 失败；跳过项与上游合并时相同。新增 32 项全部通过：`ConnectConfirmTest` 9、`ConnectConfirmState` 1、`OfficeAuthorizationTest` 6、`OfficeConfigTest` 4、`OfficeConnectRequestTlsTest` 6、`OfficeEncryptionDefaults` 1、`PromptManagerTest` 4、`PromptPin` 1；`SystemTrayTest` 改动后 11 项通过、1 项按原逻辑跳过。
- 失败与修复：[44596175477](https://github.com/linrong123/sunshine-desk/actions/runs/44596175477)（`0f361b61`）因未定义 `UNICODE` 时 `IDC_ARROW` 为窄字符资源号，`LoadCursorW(nullptr, IDC_ARROW)` 编译失败；`b54a886f` 改用 `LoadCursorA`。
- 原始测试 XML 保存在忽略目录 `.work/ci-artifacts/44598281947/`，不提交。
- 严格 Doxygen 仍未在 CI 中执行；新文件已列入 `dockle.toml` 并逐项写了文档注释。Linux 与 macOS 未编译，非 Windows 空实现只做了代码审阅。
- 本地：clang-format 23.1.0 对改动的 C++ 文件无修改；本机没有 C++ 编译器，编译与 GoogleTest 只在 CI 中运行。
- 复核后修正（`856deb20`）：身份改为按 TLS 连接绑定；超限弹窗在空出窗口后弹出；denied/expired 结果保留的 30 秒内 `start` 不再弹窗、`cancel` 不清除；托盘开关写回改为临时文件加改名。[成功 CI：44606312499](https://github.com/linrong123/sunshine-desk/actions/runs/44606312499)，来源提交 `856deb2053e034105c3f8cf63d3f9e7528f433c5`：GoogleTest 1000 项，988 通过，12 跳过（同上），0 失败；新增 `IdentityFollowsTheConnectionNotTheLatestHandshake`、`OverLimitRequestIsShownAfterAnAnswerFreesASlot`、`DeniedAndExpiredResultsActAsCooldown`、`FailedPersistLeavesTheConfigFileUnchanged` 4 项与改动的既有用例均通过。原始 XML 在 `.work/ci-artifacts/44606312499/`。

待实机验收（不能由 CI 推出）：

- 首次配对弹窗的显示、PIN 校验、允许/拒绝与超时关闭；同时 3 个以上请求的回退。
- 托盘「已授权设备」禁用/启用/删除（含确认框）与断流；「每次连接需本机确认」开关写回配置。
- 开启确认后 Moonlight Desk 发起请求、被控端允许/拒绝/超时、断线 120 秒内免确认恢复、退出应用后需重新确认；官方 Moonlight 收到 403 后在本机允许并重试。
- 锁屏或服务模式下弹窗所在会话；加密默认 2 对旧客户端的拒绝提示。

## 原生主窗口（2026-11-19）

分支 `feat/desk-ui`：托盘左键打开原生「Sunshine Desk」主窗口（本机地址、设备名称、已授权设备、确认开关、服务状态、高级设置入口、应用并重启服务）；点击配对通知打开主窗口。设计与限制见[原生主窗口](MAIN_WINDOW.md)。

- 首次使用核实结论：上游启动时不自动打开浏览器（上游提交 `a84735d1`，#5329；`src/config.cpp` 快捷方式启动路径只确保服务运行；`src/httpcommon.cpp` 无凭据时只写日志；`src/confighttp.cpp` 仅在访问网页时重定向 `/welcome`），因此只改了配对通知的去向。
- 新增 `tests/unit/test_office_main_window.cpp`（纯逻辑与单键原子写回）和 `SystemTrayTest.LeftClickOpensSunshineDeskMainWindow`。CI 结果见下方补记。
- 本机没有 C++ 编译器，编译与 GoogleTest 只在 CI 中运行；Linux 与 macOS 未编译，非 Windows 空实现只做了代码审阅。

待实机验收（不能由 CI 推出）：

- 托盘左键打开主窗口、再次左键把已有窗口带到前台（含最小化后）、右键菜单仍正常；窗口在 100%/150% 系统缩放下布局不截断。
- 本机地址：多网卡、断开网卡、VPN/Hyper-V 虚拟网卡、IPv6（含临时地址，预期会显示）下的列表与「复制地址」；非默认端口时的方括号格式。
- 设备名称：保存、确认重启后 Moonlight Desk 看到新名称；在确认框点「否」不重启；留空回落计算机名；非法字符提示。服务模式与 ZIP 控制台模式下重启都能恢复服务。
- 已授权设备禁用/启用/删除（含确认框）与断流；与托盘子菜单、确认开关两处状态同步；串流中设备的状态行。
- 点击托盘配对通知打开主窗口并同时出现配对弹窗；服务模式和锁屏时窗口所在会话。
