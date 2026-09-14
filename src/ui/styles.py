"""Application themes and stylesheet generation."""

from __future__ import annotations

from typing import Any


THEME_OPTIONS: list[tuple[str, str]] = [
    ("mist", "薄雾浅灰（推荐）"),
    ("graphite", "石墨灰（推荐）"),
    ("sky", "天空蓝"),
    ("mint", "薄荷绿"),
    ("lavender", "淡紫 · 柔和"),
    ("peach", "浅杏 · 柔和"),
]

# Desktop/fence captions follow Explorer / Fences: light ink on dark panels
# (not app-theme palette colors — those look washed-out on dark fences).
_DEFAULT_FENCE_TITLE_COLOR = "#FFFFFF"
_DEFAULT_FENCE_TEXT_COLOR = "#FFFFFF"
_DEFAULT_FENCE_MUTED_COLOR = "#CBD5E1"

_PALETTES: dict[str, dict[str, str]] = {
    "mist": {
        "window": "#EEF1F6",
        "sidebar": "#EBEEF3",
        "content": "#F4F6F9",
        "card": "#FFFFFF",
        "card_alt": "#EEF1F5",
        "input": "#FFFFFF",
        "border": "#C9D2DE",
        "border_strong": "#A8B4C4",
        "text": "#1A2332",
        "text_muted": "#64748B",
        "text_inverse": "#FFFFFF",
        "accent": "#1D4ED8",
        "accent_soft": "#E0E7FF",
        "accent_hover": "#1E40AF",
        "primary": "#16A34A",
        "primary_hover": "#15803D",
        "danger": "#DC2626",
        "danger_hover": "#B91C1C",
        "nav_text": "#64748B",
        "nav_active": "#1E3A8A",
        "nav_active_bg": "#E0E7FF",
        "table_bg": "#FFFFFF",
        "table_alt": "#F3F5F8",
        "header_bg": "#EBEEF3",
        "scroll": "#C5CED9",
        "status_bg": "#EBEEF3",
        "fence_bg": "#FFFFFF",
        "fence_header": "#EEF2FF",
        "fence_border": "#1D4ED8",
        "fence_title": "#1E3A8A",
        "fence_text": "#334155",
        "fence_muted": "#64748B",
        "page_btn_bg": "#FFFFFF",
        "page_btn_border": "#93C5FD",
        "page_btn_active": "#16A34A",
    },
    "sky": {
        "window": "#EAF4FF",
        "sidebar": "#DCEBFA",
        "content": "#F2F8FF",
        "card": "#FFFFFF",
        "card_alt": "#E7F1FC",
        "input": "#FFFFFF",
        "border": "#B9D4F0",
        "border_strong": "#8FB6DE",
        "text": "#0F2744",
        "text_muted": "#5B7391",
        "text_inverse": "#FFFFFF",
        "accent": "#0EA5E9",
        "accent_soft": "#E0F2FE",
        "accent_hover": "#0284C7",
        "primary": "#0EA5E9",
        "primary_hover": "#0284C7",
        "danger": "#E11D48",
        "danger_hover": "#BE123C",
        "nav_text": "#4B6A88",
        "nav_active": "#0369A1",
        "nav_active_bg": "#BAE6FD",
        "table_bg": "#FFFFFF",
        "table_alt": "#EEF6FF",
        "header_bg": "#DCEBFA",
        "scroll": "#A7C7E7",
        "status_bg": "#DCEBFA",
        "fence_bg": "#FFFFFF",
        "fence_header": "#E0F2FE",
        "fence_border": "#0EA5E9",
        "fence_title": "#0369A1",
        "fence_text": "#1E3A5F",
        "fence_muted": "#5B7391",
        "page_btn_bg": "#FFFFFF",
        "page_btn_border": "#7DD3FC",
        "page_btn_active": "#0284C7",
    },
    "mint": {
        "window": "#EAF8F2",
        "sidebar": "#DCF3E9",
        "content": "#F3FBF7",
        "card": "#FFFFFF",
        "card_alt": "#E6F6EE",
        "input": "#FFFFFF",
        "border": "#B7DFCB",
        "border_strong": "#8FCBB0",
        "text": "#12352A",
        "text_muted": "#4F7466",
        "text_inverse": "#FFFFFF",
        "accent": "#10B981",
        "accent_soft": "#D1FAE5",
        "accent_hover": "#059669",
        "primary": "#059669",
        "primary_hover": "#047857",
        "danger": "#DC2626",
        "danger_hover": "#B91C1C",
        "nav_text": "#4F7466",
        "nav_active": "#047857",
        "nav_active_bg": "#A7F3D0",
        "table_bg": "#FFFFFF",
        "table_alt": "#EFFAF5",
        "header_bg": "#DCF3E9",
        "scroll": "#9FD4BB",
        "status_bg": "#DCF3E9",
        "fence_bg": "#FFFFFF",
        "fence_header": "#D1FAE5",
        "fence_border": "#10B981",
        "fence_title": "#047857",
        "fence_text": "#1F4D3D",
        "fence_muted": "#4F7466",
        "page_btn_bg": "#FFFFFF",
        "page_btn_border": "#6EE7B7",
        "page_btn_active": "#059669",
    },
    "lavender": {
        "window": "#F2EEFF",
        "sidebar": "#E8E1FA",
        "content": "#F7F4FF",
        "card": "#FFFFFF",
        "card_alt": "#EFE9FC",
        "input": "#FFFFFF",
        "border": "#D2C7EF",
        "border_strong": "#B5A4E0",
        "text": "#2A2148",
        "text_muted": "#6B6288",
        "text_inverse": "#FFFFFF",
        "accent": "#8B5CF6",
        "accent_soft": "#EDE9FE",
        "accent_hover": "#7C3AED",
        "primary": "#7C3AED",
        "primary_hover": "#6D28D9",
        "danger": "#E11D48",
        "danger_hover": "#BE123C",
        "nav_text": "#6B6288",
        "nav_active": "#6D28D9",
        "nav_active_bg": "#DDD6FE",
        "table_bg": "#FFFFFF",
        "table_alt": "#F4F0FE",
        "header_bg": "#E8E1FA",
        "scroll": "#C4B5FD",
        "status_bg": "#E8E1FA",
        "fence_bg": "#FFFFFF",
        "fence_header": "#EDE9FE",
        "fence_border": "#8B5CF6",
        "fence_title": "#6D28D9",
        "fence_text": "#312E81",
        "fence_muted": "#6B6288",
        "page_btn_bg": "#FFFFFF",
        "page_btn_border": "#C4B5FD",
        "page_btn_active": "#7C3AED",
    },
    "peach": {
        "window": "#FFF1E8",
        "sidebar": "#FFE6D8",
        "content": "#FFF7F2",
        "card": "#FFFFFF",
        "card_alt": "#FFEDE2",
        "input": "#FFFFFF",
        "border": "#F0CDB8",
        "border_strong": "#E0AE92",
        "text": "#3B2418",
        "text_muted": "#8A6452",
        "text_inverse": "#FFFFFF",
        "accent": "#F97316",
        "accent_soft": "#FFEDD5",
        "accent_hover": "#EA580C",
        "primary": "#EA580C",
        "primary_hover": "#C2410C",
        "danger": "#DC2626",
        "danger_hover": "#B91C1C",
        "nav_text": "#8A6452",
        "nav_active": "#C2410C",
        "nav_active_bg": "#FED7AA",
        "table_bg": "#FFFFFF",
        "table_alt": "#FFF4ED",
        "header_bg": "#FFE6D8",
        "scroll": "#FDBA74",
        "status_bg": "#FFE6D8",
        "fence_bg": "#FFFFFF",
        "fence_header": "#FFEDD5",
        "fence_border": "#F97316",
        "fence_title": "#C2410C",
        "fence_text": "#7C2D12",
        "fence_muted": "#8A6452",
        "page_btn_bg": "#FFFFFF",
        "page_btn_border": "#FDBA74",
        "page_btn_active": "#EA580C",
    },
    "graphite": {
        "window": "#E8EAED",
        "sidebar": "#DEE1E6",
        "content": "#F0F1F3",
        "card": "#FFFFFF",
        "card_alt": "#E6E8EC",
        "input": "#FFFFFF",
        "border": "#C5CAD3",
        "border_strong": "#A8AFBB",
        "text": "#1F2933",
        "text_muted": "#6B7280",
        "text_inverse": "#FFFFFF",
        "accent": "#4B5563",
        "accent_soft": "#E5E7EB",
        "accent_hover": "#374151",
        "primary": "#374151",
        "primary_hover": "#1F2937",
        "danger": "#DC2626",
        "danger_hover": "#B91C1C",
        "nav_text": "#6B7280",
        "nav_active": "#111827",
        "nav_active_bg": "#D1D5DB",
        "table_bg": "#FFFFFF",
        "table_alt": "#F3F4F6",
        "header_bg": "#DEE1E6",
        "scroll": "#9CA3AF",
        "status_bg": "#DEE1E6",
        "fence_bg": "#FFFFFF",
        "fence_header": "#F3F4F6",
        "fence_border": "#4B5563",
        "fence_title": "#111827",
        "fence_text": "#1F2937",
        "fence_muted": "#6B7280",
        "page_btn_bg": "#FFFFFF",
        "page_btn_border": "#9CA3AF",
        "page_btn_active": "#374151",
    },
}


