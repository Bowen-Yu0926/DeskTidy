"""DeskNote in-app help — how to use each feature."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QTextBrowser,
    QVBoxLayout,
)

# (topic_id, title)
DESKNOTE_HELP_TOPICS: list[tuple[str, str]] = [
    ("overview", "总览"),
    ("tabs_files", "页签与文件"),
    ("library", "笔记库侧栏"),
    ("edit_find", "编辑与查找"),
    ("markdown", "Markdown 三栏"),
    ("edit_modes", "编辑模式"),
    ("headings", "标题与大纲"),
    ("shortcuts", "快捷键"),
    ("desktidy", "与 DeskTidy 联动"),
]


def _op_table(rows: list[tuple[str, str]]) -> str:
    body = "".join(f"<tr><td>{op}</td><td>{effect}</td></tr>" for op, effect in rows)
    return (
        "<table class='op-table' cellspacing='0' cellpadding='6'>"
        "<tr><th align='left'>操作</th><th align='left'>说明</th></tr>"
        f"{body}</table>"
    )


def _wrap(body: str) -> str:
    return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{
  font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
  font-size: 13px;
  line-height: 1.55;
  color: #1f2937;
  margin: 8px 12px 16px;
}}
h2 {{ font-size: 1.35em; margin: 0.2em 0 0.55em; }}
h3 {{ font-size: 1.1em; margin: 1em 0 0.4em; }}
p.purpose {{ color: #4b5563; margin-top: 0; }}
p.tip {{
  background: #f8fafc;
  border-left: 3px solid #93c5fd;
  padding: 8px 10px;
  color: #374151;
}}
code {{
  font-family: Consolas, "Cascadia Mono", monospace;
  background: #f3f4f6;
  padding: 0.05em 0.3em;
  border-radius: 3px;
}}
table.op-table {{
  width: 100%;
  border-collapse: collapse;
  margin: 0.5em 0 0.8em;
}}
table.op-table th, table.op-table td {{
  border: 1px solid #e5e7eb;
  text-align: left;
  vertical-align: top;
}}
table.op-table th {{ background: #f3f4f6; }}
</style></head><body>{body}</body></html>"""


def desknote_topic_inner_html(topic_id: str) -> str:
    """Inner HTML for one DeskNote handbook topic (no document chrome)."""
    # Reuse bodies from desknote_help_html without the wrapper chrome.
    full = desknote_help_html(topic_id)
    start = full.find("<body>")
    end = full.rfind("</body>")
    if start >= 0 and end > start:
        return full[start + len("<body>") : end].strip()
    return full


