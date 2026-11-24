# DeskTidy（C++ 原生）

Windows 桌面整理工具：分区、分页、钉选整理、布局快照、截图/录屏、云端账号库、DeskNote 便签。

公开仓库与安装包：[github.com/Bowen-Yu0926/DeskTidy](https://github.com/Bowen-Yu0926/DeskTidy) · [Releases](https://github.com/Bowen-Yu0926/DeskTidy)

> 本分支以 **`native/` Win32 实现** 为准。原 Python / PyQt 应用代码已移除。

## 构建 / 运行

```bat
cd native
build.bat
..\scripts\run_native.bat
..\scripts\selftest_native.bat
..\scripts\build_native_installer.bat
```

数据目录：`%LOCALAPPDATA%\DeskTidyNative\`

说明见 [docs/cpp-greenfield.md](docs/cpp-greenfield.md) 与 [native/README.md](native/README.md)。

第三方组件许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)；发版流程见 [docs/RELEASING.md](docs/RELEASING.md)。

## 仓库结构

| 路径 | 内容 |
|------|------|
| `native/` | C++ 主程序（DeskTidy + DeskNote） |
| `assets/` | 共享资源（宠物帧、对话框 HTML 等） |
| `installer/DeskTidyNative.iss` | 原生安装包脚本 |
| `server/` | 账号库 PHP API |
| `scripts/` | 原生构建/自测、资源生成、站点上传 |
| `docs/` | 产品与迁移文档 |

## 版本

见根目录 `VERSION`。
