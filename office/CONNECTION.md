# 直连授权（服务端实现）

本文说明 Sunshine Desk 一侧的直连授权实现：首次配对弹窗、每次连接确认、托盘授权管理和默认强制加密。双端共同遵守的协议约定以 Moonlight Desk 的 [CONNECTION.md](https://github.com/linrong123/moonlight-desk/blob/main/office/CONNECTION.md) 为准，决策背景见同仓库 [ADR 0001](https://github.com/linrong123/moonlight-desk/blob/main/office/adr/0001-direct-ip-authorization.md)。本文只补充服务端的结构、时序、配置和限制。

## 模块

| 文件 | 职责 |
| --- | --- |
| `office/connect_confirm.*` | 与平台无关的确认状态机：待确认请求、结果保留、一次性许可、断线宽限。时钟可注入，便于单测 |
| `office/authorization.*` | 全局 tracker、配置开关 `desk_confirm_connections`、`/office/connect-request` 处理、launch/resume 闸门、禁用/删除客户端并断流 |
| `office/authorization_ui.*` | 弹窗规则（每类最多 3 个窗口、不重复弹出、PIN 校验、允许/拒绝分派）；非 Windows 的空实现 |
| `office/windows/authorization_ui.cpp` | Windows 弹窗线程、Win32 窗口、托盘设备列表与回调 |
| `office/tray_devices.h` | 托盘「已授权设备」子菜单存储与回调声明（仅 Windows 托盘构建） |
| `office/nvhttp_routes.*` | 注册 `^/office/connect-request$`，serverinfo 宣告 `OfficeConnectConfirm`，从请求取已验证身份 |

上游文件只有小型适配点，清单见 [UPSTREAM.md](UPSTREAM.md)。

## 首次配对弹窗

1. 上游 `/pair` 收到配对请求后把会话放进待配对列表（`nvhttp::get_pending_pairings()`），并照常发托盘通知。
2. 弹窗线程每 500 ms 轮询该列表，为新请求打开「授权请求」窗口：设备名称、来源 IP、4 位验证码输入框、允许/拒绝。
3. 允许：输入必须恰为 4 位 ASCII 数字，否则窗口提示并保持打开；通过后在短命工作线程调用 `nvhttp::pin(id, pin, name)`（它会等待配对交换，不能阻塞 UI 线程）。验证码错误时配对失败，与上游一致。
4. 拒绝或关闭窗口：调用上游 `nvhttp::cancel_pairing(id)`，配对请求立即失败，主控端不必等超时。
5. 配对会话完成、过期或被网页端处理后，下一次轮询自动关闭对应窗口。
6. 每类窗口同时最多 3 个；超出的请求先排队，有窗口关闭（被处理或请求消失）后在下一次轮询时弹出，排队期间配对请求仍可走上游托盘通知和网页 PIN 入口。同一请求只弹一次，用户处理后即使请求仍在也不会重新弹出。

## 每次连接确认

### 开关

- 配置键 `desk_confirm_connections`，取值 `enabled`/`disabled`（也接受 true/false、on/off、yes/no、1/0），默认 `disabled`；非法值记警告并按关闭处理。说明见 `docs/configuration.md`。
- 托盘勾选项「每次连接需本机确认」即时生效并写回配置文件。写回会重新生成文件：保留所有键值，但注释丢失、键按字母排序。写入先落到同目录的 `<配置文件>.office-tmp`，再改名覆盖原文件；写入或改名失败时原文件不变，托盘开关仍按新值生效到下次启动。
- 上游网页「保存配置」会原样保留 `desk_confirm_connections`：`/api/config` 返回文件中的全部键（`src/confighttp.cpp` `getConfig`），页面只删除等于各标签页默认值的已知选项（`Config.vue` `save()`），`saveConfig` 写回请求中的全部非空键。
- 生效条件：开关打开 **且** 弹窗线程在运行（`prompt_available`）。非 Windows 或弹窗线程启动失败时不生效（fail-open），serverinfo 报 `OfficeConnectConfirm=0`，`connect-request` 返回 `not_required`，避免无人能批准导致主机永久不可连。
- 关闭开关时清空所有请求、许可和宽限。

### 协议

仅注册在 GameStream HTTPS 上，使用已配对且启用的客户端证书认证；身份按连接绑定：TLS 握手成功后以客户端 TCP 端点为键记录该连接的证书（连接释放后失效），请求按自己的远端端点取回证书并重新确认客户端仍为启用状态，取不到时返回 `status_code=401`。来源地址取规范化后的远端 IP。launch/resume 也用同一身份覆盖会话记录的客户端证书与名称。

- `serverinfo`：`OfficeConnectConfirm` 为 `1`（确认生效）或 `0`。
- `GET /office/connect-request?action=start|status|cancel` 返回：

  ```xml
  <root status_code="200"><state>pending</state><timeoutSeconds>60</timeoutSeconds></root>
  ```

  - `action` 缺失、重复或非法：`status_code=400`，不改变状态。
  - 确认未生效：任何合法 action 都返回 `not_required`。
  - `start`：幂等；已有 pending/approved（含仍在串流或宽限期内）时原样返回，不重复弹窗。上次结果为 denied/expired 时，在该结果保留的 30 秒内（冷却期）原样返回 `denied`/`expired`，不建立新请求、不弹窗；冷却期过后才会建立新的 pending。每证书最多 1 个 pending；全局已有 3 个 pending 时，需要新建请求的证书得到 `status_code=503`，`state` 为 `none`。
  - `status`：只查询，不创建；无记录时为 `none`。
  - `cancel`：撤回 pending 或未使用的许可并关闭窗口，返回 `none`；冷却期内的 `denied`/`expired` 不会被清除，原样返回。
- launch/resume 闸门：确认生效且该证书没有许可、未在串流、不在宽限期时，返回 `status_code=403`、`status_message="Host approval required"`（`gamesession`/`resume` 为 0），同时自动执行一次 `start`，官方 Moonlight 用户在被控端允许后手动重试即可。

### 时序

| 项 | 时长 |
| --- | --- |
| pending 无人处理变为 `expired` | 60 秒 |
| `denied`/`expired` 结果保留供查询 | 30 秒 |
| 批准后一次性许可（绑定证书，由下一次 launch/resume 消耗） | 30 秒 |
| 准入后等待串流会话启动 | 30 秒 |
| 已批准串流断开后的宽限（同证书免确认恢复） | 120 秒 |

- 准入后 30 秒内必须有串流会话启动（`stream.cpp` 会话进入 RUNNING 时调用 `office::stream_started`），否则准入失效，失败的 launch 不会让该证书一直免确认；会话启动后准入持续到会话结束。
- 宽限从该证书的串流会话结束时开始（`office::stream_stopped`）。客户端主动 `/cancel`（退出应用）、托盘禁用或删除、关闭开关都立即取消宽限和许可。
- 已准入且会话仍在（例如旧会话尚未超时就重连）的证书再次 launch/resume 不需要重新确认；准入后 30 秒内的重试同样不需要。
- 状态机由单把互斥锁保护，每次调用先按当前时间清理过期项，没有后台定时器。

## 托盘授权管理（Windows）

- 「已授权设备」子菜单列出最多 16 个已配对设备（名称 + 状态），超出时提示「更多设备请在主窗口管理」。托盘左键打开的[原生主窗口](MAIN_WINDOW.md)列出全部设备，操作与托盘共用同一组函数（`office::request_client_access` / `request_delete_client` / `request_confirm_connections`），确认开关两处状态同步。弹窗线程每 500 ms 比对设备列表和开关状态，有变化才重建菜单。
- 每项可「禁用/启用」和「删除授权」（删除前弹确认框）。复用上游 `nvhttp::set_client_enabled` / `nvhttp::unpair_client`；禁用和删除都会结束该证书的串流，若已无其他串流则结束 Desktop 应用，并撤销该证书的许可与宽限。
- 菜单回调把会阻塞的操作（改配对文件、确认框）放到短命工作线程，不占用托盘线程。

## 加密默认值

`lan_encryption_mode` 与 `wan_encryption_mode` 未显式配置时默认 `2`（强制加密），`config.cpp`、`docs/configuration.md` 和 Web `config_tabs.json` 三处一致；已有显式配置优先。不支持加密的旧客户端会被拒绝，这是有意的取舍。

## 线程模型

- 弹窗线程：`main.cpp` 构造 `office::authorization_ui_t` 时启动，拥有全部弹窗窗口与 500 ms 定时器，析构时投递 `WM_QUIT` 并 join。`prompt_manager_t` 只在该线程调用，不加锁。
- HTTPS 处理线程：读写确认状态机（内部加锁），不直接操作窗口；窗口通过下一次轮询与状态同步。
- 托盘线程：只读写托盘菜单存储和设备槽（各自加锁），不等待网络或文件 I/O。

## 已知限制

- 弹窗与托盘管理只在 Windows 实现；其他平台编译为空实现，仍走上游网页 PIN 流程，确认开关不生效。
- Windows 锁屏或安全桌面时弹窗不可见，请求按 60 秒超时处理；本软件不绕过 Windows 登录。服务模式下弹窗显示在 Sunshine 进程所在的交互会话。
- 关闭窗口按拒绝处理。
- 网页端禁用或取消配对不会清理该证书的许可和宽限，但 TLS 校验已拒绝该证书，残留状态无法被使用，并在到期后清除。
- 请求身份按连接绑定（见上），不受其他客户端并发握手影响。上游自身的 pair 等路径和 `make_launch_session` 仍使用上游「最后一次 TLS 校验的证书」全局变量；办公版只在 launch/resume 里用按连接身份覆盖它，若按连接查不到（理论上不会发生）则保留上游行为，该竞态只涉及已配对且启用的设备。
- `/cancel` 按上游语义结束所有会话，但只取消调用者自己的宽限。
- 许可在闸门处即被消耗并转为 30 秒准入；launch 失败后 30 秒内可直接重试，超过后需要重新确认。
- 同一证书同时有多个会话时，任一会话结束即进入宽限计时。
- 托盘开关写回配置会丢失注释并排序键。
- 网页配置页在打开时读取配置；若打开页面后又在托盘切换了开关，再在网页点「保存」会写回页面里的旧值（重启后生效）。上游 `saveConfig` 本身不是原子写入。

## 验证

- `tests/unit/test_office_authorization.cpp`：状态机（假时钟覆盖幂等、许可 30 秒、pending 60 秒与结果保留、denied/expired 冷却、全局上限、宽限的建立与取消）、协议处理、闸门、弹窗规则（假窗口后端，含超限请求在空出窗口后弹出）、PIN 校验、配置解析与原子写回（失败时原文件不变）、加密默认值。
- `tests/unit/test_nvhttp_client_auth.cpp` 的 `OfficeConnectRequestTlsTest`：经生产 TLS 服务与认证回调验证非法 action、身份跟随连接（两台设备交错握手后复用的旧连接仍识别为原设备，禁用后取不到身份）、start/status/cancel 使用已验证身份与冷却、未生效时 `not_required`、未知/禁用/无证书/明文拒绝、无许可 launch 返回 403 并自动发起请求、禁用/删除客户端撤销许可。
- 真实弹窗、托盘操作和双端联调需要实机验收，结果记录在 [VALIDATION.md](VALIDATION.md)。
