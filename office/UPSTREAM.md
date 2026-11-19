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
| `src/nvhttp.cpp` | 3 处：`#include "office/nvhttp_routes.h"`；`serverinfo` 中调用 `office::add_serverinfo_capabilities(tree)`；`start()` 注册完上游 HTTPS 路由后调用 `office::register_https_routes(https_server)`；另在 `test_support` 末尾有仅测试用的 `make_verified_https_server` | 只注册到 HTTPS 服务器，绝不注册到明文 HTTP；测试工厂的校验逻辑须与 `start()` 中上游 verify lambda 保持一致（证书链校验 + 客户端启用检查 + 401 响应） |
| `office/nvhttp_routes.*` | 办公 HTTPS 扩展：路由表（cursor、display-scale GET/POST）与 serverinfo capability（`OfficeCursorSet`，Windows 上 `OfficeDisplayScale`） | 新增已认证扩展只改这里的路由表；处理函数签名跟随 `SimpleWeb::ServerBase<nvhttp::SunshineHTTPS>` |
| `office/policy.h` | 固定 Desktop JSON、严格 cursor 参数校验、`desktop_catalog_only` 开关 | 保持独立，不引入主机私有配置 |
| `src/main.cpp` | 首次 `proc::refresh` 前置 `office::desktop_catalog_only = true` | 确认仍在首次 refresh 之前 |
| `src/process.cpp` | `parse` 在开关打开时读取固定 Desktop 目录而非 apps 文件；函数签名与上游一致 | 检查所有生产加载入口仍经过 `parse`；单元测试中开关默认关闭，上游 apps 文件测试照常运行 |
| `src/confighttp.cpp` | `getApps` 在开关打开时返回固定目录；不注册 `POST /api/apps` 与 `DELETE /api/apps/{id}` | 防止上游新增其他应用导入/创建入口；处理函数本身保留，上游测试直接调用它们 |
| `src/config.cpp`、`docs/configuration.md`、Web `config_tabs.json` | `dd_configuration_option` 默认 `ensure_active` | 三处默认一致；不覆盖已配置的 disabled/manual |
| `Navbar.vue`、`router.js` | 隐藏“应用”“精选”导航，两条路由重定向到首页 | 上游新增应用相关页面时同样隐藏 |
| 三个平台 `assets/apps.json` | 新安装 Desktop-only | 上游 config.cpp 仍仅在用户文件不存在时复制模板 |
| `src/globals.*`、`src/platform/common.h`、各平台 capture 签名及测试替身 | `display_cursor` 与 capture 的 `cursor` 参数为 `std::atomic_bool` | 上游新增的 capture 实现（本次合并新增 macOS ScreenCaptureKit 路径）和测试里的 `display_t` 替身（本次为 `tests/unit/platform/linux/test_display_geometry.cpp`、`tests/unit/platform/macos/test_display_helpers.mm`）须同步改签名，只改签名、不删测试；合并后 `grep -rn "capture(.*, bool \*" src tests` 应无结果；见下文取舍 |
| `office/display_scale.*`、`office/windows/display_scale.cpp` | 捕获 source 的真实系统 DPI、严格参数、随机目标令牌、串行读写与读回 | Windows 非公开 -3/-4 布局和 effective DPI 核验必须同时有效；未知值拒绝，不能 clamp 或改其他显示器；保留 mock/TLS 测试 |
| `cmake/compile_definitions/common.cmake` | 把 office 源码加入公共构建清单 | Windows 以外不宣告 DPI 能力、不调用 Windows API；新符号必须完整 Doxygen 文档化 |
| `dockle.toml` | `[targets.doxygen] inputs` 逐个列入 office 源文件 | 上游已用 dockle 取代 `docs/Doxyfile`，严格模式（警告即失败）随 `BUILD_DOCS=ON` 在 Windows CI 运行；新增 office 源文件要同时加到这里，不整目录加入以免中文 Markdown 进入文档构建 |
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
