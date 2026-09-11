"""In-app help — one topic per feature; all related ops live together."""

from __future__ import annotations

from src.hotkey_manager import HOTKEY_LABELS
from src.i18n import APP_NAME_ZH

# (topic_id, title) — feature-centric; avoid splitting one feature across chapters.
HELP_TOPICS: list[tuple[str, str]] = [
    ("overview", "总览"),
    ("fences", "桌面分区"),
    ("pages", "分页"),
    ("public", "公共区域"),
    ("snapshot", "布局快照"),
    ("screenshot", "区域截图"),
    ("record", "桌面录屏"),
    ("file_search", "文件搜索"),
    ("notepad", "记事本"),
    ("minutes", "会议纪要"),
    ("calculator", "计算器"),
    ("todos", "桌面待办"),
    ("pet", "桌面宠物"),
    ("wallpaper", "壁纸"),
    ("folders", "分页文件夹"),
    ("settings", "设置与备份"),
]

# Old topic ids → current (tests / bookmarks / tray).
_TOPIC_ALIASES: dict[str, str] = {
    "organize": "fences",
    "capture": "screenshot",
    "hotkeys": "overview",
    "shell": "fences",
    "extensions": "overview",
}

_HOTKEY_DEFAULTS = {
    "organize": "Ctrl+Shift+O",
    "toggle_fences": "Ctrl+Shift+F",
    "toggle_icons": "Ctrl+Shift+H",
    "peek_fences": "Ctrl+Space",
    "page_next": "Ctrl+Shift+Right",
    "page_prev": "Ctrl+Shift+Left",
    "screenshot": "F1",
    "show_screenshot": "F2",
    "screen_record": "F3",
    "file_search": "F4",
    "calculator": "Ctrl+Alt+C",
}


def _resolve_topic(topic_id: str) -> str:
    return _TOPIC_ALIASES.get(topic_id, topic_id)


def _hotkey(action: str, settings: dict | None = None) -> str:
    hotkeys = (settings or {}).get("hotkeys") if isinstance(settings, dict) else None
    if not isinstance(hotkeys, dict):
        hotkeys = {}
    key = str(hotkeys.get(action) or _HOTKEY_DEFAULTS.get(action) or "").strip()
    return key or "（未绑定）"


def _hotkey_rows(actions: list[str], settings: dict | None = None) -> str:
    rows = []
    for action in actions:
        label = HOTKEY_LABELS.get(action, action)
        rows.append(f"<tr><td>{label}</td><td><code>{_hotkey(action, settings)}</code></td></tr>")
    return (
        "<table class='op-table' cellspacing='0' cellpadding='6'>"
        "<tr><th align='left'>快捷键</th><th align='left'>当前绑定</th></tr>"
        + "".join(rows)
        + "</table>"
        "<p class='tip'>在「设置 → 全局快捷键」或「扩展功能」对应卡片中修改；留空表示不绑定。"
        "勾选「分页栏 / 托盘显示」只控制入口显隐，快捷键始终可用。</p>"
    )


def _op_table(rows: list[tuple[str, str]]) -> str:
    body = "".join(f"<tr><td>{op}</td><td>{effect}</td></tr>" for op, effect in rows)
    return (
        "<table class='op-table' cellspacing='0' cellpadding='6'>"
        "<tr><th align='left'>操作 / 入口</th><th align='left'>作用</th></tr>"
        + body
        + "</table>"
    )


