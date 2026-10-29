#include "markdown_preview.h"

#include "mist_msg.h"
#include "paths_util.h"

#include <Shellapi.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::string PathToFileUri(const std::wstring& abs_path) {
  std::string utf8 = NarrowUtf8(abs_path);
  for (char& c : utf8) {
    if (c == '\\') {
      c = '/';
    }
  }
  // file:///C:/foo/bar.png
  std::string uri = "file:///";
  for (unsigned char c : utf8) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '/' || c == ':' || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      uri.push_back(static_cast<char>(c));
    } else {
      char buf[8];
      sprintf_s(buf, "%%%02X", c);
      uri += buf;
    }
  }
  return uri;
}

std::string ResolveImageSrc(const std::string& src,
                            const std::wstring& base_dir) {
  if (src.empty() || src.rfind("http://", 0) == 0 ||
      src.rfind("https://", 0) == 0 || src.rfind("data:", 0) == 0 ||
      src.rfind("file:", 0) == 0 || src[0] == '#') {
    return src;
  }
  if (base_dir.empty()) {
    return src;
  }
  std::wstring rel = WidenUtf8(src);
  for (wchar_t& c : rel) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  const std::wstring full = JoinPath(base_dir, rel);
  wchar_t canon[MAX_PATH * 4] = {};
  const DWORD n =
      GetFullPathNameW(full.c_str(), static_cast<DWORD>(MAX_PATH * 4), canon,
                       nullptr);
  if (n == 0 || n >= MAX_PATH * 4) {
    return src;
  }
  return PathToFileUri(canon);
}

std::string InlineFormat(const std::string& line,
                         const std::wstring& base_dir) {
  // Convert ![alt](src) before HTML-escaping the rest.
  std::string work;
  work.reserve(line.size() + 16);
  size_t i = 0;
  while (i < line.size()) {
    if (line[i] == '!' && i + 1 < line.size() && line[i + 1] == '[') {
      const size_t alt_end = line.find(']', i + 2);
      if (alt_end != std::string::npos && alt_end + 1 < line.size() &&
          line[alt_end + 1] == '(') {
        const size_t src_end = line.find(')', alt_end + 2);
        if (src_end != std::string::npos) {
          const std::string alt = line.substr(i + 2, alt_end - (i + 2));
          const std::string src =
              ResolveImageSrc(line.substr(alt_end + 2, src_end - (alt_end + 2)),
                              base_dir);
          work += "<img src=\"";
          work += EscapeHtml(src);
          work += "\" alt=\"";
          work += EscapeHtml(alt);
          work += "\">";
          i = src_end + 1;
          continue;
        }
      }
    }
    work.push_back(line[i]);
    ++i;
  }

  std::string s;
  s.reserve(work.size() + 16);
  i = 0;
  while (i < work.size()) {
    if (work.compare(i, 4, "<img") == 0) {
      const size_t end = work.find('>', i);
      if (end != std::string::npos) {
        s.append(work, i, end - i + 1);
        i = end + 1;
        continue;
      }
    }
    switch (work[i]) {
      case '&':
        s += "&amp;";
        break;
      case '<':
        s += "&lt;";
        break;
      case '>':
        s += "&gt;";
        break;
      case '"':
        s += "&quot;";
        break;
      default:
        s.push_back(work[i]);
        break;
    }
    ++i;
  }

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

std::string ConvertMarkdown(const std::string& md,
                            const std::wstring& base_dir) {
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
      body << "<h" << heading << ">"
           << InlineFormat(line.substr(heading + 1), base_dir) << "</h"
           << heading << ">\n";
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
      body << "<li>" << InlineFormat(line.substr(2), base_dir) << "</li>\n";
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
      body << "<li>" << InlineFormat(line.substr(3), base_dir) << "</li>\n";
      continue;
    }
    close_lists();
    body << "<p>" << InlineFormat(line, base_dir) << "</p>\n";
  }
  close_lists();
  if (in_code) {
    body << "</code></pre>\n";
  }
  return body.str();
}

}  // namespace

std::wstring MarkdownToHtmlDocument(const std::wstring& md,
                                    const std::wstring& title,
                                    const std::wstring& base_dir) {
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
       << "img{max-width:100%;height:auto;}"
       << "</style></head><body>\n"
       << ConvertMarkdown(NarrowUtf8(md), base_dir) << "</body></html>";
  return WidenUtf8(html.str());
}

