# deskNote Markdown 增强（分期 · 一期）

**日期：** 2026-09-10  
**状态：** 待用户审阅  
**产品决策：** 方案 C（分期做齐 Typora 级能力）+ 一期采用方案 A（渲染增强 / 同步滚动 / 导出）  
**技术选型：** 方案 1 — `QWebEngineView` + 离线前端渲染栈  

---

## 1. 背景与目标

当前 deskNote（`src/ui/notepad_window.py` + `src/markdown_preview.py` + `src/ui/markdown_pane.py`）对 `.md` 使用「大纲 | 源码 `QPlainTextEdit` | 预览 `QTextBrowser`」，Python `markdown` 库渲染，缺少：

- 代码高亮、数学公式、Mermaid
- 源码 ↔ 预览同步滚动
- 导出 PDF / HTML
- 脚注 / `[TOC]` 等增强语法的可靠支持

**一期目标：** 在保持现有三栏编辑模型的前提下，把预览与导出拉到接近 Typora 阅读侧体验；不引入 WYSIWYG。

**非目标（二～四期）：** WYSIWYG、专注/打字机模式、文件树与库内全局搜索、图床、Word/Pandoc 导出。

---

## 2. 用户可见行为（一期）

### 2.1 预览

- `.md` / `.markdown` 仍为三栏；非 Markdown 仍为单栏纯文本（不变）。
- 预览区由 `QTextBrowser` 替换为 `QWebEngineView`（封装在现有 `MarkdownPreviewBrowser` 或其继任类中，对外 API 尽量稳定）。
- 支持（离线、无外网）：
  - GFM 常用语法（标题、列表、任务列表、表格、引用、链接、图片）
  - **围栏代码块语法高亮**（highlight.js）
  - **数学：** 行内 `$…$` / 块级 `$$…$$`（KaTeX）
  - **Mermaid** 围栏（\`\`\`mermaid）
  - **脚注**、**`[TOC]`**（或等价自动目录）
  - YAML front matter：解析后不渲染为正文（可忽略或折叠显示元数据，一期以「不污染正文」为准）
- 相对路径图片仍解析为 `file://`（沿用现有 `base_dir` 语义）。
- 主题：预览 CSS 跟随 deskNote / DeskTidy 明暗主题（至少提供 light；dark 与全局 theme 对齐）。

### 2.2 同步滚动

- 源码滚动 → 预览跟滚；预览滚动 → 源码跟滚。
- 策略：**滚动比例为基线**，若两侧能解析到相同标题锚点则用锚点校正，减少长文漂移。
- 程序化滚动时加短防抖 / 抑制回声，避免双向死循环。
- 开关：查看菜单「同步滚动」（默认开），写入 `settings.notepad.md_sync_scroll`。

### 2.3 导出

- 菜单 **文件 → 导出 HTML…** / **导出 PDF…**（仅当前为 Markdown 文档时启用）。
- HTML：写出完整单文件或「HTML + 旁路资源」二选一；一期优先 **单文件 HTML**（关键 CSS/关键必要时内联或同目录复制最小资源集；以可离线打开为准）。
- PDF：对当前预览文档调用 WebEngine `printToPdf`（或等价打印管线），用户选路径。
- 失败时 toast / 状态栏提示，不静默失败。

### 2.4 降级

- 若运行环境无 WebEngine（未安装 / 打包漏带）：预览回退到现有 `QTextBrowser` + Python `markdown` 路径，并提示「增强预览不可用」；导出 PDF 不可用，HTML 可用 Python 渲染降级版。

---

## 3. 架构

```
notepad_window.py
  └─ TabSession (md)
       ├─ MarkdownOutlineList          (现有，大纲仍可由 Python 抽标题)
       ├─ QPlainTextEdit               (源码，唯一编辑面)
       └─ MarkdownPreviewHost
            ├─ QWebEngineView          (首选)
            └─ MarkdownPreviewBrowser  (降级)

assets/md_preview/                     (随安装包分发，禁止运行时拉 CDN)
  ├─ index.html / viewer.html
  ├─ vendor/marked|markdown-it, highlight.js, katex, mermaid, …
  └─ theme-light.css / theme-dark.css

markdown_preview.py
  ├─ extract_outline / normalize_atx   (保留)
  ├─ build_preview_payload(md, base_dir, theme)  → 交给 Web 页 setMarkdown
  └─ render_html_fallback(...)         (无 WebEngine 时)

export_markdown.py (新)
  ├─ export_html(path, md, …)
  └─ export_pdf(web_view|html, path)
```