def normalize_theme(theme: str | None) -> str:
    if theme in _PALETTES:
        return theme
    # migrate legacy dark theme
    if theme in ("dark", "black"):
        return "mist"
    return "mist"


# Cool-ink cabinet chrome — each theme has a distinct hue (not near-identical slate).
_SIDEBAR_INK: dict[str, dict[str, str]] = {
    "mist": {
        "sidebar": "#1A2740",
        "sidebar_alt": "#141E32",
        "sidebar_text": "#EAF0FA",
        "sidebar_muted": "#93A4BF",
    },
    "sky": {
        "sidebar": "#0A2F4D",
        "sidebar_alt": "#072438",
        "sidebar_text": "#E7F5FF",
        "sidebar_muted": "#7EB6D9",
    },
    "mint": {
        "sidebar": "#0D2F26",
        "sidebar_alt": "#0A241D",
        "sidebar_text": "#E8FBF4",
        "sidebar_muted": "#7DBFA8",
    },
    "lavender": {
        "sidebar": "#24183A",
        "sidebar_alt": "#1A1230",
        "sidebar_text": "#F3EDFF",
        "sidebar_muted": "#B5A4D9",
    },
    "peach": {
        "sidebar": "#2E1A12",
        "sidebar_alt": "#23140E",
        "sidebar_text": "#FFF4EC",
        "sidebar_muted": "#D4A88A",
    },
    "graphite": {
        "sidebar": "#1C1F24",
        "sidebar_alt": "#15171B",
        "sidebar_text": "#F1F3F5",
        "sidebar_muted": "#9AA3AF",
    },
}


def _hex_to_rgba(hex_color: str, alpha: float) -> str:
    raw = (hex_color or "").strip().lstrip("#")
    if len(raw) != 6:
        return f"rgba(255, 255, 255, {alpha:.3f})"
    try:
        r = int(raw[0:2], 16)
        g = int(raw[2:4], 16)
        b = int(raw[4:6], 16)
    except ValueError:
        return f"rgba(255, 255, 255, {alpha:.3f})"
    return f"rgba({r}, {g}, {b}, {alpha:.3f})"


def get_theme_palette(theme: str | None) -> dict[str, str]:
    key = normalize_theme(theme)
    p = dict(_PALETTES[key])
    ink = _SIDEBAR_INK[key]
    p.update(ink)
    accent = p["accent"]
    p["sidebar_hover"] = _hex_to_rgba(accent, 0.16)
    p["sidebar_active"] = _hex_to_rgba(accent, 0.26)
    p["sidebar_line"] = _hex_to_rgba(accent, 0.28)
    p["inlay"] = accent
    return p