bool PreviewMarkdownText(HWND owner, const std::wstring& md,
                         const std::wstring& title,
                         const std::wstring& base_dir) {
  const std::wstring html = MarkdownToHtmlDocument(md, title, base_dir);
  const std::wstring path = JoinPath(NativeDataDir(), L"preview_note.html");
  {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      MistAlert(owner, L"预览", L"无法写入预览文件。", MistMsgTone::Warn);
      return false;
    }
    out << NarrowUtf8(html);
  }
  const HINSTANCE r =
      ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(r) <= 32) {
    MistAlert(owner, L"预览", L"无法打开浏览器预览。", MistMsgTone::Warn);
    return false;
  }
  return true;
}

bool PreviewMarkdownFile(HWND owner, const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    MistAlert(owner, L"预览", L"无法读取文件。", MistMsgTone::Warn);
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string raw = ss.str();
  // Strip RTF roughly — if looks like RTF, preview plain GetWindowText instead.
  if (raw.rfind("{\\rtf", 0) == 0) {
    MistAlert(owner, L"预览",
              L"当前是 RTF。请另存为 .md/.txt 后再预览，或直接预览编辑区文本。",
              MistMsgTone::Info);
    return false;
  }
  std::wstring base;
  const size_t slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    base = path.substr(0, slash);
  }
  return PreviewMarkdownText(owner, WidenUtf8(raw), path, base);
}

namespace {

std::wstring DecodeEntities(const std::wstring& s) {
  std::wstring o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != L'&') {
      o.push_back(s[i]);
      continue;
    }
    size_t semi = s.find(L';', i);
    if (semi == std::wstring::npos || semi - i > 8) {
      o.push_back(s[i]);
      continue;
    }
    const std::wstring ent = s.substr(i + 1, semi - i - 1);
    if (ent == L"amp") {
      o.push_back(L'&');
    } else if (ent == L"lt") {
      o.push_back(L'<');
    } else if (ent == L"gt") {
      o.push_back(L'>');
    } else if (ent == L"quot") {
      o.push_back(L'"');
    } else if (ent == L"apos" || ent == L"#39") {
      o.push_back(L'\'');
    } else if (ent == L"nbsp") {
      o.push_back(L' ');
    } else if (ent == L"#160") {
      o.push_back(L' ');
    } else if (!ent.empty() && ent[0] == L'#') {
      const int cp = _wtoi(ent.c_str() + 1);
      if (cp > 0 && cp <= 0x10FFFF) {
        if (cp <= 0xFFFF) {
          o.push_back(static_cast<wchar_t>(cp));
        } else {
          const int v = cp - 0x10000;
          o.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
          o.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
        }
      } else {
        o.push_back(L'&');
        o.append(ent);
        o.push_back(L';');
      }
    } else {
      o.push_back(L'&');
      o.append(ent);
      o.push_back(L';');
    }
    i = semi;
  }
  return o;
}

std::wstring LowerTag(const std::wstring& s) {
  std::wstring r = s;
  for (auto& c : r) {
    if (c >= L'A' && c <= L'Z') {
      c += (L'a' - L'A');
    }
  }
  return r;
}

std::wstring AttrValue(const std::wstring& tag, const std::wstring& name) {
  const std::wstring needle = name + L"=";
  const std::wstring low = LowerTag(tag);
  size_t p = low.find(needle);
  if (p == std::wstring::npos) {
    return {};
  }
  p += needle.size();
  if (p >= tag.size()) {
    return {};
  }
  const wchar_t q = tag[p];
  if (q == L'"' || q == L'\'') {
    const size_t end = tag.find(q, p + 1);
    if (end == std::wstring::npos) {
      return tag.substr(p + 1);
    }
    return tag.substr(p + 1, end - p - 1);
  }
  size_t end = p;
  while (end < tag.size() && tag[end] != L' ' && tag[end] != L'\t' &&
         tag[end] != L'>') {
    ++end;
  }
  return tag.substr(p, end - p);
}

