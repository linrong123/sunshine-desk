# 架构与上游合并维护

## 基线

- 上游仓库：`SunshineStream/Sunshine`，通过 `upstream` remote 跟踪。
- 初始 tag：`v2026.914.233613`。
- 初始提交：`63d35f702ee9e362e43263742981836ec0710384`。
- 独立服务端：`linrong123/sunshine-desk`；配套主控端：`linrong123/moonlight-desk`。
- 保留完整历史；办公新增代码与文档放在 `office/`，不复制一套编码器、网络协议或驱动栈。

## 边界与补丁位置

| 位置 | 办公差异 | 合并时检查 |
| --- | --- | --- |
| `src/crypto.cpp` 与加密测试 | OpenSSL 4.1 使用新 ASN.1 length API，旧版本保留原 API | 在最新 MSYS2 与旧版 OpenSSL 都保留编译兼容；不降低 Werror |
| `office/policy.h` | 固定 Desktop JSON、严格 cursor 参数校验 | 保持独立，不引入主机私有配置 |
| `src/process.*` | parse 增加可选固定目录参数，refresh 强制办公目录 | 检查所有生产初始化和重新加载入口仍走 refresh；旧文件不被改写 |
| `src/config.cpp` 与 Web config 默认值 | 默认 ensure_active，分辨率/刷新率 auto；配置解析仍尊重显式值 | 检查 C++ 与 Web 默认一致，不覆盖已配置 disabled/manual |
| 三个平台 `assets/apps.json` | 新安装 Desktop-only | 上游 config.cpp 仍仅在用户文件不存在时复制模板 |
| `src/confighttp.cpp` 与 Navbar.vue | 移除应用增删路由/应用精选页面导航，读取固定目录 | 防止上游新增其他应用导入/创建入口；原文件不删 |
| `src/nvhttp.cpp` | capability、仅已认证 HTTPS cursor set 与显示比例 GET/POST | 不注册到明文 HTTP；保留现有配对和禁用客户端验证 |
| `office/display_scale.*`、`office/windows/display_scale.cpp` | 捕获 source 的真实系统 DPI、严格参数、随机目标令牌、串行读写与读回 | Windows 非公开 -3/-4 布局和 effective DPI 核验必须同时有效；未知值拒绝，不能 clamp 或改其他显示器；保留 mock/TLS 测试 |
| `docs/Doxyfile`、公共源码构建清单 | 将 office 源码纳入编译与严格文档检查 | Windows 以外不宣告能力、不调用 Windows API；新符号必须完整文档化 |
| `src/globals.*`、平台 capture 签名 | bool 改为 atomic_bool | 所有覆盖方法一致，读线程不再访问非原子状态 |
| `office/windows` | 复用 MSI 的干净安装包装、只写候选的配置工具 | 不擅自安装 VDD、覆盖现有配置或重启 |
| `.github/workflows` | 上游自动入口增加源仓库条件；独立 office Windows 验证 | 新增上游 workflow 也必须审查，防自动发布/同步/写外部仓库 |

原上游自动入口的每个 job 增加仓库条件，防止独立项目首次 push 或手动运行触发上游发布、文档站、翻译或包仓库同步。可复用 workflow_call 构建文件保留；office 工作流仅调用 Windows 构建，`publish_release=false`，不传发布或签名密钥。仅写 `contents: read`，工件留在本项目 Actions。

首期 Windows 矩阵仅保留 AMD64，减少首次构建耗时；重新启用 ARM64 时恢复原矩阵项，并先验证依赖与安装包。

当前首版 Windows AMD64 构建来源为 `4fa4cf2e`，完整 CI、测试 XML 统计与下载文件哈希见[验证记录](VALIDATION.md)。构建通过与真实主机/双端验收分开记录；不要把硬件跳过项当作已通过。

## 合并步骤

1. 工作树干净后获取上游 tag，创建维护分支，记录新 tag 和提交。
2. 合并上游，按上表解决冲突，尤其检查认证、应用启动目录和所有新增 API 写入口。不要用大面积替换抹掉上游安全修复。
3. 运行独立策略检查、Windows 配置脚本检查、完整 Windows 构建和 GoogleTest。检查 Doxygen 和格式，不删除上游测试来迎合办公限制。
4. 在干净 Windows VM 中完成 README 中的安装与串流验收。使用合成账户、临时证书和隔离数据，不把真实凭据作为测试夹具。
5. 更新本文件基线、验收结果与未解决项。对比上游工作流新增文件，确认无自动发布或外部同步权限。

## 当前不实现的功能

自建虚拟显示驱动、静默导入账户或已配对身份、禁用系统锁屏、替换控制台会话迁移、多个独立观看者的独立光标显示、远端光标形状/坐标协议均不在本轮实现范围。VDD 安装与签名验证由可信驱动发行方和 Windows 负责，服务端仅复用上游显示模式管理。

## 显示比例扩展的来源与状态

Windows 私有 DPI packet 布局与标准比例表参考 [imniko/SetDPI](https://github.com/imniko/SetDPI) 的 `DpiHelper.h` / `DpiHelper.cpp`，其 Unlicense 原文保存于 `office/licenses/SetDPI-Unlicense.txt`。此实现保留来源说明，独立补充严格边界校验、捕获目标选择、无身份令牌、effective DPI 交叉核验、配对 HTTPS 授权和失败读回策略。它没有继承参考实现的 clamp 行为。

本轮仅完成源码、独立本地 mock 测试与文档验证；完整 Windows 构建、生产 TLS 新用例和真实 DPI 双端验收在交付时仍需单独记录，不能以 mock 成功代替实机成功。禁止为了验收自动重启、注销或安装驱动，正在串流的主机应先保留当前会话。完整协议与限制见 [DISPLAY_SCALE.md](DISPLAY_SCALE.md)。
