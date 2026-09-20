"""Desktop floating account vault panel."""

from __future__ import annotations

from typing import Any

from PyQt6.QtCore import QByteArray, QEvent, QMimeData, QPoint, QRect, QSize, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QDrag, QGuiApplication, QMouseEvent
from PyQt6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from src.account_vault import (
    account_vault_api_base,
    account_vault_refresh_interval_ms,
    clear_session,
    load_session,
    mask_login,
    normalize_display_name,
    normalize_login,
    reorder_credential_ids,
    save_session,
    session_display_name,
)
from src.account_vault_api import VaultApiClient, VaultApiError
from src.settings import save_settings
from src.ui.account_vault_async import VaultAsyncRunner
from src.ui.screen_snap import rect_visible_on_any_screen, snap_geometry, work_screen
from src.ui.styles import get_theme_palette, normalize_theme, _hex_to_rgba
from src.ui.toast import show_toast
from src.win_shell import configure_desktop_overlay

_DEFAULT_W = 420
_MIN_W = 340
_MAX_W = 560
_CARD_ALPHA = 0.92
_VAULT_DRAG_MIME = "application/x-desktidy-vault-cred"
_TITLE_ALPHA = 0.88
_BORDER_ALPHA = 0.55
_ANCHOR_GAP = 12


def panel_pos_beside_anchor(
    anchor: QRect,
    panel_size: QSize,
    screen: QRect,
    *,
    gap: int = _ANCHOR_GAP,
) -> QPoint:
    """Prefer left of buoy, then right; clamp into *screen* work area."""
    w = max(1, int(panel_size.width()))
    h = max(1, int(panel_size.height()))
    y = anchor.center().y() - h // 2
    # Left of buoy first (panel opens toward desktop center when buoy is on the right).
    x = anchor.x() - w - gap
    if x < screen.x():
        x = anchor.x() + anchor.width() + gap
    if x + w > screen.x() + screen.width():
        x = screen.x() + screen.width() - w
    if y + h > screen.y() + screen.height():
        y = screen.y() + screen.height() - h
    if y < screen.y():
        y = screen.y()
    x = max(screen.x(), min(x, screen.x() + screen.width() - w))
    y = max(screen.y(), min(y, screen.y() + screen.height() - h))
    return QPoint(int(x), int(y))


