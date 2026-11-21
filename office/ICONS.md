# 应用图标与托盘图标（Sunshine Desk）

## 背景与目标

办公版不再使用上游 Sunshine 的太阳图标，换成原创矢量图标，与配套的 Moonlight Desk 共用同一套“显示器”造型：客户端是蓝紫底板加指针，被控端是青绿底板加信号弧线。

## 设计

- 应用图标：圆角方形底板（824 × 824，位于 1024 画布 (100, 100)，圆角 185），白色显示器（短颈、宽底座），右上两道四分之一圆弧信号带（表示“共享”）。
- 托盘图标：同一显示器与信号带，改为实心青绿填充、透明屏幕开口和深色细轮廓，保证 16 px 下可辨认；播放/暂停/锁定三种状态在右下角叠加 25 px 徽标（画布 39%）加 2 px 白圈。
- 无文字、无天体图案，没有使用任何上游或第三方美术资源。

| 用途 | 颜色 |
| --- | --- |
| 被控端渐变（左上到右下） | `#14A9AD` → `#117D57` |
| 客户端渐变 | `#326BEA` → `#4332A9` |
| 托盘主色 / 描边 | `#14A9AD` / `#075761` |
| 播放 / 暂停 / 锁定徽标 | `#23864F` / `#C5840C` / `#61717D` |
| 图形与徽标白圈 | `#FFFFFF` |

## 文件

应用图标（源为 `office/icons/host-app.svg`）：

| 路径 | 格式 | 用途 |
| --- | --- | --- |
| `sunshine.svg`、`src_assets/macos/sunshine-icon.svg` | SVG，1024 | README、Linux 安装、macOS 图标源 |
| `sunshine.png` | 256×256 RGBA | CPack、Linux |
| `sunshine.ico` | 256 px PNG + 128/64/48/32/24/16 px 32 位 BMP | Windows 可执行文件资源（`cmake/windows.rc`，主窗口 `LoadIconW` 也用它）、安装包 |
| `src_assets/macos/build/sunshine.icns` | `iconutil` 生成，16–512 及 @2x | macOS 应用包 |
| `src_assets/common/assets/web/public/images/sunshine.ico` | 16–256 共 25 档 PNG | 网页 favicon |
| `…/images/logo-sunshine-16.png`、`logo-sunshine-45.png` | RGBA | 网页导航栏 Logo（45 px） |

托盘图标（源为 `office/icons/tray-*.svg`）：

| 路径 | 说明 |
| --- | --- |
| `…/images/logo-sunshine.svg`（构建产物） | 默认托盘图标；由 `vite.config.js` 的 `emitTrayIconsPlugin` 从 `office/icons/tray-normal.svg` 发出，单元测试资源（`tests/CMakeLists.txt`）同样取该文件 |
| `…/images/sunshine-{playing,pausing,locked}.svg` | 托盘状态图标，`src/system_tray.cpp` 直接按此路径加载（Qt 渲染 SVG） |
| `…/images/sunshine-{playing,pausing,locked}.{ico,png}`、`-16.png`、`-45.png` | 保留的位图/ico 变体，格式沿用原文件（ico 为 25 档 PNG，png 为 256×256 RGBA） |

文件名与路径保持上游原样，代码引用不变。唯一的适配点：托盘默认图标不再复用应用图标 `sunshine.svg`，改取 `office/icons/tray-normal.svg`（见 UPSTREAM.md）。

## 重新生成

在 macOS 上（需要 ImageMagick 7、`iconutil`、Pillow）：

```sh
cd office/icons
python3 build-assets.py /tmp/icons-out    # 输出 server/ 树（路径与仓库相对路径一致）和 client/ 树
# 把 /tmp/icons-out/server/ 下的文件复制到仓库对应路径；SVG 直接复制：
cp host-app.svg ../../sunshine.svg
cp host-app.svg ../../src_assets/macos/sunshine-icon.svg
for s in playing pausing locked; do cp tray-$s.svg ../../src_assets/common/assets/web/public/images/sunshine-$s.svg; done
python3 render-previews.py                # 可选：刷新 preview/ 与 sheet.png
```

不要直接 `magick x.svg` 栅格化：Mac 上的 ImageMagick 7 SVG 读取器会丢掉渐变和描边透明度。`render-previews.py` 把 SVG 几何转成 ImageMagick 绘图原语再渲染。

## 来源与许可

矢量图由 OpenAI Codex 按所有者的指示在所有者的机器上原创生成；未使用上游 Sunshine 或任何第三方美术。随本仓库以 GPL-3.0 许可。

## 已知限制

- `sunshine-{playing,pausing,locked}.png` 原文件是 237–247 px 的不规则尺寸，现统一为 256×256。
- macOS `.icns` 由当前系统 `iconutil` 生成，容器块类型（`ic04/ic05`）与上游原文件（`icp4/icp5`）不同，系统均可识别。
- `src/system_tray.cpp` 的托盘 SVG 在 Windows 上由 Qt 渲染；实机托盘显示需在真实主机上目视验收（本次只验证了构建，未实机验收）。
