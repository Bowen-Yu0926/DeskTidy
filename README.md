# DeskTidy - 桌面整理助手

一款功能完整的 Windows 桌面整理工具：自动分类、分区管理、分页桌面、Dock 栏、区域截图、云端账号管理。

公开仓库与安装包：[github.com/Bowen-Yu0926/DeskTidy](https://github.com/Bowen-Yu0926/DeskTidy) · [v4.0.25 安装包](https://github.com/Bowen-Yu0926/DeskTidy/releases/download/v4.0.25/DeskTidy_Setup_4.0.25.exe)

**账号管理：** 侧栏启用后，桌面出现独立「账」浮标（不在分页栏）。单击打开清单，维护名称 / 账号 / 密码 / 网址，双击复制；手机号或邮箱登录后云端同步。默认关闭。

## 功能一览

| 模块 | 功能 |
|------|------|
| 整理 | 一键/定时/实时监控自动分类，三种整理模式 |
| 分区 | 可拖拽缩放折叠，Folder Portal 镜像，拖放归类 |
| 分页 | 多桌面页面，底部指示器，快捷键切换 |
| 快照 | 保存/恢复桌面图标位置 |
| Dock | 快捷启动栏，底部/左/右 |
| 截图 | 区域截图，复制/保存/贴图 |
| 录屏 | F3 开始/结束桌面录屏（需 FFmpeg） |
| 账号 | 云端账号保险库：桌面浮标入口，双击复制，后台定时同步 |
| 快捷 | 全局热键，Peek 置顶，双击隐藏 |

## 安装运行

```bash
pip install -r requirements.txt
python main.py
```

## 打包

```bash
# 日常：仅 exe（跳过 pip / 版本号 / 安装包）— 最快
scripts\build_fast.bat

# 发布：exe + 安装包（默认跳过 pip；大二进制不二次压缩）
scripts\build_installer.bat

# 可选：强制重装依赖 / 更小安装包（更慢）
set DESKTIDY_FORCE_PIP=1
set DESKTIDY_INSTALLER_MAX=1
scripts\build_installer.bat
```

慢点曾经在：每次 `pip install`、以及 Inno solid/max 对整包二次压缩。现在默认跳过 pip、`lzma2/fast` + `SolidCompression=no`，并在 `build.bat` 剪掉 WebEngine debug/DevTools/多余 locale，减小安装包以便首次双击时 Defender 扫描更快。`fd.exe` 使用 `nocompression`。

| 输出文件 | 说明 |
|----------|------|
| `dist\DeskTidy.exe` | 绿色版，单文件直接运行 |
| `dist\DeskTidy_Setup_x.y.z.exe` | 安装包，含卸载程序和快捷方式 |

安装包功能：
- 安装到 `C:\Users\你\AppData\Local\Programs\DeskTidy\`
- 可选桌面快捷方式
- 可选开机自动启动
- 开始菜单 + 卸载入口

## 默认快捷键

| 快捷键 | 功能 |
|--------|------|
| `Ctrl+Shift+O` | 一键整理 |
| `Ctrl+Shift+F` | 切换分区 |
| `Ctrl+Shift+H` | 切换图标 |
| `Ctrl+Space` | Peek 置顶 |
| `Ctrl+Shift+←/→` | 切换分页 |
| `F3` | 录屏开始/结束 |
| `Ctrl+Shift+S` | 区域截图 |

## 录屏（FFmpeg）

DeskTidy 使用开源 [FFmpeg](https://ffmpeg.org/) 做轻量桌面录屏（与 Captura 等工具同类方案）。

**打包用精简 bundle（保留硬件编码，体积更小）：**

```bash
python scripts/prepare_ffmpeg_recording.py
```

默认安装 BtbN **gpl-shared**（`h264_mf` + `ddagrab` + `gdigrab` + `libx264`），安装目录约 **70–90 MB**，低于完整 static 版（~210 MB）。打包前会自动校验 `h264_mf` 可用，避免退化成高 CPU 的纯软件编码。

1. 安装 FFmpeg 并加入 PATH，例如：`winget install Gyan.FFmpeg`
2. 按 **F3** 开始录屏，再按 **F3** 结束
3. 文件默认保存到 `%USERPROFILE%\\Videos\\DeskTidy\\`

安装版会在程序目录附带 FFmpeg（`assets\\ffmpeg\\`，含 `ffmpeg.exe` 与同目录 DLL），无需单独安装。

也可在托盘菜单「录屏（开始/结束）」或设置里修改快捷键。

## Tab 说明

- **整理规则** — 扩展名/文件名匹配
- **桌面分区** — 分区 CRUD 与样式
- **桌面分页** — 多页面管理
- **布局快照** — 图标位置备份
- **账号管理** — 云端账号保险库（浮标入口、登录、增删改查）
- **扩展功能** — Dock、区域截图、录屏、待办等
- **设置** — 模式、热键、定时、导入导出

配置目录：`%USERPROFILE%\.desktidy\`