class AccountVaultAuthDialog(QDialog):
    """Login / register dialog for the cloud account vault."""

    def __init__(self, api_base: str, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("账号管理 · 登录 / 注册")
        self.setModal(True)
        self.resize(380, 320)
        self._api_base = api_base
        self.session: dict[str, Any] | None = None
        self._async = VaultAsyncRunner(self)

        layout = QVBoxLayout(self)
        tip = QLabel("请使用手机号或邮箱登录；没有账号可切换到「注册」。")
        tip.setWordWrap(True)
        tip.setObjectName("appSubtitle")
        layout.addWidget(tip)
        tabs = QTabWidget()
        layout.addWidget(tabs)

        self._login_form = self._build_form(register=False)
        self._reg_form = self._build_form(register=True)
        tabs.addTab(self._login_form["widget"], "登录")
        tabs.addTab(self._reg_form["widget"], "注册")

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("确定")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("取消")
        buttons.accepted.connect(self._submit)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
        self._tabs = tabs
        self._buttons = buttons
        self._status = QLabel("")
        self._status.setObjectName("appSubtitle")
        layout.addWidget(self._status)

    def _build_form(self, *, register: bool) -> dict[str, Any]:
        w = QWidget()
        form = QFormLayout(w)
        login = QLineEdit()
        login.setPlaceholderText("手机号或邮箱")
        password = QLineEdit()
        password.setEchoMode(QLineEdit.EchoMode.Password)
        password.setPlaceholderText("密码（至少 6 位）")
        form.addRow("账号", login)
        form.addRow("密码", password)
        confirm = None
        nickname = None
        if register:
            nickname = QLineEdit()
            nickname.setPlaceholderText("2–20 个字")
            form.addRow("昵称*", nickname)
            confirm = QLineEdit()
            confirm.setEchoMode(QLineEdit.EchoMode.Password)
            confirm.setPlaceholderText("再输入一次")
            form.addRow("确认密码", confirm)
        return {
            "widget": w,
            "login": login,
            "password": password,
            "confirm": confirm,
            "nickname": nickname,
        }

    def _set_busy(self, busy: bool, text: str = "") -> None:
        ok = self._buttons.button(QDialogButtonBox.StandardButton.Ok)
        cancel = self._buttons.button(QDialogButtonBox.StandardButton.Cancel)
        if ok:
            ok.setEnabled(not busy)
            ok.setText("请稍候…" if busy else "确定")
        if cancel:
            cancel.setEnabled(not busy)
        self._tabs.setEnabled(not busy)
        self._status.setText(text)

    def _submit(self) -> None:
        if self._async.busy:
            return
        register = self._tabs.currentIndex() == 1
        form = self._reg_form if register else self._login_form
        login_raw = form["login"].text().strip()
        password = form["password"].text()
        try:
            login, _ = normalize_login(login_raw)
        except ValueError as exc:
            QMessageBox.warning(self, "账号管理", str(exc))
            return
        if len(password) < 6:
            QMessageBox.warning(self, "账号管理", "密码至少 6 位")
            return
        display_name = ""
        if register:
            try:
                display_name = normalize_display_name(
                    form["nickname"].text() if form.get("nickname") else ""
                )
            except ValueError as exc:
                QMessageBox.warning(self, "账号管理", str(exc))
                return
            confirm = form["confirm"].text() if form["confirm"] else ""
            if password != confirm:
                QMessageBox.warning(self, "账号管理", "两次密码不一致")
                return
        api_base = self._api_base
        nick = display_name

        def work() -> dict[str, Any]:
            client = VaultApiClient(api_base)
            if register:
                return client.register(login, password, nick)
            return client.login(login, password)

        def on_ok(data: object) -> None:
            self._set_busy(False)
            if not isinstance(data, dict):
                QMessageBox.warning(self, "账号管理", "服务器响应无效")
                return
            token = str(data.get("token") or "")
            if not token:
                QMessageBox.warning(self, "账号管理", "服务器未返回登录态")
                return
            save_session(
                api_base=api_base,
                login=str(data.get("login") or login),
                token=token,
                user_id=data.get("user_id"),
                display_name=str(data.get("display_name") or nick),
            )
            self.session = load_session()
            self.accept()

        def on_err(exc: BaseException) -> None:
            self._set_busy(False)
            msg = exc.message if isinstance(exc, VaultApiError) else str(exc)
            QMessageBox.warning(self, "账号管理", msg or "登录失败")

        self._set_busy(True, "正在连接服务器…")
        if not self._async.run(
            work,
            on_ok=on_ok,
            on_err=on_err,
            busy_text="正在连接服务器…",
        ):
            self._set_busy(False)


# Back-compat alias used by AccountVaultWidget
_AuthDialog = AccountVaultAuthDialog


class CredentialEditDialog(QDialog):
    def __init__(self, item: dict[str, Any] | None = None, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("编辑账号" if item else "新建账号")
        self.resize(400, 280)
        item = item or {}
        form = QFormLayout(self)
        self.title_edit = QLineEdit(str(item.get("title") or ""))
        self.user_edit = QLineEdit(str(item.get("username") or ""))
        self.pass_edit = QLineEdit(str(item.get("password") or ""))
        self.pass_edit.setEchoMode(QLineEdit.EchoMode.Password)
        self.url_edit = QLineEdit(str(item.get("url") or ""))
        self.note_edit = QLineEdit(str(item.get("note") or ""))
        form.addRow("名称*", self.title_edit)
        form.addRow("网址", self.url_edit)
        form.addRow("账号", self.user_edit)
        form.addRow("密码", self.pass_edit)
        form.addRow("备注", self.note_edit)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)

    def _accept(self) -> None:
        if not self.title_edit.text().strip():
            QMessageBox.warning(self, "账号管理", "名称不能为空")
            return
        self.accept()

    def payload(self) -> dict[str, str]:
        return {
            "title": self.title_edit.text().strip(),
            "username": self.user_edit.text().strip(),
            "password": self.pass_edit.text().strip(),
            "url": self.url_edit.text().strip(),
            "note": self.note_edit.text().strip(),
        }


# Back-compat
_EditDialog = CredentialEditDialog


class _CopyFieldLabel(QLabel):
    """Copyable vault field with hover accent (panel tip is fixed in the title bar)."""

    def __init__(
        self,
        owner: "VaultCredentialRow",
        field_name: str,
        value: str,
        text: str,
        parent: QWidget | None = None,
    ):
        super().__init__(text, parent)
        self._owner = owner
        self._field_name = field_name
        self.setObjectName("vaultRowMeta")
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setToolTip(f"双击复制{field_name}")
        self.setProperty("copy_tip", field_name)
        self.setProperty("copy_value", value)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)

    def enterEvent(self, event) -> None:  # noqa: N802
        self.setProperty("copyHot", "true")
        self.style().unpolish(self)
        self.style().polish(self)
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:  # noqa: N802
        self.setProperty("copyHot", "false")
        self.style().unpolish(self)
        self.style().polish(self)
        super().leaveEvent(event)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self._owner._on_label_double_click(
                self._field_name, str(self.property("copy_value") or ""), event
            )
            return
        super().mouseDoubleClickEvent(event)


class VaultDragHandle(QFrame):
    """Left grip: hover highlights the card; press-drag reorders."""

    def __init__(self, owner: "VaultCredentialRow"):
        super().__init__(owner)
        self._owner = owner
        self._press_pos: QPoint | None = None
        self.setObjectName("vaultDragHandle")
        self.setFixedWidth(34)
        self.setCursor(Qt.CursorShape.OpenHandCursor)
        self.setToolTip("按住此处拖动调整顺序")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)
        grip = QLabel("⋮\n⋮")
        grip.setObjectName("vaultDragGrip")
        grip.setAlignment(Qt.AlignmentFlag.AlignCenter)
        grip.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        lay.addStretch(1)
        lay.addWidget(grip)
        lay.addStretch(1)

    def enterEvent(self, event) -> None:  # noqa: N802
        self._owner.set_drag_hover(True)
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:  # noqa: N802
        self._owner.set_drag_hover(False)
        super().leaveEvent(event)

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton and self._owner.drag_enabled:
            self._press_pos = event.position().toPoint()
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if (
            self._press_pos is not None
            and event.buttons() & Qt.MouseButton.LeftButton
            and self._owner.drag_enabled
        ):
            delta = event.position().toPoint() - self._press_pos
            if delta.manhattanLength() >= _start_drag_distance():
                self._press_pos = None
                self._owner.begin_reorder_drag()
                event.accept()
                return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        self._press_pos = None
        self.setCursor(Qt.CursorShape.OpenHandCursor)
        super().mouseReleaseEvent(event)