def _topic_body(topic_id: str, settings: dict | None = None) -> str:
    """Inner HTML for one topic (no document chrome)."""
    tid = _resolve_topic(topic_id)
    bodies: dict[str, str] = {
        "overview": f"""
<h2>{APP_NAME_ZH} 是什么</h2>
<p class='purpose'>在桌面上用<strong>分区</strong>分类显示文件。文件仍留在系统桌面文件夹，
分区只做「虚拟钉选」——归类、查找，不改变真实路径。删除分区不会删文件。</p>

<h3>怎么查说明</h3>
<p>左侧按<strong>功能</strong>分章：每个功能把用途、桌面操作、浮标/托盘入口、快捷键和相关设置写在同一页，
避免同一件事散落在多处。</p>
{_op_table([
    ("桌面分区", "分区、一键整理、文件夹门户、分区右键与相关热键。"),
    ("分页", "多套布局切换；右侧浮标栏怎么用。"),
    ("公共区域", "从分区拖出的图标如何显示。"),
    ("布局快照", "保存 / 预览 / 还原整套分区布局。"),
    ("区域截图 / 录屏 / 文件搜索 …", "各自成章，含快捷键与扩展开关说明。"),
    ("桌面宠物", "桌面小伴侣：卫衣少年；摸摸 / 等待 / 跌落；拖桌面/分区图标到身上会揉进回收站；可调大小。"),
    ("设置与备份", "主题、开机启动、导入导出、数据目录。"),
])}

<p class='tip'>第一次安装后，桌面会依次出现小提示卡片。之后可在帮助页点「入门指引」再看一遍。
侧栏「帮助」、托盘「使用说明」打开的是 <strong>DeskTidy</strong> 介绍与手册；
DeskNote 请在 DeskNote 窗口按 F1 打开<strong>另一份</strong>帮助。</p>
""",
        "fences": f"""
<h2>桌面分区</h2>
<p class='purpose'>分区是桌面上的分类容器：把图标<strong>钉选</strong>进去显示，文件仍在系统桌面。
可拖动、缩放、折叠；也可用规则<strong>一键整理</strong>自动钉选。</p>

<h3>一键整理</h3>
<p>按当前分页下各分区的匹配规则（软件 / 文档等），把桌面上符合条件的项钉进对应分区。
白名单路径会被跳过。</p>
{_op_table([
    ("主窗口「整理」· 一键整理", "立即按规则钉选；不移动文件。"),
    ("托盘 · 一键整理", "后台整理，效果相同。"),
    ("主窗口「整理」· 白名单", "指定永不参与自动整理的路径。"),
    ("设置 · 启动/监视自动整理", "可选：启动时或桌面变化时自动整理（见设置页）。"),
])}
{_hotkey_rows(["organize"], settings)}

<h3>分区上的操作</h3>
{_op_table([
    ("拖入图标 / 文件", "钉选到该分区；文件仍在桌面原位置。"),
    ("拖出到桌面空白", "取消钉选；图标变为浮标（是否全分页可见见「公共区域」）。"),
    ("拖动标题栏 / 边缘", "移动位置或调整大小。"),
    ("折叠按钮", "收起 / 展开分区内容。"),
    ("图标 · 右键", "系统菜单 + 移出分区 / 移动到分区 / 移动到分页 / 整理白名单 / 记事本打开等。"),
    ("空白处 · 右键", "系统资源管理器菜单（新建、粘贴、排序视图等）。"),
    ("标题栏 · 右键", "刷新列表；解散分区（图标变浮标，不删文件）。"),
    ("系统桌面 · 右键 → DeskTidy", "可开关：新建分区、刷新等（设置里配置）。"),
])}
{_hotkey_rows(["toggle_fences", "peek_fences", "toggle_icons"], settings)}
<p>分区窗口<strong>不抢系统焦点</strong>。复制 / 粘贴 / 删除：选中分区或公共区图标、或鼠标在其上时由 DeskTidy 接管；
焦点在其它程序时不抢键。</p>

<h3>文件夹门户</h3>
<p class='purpose'>分区可绑定真实文件夹，内容镜像该文件夹。
与虚拟钉选不同：拖入 / 粘贴会<strong>真正复制或移动</strong>磁盘文件。</p>
{_op_table([
    ("分区设置 · 门户路径", "指定要镜像的文件夹。"),
    ("拖入 / 拖出", "按系统拖放规则在门户文件夹与目标之间复制或移动。"),
])}
""",
        "pages": f"""
<h2>分页</h2>
<p class='purpose'>把桌面分成多套独立布局（如「工作」「娱乐」），每页有自己的分区和未归类浮标。
切换分页只改变 DeskTidy <strong>显示哪一套</strong>，不会切换 Windows 虚拟桌面。</p>

<p><strong>与 Windows 虚拟桌面：</strong>本分页 ≠「任务视图 / Win+Ctrl+←→」。两边可同时用，互不影响。</p>

<h3>怎么切换</h3>
{_op_table([
    ("右侧浮标 · 单击页名", "切换到该分页。"),
    ("右侧浮标 · 右键页名", "在该分页新建分区。"),
    ("主窗口 · 桌面分区", "左栏分页卡片、右栏分区卡片（类似布局快照）；编辑分区可调透明度滑条。"),
    ("未归入分区的新文件", "以浮标形式出现在<strong>当前分页</strong>。"),
])}
{_hotkey_rows(["page_prev", "page_next"], settings)}

<h3>右侧浮标栏</h3>
<p>分页名下方可显示扩展按钮（在「扩展功能」勾选「在分页栏显示」后出现）。
各按钮的完整用法见对应功能章：录屏、纪要、计算器、待办、分页文件夹。记事本改为 DeskNote 快捷方式，见「记事本」章。</p>
{_op_table([
    ("录屏 / note / 纪要 / 计算器 / 待办 / 文件夹", "入口在浮标栏；说明分别见左侧同名章节。"),
])}
""",
        "public": f"""
<h2>公共区域</h2>
<p class='purpose'>存放「已从分区取出、但仍在桌面显示」的图标。
是否全分页共享，由设置中的「启用公共区域」决定。</p>

{_op_table([
    ("启用公共区域", "从分区拖出的图标在所有分页均可见。"),
    ("关闭公共区域", "拖出后只在<strong>当前分页</strong>显示。"),
    ("拖出后落点", "按屏幕左缘网格排列；会跳过被分区占用的格子。"),
    ("拖动浮标", "松手后吸附到最近空闲格。"),
    ("拖回分区", "重新钉选。"),
    ("浮标 · 右键", "系统菜单；启用公共区域时不提供「移动到分区 / 移动到分页」（拖回分区仍可钉选）。"),
])}
<p class='tip'>相关设置项在「设置与备份」；分区拖入拖出见「桌面分区」。</p>
""",
        "snapshot": f"""
<h2>布局快照</h2>
<p class='purpose'>把当前分区的位置、大小、样式等保存成快照，便于备份或一键还原。</p>
{_op_table([
    ("主窗口 · 布局快照 · 保存", "弹出命名框，默认当天时间戳，可改名后保存布局与外观。"),
    ("卡片 · 分区/图标数量条", "顶部显示该快照含多少分区与钉选图标。"),
    ("卡片 · 预览", "放大查看布局图。"),
    ("卡片 · 应用 / 双击卡片", "将桌面还原为该快照。"),
    ("卡片 · 删除", "删除该备份，不影响当前桌面。"),
])}
""",
        "screenshot": f"""
<h2>区域截图</h2>
<p class='purpose'>冻结画面后框选区域，可复制、存文件或贴到桌面。快捷键始终可用；
托盘始终有截图入口。贴图上限等在「扩展功能 · 区域截图」配置。</p>

{_op_table([
    ("快捷键 · 区域截图（默认 F1）", "冻结画面后框选；可复制、保存或贴图。"),
    ("快捷键 · 显示贴图（默认 F2）", "把最近一张截图贴到桌面；可叠多张。"),
    ("托盘 · 区域截图", "等同 F1。"),
    ("贴图 · 右键", "另存为图片或复制到剪贴板。"),
    ("扩展功能 · 自动贴图 / 同时张数", "截图后是否自动贴图；最多同时 5 张。"),
])}
{_hotkey_rows(["screenshot", "show_screenshot"], settings)}
<p>截图会先冻结画面（含右键菜单），再进入框选，避免误触 Escape 取消系统菜单。</p>
""",
        "record": f"""
<h2>桌面录屏</h2>
<p class='purpose'>录制桌面画面为视频。需 FFmpeg（安装版已内置，也可 <code>winget install Gyan.FFmpeg</code>）。
默认保存在安装目录下的「录屏」文件夹，可在扩展功能中改路径。</p>

{_op_table([
    ("快捷键 · 录屏（默认 F3）", "选屏后开始；再次操作结束。"),
    ("浮标「录屏」· 单击", "开始 / 结束（需在扩展功能勾选分页栏显示）。"),
    ("浮标「录屏」· 右键", "打开录屏保存目录。"),
    ("托盘 · 录屏", "后台开始 / 结束。"),
    ("录制中 · 右上角 REC", "点「结束录制」停止。"),
    ("结束对话框", "可改文件名后「保留」；「取消」删除。关窗或 Esc 也会保留。"),
    ("扩展功能 · 保存目录", "自定义录屏文件夹。"),
])}
{_hotkey_rows(["screen_record"], settings)}
""",
        "file_search": f"""
<h2>文件搜索</h2>
<p class='purpose'>按文件名快速定位。使用开源工具 <code>fd</code>（安装版已内置）。</p>

{_op_table([
    ("快捷键 · 文件搜索（默认 F4）", "打开搜索框。"),
    ("本地搜索", "默认搜桌面、文档、下载及当前已打开的资源管理器文件夹。"),
    ("全局搜索", "本地结果不足时可扫本地磁盘（较慢）。"),
    ("扩展功能", "配置快捷键与搜索范围相关选项。"),
])}
{_hotkey_rows(["file_search"], settings)}
""",
        "notepad": f"""
<h2>记事本（DeskNote）</h2>
<p class='purpose'>DeskNote 是独立进程的记事本，使用说明<strong>单独成页</strong>，不在本手册展开。</p>
{_op_table([
    ("打开 DeskNote 帮助", "在 DeskNote 窗口按 F1，或打开 DeskNote 介绍页（介绍 + 使用手册）。"),
    ("DeskNote 快捷方式", "桌面或开始菜单；扩展功能里可开关快捷方式。"),
    ("图标右键 · 用记事本打开", "文本类文件用 DeskNote 打开。"),
])}
<p class='tip'>DeskTidy「帮助」只讲桌面整理；笔记编辑、Markdown、页签等请看 DeskNote 自己的帮助。</p>
""",
        "minutes": f"""
<h2>会议纪要</h2>
<p class='purpose'>在指定文件夹生成 Word（.docx）格式纪要，便于会后整理。默认保存在安装目录下的「纪要」文件夹。</p>

{_op_table([
    ("浮标「纪要」· 双击", "打开或新建纪要文档（需勾选分页栏显示）。"),
    ("浮标「纪要」· 右键", "打开纪要所在文件夹。"),
    ("扩展功能 · 会议纪要", "开关入口；留空保存目录则使用安装目录「纪要」，也可自定义。"),
])}
""",
        "calculator": f"""
<h2>计算器</h2>
<p class='purpose'>调用系统计算器；再次操作可收起。快捷键始终可用。</p>

{_op_table([
    ("快捷键（默认 Ctrl+Alt+C）", "打开或收起系统计算器。"),
    ("浮标「计算器」· 双击", "同上（需勾选分页栏显示）。"),
    ("扩展功能 · 计算器", "仅控制分页栏是否显示入口。"),
])}
{_hotkey_rows(["calculator"], settings)}
""",
        "todos": f"""
<h2>桌面待办</h2>
<p class='purpose'>桌面上的悬浮待办便签：添加、勾选完成、删除；可拖动标题栏移动。</p>

{_op_table([
    ("浮标「待办」· 单击", "显示或置顶待办面板（需勾选分页栏显示）。"),
    ("面板内 · 添加 / 勾选 / 删除", "维护待办列表。"),
    ("扩展功能 · 桌面待办", "开关分页栏入口。"),
])}
<p class='tip'>数据保存在用户配置目录下的 <code>todos.json</code>。</p>
""",
        "pet": f"""
<h2>桌面宠物</h2>
<p class='purpose'>桌面上的小伴侣：互动为摸摸、玩手机/睡觉、跌落；卫衣少年休息时会在玩手机与睡觉之间轮流切换。</p>

{_op_table([
    ("桌面宠物菜单 · 启用桌面宠物", "启用后在桌面显示宠物；隐藏后可在本页点「显示到桌面」找回。"),
    ("桌面宠物菜单 · 可互动动作", "可勾选：摸摸、等待、跌落（默认全开）。未勾选的动作不会出现在「⋯」菜单；「隐藏」始终可用。"),
    ("桌面宠物菜单 · 宠物大小", "滑块调节桌面宠物显示比例（约 50%～160%，默认 100%）。在设置页调节时会即时预览，不会把分区抬到设置窗上面。"),
    ("桌面宠物菜单 · 形象选择", "卫衣少年：玩手机 / 睡觉为两个动作，等待时自动轮流；其他形象仍为单一等待姿态。"),
    ("桌面宠物菜单 · 显示到桌面", "设置页一键重新显示（隐藏后若找不到可用此入口）。"),
    ("自主行为", "靠近鼠标会转头理你；久不互动时卫衣少年会轮流「玩手机」与「睡觉」；不会自己走动（跌落需手动点「⋯」或甩出）。"),
    ("宠物上的浮标栏", "默认把右侧浮标栏整条放到宠物头上（分页、文件夹快捷方式、录屏、纪要、计算器、待办）。关闭该项后浮标栏回到屏幕右侧。分页左键切换、右键新建分区；录屏/纪要右键打开对应文件夹。记事本请用 DeskNote 快捷方式打开。"),
    ("摸摸", "单击宠物，或在「⋯」里点「♡」。会开心弹跳并冒爱心。"),
    ("等待", "卫衣少年：「⋯」里可点「玩手机」或「睡觉」，休息中会轮流切换；点「起」站起来。其他形象仍为「等 / 起」。"),
    ("跌落", "「⋯」里点「落」，或拖动后甩出：揪着衣领拖动，松开后抛物线跌落（Shimeji 式）。"),
    ("拖文件到宠物", "把桌面浮标或分区里的图标拖到宠物身上：揉成纸团丢进垃圾桶，文件进系统回收站（可还原）。不要拖到头上的分页浮标。从资源管理器文件夹拖到桌面空白处 / 分区仍归 Explorer，不会被宠物抢走。"),
    ("拖动宠物", "按住拖动时显示被揪领姿态；甩出则抛物线落地，松手在空中也会落下。"),
    ("右键 / 点「⋯」图标", "展开已勾选的动作图标；「隐藏」始终可用。"),
])}
<p class='tip'>互动动作、大小与位置保存在用户配置中；关闭扩展后宠物不会显示。卫衣少年：玩手机与睡觉为两个菜单项，休息时自动轮流。</p>
""",
        "wallpaper": f"""
<h2>壁纸</h2>
<p class='purpose'>从壁纸库更换桌面背景，并可一键还原系统原先壁纸。</p>

{_op_table([
    ("主窗口 · 扩展功能 · 壁纸", "浏览壁纸库、应用、还原。"),
])}
""",
        "folders": f"""
<h2>分页文件夹</h2>
<p class='purpose'>在右侧浮标栏添加常用文件夹快捷方式，双击打开。</p>

{_op_table([
    ("扩展功能 · 添加文件夹", "写入分页栏快捷方式列表。"),
    ("浮标 · 文件夹 · 双击", "打开该文件夹。"),
    ("浮标 · 文件夹 · 右键", "在资源管理器中定位。"),
    ("扩展功能 · 删除", "移除该快捷方式（不删真实文件夹）。"),
])}
""",
        "settings": f"""
<h2>设置与备份</h2>
<p class='purpose'>应用外观、启动行为，以及配置的导出与迁移。
各扩展功能自己的开关、路径、热键，优先在「扩展功能」对应卡片里改；
全局热键也可在本页「全局快捷键」统一改。</p>

{_op_table([
    ("主题", "切换界面配色。"),
    ("开机启动", "登录 Windows 后自动运行。"),
    ("双击桌面隐藏系统图标", "只显示分区与浮标。"),
    ("关闭到托盘", "关主窗口时进托盘继续运行。"),
    ("启用公共区域", "拖出分区的图标是否全分页共享（详见「公共区域」）。"),
    ("全局快捷键", "整理、分区、分页、截图、录屏、搜索、计算器等。"),
    ("整理白名单 / 自动整理相关", "与「桌面分区 · 一键整理」配合。"),
    ("导出 / 导入配置", "备份或迁移整套设置与分区布局。"),
    ("托盘 · 显示主窗口 / 帮助 / 关于", "不打开侧栏也能进管理与说明。"),
])}

<h3>文件位置</h3>
<ul>
<li>用户配置：<code>%USERPROFILE%\\.desktidy\\</code></li>
<li>默认安装目录：<code>%LOCALAPPDATA%\\Programs\\DeskTidy</code></li>
<li>笔记 / 录屏默认子文件夹：安装目录下「笔记」「录屏」</li>
</ul>
<p class='tip'>卸载时可选择是否清除本地数据。确认后将删除配置目录、分区存储、布局快照，
以及安装目录下默认的「笔记」「录屏」；自定义路径不会自动删，卸载后会提示供手动清理。</p>
""",
    }
    return bodies.get(tid) or "<p>暂无该主题说明。</p>"