def _file_search_overlay_styles(p: dict[str, str]) -> str:
    """Styles for the frameless file-search Tool window (follows app theme)."""
    return f"""
QWidget#fileSearchOverlay {{
    background: transparent;
}}
QWidget#fileSearchCard {{
    background: {p['card']};
    border: 1px solid {p['border']};
    border-left: 3px solid {p['accent']};
    border-radius: 14px;
}}
QWidget#fileSearchTitleBar {{
    background: transparent;
    border: none;
}}
QWidget#fileSearchCard QLabel#fileSearchTitle {{
    color: {p['text']};
    font-size: 15px;
    font-weight: 600;
    background: transparent;
}}
QWidget#fileSearchCard QPushButton#fileSearchCloseBtn {{
    background: transparent;
    color: {p['text_muted']};
    border: none;
    border-radius: 8px;
    font-size: 18px;
    font-weight: 400;
    padding: 0;
    min-height: 0;
}}
QWidget#fileSearchCard QPushButton#fileSearchCloseBtn:hover {{
    background: {p['card_alt']};
    color: {p['text']};
}}
QWidget#fileSearchCard QPushButton#fileSearchCloseBtn:pressed {{
    background: {p['accent_soft']};
    color: {p['accent']};
}}
QWidget#fileSearchCard QPushButton#fileSearchGlobalBtn {{
    background: {p['card']};
    color: {p['accent']};
    border: 1px solid {p['accent']};
    border-radius: 8px;
    padding: 4px 12px;
    font-size: 12px;
    font-weight: 600;
    min-height: 0;
}}
QWidget#fileSearchCard QPushButton#fileSearchGlobalBtn:hover {{
    background: {p['accent_soft']};
}}
QWidget#fileSearchCard QPushButton#fileSearchGlobalBtn:pressed {{
    background: {p['accent']};
    color: #ffffff;
}}
QWidget#fileSearchCard QLabel#fileSearchScope,
QWidget#fileSearchCard QLabel#fileSearchStatus,
QWidget#fileSearchCard QLabel#fileSearchHint {{
    color: {p['text_muted']};
    font-size: 12px;
    background: transparent;
}}
QWidget#fileSearchCard QLabel#fileSearchLocation {{
    color: {p['text_muted']};
    font-size: 12px;
    background: {p['card_alt']};
    border: 1px solid {p['border']};
    border-radius: 8px;
    padding: 8px 10px;
}}
QWidget#fileSearchCard QLineEdit#fileSearchInput {{
    background: {p['input']};
    color: {p['text']};
    border: 1px solid {p['border']};
    border-radius: 10px;
    padding: 10px 12px;
    font-size: 14px;
    selection-background-color: {p['accent_soft']};
}}
QWidget#fileSearchCard QLineEdit#fileSearchInput:focus {{
    border: 1px solid {p['accent']};
}}
QWidget#fileSearchCard QListWidget#fileSearchList {{
    background: {p['card_alt']};
    color: {p['text']};
    border: 1px solid {p['border']};
    border-radius: 10px;
    outline: none;
    padding: 4px;
}}
QWidget#fileSearchCard QListWidget#fileSearchList::item {{
    padding: 8px 10px;
    border-radius: 8px;
}}
QWidget#fileSearchCard QListWidget#fileSearchList::item:hover {{
    background: {p['accent_soft']};
}}
QWidget#fileSearchCard QListWidget#fileSearchList::item:selected {{
    background: {p['accent_soft']};
    color: {p['text']};
}}
QWidget#fileSearchCard QScrollBar:vertical {{
    background: transparent;
    width: 8px;
    margin: 2px;
}}
QWidget#fileSearchCard QScrollBar::handle:vertical {{
    background: {p['scroll']};
    border-radius: 4px;
    min-height: 24px;
}}
QWidget#fileSearchCard QScrollBar::add-line:vertical,
QWidget#fileSearchCard QScrollBar::sub-line:vertical {{
    height: 0;
}}
"""


