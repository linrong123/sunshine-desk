# 文本剪贴板同步

## 背景与目标

办公串流里最常见的跨端操作是复制粘贴文字：在主控端复制一段文字，粘贴到远端 Windows；或者反过来。上游 GameStream 协议没有剪贴板通道。本扩展在现有已配对 HTTPS 服务上增加一个读写接口，由 Moonlight Desk 客户端决定何时读、何时写，被控端只负责安全地读写本机剪贴板。

目标：

- 纯文本（Unicode）双向同步，大小上限 1 MiB。
- 只有正在串流的、已配对且启用的那台设备能读写；被控端可以随时关闭。
- 不新增端口、不新增凭据、不常驻监听剪贴板。

非目标：图片、文件、富文本（HTML/RTF）；服务端主动推送变化；多客户端之间的仲裁（最后一次写入生效）。

## 协议

两个请求都只注册在 GameStream HTTPS 服务上，复用配对证书双向 TLS；不接受 `uniqueid`、`uuid` 等参数作为身份。Windows 构建的 `serverinfo` 返回 `OfficeClipboard=1`（开关关闭时仍是 1，由接口返回 403），其他平台不宣告。所有响应是 GameStream XML，客户端必须读 `root.<xmlattr>.status_code`，不能只看 HTTP 状态；每个响应后服务端关闭连接。

### 读取 `GET /office/clipboard`

无 query、无 body。

```xml
<root status_code="200">
  <sequence>1234</sequence>
  <hasText>1</hasText>
  <text>5L2g5aW9CndvcmxkCg==</text>
</root>
```

- `sequence`：Windows `GetClipboardSequenceNumber`，剪贴板每次变化都会增加。客户端可用它判断远端剪贴板是否变过，避免重复粘贴。
- `hasText`：剪贴板里有非空 Unicode 文本时为 1，否则为 0（没有文本、只有图片或文件、文本为空都算 0）。
- `text`：仅 `hasText=1` 时出现，为 UTF-8 文本的标准 base64（RFC 4648，带 `=` 填充，无换行）。
- 文本超过 1 MiB（按转换成 LF 后的 UTF-8 字节数计）时返回 200、`hasText=0`、`tooLarge=1`，不带 `text`，服务端写一条 warning 日志。

### 写入 `POST /office/clipboard`

body 为原始 UTF-8 文本（不是 base64），无 query。成功时返回写入后的 `sequence`：

```xml
<root status_code="200"><sequence>1235</sequence></root>
```

### 状态码

| XML status_code | 含义 |
| --- | --- |
| 200 | 成功 |
| 400 | 带了 query；GET 带 body；POST body 为空或不是合法 UTF-8 |
| 401 | 证书未配对或客户端已禁用（GameStream 认证层统一返回）；没有证书可能在 TLS 握手就失败 |
| 403 | 已配对但当前没有在串流、或被控端关闭了剪贴板同步；`status_message` 说明原因 |
| 413 | POST body 超过 1 MiB（`Content-Length` 超限时不回响应、直接断开连接，见下） |
| 501 | 非 Windows 平台 |
| 503 | 剪贴板被其他程序占用（重试 5 次、间隔 30 ms 后仍打不开）或系统调用失败 |

注意超限有两种表现：

- `Content-Length` 超过 1 MiB 时，服务端在读取 body 之前**直接关闭连接，不回任何 HTTP 响应**（客户端看到 EOF 或连接被重置）。这是为了不把超大 body 读进内存（HTTPS 服务的 `max_request_streambuf_size` 设为 1 MiB）。Simple-Web-Server 在这条路径上虽然构造了 413 响应，但该响应对象没有挂发送逻辑（只有路由处理器的响应在析构时发送，见 `third-party/Simple-Web-Server/server_http.hpp` 的 `read()` 与 `write()`），所以实际从不发出。客户端应把「连接被关闭」和裸 HTTP 413（将来上游修复后可能出现）都当作终态、不重试；先检查 HTTP 状态行，再解析 XML。
- 其余超限情况（理论上不会出现，作为兜底）返回 XML `status_code=413`。

客户端应在发送前自己检查长度，不依赖服务端的 413。正好 1 MiB 允许。请求头也受同一上限约束（上游请求头远小于此）。

## 授权

每个请求依次检查：

1. 证书：认证层把 TLS 连接映射到已配对证书（`nvhttp::office_verified_client`）。未知、已禁用的客户端在认证层就返回 401，不会进入处理函数。
2. 串流：证书必须在 `office::streaming_certs()` 里，即该设备当前有正在运行的串流（由 `stream.cpp` 的 `stream_started` / `stream_stopped` 维护，按证书计数）。别的已配对设备、或该设备在串流开始前/结束后的请求都返回 403。
3. 开关：`desk_clipboard_sync` 关闭时返回 403。

