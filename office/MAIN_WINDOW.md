# Sunshine Desk 原生主窗口（Windows）

## 背景

办公场景下被控端的日常操作只有几件事：告诉主控端连哪个地址、改设备名称、管理已授权设备、开关「每次连接需本机确认」、看服务是否在串流。上游把这些放在网页管理界面里，需要先设置网页账户、用浏览器登录，对不熟悉的用户门槛偏高。托盘菜单只能放开关和少量设备，看不到本机地址。

## 目标与非目标

目标：

- 托盘图标左键打开原生「Sunshine Desk」主窗口，单实例，再次打开时把已有窗口带到前台；右键菜单不变。
- 日常使用不需要网页账户：配对请求有原生弹窗（见 [CONNECTION.md](CONNECTION.md)），其余日常设置都在主窗口完成。
- 其他平台照常编译，行为与上游一致。

非目标：

- 不替代网页高级设置（编码器、显示设备、音频等），主窗口提供「打开高级设置（网页）」入口。
- 不显示「最后连接时间」：服务端没有记录这项数据，不编造。
- 不做 IPv6 临时地址过滤（见已知限制），不修改系统网络或防火墙。

## 窗口内容

| 区域 | 内容与行为 |
| --- | --- |
| 本机地址 | `GetAdaptersAddresses` 枚举的单播地址，只保留可连接的（见下）。IPv4 在前，IPv6 在后，同族内按网卡顺序。每行先是连接文本、后跟网卡名；「复制地址」复制选中项（未选时复制第一项）的连接文本到剪贴板，按钮短暂显示「已复制」 |
| 端口提示 | 默认端口 47989 时提示直接输入地址；非默认端口时提示连同端口输入（IPv4 `a.b.c.d:port`，IPv6 `[v6]:port`）。同时列出防火墙需放行的端口：TCP 基准−5、基准、基准+21，UDP 基准+9 至 +11 |
| 设备名称 | 编辑框显示配置文件中的 `sunshine_name`；未设置时编辑框为空，提示「留空时使用计算机名「…」」——与上游默认值一致（`src/config.cpp` 中 `sunshine_name` 默认取 `platf::get_host_name()`）。只有输入内容与已保存的值不同才写回配置，打开窗口后直接保存不会把运行中的名称写进配置。读取配置完成前编辑框和按钮禁用。按钮按情况显示「保存」（只写配置，名称与运行中一致，无需重启）或「应用并重启服务」 |
| 已授权设备 | 名称 +「（已禁用）」+「· 串流中」。选中后可「禁用/启用」和「删除授权」（删除前确认框，文字与托盘一致）。名称为空或 `-` 时显示「设备 N」 |
| 每次连接需本机确认 | 勾选框，与托盘勾选项同一份状态（`office::confirm_connections`），切换时与托盘一样即时生效并原子写回配置 |
| 服务状态 | 「服务运行中 · 当前无串流」或「服务运行中 · 正在串流：设备A、设备B、其他会话 N 个」（其他会话指无法对应到已配对设备的串流，如未配对测试会话） |
| 底部按钮 | 「打开高级设置（网页）」调用上游 `launch_ui()`；「关闭」关闭窗口（Esc 同效） |

可连接地址的筛选（`office::connectable_addresses`）：丢弃未连接的网卡（`OperStatus != IfOperStatusUp`）、隧道与软件环回网卡、环回、未指定、组播、链路本地（IPv4 169.254/16、IPv6 fe80::/10）、IPv4 映射的 IPv6 地址，以及 DAD 状态不是 `IpDadStatePreferred` 的地址（试探中、已弃用、重复）。地址去掉 `%zone` 后按规范文本去重。

## 方案

### 代码结构

