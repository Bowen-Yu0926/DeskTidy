"""Dedicated settings page for cloud account vault + credential list."""

from __future__ import annotations

from typing import Any

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QGuiApplication
from PyQt6.QtWidgets import (
    QCheckBox,
    QDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from src.account_vault import (
    DEFAULT_API_BASE,
    DEFAULT_REFRESH_INTERVAL_HOURS,
    MAX_REFRESH_INTERVAL_HOURS,
    MIN_REFRESH_INTERVAL_HOURS,
    account_vault_api_base,
    account_vault_refresh_interval_hours,
    account_vault_settings,
    clear_session,
    load_session,
    mask_login,
    read_credentials_import_file,
    reorder_credential_ids,
    write_credentials_export_file,
)
from src.account_vault_api import VaultApiClient, VaultApiError
from src.settings import save_settings
from src.ui.account_vault_async import VaultAsyncRunner
from src.ui.account_vault_widget import (
    AccountVaultAuthDialog,
    CredentialEditDialog,
    VaultCredentialRow,
)
from src.ui.toast import show_toast


class AccountVaultSettingsWidget(QWidget):
    """Top-level「账号管理」: login + inline credential CRUD."""

    vault_settings_changed = pyqtSignal()

    def __init__(self, settings: dict, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self._items: list[dict[str, Any]] = []
        self._rows: list[VaultCredentialRow] = []
        self._async = VaultAsyncRunner(self)
        self._async.busy_changed.connect(self._on_busy_changed)
        self._build_ui()
        self.reload()

    def _panel_layout(self, panel: QFrame) -> QVBoxLayout:
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(10)
        return layout

    def _build_ui(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        content = QWidget()
        content.setObjectName("extensionsPanel")
        layout = QVBoxLayout(content)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(14)

        # --- account / API ---
        panel = QFrame()
        panel.setObjectName("panelCard")
        panel_layout = self._panel_layout(panel)
        title = QLabel("登录与入口")
        title.setObjectName("panelTitle")
        panel_layout.addWidget(title)
        hint = QLabel(
            "先登录云端账号，再在下方维护名称、账号、密码、网址。"
            "启用后桌面会出现账号浮标，点击即可打开账号面板。"
        )
        hint.setObjectName("appSubtitle")
        hint.setWordWrap(True)
        panel_layout.addWidget(hint)

        vault_cfg = account_vault_settings(self.settings)
        self.enabled_cb = QCheckBox("启用桌面账号浮标")
        self.enabled_cb.setChecked(bool(vault_cfg.get("enabled", False)))
        self.enabled_cb.setToolTip("启用后桌面显示可拖动的账号浮标，点击打开账号管理。")
        self.enabled_cb.stateChanged.connect(self._on_enabled_changed)
        panel_layout.addWidget(self.enabled_cb)

        refresh_row = QHBoxLayout()
        refresh_row.addWidget(QLabel("后台刷新间隔（小时）"))
        self.refresh_spin = QSpinBox()
        self.refresh_spin.setRange(MIN_REFRESH_INTERVAL_HOURS, MAX_REFRESH_INTERVAL_HOURS)
        self.refresh_spin.setValue(
            account_vault_refresh_interval_hours(self.settings)
        )
        self.refresh_spin.setToolTip(
            f"启用后定时从云端静默拉取清单；默认 {DEFAULT_REFRESH_INTERVAL_HOURS} 小时。"
            "打开浮标不再同步；新建/编辑/删除后会立即刷新一次。"
        )
        self.refresh_spin.valueChanged.connect(self._on_refresh_interval_changed)
        refresh_row.addWidget(self.refresh_spin)
        refresh_row.addStretch(1)
        panel_layout.addLayout(refresh_row)

        api_row = QHBoxLayout()
        api_row.addWidget(QLabel("API 根地址"))
        self.api_edit = QLineEdit(str(vault_cfg.get("api_base") or DEFAULT_API_BASE))
        self.api_edit.setPlaceholderText(DEFAULT_API_BASE)
        self.api_edit.editingFinished.connect(self._on_api_edited)
        api_row.addWidget(self.api_edit, stretch=1)
        panel_layout.addLayout(api_row)

        session_row = QHBoxLayout()
        self.session_lbl = QLabel("")
        self.session_lbl.setObjectName("appSubtitle")
        session_row.addWidget(self.session_lbl, stretch=1)
        self.login_btn = QPushButton("登录 / 注册")
        self.login_btn.setObjectName("primaryBtn")
        self.login_btn.clicked.connect(self._on_login)
        session_row.addWidget(self.login_btn)
        self.logout_btn = QPushButton("退出登录")
        self.logout_btn.setObjectName("secondaryBtn")
        self.logout_btn.clicked.connect(self._on_logout)
        session_row.addWidget(self.logout_btn)
        panel_layout.addLayout(session_row)
        layout.addWidget(panel)

        # --- credentials ---
        list_panel = QFrame()
        list_panel.setObjectName("panelCard")
        list_layout = self._panel_layout(list_panel)
        list_title = QLabel("账号清单")
        list_title.setObjectName("panelTitle")
        list_layout.addWidget(list_title)

        toolbar = QHBoxLayout()
        self.search_edit = QLineEdit()
        self.search_edit.setPlaceholderText("搜索名称 / 账号 / 网址…")
        self.search_edit.textChanged.connect(self._rebuild_rows)
        toolbar.addWidget(self.search_edit, stretch=1)
        self.refresh_btn = QPushButton("刷新")
        self.refresh_btn.setObjectName("secondaryBtn")
        self.refresh_btn.clicked.connect(self._reload_credentials)
        toolbar.addWidget(self.refresh_btn)
        self.export_btn = QPushButton("导出")
        self.export_btn.setObjectName("secondaryBtn")
        self.export_btn.setToolTip("导出当前账号清单为 JSON 文件（含密码，请妥善保管）")
        self.export_btn.clicked.connect(self._on_export)
        toolbar.addWidget(self.export_btn)
        self.import_btn = QPushButton("导入")
        self.import_btn.setObjectName("secondaryBtn")
        self.import_btn.setToolTip("从 JSON 账号文件导入到云端")
        self.import_btn.clicked.connect(self._on_import)
        toolbar.addWidget(self.import_btn)
        self.add_btn = QPushButton("新建")
        self.add_btn.setObjectName("primaryBtn")
        self.add_btn.clicked.connect(self._on_add)
        toolbar.addWidget(self.add_btn)
        list_layout.addLayout(toolbar)

        self._list_host = QWidget()
        self._list_layout = QVBoxLayout(self._list_host)
        self._list_layout.setContentsMargins(0, 4, 0, 0)
        self._list_layout.setSpacing(12)
        list_layout.addWidget(self._list_host)

        self.status_lbl = QLabel("")
        self.status_lbl.setObjectName("appSubtitle")
        self.status_lbl.setWordWrap(True)
        list_layout.addWidget(self.status_lbl)

        layout.addWidget(list_panel)
        layout.addStretch()
        scroll.setWidget(content)
        outer.addWidget(scroll)

        self._busy_buttons = [
            self.refresh_btn,
            self.export_btn,
            self.import_btn,
            self.add_btn,
            self.login_btn,
            self.logout_btn,
        ]

    def reload(self) -> None:
        cfg = account_vault_settings(self.settings)
        self.enabled_cb.blockSignals(True)
        self.enabled_cb.setChecked(bool(cfg.get("enabled", False)))
        self.enabled_cb.blockSignals(False)
        self.refresh_spin.blockSignals(True)
        self.refresh_spin.setValue(account_vault_refresh_interval_hours(self.settings))
        self.refresh_spin.blockSignals(False)
        self.api_edit.setText(str(cfg.get("api_base") or DEFAULT_API_BASE))
        self._refresh_session_label()
        self._reload_credentials()

    def _on_busy_changed(self, busy: bool, hint: str) -> None:
        for btn in self._busy_buttons:
            btn.setEnabled(not busy)
        # Do not disable the list host — empty→fill must stay visible during sync.
        if busy and hint:
            self.status_lbl.setText(hint)

    def _guard_busy(self) -> bool:
        if self._async.busy:
            show_toast("请稍候，正在同步…", level="warn")
            return True
        return False

    def _refresh_session_label(self) -> None:
        session = load_session()
        if session:
            masked = mask_login(str(session.get("login") or ""))
            nick = str(session.get("display_name") or "").strip()
            if nick:
                self.session_lbl.setText(f"已登录：{nick}（{masked}）")
            else:
                self.session_lbl.setText(f"已登录：{masked}")
            self.login_btn.setVisible(False)
            self.logout_btn.setVisible(True)
        else:
            self.session_lbl.setText("未登录 — 请先登录或注册账号")
            self.login_btn.setVisible(True)
            self.logout_btn.setVisible(False)

    def _client(self) -> VaultApiClient | None:
        session = load_session()
        if not session:
            return None
        return VaultApiClient(
            account_vault_api_base(self.settings),
            str(session.get("token") or ""),
        )

    def _ensure_login(self) -> bool:
        if load_session():
            return True
        self._on_login()
        return load_session() is not None

    def _apply_list_result(self, items: object, *, ok_suffix: str = "已从云端同步") -> None:
        if not isinstance(items, list):
            items = []
        self._items = [x for x in items if isinstance(x, dict)]
        self.status_lbl.setText(f"共 {len(self._items)} 条 · {ok_suffix}")
        self._rebuild_rows()

    def _handle_api_error(self, exc: BaseException) -> None:
        if isinstance(exc, VaultApiError):
            if exc.status == 401 or exc.error == "unauthorized":
                clear_session()
                self._refresh_session_label()
                self._items = []
                self.status_lbl.setText("登录已失效，请重新登录")
                self._rebuild_rows()
                return
            self.status_lbl.setText(exc.message)
            QMessageBox.warning(self, "账号管理", exc.message)
            return
        msg = str(exc) or "请求失败"
        self.status_lbl.setText(msg)
        QMessageBox.warning(self, "账号管理", msg)

    def _run_list(self, *, busy_text: str = "正在从云端同步…") -> None:
        if not load_session():
            self._items = []
            self.status_lbl.setText("登录后可在此新增、编辑、复制账号信息。")
            self._rebuild_rows()
            return
        client = self._client()
        if client is None:
            return

        def work() -> list[dict[str, Any]]:
            return client.list_credentials()

        # If a sync is already running, queue this list so cold-start / re-entry
        # cannot drop the refresh the user just triggered.
        self._async.run(
            work,
            on_ok=lambda items: self._apply_list_result(items),
            on_err=self._handle_api_error,
            busy_text=busy_text,
        )

    def _run_mutate_then_list(
        self,
        mutate: Any,
        *,
        busy_text: str,
        toast: str | None = None,
        on_list: Any = None,
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
            if on_list:
                on_list(items)
            else:
                self._apply_list_result(items)

        self._async.run(
            work,
            on_ok=ok,
            on_err=self._handle_api_error,
            busy_text=busy_text,
        )

    def _reload_credentials(self) -> None:
        self._run_list()

    def _rebuild_rows(self) -> None:
        while self._list_layout.count():
            item = self._list_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()
        self._rows.clear()
        q = self.search_edit.text().strip().lower()
        drag_ok = not bool(q) and not self._async.busy
        for item in self._items:
            if q:
                blob = " ".join(
                    str(item.get(k) or "") for k in ("title", "username", "url", "note")
                ).lower()
                if q not in blob:
                    continue
            row = VaultCredentialRow(item, self._list_host, drag_enabled=drag_ok)
            row.copy_field.connect(self._on_copy)
            row.edit_requested.connect(self._on_edit)
            row.delete_requested.connect(self._on_delete)
            row.pin_requested.connect(self._on_pin)
            row.reorder_drop.connect(self._on_reorder)
            self._list_layout.addWidget(row)
            self._rows.append(row)
        self._list_layout.addStretch(1)

    def _on_reorder(self, source_id: int, target_id: int) -> None:
        if self.search_edit.text().strip():
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

    def _on_enabled_changed(self, _state: int) -> None:
        cfg = account_vault_settings(self.settings)
        enabled = self.enabled_cb.isChecked()
        if not enabled:
            cfg["enabled"] = False
            save_settings(self.settings)
            self.vault_settings_changed.emit()
            return
        # Enabling: probe server in background — never block the UI thread.
        self.enabled_cb.setEnabled(False)
        self.status_lbl.setText("正在检测账号服务器…")
        base = account_vault_api_base(self.settings)

        def work() -> bool:
            from src.account_vault_api import probe_vault_reachable

            ok, msg = probe_vault_reachable(base, timeout=8.0)
            if not ok:
                raise VaultApiError(msg or "无法访问账号服务器", error="network", status=0)
            return True

        def on_ok(_result: object) -> None:
            self.enabled_cb.setEnabled(True)
            cfg2 = account_vault_settings(self.settings)
            cfg2["enabled"] = True
            save_settings(self.settings)
            self.vault_settings_changed.emit()
            self.status_lbl.setText("")
            if load_session() is None:
                self._on_login()

        def on_err(exc: BaseException) -> None:
            self.enabled_cb.setEnabled(True)
            self.enabled_cb.blockSignals(True)
            self.enabled_cb.setChecked(False)
            self.enabled_cb.blockSignals(False)
            cfg2 = account_vault_settings(self.settings)
            cfg2["enabled"] = False
            save_settings(self.settings)
            msg = ""
            if isinstance(exc, VaultApiError):
                msg = exc.message
            tip = msg or "无法访问账号服务器"
            self.status_lbl.setText(tip)
            show_toast("账号服务器无法访问，已关闭账号管理", tip, level="warn")
            self.vault_settings_changed.emit()

        self._async.run(work, on_ok=on_ok, on_err=on_err, busy_text="正在检测账号服务器…")

    def _on_refresh_interval_changed(self, hours: int) -> None:
        cfg = account_vault_settings(self.settings)
        cfg["refresh_interval_hours"] = int(hours)
        save_settings(self.settings)
        self.vault_settings_changed.emit()

    def _on_api_edited(self) -> None:
        cfg = account_vault_settings(self.settings)
        text = self.api_edit.text().strip() or DEFAULT_API_BASE
        if not text.endswith("/"):
            text += "/"
        cfg["api_base"] = text
        self.api_edit.setText(text)
        save_settings(self.settings)

    def _on_login(self) -> None:
        if self._guard_busy():
            return
        dlg = AccountVaultAuthDialog(account_vault_api_base(self.settings), self)
        if dlg.exec() == QDialog.DialogCode.Accepted:
            self._refresh_session_label()
            self._reload_credentials()

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
                self._refresh_session_label()
                self._items = []
                self.status_lbl.setText("已退出登录")
                self._rebuild_rows()

            self._async.run(
                work,
                on_ok=ok,
                on_err=lambda _e: ok(None),
                busy_text="正在退出…",
            )
        else:
            clear_session()
            self._refresh_session_label()
            self._items = []
            self.status_lbl.setText("已退出登录")
            self._rebuild_rows()

    def _on_copy(self, tip: str, value: str) -> None:
        if not value:
            show_toast("内容为空，无法复制", level="warn")
            return
        QGuiApplication.clipboard().setText(value)
        show_toast(tip if tip.startswith("已") else f"已{tip}", level="success")

    def _on_add(self) -> None:
        if not self._ensure_login():
            return
        if self._guard_busy():
            return
        dlg = CredentialEditDialog(parent=self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        payload = dlg.payload()
        self._run_mutate_then_list(
            lambda c: c.create_credential(payload),
            busy_text="正在保存…",
            toast="已保存",
        )

    def _on_export(self) -> None:
        from PyQt6.QtWidgets import QFileDialog

        if not self._ensure_login():
            return
        if not self._items:
            QMessageBox.information(self, "账号管理", "当前没有可导出的账号")
            return
        path, _ = QFileDialog.getSaveFileName(
            self,
            "导出账号文件",
            "desktidy-accounts.json",
            "账号文件 (*.json)",
        )
        if not path:
            return
        try:
            write_credentials_export_file(path, self._items)
            show_toast(f"已导出 {len(self._items)} 条", level="success")
        except OSError as exc:
            QMessageBox.warning(self, "账号管理", f"导出失败：{exc}")

    def _on_import(self) -> None:
        from PyQt6.QtWidgets import QFileDialog

        if not self._ensure_login():
            return
        if self._guard_busy():
            return
        path, _ = QFileDialog.getOpenFileName(
            self,
            "导入账号文件",
            "",
            "账号文件 (*.json);;所有文件 (*.*)",
        )
        if not path:
            return
        try:
            payloads = read_credentials_import_file(path)
        except ValueError as exc:
            QMessageBox.warning(self, "账号管理", str(exc))
            return
        if (
            QMessageBox.question(
                self,
                "账号管理",
                f"将导入 {len(payloads)} 条账号到云端，是否继续？",
            )
            != QMessageBox.StandardButton.Yes
        ):
            return
        client = self._client()
        if client is None:
            return
        batch = list(payloads)

        def work() -> tuple[list[dict[str, Any]], int, list[str]]:
            ok = 0
            errors: list[str] = []
            for payload in batch:
                try:
                    client.create_credential(payload)
                    ok += 1
                except VaultApiError as exc:
                    errors.append(f"{payload.get('title')}: {exc.message}")
            items = client.list_credentials()
            return items, ok, errors

        def on_ok(result: object) -> None:
            items, ok, errors = result  # type: ignore[misc]
            self._apply_list_result(items)
            if errors:
                QMessageBox.warning(
                    self,
                    "账号管理",
                    f"成功 {ok} 条，失败 {len(errors)} 条。\n" + "\n".join(errors[:8]),
                )
            else:
                show_toast(f"已导入 {ok} 条", level="success")

        self._async.run(
            work,
            on_ok=on_ok,
            on_err=self._handle_api_error,
            busy_text=f"正在导入 {len(batch)} 条…",
        )

    def _on_edit(self, item: dict[str, Any]) -> None:
        if self._guard_busy():
            return
        dlg = CredentialEditDialog(item, self)
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