授权只看 TLS 证书，与 `serverinfo` 的 `uniqueid` 无关。这样一台已配对但没在串流的设备（例如家里另一台电脑）读不到办公机的剪贴板。

## 换行与编码

- 线上一律 UTF-8 + LF。
- 写入：LF → CRLF；已有的 CRLF 保持不变（不会变成 CR CR LF）；孤立的 CR 保留。然后转为 UTF-16 写入 `CF_UNICODETEXT`。
- 读取：`CF_UNICODETEXT` → UTF-8（孤立的 UTF-16 代理项替换为 U+FFFD，保证输出是合法 UTF-8）→ CRLF 变 LF，孤立的 CR 保留。
- 读取时按剪贴板内存块大小截断查找结尾 NUL，不会越界读取其他程序的内存。超过 2 Mi 个 UTF-16 单元的文本不做转换直接报 `tooLarge`（转换后必然超过 1 MiB）。
- POST 校验严格 UTF-8：拒绝截断序列、过长编码、代理区（U+D800–U+DFFF）和超过 U+10FFFF 的码点。文本中的 NUL 字节合法，但 Windows 剪贴板会在第一个 NUL 处截断。

## 配置与开关

| 项 | 说明 |
| --- | --- |
| 配置键 | `desk_clipboard_sync`，`enabled`（默认）或 `disabled`；也接受 `true/false/on/off/1/0` 等与 `desk_confirm_connections` 相同的写法。非法值写 warning 日志后按开启处理 |
| 主窗口 | 「允许剪贴板同步」勾选框，切换即时生效并原子写回配置，见 [MAIN_WINDOW.md](MAIN_WINDOW.md) |
| 生效 | 运行时生效，不需要重启服务 |

默认开启的理由：只有正在串流的已配对设备能用，而串流本身已经能看到并操作整个桌面，剪贴板同步没有扩大该设备的能力范围。

## 代码结构

| 文件 | 内容 |
| --- | --- |
| `office/clipboard.h/.cpp` | 协议处理 `clipboard_request`、授权判定 `clipboard_access`、换行转换、base64、UTF-8 校验、配置 `apply_clipboard_config` / `set_clipboard_sync`、可注入的 `clipboard_backend` 接口。请求在进程级 mutex 下串行执行 |
| `office/windows/clipboard.cpp` | Win32 实现：`OpenClipboard`（重试）、`GetClipboardData`/`SetClipboardData(CF_UNICODETEXT)`、`GetClipboardSequenceNumber`。写入时临时创建 message-only 窗口作为剪贴板所有者 |
| `office/nvhttp_routes.cpp` | 路由注册、`serverinfo` 能力、HTTPS 请求体上限 |
| `office/authorization.cpp` | `apply_config` 末尾调用 `apply_clipboard_config` |

非 Windows 平台没有 backend，请求通过授权检查后返回 501。

日志：debug 级只记录字节数，任何级别都不记录剪贴板内容。

## 已知限制（未实机核实）

- Windows 剪贴板按 window station 隔离。Sunshine 以服务方式运行时，若进程不在交互用户的 `WinSta0` 中，读写的将是另一个剪贴板，客户端看到的是「成功但没效果」。上游服务 `tools/sunshinesvc.cpp` 用 SYSTEM 令牌、把会话号改成活动控制台会话、桌面指定 `winsta0\default` 启动 Sunshine，预期与登录用户共用同一个剪贴板，但尚未实机验证；远程桌面（RDP）会话不是控制台会话，此时预期读写不到 RDP 用户的剪贴板。
- 锁屏、UAC 安全桌面、用户切换期间，剪贴板可能打不开（503）或读到的是另一个会话的内容。
- 写入的文本会进入 Windows 剪贴板历史（Win+V）和云剪贴板（若用户开启），与本地复制相同。
- 不处理延迟渲染（delayed rendering）的剪贴板格式失败以外的情况：某些程序只在被请求时才生成文本，生成失败时读到 `hasText=0`。
- 多个客户端同时串流时，任一正在串流的设备都可以读写，最后一次写入生效。

## 测试

- `tests/unit/test_office_clipboard.cpp`：换行转换、RFC 4648 base64 向量、严格 UTF-8、授权判定（无证书、别的设备、未串流、开关关闭）、请求处理（假 backend：tooLarge 边界、CRLF 转换、400/413/503、query 拒绝）、配置解析与持久化。
- `tests/unit/test_nvhttp_client_auth.cpp` 的 `OfficeClipboardTlsTest`：经生产 TLS 服务器与假 backend 验证串流中的设备读写、未串流和开关关闭的 403、未知/禁用证书的 401、超大 `Content-Length` 只发请求头即被断开（不等 body，也不写剪贴板）。
- 测试不触碰真实系统剪贴板。真实剪贴板读写的验收项见 [VALIDATION.md](VALIDATION.md)。