**数据流（预览刷新）：**

1. 源码 `textChanged` → 现有 ~250ms debounce 不变。  
2. Python 侧可选做 front-matter 剥离与相对图路径预处理，或把 `baseUrl` 设为笔记目录让 Web 引擎解析。  
3. 调用页面 JS：`window.__desknoteSetMarkdown(text, { baseUrl, theme })`。  
4. 页面内完成 MD→HTML、高亮、KaTeX、Mermaid、TOC。

**同步滚动：**

- Qt：监听 editor scrollbar；`runJavaScript` 设置 preview scroll ratio / scrollToHeading。  
- Web：`scroll` 事件 → `qwebchannel` 或 URL interceptor / 预留 bridge → 调 editor 滚动。  
- 一期可用 `QWebChannel`；若集成成本高，可用 `title`/`console` 桥临时方案，但正式以 `QWebChannel` 为准。

---

## 4. 依赖与打包

| 项 | 说明 |
|----|------|
| Python | 新增 `PyQt6-WebEngine`（版本与现有 PyQt6 对齐） |
| 静态资源 | `Desktidy/assets/md_preview/**`，PyInstaller datas 一并打入 |
| 体积 | 安装包显著增大（Chromium）；可接受，需在发布说明中一句带过 |
| FFmpeg/fd | 与本期无关，保持现有校验 |

`requirements.txt` / 构建脚本增加 WebEngine；`verify` 自测在无 WebEngine 时走降级断言。

---

## 5. 设置键

| Key | 默认 | 含义 |
|-----|------|------|
| `notepad.md_preview` | true | 已有 |
| `notepad.md_outline` | true | 已有 |
| `notepad.md_sync_scroll` | true | **新增** 同步滚动 |
| （可选）`notepad.md_engine` | `"web"` / `"fallback"` | 仅调试/强制降级，默认自动探测 |

---

## 6. 测试计划

- 单元 / 契约：`extract_outline`、front-matter 剥离、导出路径、设置读写。  
- 有 WebEngine 的机器：`selftest_desknote_md_preview.py`（新）覆盖  
  - 预览类存在且优先 WebEngine  
  - 同步滚动开关写入 settings  
  - 导出菜单 action 存在且非 md 时 disabled  
  - 资源目录 `assets/md_preview` 存在关键入口文件  
- 无 WebEngine CI/环境：降级路径不崩溃。  
- 手工：含公式、mermaid、代码块、脚注、`[TOC]`、相对图片的样例 `.md`；双向滚动；导出 PDF/HTML 用系统阅读器打开。

---

## 7. 分期路线图（C）

| 期 | 内容 | 依赖一期 |
|----|------|----------|
| **一期（本文）** | Web 预览、高亮/公式/Mermaid/TOC/脚注、同步滚动、导出 PDF/HTML | — |
| **二期** | 混合/WYSIWYG 编辑、专注/打字机、智能粘贴 | 复用 Web 内核 |
| **三期** | 文件夹侧栏、文件树、库内搜索 | 独立于预览引擎 |
| **四期** | 图床、Pandoc Word 等 | 外置工具可选 |

---

## 8. 风险与缓解

| 风险 | 缓解 |
|------|------|
| WebEngine 打包失败 / 体积 | datas 清单自检；文档说明；降级预览 |
| Mermaid 大图卡顿 | debounce 保持；超大文档可延迟渲染 mermaid |
| 同步滚动不准 | 比例 + 标题锚点；提供关闭开关 |
| XSS（预览执行 MD 内脚本） | 渲染管线消毒 / 禁用裸 script；仅加载本地 vendor |

---

## 9. 成功标准（一期完成定义）

1. 样例文档中代码高亮、KaTeX、Mermaid、脚注、`[TOC]` 在预览中可见（离线）。  
2. 默认同步滚动可用，关闭后互不影响。  
3. 能导出可打开的 HTML 与 PDF。  
4. 无 WebEngine 时不崩溃，仍可编辑与基础预览。  
5. 相关 selftest 通过；改完按仓库惯例源码重启 deskNote/DeskTidy。

---

## 10. 实现顺序（批准后写入 implementation plan）

1. 引入依赖与 `assets/md_preview` 最小可运行页  
2. 替换预览控件 + 降级分支  
3. 同步滚动 + 设置  
4. 导出 HTML/PDF  
5. selftest + 帮助文案更新（`desknote_help` / `help_content`）
