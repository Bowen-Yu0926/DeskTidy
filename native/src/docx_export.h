#pragma once

#include <string>

// Export Markdown text into a native OpenXML .docx file (compatible with Microsoft Word and WPS Office)
// with complete headings, paragraphs, lists, code blocks, blockquotes, inline formatting, and embedded images.
bool ExportMarkdownToDocx(const std::wstring& md_text,
                          const std::wstring& title,
                          const std::wstring& base_dir,
                          const std::wstring& dst_docx_path,
                          std::wstring* err = nullptr);
