# Sunshine Desk 服务端

这是基于 Sunshine 的公开独立办公版，定位为 Windows 被控端、单一 Desktop 串流。上游源码与历史保留，办公策略集中在本目录，尽量复用安装器、证书配对、编码器、显示设备管理和 Windows 服务。

项目现名为 Sunshine Desk，配套客户端为 Moonlight Desk。`office/` 源码目录、`OfficeCursorSet` 和 `office/cursor` 协议名保持兼容；本次更名不迁移服务或配置，也不改变安装器身份。

Windows AMD64 已由[完整 CI](https://github.com/linrong123/sunshine-desk/actions/runs/44508528599)完成构建、测试及 MSI/ZIP 打包：639 通过、5 跳过、0 失败。尚未完成真实机器安装、发行版签名和双端连接验收，不能称为完整无人值守交付。现有 MSI 产品标识、服务名和默认配置位置沿用上游，因此不能与官方版并存；请使用全新测试机，不在生产安装上覆盖试用。

## 已实现的行为

- 新安装所有平台的默认应用模板仅含 Desktop。
- 运行时 `proc::refresh()` 固定加载内存中的 Desktop 目录，GameStream 应用列表与启动仅使用该目录。旧应用文件不会被读取为可启动目录，也不会被删除或覆盖。直接编辑文件也不会恢复其他应用。
- Web 应用与精选导航、对应页面入口，以及应用新增和删除 API 路由移除。只读应用 API 返回固定 Desktop。上游相关源文件和处理函数保留，便于合并；旧接口调用不会添加应用。
- 新安装及未显式配置的 Windows 显示设备策略默认为 `ensure_active`，分辨率和刷新率沿用 `auto`，客户端连接内调整后可直接按协商值重连应用。已有明确的 disabled/manual 等设置仍优先，不覆盖用户配置，不关闭其他实体显示器。
- 其他管理功能、首次账户设置和 PIN 配对保留。客户端断线恢复不由服务端改写会话迁移实现代替。

此产品限制不是对拥有系统管理员权限者的隔离措施。保留的管理配置、全局准备命令和主机操作权限仍是受信任的管理功能。

## Windows 安装与初始化

1. 在独立干净 Windows 测试机上，从本项目 Windows CI 的 `build-Windows-AMD64` 工件取得 MSI。核对构建提交与 SHA256。CI 只上传工件，不发布发行版，不使用上游签名密钥。
2. 可运行 `office/windows/Install-Office.ps1 -InstallerPath <MSI文件> -ExpectedSha256 <经核对的哈希>`，先加 `-WhatIf` 查看动作。脚本检测已有 Sunshine 服务或进程即拒绝，不停止服务、不重启。安装包沿用上游 MSI 行为；这不是新的安装器。
3. 启动后在本机浏览器打开 Sunshine 管理页面，按原流程设置唯一管理账户，再用 Moonlight Desk 发起 PIN 配对并在服务端批准。不要把账户、PIN、证书私钥或已配对状态放入仓库、日志附件或共享安装包。不得共享其他用户的配置目录。
4. 若需要无头显示器，采用可信来源发布且具有有效 Windows 驱动签名的 VDD 安装包，按其安装说明安装；由 Windows 校验签名。不要启用测试签名、关闭签名校验或把未知 INF 加入驱动库。本项目暂不绑定、下载、打包或自动安装某个 VDD，也没有验证具体型号兼容性。先用实体显示器完成基本验收，再加入 VDD。
5. 新安装无需运行额外初始化脚本即可启用自动分辨率。仅在需要选定目标显示器或检查已有配置时，用上游 `dxgi-info.exe` 确认目标显示器标识。运行 `office/windows/Initialize-Office.ps1 -ConfigPath <现有配置> -OutputPath <新的候选文件> -DisplayId <显示器标识>`。不指定 ConfigPath 时生成最小候选配置；不指定 DisplayId 时保留原 output_name。工具只生成新文件，拒绝覆盖，不读取或更改 apps.json、配对和服务。
6. 可选候选配置将 `dd_configuration_option` 设为 `ensure_active`，将分辨率和刷新率设为 `auto`。复核候选后，在停止测试机串流时备份原配置到本机私有目录，再由管理员按上游配置流程应用。不要在仓库保存备份。工具不自动重启服务。
7. Moonlight 必须启用上游要求的“优化游戏设置”协商选项，并发送期望分辨率。Windows 显示器/VDD 必须支持该模式；auto 不会创造未支持模式。默认选择 ensure_active，避免主动关闭其他本地显示器。

后续若要覆盖已有官方安装，必须另行设计私有备份、恢复及 MSI 升级测试。目前安装脚本有意只接收干净测试机，不提供原地迁移。

## 光标协议

`GET /office/cursor?visible=0|1` 仅注册在现有 GameStream HTTPS 服务上，使用现有已配对且启用的客户端证书校验。不新增端口，不在明文 HTTP 上注册，不复用管理员密码或额外令牌。

`serverinfo` 返回 `OfficeCursorSet=1` 表示该扩展存在。成功示例：

```xml
<root status_code="200"><cursorVisible>0</cursorVisible></root>
```

必须恰有一个 visible，值只能是字符串 `0` 或 `1`；缺失、重复、空值和其他值返回 GameStream XML `status_code=400`，不改变状态。XML 状态码沿用上游惯例，客户端必须读取 XML，不能仅凭 HTTP 200 判断成功。未配对客户端沿用现有授权失败行为；没有客户端证书可能在 TLS 层被拒绝。

接口是幂等 set，不是 toggle。控制的是全局串流画面中的光标合成开关，所有观看者共享；不隐藏主机本地 Windows 指针，不控制客户端本地指针，不同步形状或坐标。默认启动为显示，状态不写入配置；传统快捷键仍可改变同一个状态。`std::atomic_bool` 保证跨请求、输入和采集线程访问安全。

首期支持目标为 Windows Desktop Duplication。其他平台采集后端保留原行为，部分后端并不支持动态光标合成，不应将“接口存在”理解为所有平台均有视觉保证。旧服务器无 capability 或返回 404 时，客户端只提示不支持远端光标控制，不模拟 toggle、不阻止串流。并发配对客户端的最后一次写入生效；单客户端也应串行发送设置请求，避免网络乱序造成旧状态后到。

## 验证与交接

- 本地独立策略测试：`clang++ -std=c++17 -Wall -Wextra -Werror -I. office/tests/policy.cpp -o <临时测试程序>`，运行生成程序。
- `OfficeCursorTlsTest` 通过生产 TLS 服务器与认证回调测试已配对幂等 set、未知/禁用/解除配对拒绝、无证书及明文拒绝；使用环回临时端口和临时合成证书，不启动真实主机服务。
- 完整 GoogleTest 新增 `tests/unit/test_office.cpp`，检查严格参数及固定运行目录。
- Windows 初始化脚本测试：`pwsh -File office/tests/Initialize-Office.Tests.ps1`，只使用临时目录和合成配置，验证原文件不变、保留其他设置、候选幂等、拒绝覆盖和换行注入。
- `.github/workflows/office-windows.yml` 在 main 的相关修改或手动运行时，复用上游 Windows AMD64 编译、MSI/ZIP 打包与全套测试；不执行生产安装。其工件是未签名测试构建，不是经过验收的发行版。
- 仍需实机验收：MSI 干净安装/卸载、首次账户与配对、真实 Moonlight Desk 的权限拒绝和 cursor 画面行为、动态分辨率、VDD 模式切换/恢复、锁屏与解锁后的客户端恢复、实体显示器回退。环回 TLS 测试已通过，不能代替这些双端验收。

详见[验证结果和工件哈希](VALIDATION.md)、[架构与上游维护](UPSTREAM.md)和[可回滚试用方案](TRIAL.md)。
