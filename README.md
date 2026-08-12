# RC-ScreenShot

0.4.1 支持可选的连拍快捷键：截图快捷键负责单张截图，连拍快捷键可选，按设置
连续获取 2-30 帧（间隔 0.05-0.99 秒，默认 5 帧、0.08 秒）。可在设置页
编辑连拍快捷键、连拍张数和间隔秒数。

当前版本：0.4.1

![RC-ScreenShot Logo](assets/rc-screenshot-logo.png)

RC-ScreenShot 是面向 Windows 10 1903+ x64 的轻量原生截图工具。它使用 DXGI 1.5/D3D11 捕获 SDR、WCG 和 HDR 桌面，提供普通框选、顶层窗口吸附、原图单元识别、GPU 标注、横排/竖排文字、马赛克、剪贴板输出，以及带 gain map 的 Ultra HDR JPEG 保存。

## 构建

需要 Visual Studio 2022（Desktop development with C++）、Windows 10/11 SDK 和 CMake 3.24+。完整构建会固定拉取 `libultrahdr` commit `b2aacb366e1542cfc29605cb0d8a0ebd06bb07f8`，静态链接 JPEG 依赖，并同时写入 Ultra HDR XMP 与 ISO 21496-1 元数据。

```powershell
.\build.ps1 -Preset windows-release -Test -Install
```

快速开发构建可暂时关闭 HDR JPEG 编码器：

```powershell
.\build.ps1 -Preset windows-fast -Test
```

## 使用

- 默认截图热键：`Ctrl+\`；可在设置中按行添加多组热键。
- 截图中按空格循环普通、窗口、单元模式。
- 工具快捷键：`P` 画笔、`R` 矩形、`E` 椭圆、`L` 直线、`A` 箭头、`T` 文字、`M` 马赛克、`Shift+M` 框选马赛克、`F` 外框。
- 工具栏第二行提供尺寸滑条、颜色预设和当前工具的详细参数；编辑区右键可打开完整工具与参数菜单。
- 选择文字工具后点击选区输入内容，`Enter` 确认、`Esc` 取消；横排/竖排可在参数栏或右键菜单切换。
- `[`/`]` 调整尺寸，`C` 选择颜色，`O` 循环透明度，`Ctrl+Z/Ctrl+Y` 撤销/重做。
- `Ctrl+C` 复制、`Ctrl+S` 保存、`Enter` 执行设置中的默认动作、`Esc` 取消。

命令行接口：`--silent`、`--capture`、`--settings`。配置以 UTF-8 JSON 原子写入 EXE 同目录的 `RC-ScreenShot.config.json`。

## 已知平台限制

- 不捕获安全桌面，也不绕过 DRM/受保护内容。
- 窗口模式裁剪冻结桌面画面；窗口被遮挡的部分不会被重新抓取。
- HDR 显示器上若 DXGI 捕获不可用，应用会拒绝错误的 GDI 降级；SDR 显示器可使用 GDI 兜底。
- 当前版本不包含 OCR、滚动截图、云上传、截图历史和 ARM64。

## 第三方组件

Ultra HDR 编码使用 Google/AOSP `libultrahdr`，遵循 Apache-2.0 或 MIT 双许可证。Windows 平台 API 和编解码器由系统提供。
