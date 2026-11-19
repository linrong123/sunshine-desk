# 初版验证记录

验证日期：2026-11-19。以下记录区分本机检查与待验证项，不能代替 Windows 端到端验收。

## 已通过

- Apple clang 21.0.0，C++17，`-Wall -Wextra -Werror` 编译并运行独立 cursor 策略测试：缺失、空值、非法值和重复参数拒绝；0/1 接受；重复隐藏保持隐藏。
- 在项目内部隔离目录运行 PowerShell 7.5.0 的配置工具测试：保留原配置、保留其他设置、auto 替换、输出幂等、拒绝覆盖、拒绝显示器标识换行。使用临时合成文件，未触碰真实配置。
- 安装包装脚本通过 PowerShell 语法解析；未调用 Windows 安装器。
- 三平台初始 apps.json 解析有效，均仅 Desktop。
- 所有工作流 YAML 解析有效；上游自动入口每个 job 均带源仓库条件。独立办公 CI 仅请求读取权限，不传发布和签名密钥。
- 已核对 C++ 与 Web 显示配置默认值均为 ensure_active，分辨率/刷新率沿用上游 auto；显式配置仍由原解析流程覆盖默认值。新增 GoogleTest 覆盖空配置和显式覆盖，待 Windows CI 运行。
- 修改文件按仓库 clang-format 格式化；`git diff --check` 通过。

## 待验证

- 新增 OfficeCursorTlsTest 复用生产 TLS 认证路径，自动验证证书授权与 cursor 幂等行为；尚待下次 CI 执行，不作为真实 Moonlight 连接验收。
- Sunshine 完整构建与新增/原有 GoogleTest；Windows AMD64 MSI 和 ZIP 工件是否生成。
- MSI 干净安装/卸载、服务启动、首次账户设置、PIN 配对。
- TLS 无证书、未配对证书、禁用客户端证书、明文 HTTP 对 cursor 路由的拒绝；已配对客户端设置及画面观察。
- Windows Desktop Duplication 下连续显隐、重连、锁屏/解锁后恢复、不同会话边界。
- 指定 signed VDD 的来源、签名、安装、支持模式、动态分辨率/刷新率和恢复表现。当前不自动下载/安装或推荐特定驱动。

开发环境仅增加忽略目录中的临时工具，未替换正在运行的 Sunshine，未修改 Windows 或 Mac 配置，未重启。没有发行版签名或无人值守安装验收结论。

## 首次 Windows CI 问题修复

首次完整构建发现 MSYS2 已提供 OpenSSL 4.1，上游 ASN1_STRING_length 已弃用且 Werror 阻止编译。按 OpenSSL 官方接口历史在 4.1 以上使用 ASN1_STRING_get_length，旧版本保留原调用；同步调整证书公共名测试。不关闭警告、不跳过加密或授权测试。修复后的完整结果待重跑确认。
