# Read-Flash iOS 外壳

> **本目录未在本仓库的 Linux 环境中编译或测试**（无 Xcode）。以下为源码与构建约定，需在 macOS 上验证。

## 组成

| 文件 | 作用 |
|---|---|
| `../src/pal/pal_ios.m` | PAL：目录（`Library/Application Support/ReadFlash` 与 `Documents/import`）、时钟、退出、**UIDocumentPicker 文件选择**、**CoreText 字形光栅化** |
| `Info.plist` | `UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace`（目录对"文件"App 可见） |
| `AppShell.m` | UIApplicationDelegate + 一个把 C 帧缓冲贴到屏幕、并把触摸转给 `rf_app_tap()` 的 UIView |

C 核心（`src/core/*.c`）与安卓完全共用，不需要任何 iOS 专有分支。

## 构建（在 macOS 上）

```bash
# 1) 静态库：C 核心 + iOS PAL
clang -c -O2 -Isrc/core src/core/*.c
clang -c -O2 -Isrc/core -fobjc-arc src/pal/pal_ios.m
libtool -static -o libreadflash.a *.o

# 2) App 外壳 + 链接（Xcode 工程或命令行皆可，需链接 CoreText/CoreGraphics/UIKit/Foundation）
```

`AppShell.m` 的启动流程：`pal_ios_init_dirs()` → `rf_store_init()`/`rf_settings_load()` →
`rf_glyph_set_provider(rf_glyph_ios, NULL)` → `rf_app_create()`；每帧 `rf_app_render()` 到 RGB565 缓冲，
转成 CGImage 贴到 `CALayer`；触摸按下/抬起按 §11.2 的单击判定调用 `rf_app_tap()`；返回手势调用 `rf_app_key_back()`。

## 版本

随仓库 `VERSION` 走；本目录的 plist 只是占位，实际版本在打包时注入。