def desknote_help_html(topic_id: str) -> str:
    topics = {
        "overview": f"""
<h2>DeskNote 总览</h2>
<p class='purpose'>DeskNote 是与 DeskTidy 同包可选安装的独立记事本：多页签编辑常见文本/代码，
并对 <code>.md</code> 提供笔记库侧栏、源码三栏预览、所见即所得、专注/打字机模式与智能粘贴。</p>
{_op_table([
    ("怎么打开", "桌面 / 开始菜单的 DeskNote 快捷方式；或在 DeskTidy 扩展里启用快捷方式。"),
    ("默认保存位置", "安装目录下的「笔记」文件夹（可在 DeskTidy → 扩展功能里改）。"),
    ("窗口关闭", "关闭窗口会保留页签内容，下次打开还会在；关掉某个页签会按规则清理对应草稿/笔记文件。"),
])}
<p class='tip'>本帮助只讲 DeskNote。DeskTidy 桌面分区等功能请在 DeskTidy 主窗口「帮助」里查看。</p>
""",
        "tabs_files": f"""
<h2>页签与文件</h2>
<p class='purpose'>一个窗口可开多个页签，类似 Notepad++。</p>
{_op_table([
    ("文件 · 新建 / Ctrl+N", "在笔记库文件夹新建 Markdown（如未命名.md）并打开。"),
    ("页签栏空白处 · 双击", "同上，在笔记库中新建文件。"),
    ("文件 · 打开 / Ctrl+O", "打开文本、Markdown、常见代码与配置文件。"),
    ("拖入文件", "把文本 / Markdown / 代码文件拖进窗口即可打开为页签。"),
    ("文件 · 保存 / Ctrl+S", "保存当前页签。"),
    ("文件 · 另存为", "换路径或换后缀保存（如改成 .md）。"),
    ("打开笔记文件夹", "在资源管理器中打开默认笔记目录。"),
    ("关闭页签 / Ctrl+W", "关闭当前页签；有未保存修改时会询问是否保存。中键点击页签也可关闭。"),
    ("页签 · 钉子 / 右键置顶", "点击页签左侧钉子可将该页签固定到最左侧；再次点击取消。置顶状态会随会话保留。"),
    ("页签 / 笔记库文件 · 右键", "打开、重命名、置顶/取消置顶、关闭、删除（两侧菜单一致）。关闭仅关页签；删除会移出磁盘文件。"),
    ("关闭窗口", "结束本窗口；已打开内容会尽量保留到下次启动。"),
])}
""",
        "library": f"""
<h2>笔记库侧栏</h2>
<p class='purpose'>左侧文件树浏览默认笔记文件夹（可在 DeskTidy → 扩展功能里改路径）。</p>
{_op_table([
    ("查看 · 文件树 / Ctrl+Shift+E", "显示或隐藏侧栏。"),
    ("双击文件", "打开；若已在页签中则切换过去。"),
    ("+ / 空白处右键 · 新建 Markdown", "与「文件 · 新建」相同：在笔记根目录新建 <code>.md</code> 并打开。"),
    ("文件右键", "与页签右键相同：打开、重命名、置顶、关闭（已打开时）、删除。"),
    ("右键 · 重命名 / F2", "只改文件名主体，扩展名（如 .md）保持不变；已打开的页签会跟上新路径。"),
    ("右键 · 删除 / Delete", "删除文件；若已打开则同时关闭对应页签。"),
    ("搜索框 / Ctrl+Shift+F", "按文件名与正文内容搜索笔记库（大文件会跳过）。"),
    ("搜索结果", "右键菜单与文件树一致（打开 / 重命名 / 置顶 / 关闭 / 删除）。"),
    ("标题栏 + / 刷新", "「+」新建 Markdown；「刷新」重载文件夹。空白处右键可打开笔记库目录。"),
    ("空白处右键", "新建 Markdown、刷新，或在资源管理器中打开笔记文件夹。"),
    ("文件右键", "重命名、置顶、打开文件所在目录、删除；关闭页签请点页签上的 ×。"),
])}
<p class='tip'>专注模式下侧栏会自动隐藏，退出专注后按你的「文件树」开关恢复。</p>
""",
        "edit_find": f"""
<h2>编辑与查找</h2>
{_op_table([
    ("撤销 / 剪切 / 复制 / 粘贴 / 全选", "编辑菜单或常规系统快捷键。"),
    ("粘贴（Markdown）", "剪贴板含 HTML（如浏览器/Word）时，自动转成 Markdown 再粘贴；纯文本则原样粘贴。"),
    ("格式 · 自动换行", "按窗口宽度折行；可关闭以便看长行。"),
    ("查找 / Ctrl+F", "在当前页签内查找。"),
    ("替换 / Ctrl+H", "查找并替换；可作用于当前页或全部页签（见查找对话框选项）。"),
    ("F3 / Shift+F3", "查找下一个 / 上一个。"),
    ("状态栏", "显示路径、是否已修改，以及当前行、列与字数（有选区时统计选区）。"),
])}
""",
        "markdown": f"""
<h2>Markdown 三栏</h2>
<p class='purpose'>仅当当前页签文件是 <code>.md</code> / <code>.markdown</code> 时显示。源码模式下中间始终是可编辑源码；左右两栏只读。</p>
{_op_table([
    ("左 · 大纲", "根据标题列出目录，点击跳转到源码对应行。"),
    ("中 · 源码", "源码模式下的编辑区，保存的也是 Markdown 原文。"),
    ("右 · 预览", "增强预览：代码高亮、公式、Mermaid、脚注、<code>[TOC]</code>；无 WebEngine 时自动降级。"),
    ("查看 · Markdown 预览", "开关右侧预览（Ctrl+Shift+V）。"),
    ("查看 · 大纲", "开关左侧大纲（Ctrl+Shift+O）。"),
    ("查看 · 同步滚动", "源码与预览按比例联动（可关）。"),
    ("文件 · 导出 HTML / PDF", "导出当前 Markdown；PDF 需增强预览引擎。"),
    ("文件 · 导出 Word", "调用本机 Pandoc 转为 .docx；未安装时会提示安装方式。"),
    ("粘贴 / 拖入图片", "在 .md 中粘贴截图或拖入图片文件，保存到笔记旁 <code>assets/</code> 并插入相对路径。"),
    ("拖拽分隔条", "调整三栏宽度。"),
])}
<p class='tip'>普通 <code>.txt</code> 等文件不会出现大纲/预览，避免干扰纯文本编辑。</p>
""",
        "edit_modes": f"""
<h2>编辑模式</h2>
<p class='purpose'>在「查看」菜单切换 Markdown 编辑体验（需已保存为 <code>.md</code>）。</p>
{_op_table([
    ("查看 · 源码三栏", "大纲 + 源码 + 预览（默认）。"),
    ("查看 · 所见即所得", "Toast UI 可视化编辑；切换回源码或保存前会同步 Markdown 原文。需 WebEngine。"),
    ("查看 · 专注模式 / F11", "隐藏大纲、预览与状态栏，只留源码区；退出后恢复原布局。"),
    ("查看 · 打字机模式", "光标所在行尽量垂直居中，便于长文连续输入。"),
])}
<p class='tip'>专注模式会临时切回源码三栏。打字机模式仅作用于源码编辑区。</p>
""",
        "headings": f"""
<h2>标题与大纲</h2>
<p class='purpose'>大纲按 ATX 标题分级。规范写法是「井号 + 空格 + 标题」。</p>
{_op_table([
    ("一级", "<code># 标题</code>"),
    ("二级", "<code>## 标题</code>"),
    ("三级", "<code>### 标题</code>（最多六级 <code>######</code>）"),
    ("手敲漏空格", "写成 <code>###正文</code> 时，大纲与预览仍会尽量识别；建议养成空格习惯。"),
    ("点击大纲项", "光标跳到源码该行并尽量居中显示。"),
])}
<p class='tip'>预览支持列表、任务列表、引用、代码高亮、表格、链接、图片、公式、Mermaid、<code>[TOC]</code>、脚注；文首 YAML front matter 不显示为正文。</p>
""",
        "shortcuts": f"""
<h2>快捷键一览</h2>
{_op_table([
    ("Ctrl+N", "新建页签"),
    ("Ctrl+O", "打开"),
    ("Ctrl+S", "保存"),
    ("Ctrl+Shift+S", "另存为（系统标准键）"),
    ("Ctrl+W", "关闭页签"),
    ("Ctrl+F", "查找"),
    ("Ctrl+H", "替换"),
    ("F3 / Shift+F3", "查找下一个 / 上一个"),
    ("Ctrl+Shift+V", "开关 Markdown 预览"),
    ("Ctrl+Shift+O", "开关大纲"),
    ("Ctrl+Shift+E", "开关文件树侧栏"),
    ("Ctrl+Shift+F", "搜索笔记库"),
    ("F11", "专注模式"),
    ("F1", "打开本帮助"),
])}
""",
        "desktidy": f"""
<h2>与 DeskTidy 联动</h2>
<p class='purpose'>DeskNote 是独立进程，随 DeskTidy 一并安装，并共用设置与笔记目录。</p>
{_op_table([
    ("安装", "安装 DeskTidy 时会同时安装 DeskNote，无需单独勾选组件。"),
    ("启用", "在 DeskTidy → 扩展功能 → DeskNote 中开关快捷方式（不卸载程序）。"),
    ("资源管理器 · 打开方式", "文本 / Markdown / 代码类文件的「打开方式」列表中提供 DeskNote（不再出现 DeskTidy）。"),
    ("围栏「用记事本打开」", "可用 DeskNote 打开文本类文件。"),
    ("单实例", "再点一次 DeskNote 会激活已有窗口，而不是再开一份。"),
    ("主题", "跟随 DeskTidy 当前主题设置。"),
])}
""",
    }
    body = topics.get(topic_id) or topics["overview"]
    return _wrap(body)


