#include "markdown_preview.h"

#include "paths_util.h"

#include <Shellapi.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string EscapeHtml(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&':
        o += "&amp;";
        break;
      case '<':
        o += "&lt;";
        break;
      case '>':
        o += "&gt;";
        break;
      case '"':
        o += "&quot;";
        break;
      default:
        o.push_back(c);
        break;
    }
  }
  return o;
}

std::string InlineFormat(const std::string& line) {
  std::string s = EscapeHtml(line);
  // **bold**
  size_t p = 0;
  while ((p = s.find("**", p)) != std::string::npos) {
    const size_t q = s.find("**", p + 2);
    if (q == std::string::npos) {
      break;
    }
    s.replace(q, 2, "</strong>");
    s.replace(p, 2, "<strong>");
    p = q + 7;
  }
  // *italic*
  p = 0;
  while ((p = s.find('*', p)) != std::string::npos) {
    if (p + 1 < s.size() && s[p + 1] == '*') {
      p += 2;
      continue;
    }
    const size_t q = s.find('*', p + 1);
    if (q == std::string::npos) {
      break;
    }
    s.replace(q, 1, "</em>");
    s.replace(p, 1, "<em>");
    p = q + 3;
  }
  // `code`
  p = 0;
  while ((p = s.find('`', p)) != std::string::npos) {
    const size_t q = s.find('`', p + 1);
    if (q == std::string::npos) {
      break;
    }
    s.replace(q, 1, "</code>");
    s.replace(p, 1, "<code>");
    p = q + 5;
  }
  return s;
}

std::string ConvertMarkdown(const std::string& md) {
  std::istringstream in(md);
  std::ostringstream body;
  std::string line;
  bool in_ul = false;
  bool in_ol = false;
  bool in_code = false;
  auto close_lists = [&]() {
    if (in_ul) {
      body << "</ul>\n";
      in_ul = false;
    }
    if (in_ol) {
      body << "</ol>\n";
      in_ol = false;
    }
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("```", 0) == 0) {
      close_lists();
      if (!in_code) {
        body << "<pre><code>";
        in_code = true;
      } else {
        body << "</code></pre>\n";
        in_code = false;
      }
      continue;
    }
    if (in_code) {
      body << EscapeHtml(line) << "\n";
      continue;
    }
    if (line.empty()) {
      close_lists();
      body << "<p></p>\n";
      continue;
    }
    int heading = 0;
    while (heading < static_cast<int>(line.size()) && line[heading] == '#') {
      ++heading;
    }
    if (heading > 0 && heading <= 6 && heading < static_cast<int>(line.size()) &&
        line[heading] == ' ') {
      close_lists();
      body << "<h" << heading << ">" << InlineFormat(line.substr(heading + 1))
           << "</h" << heading << ">\n";
      continue;
    }
    if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0) {
      if (in_ol) {
        body << "</ol>\n";
        in_ol = false;
      }
      if (!in_ul) {
        body << "<ul>\n";
        in_ul = true;
      }
      body << "<li>" << InlineFormat(line.substr(2)) << "</li>\n";
      continue;
    }
    if (line.size() >= 3 && line[0] >= '0' && line[0] <= '9' &&
        line[1] == '.' && line[2] == ' ') {
      if (in_ul) {
        body << "</ul>\n";
        in_ul = false;
      }
      if (!in_ol) {
        body << "<ol>\n";
        in_ol = true;
      }
      body << "<li>" << InlineFormat(line.substr(3)) << "</li>\n";
      continue;
    }
    close_lists();
    body << "<p>" << InlineFormat(line) << "</p>\n";
  }
  close_lists();
  if (in_code) {
    body << "</code></pre>\n";
  }
  return body.str();
}

}  // namespace

std::wstring MarkdownToHtmlDocument(const std::wstring& md,
                                    const std::wstring& title) {
  std::ostringstream html;
  html << "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
       << "<title>" << EscapeHtml(NarrowUtf8(title)) << "</title>"
       << "<style>"
       << "body{font-family:Segoe UI,Microsoft YaHei,sans-serif;max-width:720px;"
       << "margin:24px auto;padding:0 16px;line-height:1.6;color:#1f2937;}"
       << "code{background:#f3f4f6;padding:2px 4px;border-radius:4px;}"
       << "pre{background:#111827;color:#e5e7eb;padding:12px;border-radius:8px;"
       << "overflow:auto;}"
       << "h1,h2,h3{color:#111827;}"
       << "</style></head><body>\n"
       << ConvertMarkdown(NarrowUtf8(md)) << "</body></html>";
  return WidenUtf8(html.str());
}

bool PreviewMarkdownText(HWND owner, const std::wstring& md,
                         const std::wstring& title) {
  const std::wstring html = MarkdownToHtmlDocument(md, title);
  const std::wstring path = JoinPath(NativeDataDir(), L"preview_note.html");
  {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      MessageBoxW(owner, L"无法写入预览文件。", L"预览", MB_OK | MB_ICONWARNING);
      return false;
    }
    out << NarrowUtf8(html);
  }
  const HINSTANCE r =
      ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) <= 32) {
    MessageBoxW(owner, L"无法打开浏览器预览。", L"预览", MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}

bool PreviewMarkdownFile(HWND owner, const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    MessageBoxW(owner, L"无法读取文件。", L"预览", MB_OK | MB_ICONWARNING);
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string raw = ss.str();
  // Strip RTF roughly — if looks like RTF, preview plain GetWindowText instead.
  if (raw.rfind("{\\rtf", 0) == 0) {
    MessageBoxW(owner, L"当前是 RTF。请另存为 .md/.txt 后再预览，或直接预览编辑区文本。",
                L"预览", MB_OK | MB_ICONINFORMATION);
    return false;
  }
  return PreviewMarkdownText(owner, WidenUtf8(raw), path);
}
