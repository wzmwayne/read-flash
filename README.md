# Read-Flash

**终端风格、书页模型的极简纯文本阅读器** —— 面向极低端设备与跨平台场景。
纯 TXT + 极简语法（标题 / 换页符），黑底终端观感，翻页即保存，一切皆书。

> **当前状态**：规范冻结（v3.4.2）+ 核心算法已验证 + **正式应用可运行**（书库 / 阅读 / 设置 / 调试 / 内置教程 / 提示框，安卓外壳 + C 应用层）。
> 仍在早期：搜索、目录跳转、更多格式支持尚未实现。

## 验证结果（2026-10-10）

| 项 | 结果 |
|---|---|
| 网格不变量 | PASS —— 700 组合（7 屏幕 × 5 密度 × 5 字号 × 4 行距）0 失败 |
| 排版确定性 | PASS —— 5MB 中文语料 3147 页 ×2 完全一致；字号往返一致 |
| 锚点 / 不重不漏 | PASS —— 3147 页连续覆盖 5,234,824 B；进度文件往返 + CRC 一致；反向翻页 832 步铺满 |
| 单行 5MB（无 `\n`） | PASS —— 2974 页连续覆盖（v3.3 的 `canPrev` 判定在此场景会失效，v3.4 已修正） |
| 边界用例 | PASS —— 空文件 / 单换行 / 无尾换行 / 仅换页符 / 双换页符 / 三档标题 / CRLF / 全角标点 |
| 跨进程确定性 | PASS —— GLOBAL 哈希一致 |
| 安卓 APK | PASS —— `build/readflash-verify.apk`，arm64-v8a，29 KB，minSdk 26 / targetSdk 34，v2+v3 签名 |

## 文档

| 文件 | 说明 |
|---|---|
| [docs/Read-Flash-v3.3-技术理论文档.md](docs/Read-Flash-v3.3-技术理论文档.md) | 基线规范（17 章，含 I1–I9 不变量） |
| [docs/Read-Flash-v3.4-修订条文.md](docs/Read-Flash-v3.4-修订条文.md) | v3.4 补丁（正式版 · 已冻结，含 v3.4.1/v3.4.2 实现期修订） |
| [docs/Read-Flash-v3.3-设计评审提问.md](docs/Read-Flash-v3.3-设计评审提问.md) | 设计评审：69 条问题与结论 |

## 目录结构

```
src/core/        平台无关核心（纯 C99，整数运算）
  rf.h           类型与 API（网格 / UTF-8 / 排版 / 进度 / 索引定位）
  rf_grid.c      整数像素网格（字号/行距/边距 → C 列 × V 行）
  rf_utf8.c      自建 UTF-8 解码器 + 全角/半角列宽分类
  rf_layout.c    渲染行迭代器 / 分页 / 上一页回溯
  rf_render.c    验证用渲染器（网格方块，不含真实字形）
  rf_util.c      CRC32 / FNV-1a / 进度文件编解码
  rf_selftest.c  §16 最小验证 + 不重不漏自检
src/host/        宿主验证入口
android/         安卓验证 App（JNI + 单 Activity 显示自检报告）
tools/           构建与调试脚本
docs/            规范与评审文档
```

## 构建与运行

### 1) 宿主验证（无需安卓，几秒出结果）

```bash
./tools/build_host.sh verify   # 自检：期望 RESULT: PASS (失败 0)
./tools/build_host.sh demo     # 端到端演示，导出真字形画面 build/host/frames.png
```

> **验证必须带字体**：`demo` 用 FreeType + 真字体（默认文泉驿等宽正黑，`RF_FONT` 可覆盖）渲染，
> 否则只会画占位方块，**错字、断行、标点问题都看不出来**。没有字体时脚本会明确告警。

### 2) 安卓 APK（全离线，只用本机已装的 SDK/NDK）

```bash
./tools/build_android.sh app      # 正式应用 → build/readflash.apk
./tools/build_android.sh verify   # 最小验证 → build/readflash-verify.apk
adb install -r build/readflash.apk
```

导入方式：把 .txt 放进 `Android/data/com.readflash.app/files/import/`，在书库点 `[导入]`。
调试可用 Shizuku 的 rish（本仓库开发约定：只安装/调试本应用，不改系统设置、不操作其它应用）。

脚本会逐工具探测（aapt2 / d8 / apksigner / zipalign / NDK clang），自动跳过宿主机无法执行的
组件（例如 aarch64 主机上的 x86_64 预编译工具），全程**不下载任何东西**。

## 版本规范

`VERSION` 文件是唯一版本源，构建脚本注入 APK 的 `versionName` 与 `versionCode`：

```
<大版本>.<小版本><d|r><修订位>
 0.1           大功能更新、大的改动
 .1            结构性改动（页面栈、书模型、存储格式、网格模型等）、小功能更新、较严重 bug 修复
 d / r         debug / release
 1             定义与语义调整（行高、标题、进度等）、细碎修改、难以启齿的 bug 修复
```
**判定顺序**：先看是否动了结构（改小版本），再看是否只是定义/语义或小修（改 `d` 后那位）。
当前版本见 [VERSION](VERSION)（versionCode = 大*10000 + 小*100 + 次*10 + 修订位）。

## 网络策略

| 场景 | 方式 |
|---|---|
| `git clone` / `git push` / GitHub API | **直连** |
| 用 curl 从 GitHub 下载文件（releases / raw / archive） | **镜像竞速**：`tools/gh-dl.sh <url> [out]`，`ghproxy.net` 与 `cdn.gh-proxy.cn` 并行，取最快 |
| 其它依赖下载 | 国内镜像（清华 TUNA / 阿里云 / 华为云 / npmmirror 等） |

本项目当前构建**不依赖任何在线下载**。

## 路线图

1. ✅ 最小验证：网格构建 + UTF-8 解码 + 断行 + 单页渲染 + 锚点恢复
2. ⬜ 索引（自适应 K + CRC32 自校验）与 `.rfidx` / `.rfprog` / `.config` 落盘
3. ⬜ 阅读界面（顶栏 + 正文 + 底栏 2 行）、书库（单选 + `[打开][删除][导入]`）、设置（字号/行距/四向边距 + 预览）、内置《应用使用教程》
4. ⬜ 提示框书（确定/取消，返回=取消）、调试书（重建索引 / 重置进度）
5. ⬜ PAL 落地（Android NDK / iOS），字体选型（OFL）

## 许可

[GPL-3.0](LICENSE)。字体计划采用 OFL-1.1 字体（如 Noto Sans Mono CJK），以避免 GPL 字体的分发义务歧义。