def _help_document_style() -> str:
    return """
body{
  font-family:'Microsoft YaHei UI','Segoe UI',sans-serif;
  font-size:13.5px;line-height:1.65;color:#1F2937;
  margin:0;padding:4px 2px;
}
h1{font-size:22px;margin:0 0 8px 0;color:#0F172A;font-weight:800;letter-spacing:0.2px;}
h2{font-size:16px;margin:18px 0 8px 0;padding-bottom:6px;
  color:#111827;border-bottom:1px solid #E5E7EB;font-weight:700;}
h3{font-size:14px;margin:14px 0 6px 0;color:#374151;font-weight:700;}
.purpose{color:#4B5563;margin:0 0 14px 0;padding:10px 12px;
  background:#F9FAFB;border-left:3px solid #6366F1;border-radius:0 6px 6px 0;}
.tip{color:#6B7280;font-size:13px;margin:12px 0 0 0;}
.chapter{margin:0 0 6px 0;}
p,ul{margin:0 0 10px 0;}
li{margin:4px 0;}
code{background:#F3F4F6;padding:1px 6px;border-radius:4px;font-size:12.5px;}
table{border-collapse:collapse;width:100%;margin:8px 0 12px 0;}
table.op-table th,table.op-table td{
  border-bottom:1px solid #E5E7EB;text-align:left;padding:8px 6px;vertical-align:top;}
table.op-table th{color:#6B7280;font-weight:600;width:34%;}
table.op-table td:first-child{font-weight:600;color:#111827;}
th,td{border-bottom:1px solid #E5E7EB;text-align:left;padding:6px 4px;}
th{color:#6B7280;font-weight:600;}
"""


