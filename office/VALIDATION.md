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