| 文件 | 内容 |
| --- | --- |
| `office/main_window.h/.cpp` | 与平台无关的纯逻辑：地址筛选与格式化、端口提示、设备行与服务状态文本、设备名称校验、重启规则、配对通知去向；`open_main_window()`（非 Windows 恒为 `false`）、`open_desk_ui()`、`open_pin_ui()` |
| `office/windows/main_window.h/.cpp` | Win32 窗口 `main_window_t`（pimpl）与托盘、主窗口共用的设备操作 `request_client_access` / `request_delete_client` / `request_confirm_connections`；整体 `#ifdef _WIN32` |
| `office/windows/authorization_ui.cpp` | 弹窗线程里构造 `main_window_t`，500 ms 定时器同时驱动弹窗和主窗口刷新；收到 `WM_APP + 1` 时 `show()`；实现 `open_main_window()`（向弹窗线程 `PostThreadMessageW`）和托盘左键回调 `tray_open_main_window_cb` |
| `office/authorization.h/.cpp` | 新增 `persist_config_value` / `read_config_value`（通用的单键原子写回，进程内加锁，`set_confirm_connections` 改用它）和 `streaming_certs`（`stream_started`/`stream_stopped` 维护的按证书计数） |

### 线程模型

- 窗口只在弹窗线程（`office::authorization_ui_t` 启动的线程）创建和操作。其他线程（托盘线程、通知回调）只调用 `open_main_window()` 投递线程消息，弹窗线程未运行时返回 `false`，调用方退回 `launch_ui()`。
- 会阻塞的操作都放在短命工作线程：确认框、改配对文件、写配置、`platf::restart()`、打开浏览器。窗口不等待它们，结果通过下一次刷新体现。
- 刷新节奏（弹窗线程 500 ms 定时器，仅窗口打开时）：设备列表、确认开关和服务状态每次比对，有变化才改控件；每 2 秒发起一次设备名称重读（读配置文件 + 计算机名），每 5 秒发起一次网卡地址枚举（`GetAdaptersAddresses`）。
- 读配置和枚举网卡不在弹窗线程执行：由短命工作线程完成，结果放进窗口与工作线程共享的 `refresh_mailbox_t`（`shared_ptr` + 互斥锁），再 `PostMessageW(WM_APP + 2)` 通知窗口，窗口在弹窗线程取走结果更新控件。同类任务未结束时不再发起新任务；窗口销毁时（`WM_DESTROY`）在锁内清空目标句柄，之后完成的任务只丢弃结果。每次打开窗口新建一个 mailbox，旧窗口的迟到结果不会串到新窗口。
- 设备名称应用期间用原子标志防止重复提交；重启路径上标志保持置位直到进程退出。

### 设备名称与重启

- 校验（`normalize_device_name`）：去掉首尾空格和制表符；拒绝控制字符、`#`、以 `[` 开头、超过 64 个 Unicode 字符的名称——上游配置文件格式无法安全表达这些内容。超长名称直接拒绝、不截断，因此不会切断多字节字符。
- 长度上限按 Unicode 字符而不按 UTF-8 字节：已核实 `sunshine_name` 在上游只用于 serverinfo 的 `root.hostname`（`src/nvhttp.cpp` `tree.put("root.hostname", ...)`），不用作 mDNS/DNS-SD 实例名；mDNS 实例名取计算机名 `platf::get_host_name()`（`src/platform/windows/publish.cpp`、`src/platform/linux/publish.cpp`），由 `net::mdns_instance_name`（`src/network.cpp`）按 RFC 6763 截到 63 字节并在首个非字母数字字符处截断；macOS 注册时名称传空、由系统决定。上游网页配置页对 `sunshine_name` 也没有长度限制。因此 RFC 6763 的 63 字节上限不适用于设备名称，64 个字符只是本窗口为显示设定的上限。
- `sunshine_name` 只在启动时读取（`restart_required`），因此名称与运行中不同时需要重启。确认框说明串流会中断；确认后先原子写回配置（留空则删除该键，回落到计算机名），再调用上游 `platf::restart()`。写回失败时弹错误框，不重启。
- 已保存但尚未重启（例如在确认框点了「否」之后又从网页保存了名称）时，提示「已保存，重启服务后生效」，按钮可直接重启。
- `desk_confirm_connections` 运行时生效，不需要重启。

### 配置写回

`persist_config_value` 重新生成整个配置文件：保留全部键值（含上游未知键），注释丢失、键按字母排序；先写同目录临时文件 `<配置文件>.office-tmp.<进程随机令牌>-<序号>` 再改名覆盖，失败时原文件不变。与托盘开关同一实现。