std::wstring InlineToMd(const std::wstring& text) {
  // Converts inline tags (<strong>/<b>, <em>/<i>, <code>, <a>, <img>) to MD.
  std::wstring out;
  out.reserve(text.size());
  std::wstring link_text, link_href;
  std::wstring tag;
  bool in_a = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (c == L'<') {
      size_t j = text.find(L'>', i);
      if (j == std::wstring::npos) {
        out.push_back(c);
        continue;
      }
      tag = text.substr(i + 1, j - i - 1);
      i = j;
      const std::wstring low = LowerTag(tag);
      const bool close = !tag.empty() && tag[0] == L'/';
      const std::wstring name = close ? LowerTag(tag.substr(1)) : low;
      // strip attributes from name for matching
      size_t sp = name.find_first_of(L" \t");
      const std::wstring base = name.substr(0, sp);
      if (base == L"strong" || base == L"b") {
        out += L"**";
      } else if (base == L"em" || base == L"i") {
        out += L"*";
      } else if (base == L"code") {
        out += L"`";
      } else if (base == L"a") {
        if (!close) {
          in_a = true;
          link_href = DecodeEntities(AttrValue(tag, L"href"));
        } else {
          if (!link_text.empty() && !link_href.empty()) {
            out += L"[" + link_text + L"](" + link_href + L")";
          } else {
            out += link_text;
          }
          link_text.clear();
          link_href.clear();
          in_a = false;
        }
      } else if (base == L"img") {
        const std::wstring src = DecodeEntities(AttrValue(tag, L"src"));
        out += L"![](" + src + L")";
      } else if (base == L"br") {
        out += L"\n";
      }
      continue;
    }
    if (in_a) {
      link_text.push_back(c);
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::wstring TrimLine(const std::wstring& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' ||
                   s[a] == L'\n')) {
    ++a;
  }
  while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' ||
                   s[b - 1] == L'\n')) {
    --b;
  }
  return s.substr(a, b - a);
}

std::wstring PlainOf(const std::wstring& s) {
  // Strip all tags, decode entities, collapse whitespace runs.
  std::wstring o;
  bool in_tag = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == L'<') {
      in_tag = true;
      continue;
    }
    if (in_tag) {
      if (s[i] == L'>') {
        in_tag = false;
      }
      continue;
    }
    o.push_back(s[i]);
  }
  return DecodeEntities(o);
}

}  // namespace

