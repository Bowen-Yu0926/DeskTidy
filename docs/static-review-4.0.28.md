# v4.0.26→v4.0.28 改动热点静态缺陷排查报告

审查范围：`src/update_checker.py`、`src/elevated_io.py`、`src/ole_file_drag.py`、`src/settings.py`、`src/win_shell.py`、`src/app.py`、`src/notepad.py`、`src/desktop_foreground_monitor.py`、`src/icon_utils.py`、`src/ui/fence_icon_item.py`、`src/ui/fence_widget.py`、`src/ui/pet_widget.py`（另核对了关联的 `src/desknote_launch.py`、`src/ui/notepad_window.py` 调用点）。

方法：按函数/类分段读取热点区段（更新检查、提权保存、OLE 多文件拖拽、前台监视、图标缓存），仅报告可从代码行直接证明的问题。未修改 src/ 下任何文件。

---

## F1 缓存结果未按运行版本重校验 — 过期提示
- 位置：`src/update_checker.py:116-136`、`src/update_checker.py:164-192`
- 问题：`_read_cache` 直接把缓存的 `status=update-available` / `latest` / `current` 原样返回，`check_for_update` 走缓存路径（`_fetch_or_cached`）时不与当前进程 `__version__` 重新比较。`UpdateCheckResult.has_update` 只看 status 字符串。
- 触发条件：用户在 6 小时 TTL 内升级了 DeskTidy（如 4.0.26→4.0.28），而缓存里仍是旧 version 的「有新版 4.0.27」结果；启动检查命中缓存。
- 影响：已升级到更新版本的用户仍收到「发现新版本 4.0.27」的虚假提示（一次性，写 update_notified_tag 后不再重复，但首次体验错误）。
- 建议修法：`_read_cache` 命中后校验 `result.current == __version__`，不一致则视为过期返回 None；或在 `notify_update_on_startup.done` 里用 `is_newer(result.latest, __version__)` 复核后再提示。
- 严重度：medium　置信度：high

## F2 shell 图标 COM 初始化线程错配
- 位置：`src/icon_utils.py:83-94`（`_ensure_shell` 中 `ole32.CoInitialize(None)`）、`src/icon_utils.py:164`（`_shell_ready` 全局）、`src/icon_utils.py:532-559`（`prefetch_shell_icon_lookups` 后台线程）、`src/icon_utils.py:426-451`（`_imagelist_icon` / `SHGetImageList`）
- 问题：`_shell_ready` 是全局标志，而 `CoInitialize` 是按线程生效。若首个调用者是后台预取线程（`prefetch_shell_icon_lookups` 的 `_worker` → `_sys_icon_index_cached` → `_ensure_shell`），该线程完成 COM 初始化后全局置 True；随后 GUI 线程调用 `SHGetImageList` / `SHGetFileInfoW` 时不再初始化 COM。且后台线程从不 `CoUninitialize`。
- 触发条件：启动早期任何一次 `prefetch_shell_icon_lookups` 先于 GUI 线程的首次图标取用执行（分页加载/分区刷新即触发）。
- 影响：GUI 线程在未初始化 COM 的 STA 环境调用 shell COM 接口，图标可能取空或在特定 shell 扩展下不稳定；后台线程 COM 泄漏至线程退出。
- 建议修法：把 `_shell_ready` 改为按线程判断（如 `threading.local` 记录已初始化线程），或规定 shell 调用仅允许 GUI 线程并让预取线程只回填索引缓存时自行 `CoInitialize`/`CoUninitialize` 配对。
- 严重度：medium　置信度：medium

## F3 提权暂存文件的明文驻留与编码重写
- 位置：`src/notepad.py:535-551`（`write_elevated_pending`）、`src/notepad.py:569-591`（`consume_elevated_pending`）、`src/ui/notepad_window.py:2263-2275`
- 问题：
  1. pending 文件把未保存正文以明文 JSON 写入 `%USERPROFILE%\.desktidy\elevated_save_pending.json`，写入发生在 `relaunch_desknote_elevated` 之前；若进程在这两步之间崩溃/断电，该明文（可能是敏感内容）永久残留，无任何清理机制（`clear_elevated_pending` 只在 UAC 拒绝路径调用）。
  2. `consume_elevated_pending` 一律 `write_text(..., encoding="utf-8")`：对原编码为 GBK/ANSI 的受保护文件（典型如中文 hosts），保存会静默把整个文件重写为 UTF-8，改变系统文件编码。