并发：整个「读取—修改—写回」过程持有进程级互斥锁（`read_config_value` 也持有同一把锁），托盘开关、主窗口改名、主窗口开关同时写不会互相覆盖；`set_confirm_connections` 在锁内同时更新内存开关与配置文件，两次快速切换后内存与文件一致。临时文件名每次写入唯一，并发或多进程写不会争用同一个临时文件。

限制：上游网页的 `saveConfig`（`src/confighttp.cpp`）直接整文件写回，不经过这把锁，也不感知办公版的写入；网页保存与托盘/主窗口写入同时发生时，后写者覆盖先写者。

### 首次使用不需要网页账户

已核实上游不会在启动时自动打开浏览器：上游提交 `a84735d1`（#5329）已移除启动时自动打开网页；`src/config.cpp` 的快捷方式启动路径只确保服务运行；`src/httpcommon.cpp` 在没有网页凭据时只写日志；`src/confighttp.cpp` 仅在有人访问网页时才重定向到 `/welcome`；安装脚本 `sunshine-setup.ps1` 只打开文档。

因此只做了一处最小适配：点击托盘「配对请求」通知时，Windows 上弹窗线程运行则打开主窗口（配对弹窗本身由弹窗线程自动弹出），否则仍打开上游网页 PIN 页（`office::open_pin_ui`）。网页账户在需要高级设置时再创建。

## 取舍

- 用托盘左键而不是新增菜单项：不改变上游菜单下标，既有托盘测试不受影响；右键菜单保留全部入口。
- 原生 Win32 而不是嵌入网页：不引入新依赖，不需要网页账户，和配对弹窗共用线程、字体与样式。
- 地址列表不显示「推荐」地址：多网卡时无法可靠判断主控端所在网段，全部列出由用户选择。
- 删除授权的确认框沿用托盘文字，两处行为一致。

## 已知限制

- 仅 Windows。其他平台 `open_main_window()` 恒为 `false`，托盘与通知行为同上游。
- IPv6 临时（隐私）地址无法可靠区分：`IpSuffixOriginRandom` 既可能是临时地址，也可能是启用 `RandomizeIdentifiers` 后随机化的稳定地址，微软文档没有给出区分方法，因此不按后缀来源过滤，临时地址会出现在列表中；它们会定期更换，主控端应优先使用 IPv4 或稳定地址。已弃用的临时地址按 DAD 状态排除。
- 列出的是本机地址，不代表路由器、防火墙或 NAT 之后可达；端口提示只做说明，不检测防火墙。
- 服务模式下窗口显示在 Sunshine 进程所在的交互会话；锁屏或安全桌面时不可见。
- `platf::restart()` 沿用上游语义：作为服务运行时退出后由服务重新拉起；带控制台运行时在退出时自启新进程；既不是服务也没有控制台时只会退出、不会自动重启（与网页「重启」按钮相同）。
- 设备名称写回会丢失配置文件注释并排序键；网页配置页打开期间在主窗口改了设置，再在网页「保存」会写回页面里的旧值；网页保存不经过进程内配置锁（见「配置写回」）。
- 窗口固定尺寸（96 DPI 下客户区 480×484，按系统 DPI 缩放），设备多时列表滚动。

## 验证

- `tests/unit/test_office_main_window.cpp`：地址格式化与筛选（环回、未连接、隧道、未就绪、169.254、fe80、未指定、IPv4 映射、非法文本、zone、去重、IPv4 优先、非默认端口方括号）、端口提示、设备行与标签、服务状态、名称校验（含 64 字符边界）、重启规则、配对通知去向、串流证书计数、单键原子写回（保留其他键、删除键、不残留临时文件）、名称写入读回一致（含 `=`、引号、中文、首尾空白）。
- `tests/unit/test_office_authorization.cpp`：`ConcurrentWritesKeepEveryUpdate`（8 个线程各写 20 次不同键，同时 2 个线程反复切换确认开关，结束后所有键都在、内存开关与文件一致、无残留临时文件）、`TemporaryFileNamesAreUniquePerWrite`、写失败时原文件不变。
- `tests/unit/test_system_tray.cpp` 的 `LeftClickOpensSunshineDeskMainWindow`：Windows 托盘左键回调为 `office::tray_open_main_window_cb`（不实际调用，避免打开浏览器）。
- 真实窗口、托盘左键、重启与服务模式需要实机验收，见 [VALIDATION.md](VALIDATION.md)。