std::wstring HtmlToMarkdown(const std::wstring& html) {
  std::wstring md;
  std::wstring text = html;

  // Extract clipboard fragment if present.
  const size_t sf = text.find(L"<!--StartFragment-->");
  const size_t ef = text.find(L"<!--EndFragment-->");
  if (sf != std::wstring::npos && ef != std::wstring::npos && ef > sf) {
    text = text.substr(sf + 20, ef - (sf + 20));
  }

  std::wstring buf;
  std::vector<std::wstring> table_rows;
  bool in_table = false;
  std::wstring cur_row;
  std::vector<wchar_t> list_stack;   // 'u'/'o' per open list
  std::vector<int> list_counter;     // ordered-list counters
  bool quote_open = false;

  auto flush_text = [&]() {
    std::wstring t = TrimLine(buf);
    if (t.empty()) {
      buf.clear();
      return;
    }
    const std::wstring line = InlineToMd(t);
    if (list_stack.empty()) {
      if (!md.empty() && md.back() != L'\n') {
        md += L'\n';
      }
      md += (quote_open ? L"> " : L"") + line + L"\n\n";
    }
    buf.clear();
  };

  for (size_t i = 0; i < text.size();) {
    const wchar_t c = text[i];
    if (c == L'<') {
      size_t j = text.find(L'>', i);
      if (j == std::wstring::npos) {
        buf.push_back(c);
        ++i;
        continue;
      }
      const std::wstring tag = text.substr(i + 1, j - i - 1);
      i = j + 1;
      const bool close = !tag.empty() && tag[0] == L'/';
      std::wstring name = close ? tag.substr(1) : tag;
      size_t sp = name.find_first_of(L" \t");
      name = LowerTag(name.substr(0, sp));
      if (name == L"br") {
        buf += L"\n";
      } else if (name == L"p") {
        flush_text();
      } else if (name == L"h1" || name == L"h2" || name == L"h3" ||
                 name == L"h4" || name == L"h5" || name == L"h6") {
        if (!close) {
          flush_text();
        } else {
          const int level = name[1] - L'0';
          const std::wstring t = TrimLine(buf);
          buf.clear();
          if (!t.empty()) {
            md += std::wstring(static_cast<size_t>(level), L'#') + L" " +
                  InlineToMd(t) + L"\n\n";
          }
        }
      } else if (name == L"strong" || name == L"b" || name == L"em" ||
                 name == L"i" || name == L"code" || name == L"a" ||
                 name == L"img" || name == L"u" || name == L"s" ||
                 name == L"del" || name == L"mark") {
        if (name == L"a" || name == L"img") {
          buf += L"<" + tag + L">";
        } else if (name == L"u" || name == L"s" || name == L"del" ||
                   name == L"mark") {
          // No direct MD equivalent; keep text, drop tag.
        } else {
          buf += close ? L"</" + name + L">" : L"<" + name + L">";
        }
      } else if (name == L"ul" || name == L"ol") {
        flush_text();
        if (!close) {
          list_stack.push_back(name == L"ol" ? L'o' : L'u');
          list_counter.push_back(0);
        } else if (!list_stack.empty()) {
          list_stack.pop_back();
          list_counter.pop_back();
          if (list_stack.empty()) {
            md += L"\n";
          }
        }
      } else if (name == L"li") {
        if (close) {
          const std::wstring t = TrimLine(buf);
          buf.clear();
          if (!t.empty()) {
            const size_t depth = list_stack.empty() ? 0 : list_stack.size() - 1;
            std::wstring indent(depth * 2, L' ');
            std::wstring marker = L"- ";
            if (!list_stack.empty() && list_stack.back() == L'o') {
              list_counter.back() += 1;
              marker = std::to_wstring(list_counter.back()) + L". ";
            }
            md += indent + marker + InlineToMd(t) + L"\n";
          }
        }
      } else if (name == L"table") {
        if (!close) {
          flush_text();
          in_table = true;
          table_rows.clear();
          cur_row.clear();
        } else {
          in_table = false;
          // Emit GFM table.
          std::vector<std::vector<std::wstring>> rows;
          for (const auto& r : table_rows) {
            std::vector<std::wstring> cells;
            std::wstring cell;
            for (size_t k = 0; k < r.size(); ++k) {
              if (r[k] == L'\t') {
                cells.push_back(TrimLine(PlainOf(cell)));
                cell.clear();
              } else {
                cell.push_back(r[k]);
              }
            }
            cells.push_back(TrimLine(PlainOf(cell)));
            rows.push_back(std::move(cells));
          }
          size_t cols = 0;
          for (const auto& r : rows) {
            cols = (std::max)(cols, r.size());
          }
          if (!rows.empty() && cols > 0) {
            for (size_t r = 0; r < rows.size(); ++r) {
              md += L"| ";
              for (size_t k = 0; k < cols; ++k) {
                if (k < rows[r].size()) {
                  md += rows[r][k];
                }
                md += L" | ";
              }
              md += L"\n";
              if (r == 0) {
                md += L"| ";
                for (size_t k = 0; k < cols; ++k) {
                  md += L"--- | ";
                }
                md += L"\n";
              }
            }
            md += L"\n";
          }
          table_rows.clear();
        }
      } else if (name == L"tr") {
        if (close) {
          table_rows.push_back(cur_row);
          cur_row.clear();
        }
      } else if (name == L"th" || name == L"td") {
        if (in_table && close) {
          cur_row += TrimLine(buf);
          cur_row += L'\t';
          buf.clear();
        }
      } else if (name == L"pre") {
        flush_text();
        if (close) {
          const std::wstring t = TrimLine(buf);
          buf.clear();
          if (!t.empty()) {
            md += L"```\n" + PlainOf(t) + L"\n```\n\n";
          }
        }
      } else if (name == L"blockquote") {
        flush_text();
        quote_open = !close;
      } else if (name == L"div" || name == L"span" || name == L"section" ||
                 name == L"article" || name == L"header" || name == L"footer" ||
                 name == L"nav" || name == L"tbody" || name == L"thead" ||
                 name == L"html" || name == L"body") {
        // pass-through container
      } else if (name == L"head" || name == L"style" || name == L"script" ||
                 name == L"title" || name == L"meta" || name == L"link") {
        // skip entirely (rare in fragments)
      } else {
        // Unknown tag: drop it (plain-text fallback keeps its text content).
      }
      continue;
    }
    if (c == L'&') {
      const size_t semi = text.find(L';', i);
      if (semi != std::wstring::npos && semi - i <= 8) {
        buf += DecodeEntities(text.substr(i, semi - i + 1));
        i = semi + 1;
        continue;
      }
    }
    buf.push_back(c);
    ++i;
  }
  flush_text();
  // Final newline normalization.
  while (!md.empty() && (md.back() == L'\n' || md.back() == L'\r')) {
    md.pop_back();
  }
  return md;
}