class VaultCredentialRow(QFrame):
    copy_field = pyqtSignal(str, str)  # tip, value
    edit_requested = pyqtSignal(dict)
    delete_requested = pyqtSignal(dict)
    pin_requested = pyqtSignal(dict)
    reorder_drop = pyqtSignal(int, int)  # source_id, target_id

    def __init__(
        self,
        item: dict[str, Any],
        parent: QWidget | None = None,
        *,
        drag_enabled: bool = False,
    ):
        super().__init__(parent)
        self.item = item
        self._drag_enabled = drag_enabled
        self.setObjectName("vaultRow")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self._show_pass = False
        pinned = bool(item.get("pinned"))
        self.setAcceptDrops(drag_enabled)
        self._apply_card_style()

        shell = QHBoxLayout(self)
        shell.setContentsMargins(0, 0, 0, 0)
        shell.setSpacing(0)

        if drag_enabled:
            self._handle = VaultDragHandle(self)
            shell.addWidget(self._handle)
        else:
            self._handle = None

        body = QWidget(self)
        body.setObjectName("vaultRowBody")
        root = QVBoxLayout(body)
        root.setContentsMargins(10, 10, 10, 10)
        root.setSpacing(6)

        title_row = QHBoxLayout()
        title_row.setSpacing(6)
        title = QLabel(str(item.get("title") or "未命名"))
        title.setObjectName("vaultRowTitle")
        title_row.addWidget(title, stretch=1)
        if pinned:
            badge = QLabel("置顶")
            badge.setObjectName("vaultPinBadge")
            title_row.addWidget(badge)
        pin_btn = QPushButton("取消置顶" if pinned else "置顶")
        pin_btn.setObjectName("vaultTextBtn")
        pin_btn.setToolTip("取消置顶" if pinned else "置顶到列表上方")
        pin_btn.clicked.connect(lambda: self.pin_requested.emit(self.item))
        title_row.addWidget(pin_btn)
        edit_btn = QPushButton("编辑")
        edit_btn.setObjectName("vaultPrimaryTextBtn")
        edit_btn.setToolTip("编辑此账号")
        edit_btn.clicked.connect(lambda: self.edit_requested.emit(self.item))
        title_row.addWidget(edit_btn)
        root.addLayout(title_row)

        url_val = str(item.get("url") or "")
        user_val = str(item.get("username") or "")
        pass_val = str(item.get("password") or "")
        note_val = str(item.get("note") or "")

        self.url_lbl = self._copy_label("网址", url_val)
        root.addWidget(self.url_lbl)

        self.user_lbl = self._copy_label("账号", user_val)
        root.addWidget(self.user_lbl)

        pass_row = QHBoxLayout()
        pass_row.setSpacing(6)
        self.pass_lbl = self._copy_label("密码", pass_val, display=False)
        self._refresh_pass_label()
        pass_row.addWidget(self.pass_lbl, stretch=1)
        eye = QPushButton("显示" if not self._show_pass else "隐藏")
        eye.setObjectName("vaultTextBtn")
        eye.setToolTip("显示/隐藏密码")
        eye.clicked.connect(self._toggle_pass)
        self._eye_btn = eye
        pass_row.addWidget(eye)
        root.addLayout(pass_row)

        self.note_lbl = self._copy_label("备注", note_val)
        self.note_lbl.setWordWrap(True)
        root.addWidget(self.note_lbl)

        actions = QHBoxLayout()
        actions.setSpacing(6)
        actions.addStretch(1)
        edit_btn2 = QPushButton("编辑")
        edit_btn2.setObjectName("vaultPrimaryTextBtn")
        edit_btn2.setToolTip("编辑此账号")
        edit_btn2.clicked.connect(lambda: self.edit_requested.emit(self.item))
        actions.addWidget(edit_btn2)
        del_btn = QPushButton("删除")
        del_btn.setObjectName("vaultDangerTextBtn")
        del_btn.setToolTip("删除")
        del_btn.clicked.connect(lambda: self.delete_requested.emit(self.item))
        actions.addWidget(del_btn)
        root.addLayout(actions)

        shell.addWidget(body, stretch=1)

    @property
    def drag_enabled(self) -> bool:
        return self._drag_enabled

    def set_drag_hover(self, on: bool) -> None:
        self.setProperty("dragHover", "true" if on else "false")
        self.style().unpolish(self)
        self.style().polish(self)
        self.update()

    def set_drop_target(self, on: bool) -> None:
        self.setProperty("dropTarget", "true" if on else "false")
        self.style().unpolish(self)
        self.style().polish(self)
        self.update()

    def _set_row_hover(self, on: bool) -> None:
        self.setProperty("rowHover", "true" if on else "false")
        self.style().unpolish(self)
        self.style().polish(self)
        self.update()

    def event(self, event) -> bool:  # noqa: N802
        et = event.type()
        if et == QEvent.Type.HoverEnter:
            self._set_row_hover(True)
        elif et == QEvent.Type.HoverLeave:
            self._set_row_hover(False)
        return super().event(event)

    def _apply_card_style(self) -> None:
        theme = normalize_theme(None)
        # Prefer ancestor settings theme when available.
        parent = self.parent()
        while parent is not None:
            settings = getattr(parent, "settings", None)
            if isinstance(settings, dict):
                theme = normalize_theme(settings.get("theme"))
                break
            parent = parent.parentWidget() if hasattr(parent, "parentWidget") else None
        p = get_theme_palette(theme)
        border = _hex_to_rgba(p["border"], 0.85)
        hover = _hex_to_rgba(p.get("accent_soft") or p["accent"], 0.22)
        # Deeper wash for whole-row hover reminder.
        row_hover = _hex_to_rgba(p.get("accent_soft") or p["accent"], 0.38)
        handle_bg = _hex_to_rgba(p.get("accent_soft") or p["accent"], 0.18)
        handle_hot = _hex_to_rgba(p["accent"], 0.28)
        card = p.get("card") or p.get("bg") or "#ffffff"
        self.setStyleSheet(
            f"""
            QFrame#vaultRow {{
                background: {card};
                border: 1px solid {border};
                border-radius: 12px;
            }}
            QFrame#vaultRow:hover,
            QFrame#vaultRow[rowHover="true"] {{
                background: {row_hover};
                border: 1px solid {p['accent']};
            }}
            QFrame#vaultRow[dragHover="true"] {{
                background: {row_hover};
                border: 1px solid {p['accent']};
            }}
            QFrame#vaultRow[dropTarget="true"] {{
                border: 2px solid {p['accent']};
            }}
            QFrame#vaultDragHandle {{
                background: {handle_bg};
                border-top-left-radius: 12px;
                border-bottom-left-radius: 12px;
                border-right: 1px solid {border};
            }}
            QFrame#vaultRow:hover QFrame#vaultDragHandle,
            QFrame#vaultRow[rowHover="true"] QFrame#vaultDragHandle,
            QFrame#vaultDragHandle:hover {{
                background: {handle_hot};
            }}
            QLabel#vaultDragGrip {{
                color: {p['text_muted']};
                font-size: 14px;
                font-weight: 700;
                line-height: 11px;
            }}
            QLabel#vaultRowTitle {{ color: {p['text']}; font-size: 13px; font-weight: 600; }}
            QLabel#vaultRowMeta {{ color: {p['text_muted']}; font-size: 12px; }}
            QLabel#vaultRowMeta[copyHot="true"] {{
                color: {p['accent']};
                font-weight: 600;
                text-decoration: underline;
            }}
            QLabel#vaultRowHint {{ color: {p['text_muted']}; font-size: 11px; }}
            QLabel#vaultPinBadge {{
                color: {p['accent']};
                font-size: 11px;
                font-weight: 600;
                padding: 1px 6px;
                border: 1px solid {p['accent']};
                border-radius: 8px;
            }}
            QPushButton#vaultIconBtn, QPushButton#vaultTextBtn {{
                background: transparent;
                color: {p['text_muted']};
                border: none;
                border-radius: 6px;
                padding: 4px 8px;
            }}
            QPushButton#vaultPrimaryTextBtn {{
                background: {hover};
                color: {p['accent']};
                border: 1px solid {p['accent']};
                border-radius: 6px;
                padding: 4px 10px;
                font-weight: 600;
            }}
            QPushButton#vaultDangerTextBtn {{
                background: transparent;
                color: #EF4444;
                border: 1px solid #EF4444;
                border-radius: 6px;
                padding: 4px 10px;
            }}
            QPushButton#vaultIconBtn:hover, QPushButton#vaultTextBtn:hover,
            QPushButton#vaultPrimaryTextBtn:hover {{
                background: {handle_hot};
                color: {p['text']};
            }}
            QPushButton#vaultDangerTextBtn:hover {{
                background: rgba(239, 68, 68, 0.12);
            }}
            """
        )

    def _cred_id(self) -> int:
        try:
            return int(self.item.get("id"))
        except (TypeError, ValueError):
            return 0

    def begin_reorder_drag(self) -> None:
        cid = self._cred_id()
        if cid < 1 or not self._drag_enabled:
            return
        drag = QDrag(self)
        mime = QMimeData()
        mime.setData(_VAULT_DRAG_MIME, QByteArray(str(cid).encode("utf-8")))
        drag.setMimeData(mime)
        pix = self.grab()
        if not pix.isNull():
            scaled = pix.scaled(
                max(120, pix.width() // 2),
                max(60, pix.height() // 2),
                Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
            drag.setPixmap(scaled)
            drag.setHotSpot(QPoint(20, scaled.height() // 2))
        self.set_drag_hover(True)
        try:
            drag.exec(Qt.DropAction.MoveAction)
        finally:
            self.set_drag_hover(False)
            self.set_drop_target(False)
            if self._handle is not None:
                self._handle.setCursor(Qt.CursorShape.OpenHandCursor)

    def dragEnterEvent(self, event) -> None:  # noqa: N802
        if self._drag_enabled and event.mimeData().hasFormat(_VAULT_DRAG_MIME):
            event.acceptProposedAction()
            self.set_drop_target(True)
        else:
            event.ignore()

    def dragLeaveEvent(self, event) -> None:  # noqa: N802
        self.set_drop_target(False)
        super().dragLeaveEvent(event)

    def dragMoveEvent(self, event) -> None:  # noqa: N802
        if self._drag_enabled and event.mimeData().hasFormat(_VAULT_DRAG_MIME):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dropEvent(self, event) -> None:  # noqa: N802
        self.set_drop_target(False)
        if not self._drag_enabled or not event.mimeData().hasFormat(_VAULT_DRAG_MIME):
            event.ignore()
            return
        raw = bytes(event.mimeData().data(_VAULT_DRAG_MIME)).decode("utf-8")
        try:
            source_id = int(raw)
        except ValueError:
            event.ignore()
            return
        target_id = self._cred_id()
        if source_id < 1 or target_id < 1:
            event.ignore()
            return
        event.acceptProposedAction()
        self.reorder_drop.emit(source_id, target_id)

    def _copy_label(self, label: str, value: str, *, display: bool = True) -> _CopyFieldLabel:
        text = self._line(label, value) if display else ""
        return _CopyFieldLabel(self, label, value, text, self)

    def _on_label_double_click(self, tip: str, value: str, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        # Password label keeps live value on the widget property.
        if tip == "密码":
            value = str(self.item.get("password") or "")
        self.copy_field.emit(f"复制{tip}", value)

    @staticmethod
    def _line(label: str, value: str) -> str:
        shown = value if value else "（空）"
        if len(shown) > 56:
            shown = shown[:53] + "…"
        return f"{label}：{shown}"

    def _refresh_pass_label(self) -> None:
        raw = str(self.item.get("password") or "")
        if not raw:
            self.pass_lbl.setText("密码：（空）")
        elif self._show_pass:
            self.pass_lbl.setText(self._line("密码", raw))
        else:
            self.pass_lbl.setText("密码：" + "•" * min(12, max(6, len(raw))))
        self.pass_lbl.setProperty("copy_value", raw)
        self.pass_lbl.setToolTip("双击复制密码")

    def _toggle_pass(self) -> None:
        self._show_pass = not self._show_pass
        self._refresh_pass_label()
        if getattr(self, "_eye_btn", None) is not None:
            self._eye_btn.setText("隐藏" if self._show_pass else "显示")


def _start_drag_distance() -> int:
    from PyQt6.QtWidgets import QApplication

    app = QApplication.instance()
    if app is None:
        return 8
    return int(app.startDragDistance())


# Back-compat
_VaultRow = VaultCredentialRow


class AccountVaultWidget(QWidget):
    """Floating account vault list (desktop overlay)."""

    closed = pyqtSignal()
    server_unreachable = pyqtSignal(str)  # human message; App disables vault

    def __init__(self, settings: dict, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self._items: list[dict[str, Any]] = []
        self._rows: list[_VaultRow] = []
        self._drag_origin = None
        self._drag_start = None
        self._anchor: QWidget | None = None
        self._async = VaultAsyncRunner(self)
        self._async.busy_changed.connect(self._on_busy_changed)
        self.setObjectName("accountVaultPanel")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint | Qt.WindowType.Tool
        )
        # Same rule as DesktopTodoWidget / vault launcher: desktop-band only.
        # Qt stays-on-top caused F1 restore to lift the panel over apps.
        self._desktidy_raise_band = True
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, False)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self._build_ui()
        self._apply_theme()
        # Defer shell attach until showEvent (user opens the panel). Calling it
        # here maps Win32 visible while Qt stays hidden — page-switch Z-raise
        # then surfaces a ghost ledger. Same pattern as DesktopTodoWidget.
        self._place_beside_anchor()
        self._refresh_timer = QTimer(self)
        self._refresh_timer.timeout.connect(self._on_background_refresh)
        self._configure_refresh_timer()
        self._sync_session_label()
        # Prefetch once in background; do not wait for the user to open the panel.
        QTimer.singleShot(0, self._bootstrap_cache)

    def _build_ui(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        self._card = QFrame()
        self._card.setObjectName("vaultCard")
        outer.addWidget(self._card)
        lay = QVBoxLayout(self._card)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)

        title_bar = QFrame()
        title_bar.setObjectName("vaultTitleBar")
        title_bar.setCursor(Qt.CursorShape.SizeAllCursor)
        title_bar.mousePressEvent = self._title_press  # type: ignore[method-assign]
        title_bar.mouseMoveEvent = self._title_move  # type: ignore[method-assign]
        title_bar.mouseReleaseEvent = self._title_release  # type: ignore[method-assign]
        tlay = QHBoxLayout(title_bar)
        tlay.setContentsMargins(12, 8, 8, 8)
        self._title = QLabel("账号管理")
        self._title.setObjectName("vaultTitle")
        tlay.addWidget(self._title)
        self._user_lbl = QLabel("")
        self._user_lbl.setObjectName("vaultUser")
        tlay.addWidget(self._user_lbl)
        tlay.addStretch(1)
        self._copy_hint = QLabel("双击字段可复制")
        self._copy_hint.setObjectName("vaultCopyHint")
        tlay.addWidget(self._copy_hint)
        hide_btn = QPushButton("×")
        hide_btn.setObjectName("vaultHideBtn")
        hide_btn.setFixedSize(28, 28)
        hide_btn.clicked.connect(self.hide)
        tlay.addWidget(hide_btn)
        lay.addWidget(title_bar)

        toolbar = QHBoxLayout()
        toolbar.setContentsMargins(10, 8, 10, 4)
        self._search = QLineEdit()
        self._search.setPlaceholderText("搜索名称 / 账号 / 网址…")
        self._search.textChanged.connect(self._rebuild_rows)
        toolbar.addWidget(self._search, stretch=1)
        self._refresh_btn = QPushButton("刷新")
        self._refresh_btn.setObjectName("vaultTextBtn")
        self._refresh_btn.clicked.connect(lambda: self.reload(silent=False))
        toolbar.addWidget(self._refresh_btn)
        self._add_btn = QPushButton("新建")
        self._add_btn.setObjectName("vaultTextBtn")
        self._add_btn.clicked.connect(self._on_add)
        toolbar.addWidget(self._add_btn)
        self._logout_btn = QPushButton("退出")
        self._logout_btn.setObjectName("vaultTextBtn")
        self._logout_btn.clicked.connect(self._on_logout)
        toolbar.addWidget(self._logout_btn)
        lay.addLayout(toolbar)

        scroll = QScrollArea()
        scroll.setObjectName("vaultScroll")
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._list_host = QWidget()
        self._list_host.setObjectName("vaultListHost")
        self._list_layout = QVBoxLayout(self._list_host)
        self._list_layout.setContentsMargins(8, 4, 8, 8)
        self._list_layout.setSpacing(10)
        scroll.setWidget(self._list_host)
        lay.addWidget(scroll, stretch=1)

        self._status = QLabel("")
        self._status.setObjectName("vaultStatus")
        self._status.setWordWrap(True)
        self._status.setContentsMargins(12, 4, 12, 10)
        lay.addWidget(self._status)

        self.resize(_DEFAULT_W, 480)

    def set_anchor_widget(self, widget: QWidget | None) -> None:
        """Buoy / launcher used to re-anchor the panel on every open."""
        self._anchor = widget

    def _place_beside_anchor(self) -> None:
        """Always open next to the buoy; drag is ephemeral until next close."""
        anchor = self._anchor
        try:
            if anchor is None or not anchor.isVisible():
                self._place_default()
                return
            ag = anchor.frameGeometry()
        except RuntimeError:
            self._place_default()
            return
        screen = work_screen(ag.center())
        if screen is None:
            self._place_default()
            return
        avail = screen.availableGeometry()
        pos = panel_pos_beside_anchor(
            ag, QSize(self.width(), self.height()), avail, gap=_ANCHOR_GAP
        )
        self.move(pos)
        self.setGeometry(snap_geometry(self.geometry()))

    def _place_default(self) -> None:
        screen = work_screen()
        if screen is None:
            self.move(80, 120)
            return
        geo = screen.availableGeometry()
        x = geo.right() - self.width() - 48
        y = geo.top() + 120
        self.move(x, y)
        self.setGeometry(snap_geometry(self.geometry()))

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        configure_desktop_overlay(self)
        if not rect_visible_on_any_screen(self.geometry()):
            self._place_beside_anchor()

    def hideEvent(self, event) -> None:  # noqa: N802
        super().hideEvent(event)
        self.closed.emit()

    def _title_press(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_origin = self.pos()
            self._drag_start = event.globalPosition().toPoint()

    def _title_move(self, event) -> None:
        if self._drag_start is None or self._drag_origin is None:
            return
        if not (event.buttons() & Qt.MouseButton.LeftButton):
            return
        delta = event.globalPosition().toPoint() - self._drag_start
        self.move(self._drag_origin + delta)

    def _title_release(self, _event) -> None:
        self._drag_start = None
        self._drag_origin = None
        # Snap only for this session — next open re-anchors beside the buoy.
        self.setGeometry(snap_geometry(self.geometry()))

    def _apply_theme(self) -> None:
        theme = normalize_theme(self.settings.get("theme"))
        p = get_theme_palette(theme)
        card_bg = _hex_to_rgba(p["card"], _CARD_ALPHA)
        title_bg = _hex_to_rgba(p["card"], _TITLE_ALPHA)
        border = _hex_to_rgba(p["border"], _BORDER_ALPHA)
        hover = _hex_to_rgba(p.get("accent_soft") or p["accent"], 0.35)
        self.setStyleSheet(
            f"""
            QFrame#vaultCard {{
                background: {card_bg};
                border: 1px solid {border};
                border-radius: 12px;
            }}
            QFrame#vaultTitleBar {{
                background: {title_bg};
                border-top-left-radius: 12px;
                border-top-right-radius: 12px;
                border-bottom: 1px solid {border};
            }}
            QLabel#vaultTitle {{ color: {p['text']}; font-size: 14px; font-weight: 700; }}
            QLabel#vaultUser {{ color: {p['text_muted']}; font-size: 11px; }}
            QLabel#vaultCopyHint {{
                color: {p['text_muted']};
                font-size: 11px;
                padding-right: 6px;
            }}
            QLabel#vaultStatus, QLabel#vaultRowMeta {{ color: {p['text_muted']}; font-size: 12px; }}
            QLabel#vaultRowMeta[copyHot="true"] {{
                color: {p['accent']};
                font-weight: 600;
                text-decoration: underline;
            }}
            QLabel#vaultRowHint {{ color: {p['text_muted']}; font-size: 11px; }}
            QLabel#vaultRowTitle {{ color: {p['text']}; font-size: 13px; font-weight: 600; }}
            QLabel#vaultPinBadge {{
                color: {p['accent']};
                font-size: 11px;
                font-weight: 600;
                padding: 1px 6px;
                border: 1px solid {p['accent']};
                border-radius: 8px;
            }}
            QFrame#vaultRow {{
                background: transparent;
                border: 1px solid {border};
                border-radius: 10px;
            }}
            QFrame#vaultRow:hover {{ background: {hover}; }}
            QPushButton#vaultHideBtn, QPushButton#vaultIconBtn, QPushButton#vaultTextBtn {{
                background: transparent;
                color: {p['text_muted']};
                border: none;
                border-radius: 6px;
                padding: 2px 6px;
            }}
            QPushButton#vaultHideBtn:hover, QPushButton#vaultIconBtn:hover, QPushButton#vaultTextBtn:hover {{
                background: {hover};
                color: {p['text']};
            }}
            QLineEdit {{
                border: 1px solid {border};
                border-radius: 6px;
                padding: 4px 8px;
                background: {p['card']};
                color: {p['text']};
            }}
            QScrollArea#vaultScroll, QWidget#vaultListHost {{ background: transparent; border: none; }}
            """
        )

    def raise_panel(self) -> None:
        """Show cached list immediately — no network sync on open."""
        self._apply_theme()
        self._sync_session_label()
        if not self._items and not load_session():
            self._status.setText("未登录")
        elif not self._items:
            self._status.setText("正在后台同步…" if self._async.busy else "暂无账号")
        # Always re-dock beside the buoy; previous drag is discarded on close.
        self._place_beside_anchor()
        self.show()
        self.raise_()
        self.activateWindow()

    def _sync_session_label(self) -> None:
        session = load_session() or {}
        if not session:
            self._user_lbl.setText("未登录")
            return
        name = session_display_name(session)
        masked = mask_login(str(session.get("login") or ""))
        if str(session.get("display_name") or "").strip():
            self._user_lbl.setText(f"{name} · {masked}")
        else:
            self._user_lbl.setText(masked)

    def _configure_refresh_timer(self) -> None:
        ms = account_vault_refresh_interval_ms(self.settings)
        self._refresh_timer.setInterval(ms)
        if not self._refresh_timer.isActive():
            self._refresh_timer.start()

    def apply_settings(self, settings: dict) -> None:
        self.settings = settings
        self._configure_refresh_timer()
        self._apply_theme()

    def _bootstrap_cache(self) -> None:
        self.reload(silent=True)

    def _on_background_refresh(self) -> None:
        if self._async.busy:
            return
        self.reload(silent=True)

    def _on_busy_changed(self, busy: bool, hint: str) -> None:
        for btn in (self._refresh_btn, self._add_btn, self._logout_btn):
            btn.setEnabled(not busy)
        if busy and hint:
            self._status.setText(hint)

    def _guard_busy(self) -> bool:
        if self._async.busy:
            show_toast("请稍候，正在同步…", level="warn")
            return True
        return False

    def _client(self) -> VaultApiClient | None:
        session = load_session()
        if not session:
            return None
        base = account_vault_api_base(self.settings)
        return VaultApiClient(base, str(session.get("token") or ""))

    def ensure_logged_in(self) -> bool:
        if load_session():
            return True
        dlg = _AuthDialog(account_vault_api_base(self.settings), self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return False
        if load_session() is None:
            return False
        # Fresh login: pull once immediately; timer covers later updates.
        self.reload(silent=True)
        return True

    def _apply_list_result(self, items: object, *, ok_suffix: str = "已同步") -> None:
        if not isinstance(items, list):
            items = []
        self._items = [x for x in items if isinstance(x, dict)]
        self._status.setText(f"共 {len(self._items)} 条 · {ok_suffix}")
        self._rebuild_rows()

    def _handle_api_error(self, exc: BaseException, *, silent: bool = False) -> None:
        if isinstance(exc, VaultApiError):
            if exc.error == "network":
                msg = exc.message or "无法访问账号服务器"
                self._status.setText(msg)
                if not silent:
                    QMessageBox.warning(self, "账号管理", msg)
                self.server_unreachable.emit(msg)
                return
            if exc.status == 401 or exc.error == "unauthorized":
                clear_session()
                self._status.setText("登录已失效，请重新登录")
                self._items = []
                self._rebuild_rows()
                self._sync_session_label()
                if not silent and self.ensure_logged_in():
                    return
                return
            self._status.setText(exc.message)
            if not silent:
                QMessageBox.warning(self, "账号管理", exc.message)
            return
        msg = str(exc) or "请求失败"
        self._status.setText(msg)
        if not silent:
            QMessageBox.warning(self, "账号管理", msg)

    def _run_mutate_then_list(
        self,
        mutate: Any,
        *,
        busy_text: str,
        toast: str | None = None,
    ) -> None:
        client = self._client()
        if client is None:
            return
        if self._guard_busy():
            return

        def work() -> list[dict[str, Any]]:
            mutate(client)
            return client.list_credentials()

        def ok(items: object) -> None:
            if toast:
                show_toast(toast, level="success")
            self._apply_list_result(items, ok_suffix="已更新")

        self._async.run(
            work,
            on_ok=ok,
            on_err=self._handle_api_error,
            busy_text=busy_text,
        )

    def reload(self, *, silent: bool = False) -> None:
        """Pull credential list. silent=True skips login dialogs / error popups."""
        if not silent:
            if not self.ensure_logged_in():
                self._status.setText("未登录")
                self._items = []
                self._rebuild_rows()
                self._sync_session_label()
                return
        elif not load_session():
            self._sync_session_label()
            return
        self._sync_session_label()
        client = self._client()
        if client is None:
            return
        if silent and self._async.busy:
            return

        def work() -> list[dict[str, Any]]:
            return client.list_credentials()

        def on_err(exc: BaseException) -> None:
            self._handle_api_error(exc, silent=silent)

        self._async.run(
            work,
            on_ok=lambda items: self._apply_list_result(
                items, ok_suffix="已同步" if not silent else "后台已更新"
            ),
            on_err=on_err,
            busy_text="" if silent else "正在从云端同步…",
        )

    def _rebuild_rows(self) -> None:
        while self._list_layout.count():
            item = self._list_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()
        self._rows.clear()
        q = self._search.text().strip().lower()
        drag_ok = not bool(q) and not self._async.busy
        for item in self._items:
            if q:
                blob = " ".join(
                    str(item.get(k) or "") for k in ("title", "username", "url", "note")
                ).lower()
                if q not in blob:
                    continue
            row = _VaultRow(item, self._list_host, drag_enabled=drag_ok)
            row.copy_field.connect(self._on_copy)
            row.edit_requested.connect(self._on_edit)
            row.delete_requested.connect(self._on_delete)
            row.pin_requested.connect(self._on_pin)
            row.reorder_drop.connect(self._on_reorder)
            self._list_layout.addWidget(row)
            self._rows.append(row)
        self._list_layout.addStretch(1)

    def _on_reorder(self, source_id: int, target_id: int) -> None:
        if self._search.text().strip():
            show_toast("搜索时不可排序，请先清空搜索", level="warn")
            return
        if source_id == target_id:
            return
        new_ids = reorder_credential_ids(self._items, source_id, target_id)
        if not new_ids:
            show_toast("置顶与未置顶不能混排", level="warn")
            return
        ids = list(new_ids)
        self._run_mutate_then_list(
            lambda c: c.reorder_credentials(ids),
            busy_text="正在保存顺序…",
            toast="顺序已保存",
        )

    def _on_copy(self, tip: str, value: str) -> None:
        if not value:
            show_toast("内容为空，无法复制", level="warn")
            return
        QGuiApplication.clipboard().setText(value)
        show_toast(tip if tip.startswith("已") else f"已{tip}", level="success")

    def _on_add(self) -> None:
        if not self.ensure_logged_in():
            return
        if self._guard_busy():
            return
        dlg = _EditDialog(parent=self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        payload = dlg.payload()
        self._run_mutate_then_list(
            lambda c: c.create_credential(payload),
            busy_text="正在保存…",
            toast="已保存",
        )

    def _on_edit(self, item: dict[str, Any]) -> None:
        if self._guard_busy():
            return
        dlg = _EditDialog(item, self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        cred_id = item.get("id")
        payload = dlg.payload()
        self._run_mutate_then_list(
            lambda c: c.update_credential(cred_id, payload),
            busy_text="正在更新…",
            toast="已更新",
        )

    def _on_delete(self, item: dict[str, Any]) -> None:
        if self._guard_busy():
            return
        title = str(item.get("title") or "")
        if (
            QMessageBox.question(self, "账号管理", f"删除「{title}」？")
            != QMessageBox.StandardButton.Yes
        ):
            return
        cred_id = item.get("id")
        self._run_mutate_then_list(
            lambda c: c.delete_credential(cred_id),
            busy_text="正在删除…",
            toast="已删除",
        )

    def _on_pin(self, item: dict[str, Any]) -> None:
        if self._guard_busy():
            return
        pinned = not bool(item.get("pinned"))
        cred_id = item.get("id")
        self._run_mutate_then_list(
            lambda c: c.update_credential(cred_id, {"pinned": bool(pinned)}),
            busy_text="正在取消置顶…" if not pinned else "正在置顶…",
            toast="已置顶" if pinned else "已取消置顶",
        )

    def _on_logout(self) -> None:
        if self._guard_busy():
            return
        session = load_session()
        if session:
            base = account_vault_api_base(self.settings)
            token = str(session.get("token") or "")

            def work() -> None:
                try:
                    VaultApiClient(base, token).logout()
                except VaultApiError:
                    pass

            def ok(_: object) -> None:
                clear_session()
                self._user_lbl.setText("")
                self._items = []
                self._rebuild_rows()
                self._status.setText("已退出登录")
                save_settings(self.settings)

            self._async.run(
                work,
                on_ok=ok,
                on_err=lambda _e: ok(None),
                busy_text="正在退出…",
            )
        else:
            clear_session()
            self._user_lbl.setText("")
            self._items = []
            self._rebuild_rows()
            self._status.setText("已退出登录")
            save_settings(self.settings)