class DeskNoteHelpDialog(QDialog):
    """Topic list + HTML help for the DeskNote window."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("helpDialog")
        self.setWindowTitle("DeskNote 帮助")
        self.resize(720, 520)
        self.setMinimumSize(520, 360)

        root = QVBoxLayout(self)
        root.setContentsMargins(12, 12, 12, 12)
        root.setSpacing(10)

        row = QHBoxLayout()
        row.setSpacing(12)

        left = QVBoxLayout()
        tip = QLabel("选择主题")
        tip.setObjectName("sectionHint")
        left.addWidget(tip)
        self._topics = QListWidget()
        self._topics.setObjectName("helpTopicList")
        self._topics.setFixedWidth(160)
        for topic_id, title in DESKNOTE_HELP_TOPICS:
            item = QListWidgetItem(title)
            item.setData(Qt.ItemDataRole.UserRole, topic_id)
            self._topics.addItem(item)
        self._topics.currentRowChanged.connect(self._on_topic_changed)
        left.addWidget(self._topics, stretch=1)
        row.addLayout(left)

        self._browser = QTextBrowser()
        self._browser.setObjectName("helpBrowser")
        self._browser.setOpenExternalLinks(False)
        row.addWidget(self._browser, stretch=1)
        root.addLayout(row, stretch=1)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)
        buttons.accepted.connect(self.accept)
        close_btn = buttons.button(QDialogButtonBox.StandardButton.Close)
        if close_btn is not None:
            close_btn.setText("关闭")
        root.addWidget(buttons)

        self._topics.setCurrentRow(0)

    def _on_topic_changed(self, row: int) -> None:
        item = self._topics.item(row)
        if item is None:
            return
        topic_id = str(item.data(Qt.ItemDataRole.UserRole) or "overview")
        self._browser.setHtml(desknote_help_html(topic_id))


def show_desknote_help(parent=None) -> None:
    """Open DeskNote-only help in the system browser (separate from DeskTidy)."""
    from src.product_pages import open_desknote_help

    if open_desknote_help():
        return
    # Fallback when the HTML template is missing.
    dlg = DeskNoteHelpDialog(parent)
    dlg.exec()
