#pragma once

#include <Windows.h>

#include <string>

// Convert lightweight Markdown to a self-contained HTML document.
std::wstring MarkdownToHtmlDocument(const std::wstring& md, const std::wstring& title);
// Write HTML beside the note (or temp) and open with the default browser.
bool PreviewMarkdownFile(HWND owner, const std::wstring& path);
bool PreviewMarkdownText(HWND owner, const std::wstring& md,
                         const std::wstring& title = L"预览");

// Convert an HTML fragment (e.g. clipboard "HTML Format") to Markdown.
// Handles headings/lists/links/tables/bold/italic/code/images; falls back to
// plain text by stripping remaining tags.
std::wstring HtmlToMarkdown(const std::wstring& html);