- 触发条件：1) UAC 确认后、提权进程消费前进程异常退出；2) 对非 UTF-8 编码的受保护文件走提权保存。
- 影响：1) 敏感明文落盘残留；2) 系统文件编码被静默改写，可能破坏依赖 ANSI 编码的工具链。
- 建议修法：1) pending 写入前记录、提权消费/启动时对过期（如 >1h）pending 自动删除；或至少在下次正常启动时清除孤儿 pending。2) `write_elevated_pending` 时记录原文件字节编码/原始字节，`consume_elevated_pending` 按原编码写回（无法解码时提示而不是重写）。
- 严重度：medium　置信度：high

## F4 提权重启失败后互斥体重取结果被忽略
- 位置：`src/ui/notepad_window.py:2267-2289`；关联 `src/desknote_launch.py:160-180`
- 问题：`_relaunch_elevated_save` 先 `release_notepad_instance()` 再 relaunch；失败分支重新 `try_acquire_notepad_instance()` 但不检查返回值。若此时已有第三实例抢到 deskNote 互斥体（释放窗口期竞争），`try_acquire` 返回 BUSY/FAILED，当前实例在**无互斥体**的情况下继续运行，与第三实例形成双开。
- 触发条件：UAC 拒绝/ShellExecute 失败，且在释放与重取之间有另一 deskNote 实例完成启动。
- 影响：两个 deskNote 主窗口并存，会话文件/设置互相覆盖。
- 建议修法：检查 `try_acquire_notepad_instance()` 结果；非 ACQUIRED 时按 BUSY 处理（提示并退出当前实例或转发内容到新实例），不得静默继续。
- 严重度：medium　置信度：medium

## F5 防抖落盘异常被吞、待写设置丢失
- 位置：`src/settings.py:753-763`（`_flush_pending_settings`）、`src/settings.py:702-750`（`_save_settings_now` 抛 OSError/ValueError）、`src/settings.py:802`（atexit）
- 问题：`_flush_pending_settings` 在 `threading.Timer` 守护线程里直接调 `_save_settings_now`，无异常捕获。磁盘满/权限/锁冲突时异常仅打印到线程 stderr，`_PENDING_SETTINGS` 已被置 None，本次写入永久丢失且无重试；`atexit` 的 `flush_settings` 同样可能带异常退出。
- 触发条件：防抖窗口内目标盘出现瞬时写失败（杀毒占用、磁盘满、 roaming profile 延迟）。
- 影响：用户当次布局/设置改动静默丢失，与「原子写 + 备份轮转」的设计目标不符。
- 建议修法：`_flush_pending_settings` 内 try/except：失败时保留 `_PENDING_SETTINGS` 并安排短延迟重试（限次），最终失败时记录 `app_logging`。
- 严重度：medium　置信度：high

## F6 坏配置恢复路径未捕获 copy 异常
- 位置：`src/settings.py:296-303`（`load_settings` 中 `shutil.copy(DEFAULT_SETTINGS, SETTINGS_FILE)`）
- 问题：用户 settings.json 非 dict 时，先备份再 `shutil.copy(DEFAULT_SETTINGS, SETTINGS_FILE)` 重置；copy 未包 try。若原文件被占用（同步盘/杀毒句柄）或 DEFAULT_SETTINGS 不可读，`load_settings` 直接抛 OSError，主界面启动中断。
- 触发条件：settings.json 损坏 + SETTINGS_FILE 被瞬时锁定。
- 影响：本应自愈的路径变成启动崩溃。
- 建议修法：重置 copy 失败时降级为「读内存 defaults 返回」并记日志，而不是向上抛。
- 严重度：low　置信度：high

## F7 OLE 拖拽结束后 COM 释放顺序问题
- 位置：`src/ole_file_drag.py:346-372`（`do_file_ole_drag` finally 块）
- 问题：`finally` 中先 `OleUninitialize()` 再返回；局部 `_alive = (data, source)` 持有的 pywin32 `data` 对象在函数返回后由 GC 释放，其 `Release()` 发生在 apartment 已被 `OleUninitialize` 之后。
- 触发条件：每次外部 OLE 拖拽（fence_icon_item.py:2953 等 3 处调用点）。
- 影响：释放序列违反 COM 套间规则，通常无害但可能在特定 shell 数据对象实现下触发访问冲突/句柄告警。
- 建议修法：`finally` 中先 `del _alive`/显式置 None 并 `gc.collect()` 释放 data 对象，再 `OleUninitialize()`。
- 严重度：low　置信度：medium

## F8 OleInitialize 失败 HRESULT 未走 CoInitialize 兜底
- 位置：`src/ole_file_drag.py:294-302`
- 问题：`OleInitialize` 返回失败 HRESULT（如 `RPC_E_CHANGED_MODE` 0x80010106）时不抛异常，`ole_owned=False` 后直接继续；`pythoncom.CoInitialize()` 兜底只写在 `except Exception` 分支（实际几乎不可达）。此线程后续 `_shell_data_object` 的 COM 调用依赖 pywin32 自身初始化，失败时整个拖拽静默返回 0。
- 触发条件：宿主线程以 MTA 初始化过 COM（如嵌入了其他 COM 宿主逻辑）时执行拖拽。
- 影响：拖拽功能在该线程静默失效，无日志。
- 建议修法：检查 `hr < 0` 时走 `pythoncom.CoInitialize()` 兜底（并按 S_FALSE/错误码决定是否 Uninitialize），失败时记 warning。
- 严重度：low　置信度：medium

