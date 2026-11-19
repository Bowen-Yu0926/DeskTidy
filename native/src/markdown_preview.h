#pragma once

#include <Windows.h>

#include <string>

// Convert lightweight Markdown to a self-contained HTML document.
// When base_dir is non-empty, relative image paths become file:// URIs
// (Python markdown_preview._rewrite_relative_images).
std::wstring MarkdownToHtmlDocument(const std::wstring& md,
                                    const std::wstring& title,
                                    const std::wstring& base_dir = L"");
// Write HTML beside the note (or temp) and open with the default browser.
bool PreviewMarkdownFile(HWND owner, const std::wstring& path);
bool PreviewMarkdownText(HWND owner, const std::wstring& md,
                         const std::wstring& title = L"预览",
                         const std::wstring& base_dir = L"");

// Convert an HTML fragment (e.g. clipboard "HTML Format") to Markdown.
// Handles headings/lists/links/tables/bold/italic/code/images; falls back to
// plain text by stripping remaining tags.
std::wstring HtmlToMarkdown(const std::wstring& html);

std::string ResolveImageSrc(const std::string& src, const std::wstring& base_dir);
