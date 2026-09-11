"""Validate bundled FFmpeg for DeskTidy screen recording (performance-critical)."""

from __future__ import annotations

import subprocess
from dataclasses import dataclass, field
from pathlib import Path

# DeskTidy recording path (see screen_record_manager.build_record_command):
# - Single monitor: ddagrab + h264_mf (HW, low CPU)
# - Multi-monitor / crop: gdigrab + h264_mf or libx264
# - Startup probe: lavfi color + h264_mf / libx264

PREFERRED_ENCODER = "h264_mf"
FALLBACK_ENCODER = "libx264"
REQUIRED_DEMUXERS = ("gdigrab", "lavfi")
PERF_DEMUXERS = ("ddagrab",)  # missing → gdigrab only (correct but slower on single monitor)
REQUIRED_ENCODERS = (PREFERRED_ENCODER, FALLBACK_ENCODER)  # at least one must probe OK

# Bundled folder size guard — full static Gyan builds land ~200MB+ for ffmpeg.exe alone.
MAX_BUNDLE_EXE_MB = 180.0
MAX_BUNDLE_FOLDER_MB = 170.0  # gpl-shared ffmpeg.exe + DLLs (no ffplay/ffprobe)


@dataclass
class FfmpegValidation:
    ok: bool
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    preferred_encoder: str = ""
    demuxers: set[str] = field(default_factory=set)
    encoders: set[str] = field(default_factory=set)
    bundle_mb: float = 0.0


def _creation_flags() -> int:
    if hasattr(subprocess, "CREATE_NO_WINDOW"):
        return subprocess.CREATE_NO_WINDOW  # type: ignore[attr-defined]
    return 0


def _run_text(exe: Path, args: list[str], *, timeout: float = 12.0) -> str:
    completed = subprocess.run(
        [str(exe), *args],
        capture_output=True,
        creationflags=_creation_flags(),
        timeout=timeout,
        check=False,
    )
    out = (completed.stdout or b"") + (completed.stderr or b"")
    return out.decode("utf-8", errors="replace")


def _parse_ffmpeg_list(text: str) -> set[str]:
    names: set[str] = set()
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("-") or "----" in line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        head, name = parts[0], parts[1]
        if head.endswith("."):
            names.add(name)
        elif head in ("D", "DE", "E", "EV") or head[:1] in ("V", "A"):
            names.add(name)
    return names


def list_devices(exe: Path) -> set[str]:
    return _parse_ffmpeg_list(_run_text(exe, ["-hide_banner", "-devices"]))


def list_demuxers(exe: Path) -> set[str]:
    return _parse_ffmpeg_list(_run_text(exe, ["-hide_banner", "-demuxers"]))


def list_encoders(exe: Path) -> set[str]:
    return _parse_ffmpeg_list(_run_text(exe, ["-hide_banner", "-encoders"]))


def list_input_devices(exe: Path) -> set[str]:
    return list_demuxers(exe) | list_devices(exe)


def has_ddagrab(exe: Path) -> bool:
    """True when ffmpeg exposes the ``ddagrab`` lavfi source (Desktop Duplication).

    ``ddagrab`` is a *filter*, not a demuxer — probing ``demuxer=ddagrab`` always
    failed and forced the gdigrab path (hardware cursor flicker on Windows).
    """
    text = _run_text(exe, ["-hide_banner", "-h", "filter=ddagrab"], timeout=6.0)
    low = text.lower()
    if "unknown filter" in low or "unknown option" in low:
        return False
    return "ddagrab" in low and "desktop duplication" in low


def probe_working_encoder(exe: Path) -> str:
    """Mirror ScreenRecordManager warm path — pick lowest-CPU encoder that works."""
    from src.screen_record_manager import probe_preferred_encoder

    return probe_preferred_encoder(exe)


def bundle_folder_size_mb(folder: Path) -> float:
    if not folder.is_dir():
        return 0.0
    total = sum(p.stat().st_size for p in folder.rglob("*") if p.is_file())
    return total / (1024 * 1024)


def validate_recording_ffmpeg(exe: Path, *, bundle_dir: Path | None = None) -> FfmpegValidation:
    result = FfmpegValidation(ok=False)
    if not exe.is_file():
        result.errors.append(f"missing executable: {exe}")
        return result

    folder = bundle_dir or exe.parent
    result.bundle_mb = bundle_folder_size_mb(folder)
    exe_mb = exe.stat().st_size / (1024 * 1024)
    if exe_mb > MAX_BUNDLE_EXE_MB:
        result.warnings.append(
            f"ffmpeg.exe is {exe_mb:.1f} MB — consider scripts/prepare_ffmpeg_recording.py "
            f"(gpl-shared, ~70–90 MB installed) instead of a full static build."
        )
    if result.bundle_mb > MAX_BUNDLE_FOLDER_MB:
        result.warnings.append(
            f"assets/ffmpeg folder is {result.bundle_mb:.1f} MB — larger than expected for recording-only bundle."
        )

    try:
        result.demuxers = list_input_devices(exe)
        result.encoders = list_encoders(exe)
    except (OSError, subprocess.TimeoutExpired) as exc:
        result.errors.append(f"cannot query ffmpeg capabilities: {exc}")
        return result

    for name in REQUIRED_DEMUXERS:
        if name not in result.demuxers:
            result.errors.append(f"missing capture input: {name}")

    if not has_ddagrab(exe):
        result.warnings.append(
            "ddagrab filter unavailable — capture uses gdigrab "
            "(Windows hardware cursor may flicker while recording)."
        )

    listed = [enc for enc in REQUIRED_ENCODERS if enc in result.encoders]
    if not listed:
        result.errors.append("missing H.264 encoders (need h264_mf and/or libx264)")
        return result

    try:
        result.preferred_encoder = probe_working_encoder(exe)
    except Exception as exc:  # noqa: BLE001 — validation surface
        result.errors.append(f"encoder probe failed: {exc}")
        return result

    if result.preferred_encoder == FALLBACK_ENCODER:
        if PREFERRED_ENCODER in result.encoders:
            result.warnings.append(
                "h264_mf is listed but probe failed — recording will use libx264 (higher CPU)."
            )
        else:
            result.warnings.append(
                "h264_mf unavailable — recording will use libx264 (higher CPU on long captures)."
            )
    elif result.preferred_encoder != PREFERRED_ENCODER:
        result.warnings.append(f"unexpected preferred encoder: {result.preferred_encoder}")

    result.ok = not result.errors
    return result


def format_validation_report(v: FfmpegValidation, *, exe: Path) -> str:
    lines = [f"FFmpeg: {exe}", f"Bundle size: {v.bundle_mb:.1f} MB"]
    if v.preferred_encoder:
        lines.append(f"Preferred encoder: {v.preferred_encoder}")
    for msg in v.errors:
        lines.append(f"ERROR: {msg}")
    for msg in v.warnings:
        lines.append(f"WARN: {msg}")
    return "\n".join(lines)