def wrap_help_html(body: str) -> str:
    return (
        "<html><head><meta charset='utf-8'><style>"
        + _help_document_style()
        + "</style></head><body>"
        + body
        + "</body></html>"
    )


def topic_inner_html(topic_id: str, settings: dict | None = None) -> str:
    """Inner HTML for one handbook topic (no document chrome)."""
    return _topic_body(topic_id, settings)


def help_html(topic_id: str, settings: dict | None = None) -> str:
    """Return HTML document for a help topic."""
    return wrap_help_html(_topic_body(topic_id, settings))


def about_html() -> str:
    """About page for the embedded help panel."""
    from src._version import __version__

    body = f"""
<h2>关于 {APP_NAME_ZH}</h2>
<p>版本 <code>{__version__}</code></p>
<p class='purpose'>桌面分区整理工具：用虚拟钉选分类显示，文件仍留在系统桌面文件夹。</p>
<ul>
<li>录屏依赖 FFmpeg（安装版已内置）</li>
<li>文件搜索依赖 fd（安装版已内置）</li>
<li>笔记与录屏默认保存在安装目录下的「笔记」「录屏」文件夹</li>
</ul>
"""
    return wrap_help_html(body)


def operation_manual_html(settings: dict | None = None) -> str:
    """All feature chapters in one HTML document (export / tests / offline dump)."""
    parts = [
        f"<h1>{APP_NAME_ZH} 使用说明</h1>",
        "<p class='purpose'>按<strong>功能</strong>分章：每个功能的用途、操作入口、快捷键与相关设置写在一起。"
        "主窗口「帮助」会在系统浏览器打开介绍与使用手册合一页；首次安装时桌面会弹出分步小提示。</p>",
    ]
    for topic_id, _title in HELP_TOPICS:
        parts.append(f"<div class='chapter'>{_topic_body(topic_id, settings)}</div>")
    return wrap_help_html("".join(parts))


def all_help_plain_text(settings: dict | None = None) -> str:
    """Flat text dump (for tests / export)."""
    chunks: list[str] = []
    for topic_id, title in HELP_TOPICS:
        chunks.append(f"【{title}】")
        html = help_html(topic_id, settings)
        text = (
            html.replace("<li>", "• ")
            .replace("</li>", "\n")
            .replace("<br/>", "\n")
            .replace("<br>", "\n")
        )
        for tag in (
            "html",
            "head",
            "style",
            "body",
            "h1",
            "h2",
            "h3",
            "p",
            "ul",
            "div",
            "strong",
            "code",
            "table",
            "tr",
            "td",
            "th",
            "meta",
        ):
            text = text.replace(f"<{tag}>", "").replace(f"</{tag}>", "\n")
        chunks.append(text.strip())
        chunks.append("")
    return "\n".join(chunks).strip()
