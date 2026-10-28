#pragma once

#include <cstdint>
#include <string>

std::wstring NativeDataDir();
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::string NarrowUtf8(const std::wstring& ws);
std::wstring WidenUtf8(const std::string& s);

// Text file encoding for notes (P0-4). Detect on open; write back same codec.
enum class TextFileEncoding {
  Utf8,
  Utf8Bom,
  Acp,    // Windows ANSI (GBK on zh-CN)
  Utf16Le,
  Utf16Be,
};

// Decode file bytes → UTF-16. Tries utf-8-sig, utf-8, ACP, utf-16. Returns
// false only when every codec fails (caller should refuse to overwrite).
bool DecodeTextFileBytes(const std::string& raw, std::wstring* out,
                         TextFileEncoding* detected);
// Encode UTF-16 → bytes using *enc* (ACP uses CP_ACP).
bool EncodeTextFileBytes(const std::wstring& text, TextFileEncoding enc,
                         std::string* out);

// U4 portable mode. True when the exe directory contains `portable.txt` or the
// command line carries `--portable`. When portable, NativeDataDir() resolves to
// `<exe dir>\data` instead of %LOCALAPPDATA%\DeskTidyNative (Python semantics).
bool IsPortableMode();
// Directory of the running exe (no trailing slash), resolved once.
std::wstring NativeExeDir();

// Atomic UTF-8 write: temp + flush + MoveFileEx replace (optional .bak).
bool AtomicWriteUtf8File(const std::wstring& path, const std::string& utf8,
                         bool keep_bak = true);

// Escape a UTF-8 string for embedding inside a JSON "..." value.
std::string EscapeJsonUtf8(const std::string& s);

// Read a UTF-8 text file; if missing/empty/tiny, try path.bak (layout-style).
bool ReadUtf8FileWithBak(const std::wstring& path, std::string* out);

// True when path is missing or its size is below min_bytes (treat as empty).
bool FileMissingOrTiny(const std::wstring& path, std::uint32_t min_bytes = 8);

// One-shot: when running non-portable, copy sparse user files (e.g.
// page_folders.json) from orphan portable sidecars (<exe>\data and the
// Desktidy ↔ Desktidy-cpp mirror build tree). Prevents "settings vanished"
// after switching portable debug builds → installed LocalAppData.
bool AdoptOrphanPortableSidecarOnce();