## F9 拖拽看门狗可能向用户会话注入按键的窗口期
- 位置：`src/ole_file_drag.py:255-276`（`_escape_watchdog`）、`src/ole_file_drag.py:365-366`
- 问题：看门狗循环每轮先查 `stop.is_set()` 再注入 `keybd_event(ESC)` + `mouse_event`；`stop.set()` 在 `DoDragDrop` 返回后的 finally 里执行，二者存在竞态——看门狗已越过检查点时，会在拖拽已正常结束后仍向全局会话注入一轮 ESC/鼠标移动。`SetCursorPos` 同理。
- 触发条件：deadline 到达瞬间拖拽恰好结束。
- 影响：用户前台应用收到一次多余的 ESC 键（可能取消对话框/退出全屏）。
- 建议修法：注入前用 `GetAsyncKeyState(VK_LBUTTON)` 复核按键仍按下，或每轮注入前再查一次 stop 并把注入序列改小步检查。
- 严重度：low　置信度：medium

## F10 图标预取线程无并发上限
- 位置：`src/icon_utils.py:532-559`（`prefetch_shell_icon_lookups`）
- 问题：每次调用无条件 `threading.Thread(...).start()`，无在飞标志、无队列合并；缓存未命中项多的刷新（打开含大量 .lnk 的分区、切换页面）会同时派生多个线程并发执行 `SHGetFileInfoW` / `SHParseDisplayName`。
- 触发条件：短时间内多次刷新/分页切换，且包含冷缓存网络盘路径（SMB 上 SHGetFileInfo 可达秒级）。
- 影响：线程数瞬时膨胀、shell 调用叠加放大卡顿与句柄占用；与文件内注释「no recursive walk stalls the UI」的性能目标相悖。
- 建议修法：模块级单一 `_PREFETCH_INFLIGHT` 标志或固定 worker + 任务队列合并请求。
- 严重度：low　置信度：medium

## F11 提权参数未规范化相对路径
- 位置：`src/elevated_io.py:63-85`（`relaunch_desknote_elevated`）；调用点 `src/ui/notepad_window.py:2275`
- 问题：`params` 中的 `"{target}"` 原样拼接，未 `resolve()`。当前实现调用点传入的是绝对 Path，但函数契约（`Path | str`）允许相对路径；提权子进程工作目录为 `exe.parent`，相对路径会在错误目录下解析，`consume_elevated_pending` 的 `Path(target).resolve()` 比对失败，静默返回 False（保存丢失且无提示）。
- 触发条件：未来调用方传入相对路径（当前无；属边界防御）。
- 影响：提权保存静默失败。
- 建议修法：入口处 `target = Path(target).resolve()` 后再拼参数。
- 严重度：low　置信度：low

## F12 OLE 多文件分组对盘根项的空父 PIDL
- 位置：`src/ole_file_drag.py:127-144`（`_shell_data_object` 分组）
- 问题：`parent = tuple(pidl[:-1])`，对盘根（如 `C:\` 下项）父 PIDL 为空；`SHCreateDataObject(list(), ...)` 的空父路径在部分 pywin32/shell 版本下抛错。当前会落入 except → 继续尝试下一组或 ShellItemArray 兜底，功能可用但依赖兜底；若唯一一组恰为盘根且兜底也失败，整批拖拽静默返回 0（无部分成功，行为安全但无提示）。
- 触发条件：多文件拖拽且所有源都在同一盘根目录且 `SHCreateShellItemArrayFromIDLists` 亦失败（罕见）。
- 影响：拖拽无效果、无用户可见原因。
- 建议修法：空父组直接跳过 `SHCreateDataObject` 分支，走 ShellItemArray；失败时向调用方返回可区分的失败值以便 toast。
- 严重度：low　置信度：low

---

## 结论
未发现 blocker/high 级缺陷。v4.0.28 新增的四块核心逻辑（更新检查器缓存与 in-flight 保护、提权保存的 consume-before-mutex 设计、OLE 拒绝部分成功的多文件策略、前台监视的 edge 判定）主体健壮；上述 12 条以 medium 级边界/资源问题为主，建议优先处理 F1（缓存版本重校验）、F3（pending 明文与编码）、F5（防抖落盘异常吞没）。

涉及行号均已核对至当前源码（2026-10-13 版本）。