def build_stylesheet(theme: str | None = "mist") -> str:
    p = get_theme_palette(theme)
    return f"""
* {{
    font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
}}

QMainWindow, QDialog {{
    background-color: {p['window']};
}}

QWidget#centralWidget {{
    background-color: {p['window']};
}}

QFrame#sidebar {{
    background-color: {p['sidebar']};
    border-right: 1px solid {p['sidebar_line']};
}}

QFrame#sidebarBrand {{
    background-color: transparent;
    border: none;
    border-bottom: 1px solid {p['sidebar_line']};
    border-radius: 0;
}}

QLabel#brandTag {{
    color: {p['inlay']};
    font-size: 10px;
    font-weight: 800;
    letter-spacing: 2.4px;
}}

QLabel#sidebarTip {{
    color: {p['sidebar_muted']};
    font-size: 11px;
    line-height: 1.4;
    padding: 11px 12px;
    background-color: {p['sidebar_hover']};
    border: 1px solid {p['sidebar_line']};
    border-radius: 10px;
}}

QFrame#contentPanel {{
    background-color: {p['content']};
}}

QLabel#pageKicker {{
    color: {p['accent']};
    font-size: 11px;
    font-weight: 800;
    letter-spacing: 2.2px;
}}

QLabel#pageTitle {{
    color: {p['text']};
    font-size: 22px;
    font-weight: 800;
    letter-spacing: -0.2px;
}}

QListWidget#helpTopicList {{
    background-color: {p['card']};
    border: 1px solid {p['border']};
    border-radius: 12px;
    padding: 6px;
    outline: none;
}}

QListWidget#helpTopicList::item {{
    color: {p['nav_text']};
    padding: 8px 10px;
    border-radius: 8px;
    margin: 1px 0;
}}

QListWidget#helpTopicList::item:selected {{
    background-color: {p['nav_active_bg']};
    color: {p['nav_active']};
    font-weight: 700;
}}

QListWidget#helpTopicList::item:hover {{
    background-color: {p['accent_soft']};
    color: {p['text']};
}}

QTextBrowser#helpBrowser {{
    background-color: {p['card']};
    border: 1px solid {p['border']};
    border-radius: 12px;
    padding: 14px 16px;
    color: {p['text']};
    selection-background-color: {p['accent_soft']};
    selection-color: {p['text']};
}}

QDialog#helpDialog {{
    background-color: {p['window']};
}}

QDialog#firstRunGuide {{
    background-color: {p['window']};
}}

QLabel#guideKicker {{
    color: {p['accent']};
    font-size: 11px;
    font-weight: 800;
    letter-spacing: 1.4px;
}}

QLabel#guideHeading {{
    color: {p['text']};
    font-size: 18px;
    font-weight: 800;
}}

QLabel#guideTitle {{
    color: {p['text']};
    font-size: 16px;
    font-weight: 800;
}}

QLabel#guideBody {{
    color: {p['text_muted']};
    font-size: 13px;
    line-height: 1.45;
}}

QLabel#guideStep {{
    color: {p['nav_text']};
    font-size: 12px;
    padding: 7px 10px;
    border-radius: 10px;
}}

QLabel#guideStep[active="true"] {{
    color: {p['nav_active']};
    background-color: {p['nav_active_bg']};
    font-weight: 700;
}}

QFrame#guideCard {{
    background-color: {p['card']};
    border: 1px solid {p['border']};
    border-left: 3px solid {p['inlay']};
    border-radius: 14px;
}}

QWidget#guideSketch {{
    background: transparent;
}}

QPushButton#guideOpenBtn {{
    background-color: {p['accent_soft']};
    color: {p['nav_active']};
    border: 1px solid {p['border']};
    border-radius: 10px;
    padding: 8px 10px;
    font-weight: 700;
}}

QPushButton#guideOpenBtn:hover {{
    background-color: {p['nav_active_bg']};
}}

QLabel#pageBadge {{
    color: {p['nav_active']};
    background-color: {p['nav_active_bg']};
    border-radius: 10px;
    padding: 3px 10px;
    font-size: 12px;
    font-weight: 700;
}}

QLabel#desktopLayoutDate {{
    color: {p['text_muted']};
    font-size: 12px;
    font-weight: 600;
    padding: 2px 4px;
}}

/* AIGC START — page button list (no outer frame) + zone icon pane */
QListWidget#desktopPageListView {{
    background: transparent;
    border: none;
    outline: none;
    padding: 0px;
}}

QListWidget#desktopPageListView::viewport {{
    background: transparent;
}}

QListWidget#desktopPageListView::item {{
    background: transparent;
    border: none;
    border-radius: 10px;
    color: {p['text']};
    padding: 0px;
    margin: 0px;
}}

QListWidget#desktopPageListView::item:hover {{
    background: transparent;
}}

QListWidget#desktopPageListView::item:selected {{
    background: transparent;
}}

QListWidget#desktopFenceIconView {{
    background: transparent;
    border: 1px solid {p['border']};
    border-radius: 10px;
    outline: none;
    padding: 8px 6px 12px 6px;
}}

QListWidget#desktopFenceIconView::item {{
    background: transparent;
    border: none;
    border-radius: 12px;
    color: {p['text']};
    padding: 0px;
    margin: 0px;
}}

QListWidget#desktopFenceIconView::item:hover {{
    background: transparent;
}}

QListWidget#desktopFenceIconView::item:selected {{
    background: transparent;
}}

QPushButton#desktopLayoutAddBtn {{
    background-color: {p['accent_soft']};
    color: {p['nav_active']};
    border: 1px solid {p['border']};
    border-radius: 8px;
    padding: 0px;
    min-width: 28px;
    max-width: 28px;
    min-height: 28px;
    max-height: 28px;
}}

QPushButton#desktopLayoutAddBtn:hover {{
    background-color: {p['nav_active_bg']};
    border-color: {p['accent']};
}}
/* AIGC END */

QLabel#filesEmptyHint {{
    color: {p['text_muted']};
    font-size: 12px;
    padding: 2px 0 4px 0;
}}

QFrame#filesEmptyState {{
    background-color: transparent;
    border: none;
}}

QLabel#filesEmptyKicker {{
    color: {p['accent']};
    font-size: 11px;
    font-weight: 800;
    letter-spacing: 2.2px;
}}

QLabel#filesEmptyTitle {{
    color: {p['text']};
    font-size: 18px;
    font-weight: 800;
}}

QLabel#filesEmptyBody {{
    color: {p['text_muted']};
    font-size: 13px;
    max-width: 360px;
}}

QFrame#organizeStatTile {{
    background-color: {p['card_alt']};
    border: 1px solid {p['border']};
    border-radius: 10px;
}}

QLabel#organizeStatValue {{
    color: {p['text']};
    font-size: 22px;
    font-weight: 800;
    letter-spacing: -0.4px;
}}

QLabel#organizeStatCaption {{
    color: {p['text_muted']};
    font-size: 11px;
    font-weight: 600;
}}

QFrame#organizePreviewPanel {{
    background-color: {p['card']};
    border: 1px solid {p['border']};
    border-left: 3px solid {p['inlay']};
    border-radius: 12px;
}}

QLabel#organizePreviewSummary {{
    color: {p['text']};
    font-size: 13px;
    font-weight: 600;
    padding: 8px 10px;
    background-color: {p['card_alt']};
    border-radius: 8px;
}}

QLabel#organizePreviewSection {{
    color: {p['text_muted']};
    font-size: 11px;
    font-weight: 700;
    letter-spacing: 0.6px;
}}

QLabel#organizePreviewEmpty {{
    color: {p['text_muted']};
    font-size: 12px;
    padding: 6px 2px;
}}

QLabel#organizePreviewBuckets {{
    color: {p['text_muted']};
    font-size: 12px;
    line-height: 1.4;
}}

QLabel#organizePreviewFooter {{
    color: {p['text_muted']};
    font-size: 12px;
}}

QListWidget#organizePreviewList {{
    background-color: {p['card_alt']};
    border: 1px solid {p['border']};
    border-radius: 10px;
    padding: 4px;
    outline: none;
}}

QListWidget#organizePreviewList::item {{
    padding: 7px 8px;
    border-radius: 6px;
    color: {p['text']};
}}

QListWidget#organizePreviewList::item:alternate {{
    background-color: {p['card']};
}}

QSplitter#filesOrganizeSplitter::handle {{
    background: {p['border']};
    width: 1px;
    margin: 8px 8px;
}}

QFrame#toolbarCard {{
    background-color: {p['card']};
    border: 1px solid {p['border']};
    border-radius: 14px;
}}

QStackedWidget#contentStack {{
    background: transparent;
    border: none;
}}

QListWidget#sidebarNav {{
    background: transparent;
    border: none;
    outline: none;
    padding: 6px 0;
}}

QListWidget#sidebarNav::viewport {{
    background: transparent;
}}

QListWidget#sidebarNav::item {{
    color: {p['sidebar_muted']};
    padding: 7px 10px 7px 8px;
    border-radius: 8px;
    margin: 1px 0;
}}

QListWidget#sidebarNav::item:selected {{
    background-color: {p['sidebar_active']};
    color: {p['sidebar_text']};
    font-weight: 700;
    border-left: 3px solid {p['inlay']};
    padding-left: 7px;
}}

QListWidget#sidebarNav::item:hover {{
    background-color: {p['sidebar_hover']};
    color: {p['sidebar_text']};
}}

QFrame#sidebar QLabel#appTitle {{
    color: {p['sidebar_text']};
    font-size: 17px;
    font-weight: 800;
}}

QFrame#sidebar QLabel#appSubtitle {{
    color: {p['sidebar_muted']};
    font-size: 12px;
}}

QWidget#settingsPanel QLabel {{
    color: {p['text']};
}}

QWidget#extensionsPanel QLabel {{
    color: {p['text']};
}}

QScrollArea > QWidget > QWidget#extensionsPanel {{
    background: transparent;
}}

QScrollArea {{
    background: transparent;
    border: none;
}}

QScrollArea > QWidget > QWidget#settingsPanel {{
    background: transparent;
}}

QLabel#appTitle {{
    color: {p['sidebar_text']};
    font-size: 20px;
    font-weight: 800;
    letter-spacing: 0.2px;
}}

QLabel#appSubtitle {{
    color: {p['sidebar_muted']};
    font-size: 12px;
}}

QFrame#panelCard, QFrame#sectionCard {{
    background-color: {p['card']};
    border: 1px solid {p['border']};
    border-radius: 14px;
}}

QFrame#panelCard {{
    border-left: 3px solid {p['inlay']};
}}

QFrame#sectionInlay {{
    background-color: {p['inlay']};
    border: none;
    border-top-left-radius: 14px;
    border-bottom-left-radius: 14px;
}}

QWidget#sectionCardInner {{
    background: transparent;
}}

QFrame#sectionFooter {{
    border-top: 1px solid {p['border']};
}}

QSplitter::handle {{
    background-color: transparent;
}}

QSplitter::handle:vertical {{
    height: 8px;
}}

QSplitter::handle:vertical:hover {{
    background-color: {p['border']};
    border-radius: 4px;
}}

QLabel#sectionHint, QLabel#pageSubtitle {{
    color: {p['text_muted']};
    font-size: 12px;
    line-height: 1.45;
}}

QLabel#fieldLabel {{
    color: {p['text']};
    font-size: 13px;
}}

QFrame#compactCountStepper {{
    background: {p['input']};
    border: 1px solid {p['border']};
    border-radius: 12px;
    max-height: 44px;
}}

QFrame#compactCountStepper:hover {{
    border-color: {p['accent']};
}}

QPushButton#stepperBtn {{
    background: transparent;
    border: none;
    border-radius: 8px;
    color: {p['text']};
    font-size: 16px;
    font-weight: 600;
    padding: 0;
    min-width: 32px;
    max-width: 32px;
    min-height: 32px;
    max-height: 32px;
}}

QPushButton#stepperBtn:hover {{
    background: {p['accent_soft']};
    color: {p['accent']};
}}

QPushButton#stepperBtn:pressed {{
    background: {p['accent']};
    color: #FFFFFF;
}}

QPushButton#stepperBtn:disabled {{
    color: {p['text_muted']};
    background: transparent;
}}

QLabel#stepperValue {{
    color: {p['text']};
    font-size: 15px;
    font-weight: 700;
    min-width: 36px;
    padding: 0 4px;
}}

QLabel#stepperUnit {{
    color: {p['text_muted']};
    font-size: 12px;
    padding-right: 4px;
}}

QLabel#pageSubtitle {{
    font-size: 13px;
    max-width: 720px;
}}

QFrame#pinnedImage {{
    background-color: {p['card']};
    border: 2px solid {p['accent']};
    border-radius: 8px;
}}

QWidget#screenshotToolbar {{
    background-color: rgba(32, 36, 44, 236);
    border: 1px solid rgba(255, 255, 255, 40);
    border-radius: 8px;
}}

QWidget#screenshotToolbar QLabel#screenshotHint {{
    color: rgba(255, 255, 255, 160);
    font-size: 12px;
    padding-left: 8px;
}}

QWidget#screenshotToolbar QPushButton#screenshotBtnPrimary {{
    background-color: #07c160;
    color: #ffffff;
    border: none;
    border-radius: 4px;
    padding: 6px 14px;
    font-size: 13px;
    font-weight: bold;
    min-height: 18px;
}}

QWidget#screenshotToolbar QPushButton#screenshotBtnPrimary:hover {{
    background-color: #06ad56;
}}

QWidget#screenshotToolbar QPushButton#screenshotBtn {{
    background-color: transparent;
    color: #ffffff;
    border: none;
    border-radius: 4px;
    padding: 6px 12px;
    font-size: 13px;
    min-height: 18px;
}}

QWidget#screenshotToolbar QPushButton#screenshotBtn:hover {{
    background-color: rgba(255, 255, 255, 25);
}}

QWidget#screenshotToolbar QPushButton#screenshotBtn:checked {{
    background-color: rgba(7, 193, 96, 45);
    border: 1px solid rgba(7, 193, 96, 120);
    color: #ffffff;
}}

QWidget#screenshotToolbar QPushButton#screenshotColorBtn {{
    min-width: 26px;
    min-height: 26px;
    max-width: 26px;
    max-height: 26px;
    border-radius: 13px;
    border: 2px solid rgba(255, 255, 255, 80);
    color: transparent;
    padding: 0;
}}

QWidget#screenshotToolbar QPushButton#screenshotColorBtn[colorName="red"] {{
    background-color: #ff4d4f;
}}

QWidget#screenshotToolbar QPushButton#screenshotColorBtn[colorName="green"] {{
    background-color: #07c160;
}}

QWidget#screenshotToolbar QPushButton#screenshotColorBtn[colorName="yellow"] {{
    background-color: #faad14;
}}

QWidget#screenshotToolbar QPushButton#screenshotColorBtn[colorName="blue"] {{
    background-color: #40a9ff;
}}

QWidget#screenshotToolbar QPushButton#screenshotColorBtn:checked {{
    border: 2px solid #ffffff;
}}

QLabel#panelTitle {{
    color: {p['text']};
    font-size: 15px;
    font-weight: 700;
}}

QFrame#ruleGroupCard {{
    background-color: {p['card']};
    border: 1px solid {p['border']};
    border-radius: 12px;
}}

QScrollArea#rulePickerScroll {{
    background-color: {p['card_alt']};
    border: 1px solid {p['border']};
    border-radius: 12px;
}}

QWidget#rulePickerHost {{
    background-color: {p['card_alt']};
}}

QWidget#extensionRulePicker QLabel#panelTitle {{
    color: {p['accent']};
}}

QPushButton#ruleGroupActionBtn {{
    background-color: transparent;
    color: {p['text_muted']};
    border: 1px solid {p['border_strong']};
    border-radius: 8px;
    padding: 2px 10px;
    font-size: 12px;
    min-height: 22px;
}}

QPushButton#ruleGroupActionBtn:hover {{
    color: {p['text']};
    border-color: {p['accent']};
    background-color: {p['accent_soft']};
}}

QPushButton#extRuleChip {{
    background-color: {p['card']};
    color: {p['text']};
    border: 1px solid {p['border']};
    border-radius: 8px;
    padding: 6px 10px;
    font-size: 13px;
    min-width: 56px;
    min-height: 28px;
}}

QPushButton#extRuleChip:hover {{
    border-color: {p['accent']};
    color: {p['accent']};
    background-color: {p['accent_soft']};
}}

QPushButton#extRuleChip:checked {{
    background-color: {p['primary']};
    color: {p['text_inverse']};
    border: 2px solid {p['primary_hover']};
    font-weight: bold;
    padding: 5px 9px;
}}

QPushButton#extRuleChip:checked:hover {{
    background-color: {p['primary_hover']};
}}

QPushButton#primaryBtn {{
    background-color: {p['primary']};
    color: {p['text_inverse']};
    border: none;
    border-radius: 10px;
    padding: 10px 18px;
    font-size: 14px;
    font-weight: 700;
    min-height: 22px;
}}

QPushButton#primaryBtn:hover {{
    background-color: {p['primary_hover']};
}}

QPushButton#primaryBtn:pressed {{
    background-color: {p['primary_hover']};
}}

QPushButton#secondaryBtn {{
    background-color: {p['card']};
    color: {p['text']};
    border: 1px solid {p['border']};
    border-radius: 10px;
    padding: 8px 14px;
    font-size: 13px;
    min-height: 18px;
}}

QPushButton#secondaryBtn:hover {{
    background-color: {p['accent_soft']};
    border-color: {p['accent']};
    color: {p['accent_hover']};
}}

QPushButton#notepadCloseBtn {{
    background: transparent;
    color: {p['text_muted']};
    border: none;
    border-radius: 8px;
    font-size: 18px;
    font-weight: 400;
    padding: 0;
    min-height: 0;
}}

QPushButton#notepadCloseBtn:hover {{
    background: {p['card_alt']};
    color: {p['text']};
}}

QPushButton#notepadCloseBtn:pressed {{
    background: {p['accent_soft']};
    color: {p['accent']};
}}

QToolButton#notepadPinBtn {{
    border: none;
    padding: 0;
    margin: 0;
    background: transparent;
    font-size: 11px;
    color: {p['text_muted']};
}}

QToolButton#notepadPinBtn[pinned="true"] {{
    color: {p['accent']};
}}

QToolButton#notepadPinBtn:hover {{
    background: {p['card_alt']};
    border-radius: 4px;
}}

QToolButton#notepadTabCloseBtn {{
    border: none;
    padding: 0;
    margin: 0;
    background: transparent;
    font-size: 14px;
    font-weight: 600;
    color: {p['text_muted']};
}}

QToolButton#notepadTabCloseBtn:hover {{
    background: {p['card_alt']};
    border-radius: 4px;
    color: {p['text']};
}}

QPushButton#dangerBtn {{
    background-color: {p['danger']};
    color: {p['text_inverse']};
    border: none;
    border-radius: 10px;
    padding: 8px 16px;
    font-size: 13px;
    min-height: 18px;
}}

QPushButton#dangerBtn:hover {{
    background-color: {p['danger_hover']};
}}

QPushButton#iconActionBtn,
QPushButton#iconDangerBtn {{
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 0;
    min-width: 28px;
    max-width: 28px;
    min-height: 28px;
    max-height: 28px;
}}

QPushButton#iconActionBtn:hover {{
    background-color: {p['accent_soft']};
}}

QPushButton#iconDangerBtn:hover {{
    background-color: rgba(239, 68, 68, 0.12);
}}

QListWidget {{
    background-color: {p['table_bg']};
    border: 1px solid {p['border']};
    border-radius: 12px;
    color: {p['text']};
    padding: 4px;
    outline: none;
}}

QListWidget::item {{
    padding: 8px 12px;
    border-radius: 6px;
}}

QListWidget::item:selected {{
    background-color: {p['accent_soft']};
    color: {p['accent_hover']};
}}

QListWidget::item:hover {{
    background-color: {p['card_alt']};
}}

QTableWidget {{
    background-color: {p['table_bg']};
    border: 1px solid {p['border']};
    border-radius: 12px;
    color: {p['text']};
    gridline-color: transparent;
    alternate-background-color: {p['table_alt']};
    outline: none;
    show-decoration-selected: 1;
    selection-background-color: {p['accent_soft']};
    selection-color: {p['accent_hover']};
}}

QTableWidget:focus {{
    outline: none;
    border: 1px solid {p['border_strong']};
}}

QTableWidget#filesTable {{
    border: 1px solid {p['border']};
}}

QListWidget#snapshotIconView {{
    background: transparent;
    border: none;
    outline: none;
    padding: 4px 2px 12px 2px;
}}

/* Delegate owns card chrome — keep ::item transparent so action chips stay visible. */
QListWidget#snapshotIconView::item {{
    background: transparent;
    border: none;
    border-radius: 12px;
    color: {p['text']};
    padding: 0px;
    margin: 0px;
}}

QListWidget#snapshotIconView::item:hover {{
    background: transparent;
    border: none;
}}

QListWidget#snapshotIconView::item:selected {{
    background: transparent;
    border: none;
    color: {p['text']};
}}

QListWidget#snapshotIconView::item:selected:hover {{
    background: transparent;
    border: none;
}}

QTableWidget::item {{
    padding: 8px 12px;
    border: none;
    outline: none;
}}

QTableWidget::item:selected {{
    background-color: {p['accent_soft']};
    color: {p['text']};
    border: none;
    outline: none;
}}

QTableWidget::item:focus {{
    border: none;
    outline: none;
    background-color: {p['accent_soft']};
}}

QHeaderView::section {{
    background-color: {p['header_bg']};
    color: {p['text_muted']};
    border: none;
    border-bottom: 1px solid {p['border']};
    padding: 10px 14px;
    font-weight: 700;
    font-size: 11px;
    letter-spacing: 0.6px;
}}

QTreeWidget {{
    background-color: {p['table_bg']};
    border: 1px solid {p['border']};
    border-radius: 12px;
    color: {p['text']};
    alternate-background-color: {p['table_alt']};
    outline: none;
    show-decoration-selected: 1;
}}

QTreeWidget::item {{
    padding: 9px 10px;
    border: none;
    outline: none;
}}

QTreeWidget::item:hover {{
    background-color: {p['card_alt']};
    color: {p['text']};
}}

QTreeWidget::item:selected {{
    background-color: {p['table_alt']};
    color: {p['text']};
}}

QTreeWidget::item:selected:active {{
    background-color: {p['card_alt']};
    color: {p['text']};
}}

QTreeWidget::branch {{
    background: transparent;
}}

QTreeWidget::branch:hover {{
    background-color: {p['card_alt']};
}}

QTreeWidget::branch:selected {{
    background-color: {p['table_alt']};
}}

QTreeWidget::branch:has-children:closed,
QTreeWidget::branch:closed:has-children:has-siblings {{
    border-image: none;
    image: none;
}}

QTreeWidget::branch:open:has-children,
QTreeWidget::branch:open:has-children:has-siblings {{
    border-image: none;
    image: none;
}}

QTreeWidget#pageFenceTree {{
    background: transparent;
    border: none;
    padding: 4px 0;
    outline: none;
}}

QTreeWidget#pageFenceTree::item {{
    padding: 3px 0;
    margin: 0;
    border: none;
    background: transparent;
}}

QTreeWidget#pageFenceTree::item:selected,
QTreeWidget#pageFenceTree::item:selected:active,
QTreeWidget#pageFenceTree::item:hover,
QTreeWidget#pageFenceTree::branch,
QTreeWidget#pageFenceTree::branch:hover,
QTreeWidget#pageFenceTree::branch:selected {{
    background: transparent;
    color: {p['text']};
}}

QFrame#pageFencePageCard {{
    background: {p['card_alt']};
    border: 1px solid {p['border']};
    border-radius: 12px;
}}

QFrame#pageFenceItemCard {{
    background: transparent;
    border: 1px solid transparent;
    border-radius: 10px;
}}

QFrame#pageFenceItemCard:hover {{
    background: {p['accent_soft']};
    border-color: {p['border']};
}}

QLabel#pageTreeTitle {{
    color: {p['text']};
    font-size: 14px;
    font-weight: 700;
}}

QLabel#pageFenceItemTitle {{
    color: {p['text']};
    font-size: 13px;
    font-weight: 600;
}}

QLabel#pageFenceMeta {{
    color: {p['text_muted']};
    font-size: 12px;
}}

QPushButton#pageFenceChevron {{
    background: transparent;
    border: none;
    border-radius: 8px;
    padding: 0;
}}

QPushButton#pageFenceChevron:hover {{
    background: {p['accent_soft']};
}}

QPushButton#pageFenceTextBtn {{
    background: transparent;
    color: {p['text_muted']};
    border: none;
    border-radius: 8px;
    padding: 4px 10px;
    font-size: 12px;
    font-weight: 600;
    min-height: 0;
}}

QPushButton#pageFenceTextBtn:hover {{
    background: {p['card']};
    color: {p['text']};
}}

QPushButton#pageFenceTextBtn[tone="accent"] {{
    color: {p['accent_hover']};
}}

QPushButton#pageFenceTextBtn[tone="accent"]:hover {{
    background: {p['nav_active_bg']};
    color: {p['nav_active']};
}}

QWidget#pageFoldersList {{
    background: transparent;
}}

QFrame#pageFolderRow {{
    background: {p['card_alt']};
    border: 1px solid {p['border']};
    border-radius: 12px;
}}

QFrame#pageFolderRow:hover {{
    background: {p['accent_soft']};
    border-color: {p['border']};
}}

QLabel#pageFolderName {{
    color: {p['text']};
    font-size: 13px;
    font-weight: 700;
}}

QLabel#pageFolderPath {{
    color: {p['text_muted']};
    font-size: 12px;
}}

QLabel#pageFoldersEmpty {{
    color: {p['text_muted']};
    font-size: 13px;
    padding: 22px 12px;
    background: {p['card_alt']};
    border: 1px dashed {p['border']};
    border-radius: 12px;
}}

QCheckBox {{
    color: {p['text']};
    spacing: 8px;
}}

QCheckBox::indicator {{
    width: 18px;
    height: 18px;
    border-radius: 5px;
    border: 1px solid {p['border_strong']};
    background-color: {p['input']};
}}

QCheckBox::indicator:checked {{
    background-color: {p['primary']};
    border-color: {p['primary']};
}}

QGroupBox#fencePagesGroup {{
    background-color: {p['card_alt']};
    border: 1px solid {p['border']};
    border-radius: 12px;
    margin-top: 8px;
    padding-top: 6px;
}}

QGroupBox#fencePagesGroup::title {{
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 6px;
    color: {p['text_muted']};
}}

QTabWidget::pane {{
    border: 1px solid {p['border']};
    border-radius: 12px;
    background-color: {p['card']};
}}

QTabBar::tab {{
    background-color: {p['card_alt']};
    color: {p['text_muted']};
    padding: 10px 16px;
    border-top-left-radius: 10px;
    border-top-right-radius: 10px;
    margin-right: 2px;
    min-width: 72px;
}}

QTabBar::scroller {{
    width: 24px;
}}

QTabBar::tab:selected {{
    background-color: {p['card']};
    color: {p['accent']};
}}

QProgressBar {{
    background-color: {p['card_alt']};
    border: 1px solid {p['border']};
    border-radius: 8px;
    text-align: center;
    color: {p['text']};
    height: 20px;
}}

QProgressBar::chunk {{
    background-color: {p['primary']};
    border-radius: 7px;
}}

QScrollBar:vertical {{
    background: {p['card_alt']};
    width: 8px;
    border-radius: 4px;
}}

QScrollBar::handle:vertical {{
    background: {p['scroll']};
    border-radius: 4px;
    min-height: 20px;
}}

QScrollBar::handle:vertical:hover {{
    background: {p['border_strong']};
}}

QFrame#fenceContainer {{
    background-color: {p['fence_bg']};
    border: 2px solid {p['fence_border']};
    border-radius: 14px;
}}

QWidget#fenceHeader {{
    background-color: {p['fence_header']};
    border: none;
    border-top-left-radius: 12px;
    border-top-right-radius: 12px;
}}

QFrame#fenceAccent {{
    background-color: {p['fence_border']};
    border: none;
    border-radius: 2px;
}}

QLabel#fenceTitle {{
    color: {_DEFAULT_FENCE_TITLE_COLOR};
    font-size: 13px;
    font-weight: 800;
}}

QLabel#fenceItem {{
    color: {_DEFAULT_FENCE_TEXT_COLOR};
    background-color: transparent;
    border: none;
    /* Descent is reserved by caption_box_height — extra padding here clipped line 2. */
    padding: 0px;
    margin: 0px;
    font-size: 12px;
}}

QLabel#fenceEmpty {{
    color: {_DEFAULT_FENCE_MUTED_COLOR};
    font-size: 12px;
    padding: 20px;
}}

QPushButton#fenceBtn {{
    background-color: rgba(255, 255, 255, 38);
    color: {_DEFAULT_FENCE_MUTED_COLOR};
    border: none;
    border-radius: 4px;
    font-size: 14px;
}}

QPushButton#fenceBtn:hover {{
    color: {_DEFAULT_FENCE_TITLE_COLOR};
    background-color: rgba(255, 255, 255, 72);
}}

QScrollArea#fenceScroll {{
    background: transparent;
    border: none;
}}

QScrollArea#fenceScroll > QWidget > QWidget {{
    background: transparent;
}}

QPushButton#pageBtn,
QPushButton#pageFolderBtn,
QPushButton#pageMinutesBtn {{
    background: transparent;
    border: none;
    padding: 0;
    margin: 0;
    min-width: 88px;
    min-height: 40px;
    color: transparent;
}}

QMessageBox {{
    background-color: {p['card']};
}}

QMessageBox QLabel {{
    color: {p['text']};
}}

QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {{
    background-color: {p['input']};
    border: 1px solid {p['border']};
    border-radius: 10px;
    color: {p['text']};
    padding: 7px 10px;
    min-height: 18px;
}}

QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover {{
    border-color: {p['accent']};
}}

QComboBox::drop-down {{
    border: none;
    width: 24px;
}}

QComboBox QAbstractItemView {{
    background-color: {p['card']};
    color: {p['text']};
    selection-background-color: {p['accent_soft']};
    border: 1px solid {p['border']};
}}

{_file_search_overlay_styles(p)}

QStatusBar {{
    background-color: {p['content']};
    color: {p['text_muted']};
    border-top: 1px solid {p['border']};
    min-height: 26px;
    font-size: 12px;
}}
"""


# Backward-compatible default stylesheet.
STYLESHEET = build_stylesheet("mist")


def fence_theme_defaults(theme: str | None = None) -> dict[str, Any]:
    """Default fence paint style — charcoal glass (matches live desktop look).

    UI theme palettes still expose ``fence_bg`` for settings chrome; desktop
    partitions use this charcoal default unless the user applies a preset.
    """
    _ = theme  # kept for call-site compatibility
    from src.fence_style import (
        DEFAULT_FENCE_STYLE_PRESET,
        fence_style_visual_from_preset,
    )

    visual = fence_style_visual_from_preset(DEFAULT_FENCE_STYLE_PRESET)
    return {
        **visual,
        "show_title": True,
        "view_mode": "grid",
        "collapsible": True,
    }
