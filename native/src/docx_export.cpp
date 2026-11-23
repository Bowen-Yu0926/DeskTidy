#include "docx_export.h"

#include "log.h"
#include "paths_util.h"

#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ImgItem {
  std::wstring original_src;
  std::wstring disk_path;
  std::wstring media_filename;
  std::string rel_id;
  std::string ext_lower;
  int width_px = 600;
  int height_px = 400;
};

std::string XmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string DecodeHtmlEntities(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    if (in[i] == '&') {
      size_t semi = in.find(';', i + 1);
      if (semi != std::string::npos && semi - i <= 10) {
        std::string ent = in.substr(i, semi - i + 1);
        if (ent == "&#x20;" || ent == "&#32;" || ent == "&nbsp;") {
          out.push_back(' ');
          i = semi + 1;
          continue;
        }
        if (ent == "&lt;") {
          out.push_back('<');
          i = semi + 1;
          continue;
        }
        if (ent == "&gt;") {
          out.push_back('>');
          i = semi + 1;
          continue;
        }
        if (ent == "&quot;") {
          out.push_back('"');
          i = semi + 1;
          continue;
        }
        if (ent == "&apos;") {
          out.push_back('\'');
          i = semi + 1;
          continue;
        }
        if (ent == "&amp;") {
          out.push_back('&');
          i = semi + 1;
          continue;
        }
        if (ent.size() >= 4 && ent[1] == '#') {
          long code = 0;
          if (ent[2] == 'x' || ent[2] == 'X') {
            code = strtol(ent.c_str() + 3, nullptr, 16);
          } else {
            code = strtol(ent.c_str() + 2, nullptr, 10);
          }
          if (code > 0 && code < 128) {
            out.push_back(static_cast<char>(code));
            i = semi + 1;
            continue;
          } else if (code >= 128 && code <= 0x10FFFF) {
            wchar_t wc = static_cast<wchar_t>(code);
            std::wstring ws(1, wc);
            out += NarrowUtf8(ws);
            i = semi + 1;
            continue;
          }
        }
      }
    }
    out.push_back(in[i++]);
  }
  return out;
}

std::wstring ProbeImagePath(const std::wstring& src, const std::wstring& base_dir) {
  if (src.empty() || src.rfind(L"http://", 0) == 0 || src.rfind(L"https://", 0) == 0 ||
      src.rfind(L"data:", 0) == 0 || src[0] == L'#') {
    return L"";
  }
  std::wstring s = src;
  if (s.rfind(L"file:///", 0) == 0) {
    s = s.substr(8);
    for (auto& c : s) if (c == L'/') c = L'\\';
  }
  std::wstring norm = s;
  for (auto& c : norm) if (c == L'/') c = L'\\';

  if (norm.size() >= 2 && norm[1] == L':') {
    if (GetFileAttributesW(norm.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return norm;
    }
  }
  if (!base_dir.empty()) {
    std::wstring p1 = JoinPath(base_dir, norm);
    if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return p1;
    }
    size_t sl = norm.find_last_of(L"\\/");
    std::wstring leaf = (sl == std::wstring::npos) ? norm : norm.substr(sl + 1);
    std::wstring p2 = JoinPath(JoinPath(base_dir, L"assets"), leaf);
    if (GetFileAttributesW(p2.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return p2;
    }
  }

  return L"";
}

bool ReadImageSizeGdiplus(const std::wstring& disk_path, int* w, int* h) {
  if (!w || !h) return false;
  *w = 600;
  *h = 400;
  Gdiplus::Bitmap bmp(disk_path.c_str());
  if (bmp.GetLastStatus() == Gdiplus::Ok) {
    int bw = static_cast<int>(bmp.GetWidth());
    int bh = static_cast<int>(bmp.GetHeight());
    if (bw > 0 && bh > 0) {
      *w = bw;
      *h = bh;
      return true;
    }
  }
  return false;
}

std::string ExtOfPath(const std::wstring& p) {
  size_t dot = p.find_last_of(L'.');
  if (dot == std::wstring::npos) return "png";
  std::string s = NarrowUtf8(p.substr(dot + 1));
  for (auto& c : s) c = static_cast<char>(tolower(c));
  if (s == "jpg") s = "jpeg";
  return s;
}

void RemoveDirectoryRecursive(const std::wstring& dir) {
  std::wstring pattern = dir + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
        continue;
      }
      std::wstring sub = dir + L"\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        RemoveDirectoryRecursive(sub);
      } else {
        DeleteFileW(sub.c_str());
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(dir.c_str());
}

struct RunToken {
  std::string text;
  bool bold = false;
  bool italic = false;
  bool code = false;
  bool strike = false;
};

std::vector<RunToken> ParseInlineMarkdown(const std::string& line) {
  std::vector<RunToken> runs;
  if (line.empty()) {
    return runs;
  }
  size_t i = 0;
  std::string cur;
  bool bold = false;
  bool italic = false;
  bool code = false;
  bool strike = false;

  auto flush = [&]() {
    if (!cur.empty()) {
      RunToken r;
      r.text = cur;
      r.bold = bold;
      r.italic = italic;
      r.code = code;
      r.strike = strike;
      runs.push_back(r);
      cur.clear();
    }
  };

  while (i < line.size()) {
    if (!code && line[i] == '`') {
      flush();
      code = true;
      ++i;
      while (i < line.size() && line[i] != '`') {
        cur.push_back(line[i++]);
      }
      flush();
      code = false;
      if (i < line.size() && line[i] == '`') {
        ++i;
      }
      continue;
    }
    if (!code && i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
      flush();
      bold = !bold;
      i += 2;
      continue;
    }
    if (!code && i + 1 < line.size() && line[i] == '~' && line[i + 1] == '~') {
      flush();
      strike = !strike;
      i += 2;
      continue;
    }
    if (!code && line[i] == '*') {
      flush();
      italic = !italic;
      ++i;
      continue;
    }
    cur.push_back(line[i++]);
  }
  flush();
  return runs;
}

std::string RenderRunsToXml(const std::vector<RunToken>& runs) {
  std::string xml;
  for (const auto& r : runs) {
    if (r.text.empty()) continue;
    xml += "<w:r><w:rPr>";
    if (r.code) {
      xml += "<w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:eastAsia=\"Microsoft YaHei\"/>";
      xml += "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F0F2F5\"/>";
      xml += "<w:color w:val=\"C7254E\"/>";
      xml += "<w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/>";
    } else {
      xml += "<w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"Microsoft YaHei\"/>";
    }
    if (r.bold) xml += "<w:b/><w:bCs/>";
    if (r.italic) xml += "<w:i/><w:iCs/>";
    if (r.strike) xml += "<w:strike/>";
    xml += "</w:rPr>";
    xml += "<w:t xml:space=\"preserve\">" + XmlEscape(r.text) + "</w:t></w:r>";
  }
  return xml;
}

}  // namespace

bool ExportMarkdownToDocx(const std::wstring& md_text,
                          const std::wstring& title,
                          const std::wstring& base_dir,
                          const std::wstring& dst_docx_path,
                          std::wstring* err) {
  if (dst_docx_path.empty()) {
    if (err) *err = L"目标保存路径为空。";
    return false;
  }

  // Clean title extensions (.md, .markdown, .txt)
  std::wstring clean_title = title;
  for (auto& ch : clean_title) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      ch = L'_';
    }
  }
  if (clean_title.size() >= 3 && _wcsicmp(clean_title.c_str() + clean_title.size() - 3, L".md") == 0) {
    clean_title.resize(clean_title.size() - 3);
  } else if (clean_title.size() >= 9 && _wcsicmp(clean_title.c_str() + clean_title.size() - 9, L".markdown") == 0) {
    clean_title.resize(clean_title.size() - 9);
  } else if (clean_title.size() >= 4 && _wcsicmp(clean_title.c_str() + clean_title.size() - 4, L".txt") == 0) {
    clean_title.resize(clean_title.size() - 4);
  }
  while (!clean_title.empty() && (clean_title.front() == L' ' || clean_title.front() == L'\t')) clean_title.erase(clean_title.begin());
  while (!clean_title.empty() && (clean_title.back() == L' ' || clean_title.back() == L'\t')) clean_title.pop_back();

  // 1. Prepare temp directory
  wchar_t sys_tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, sys_tmp);
  const std::wstring tmp_root = std::wstring(sys_tmp) + L"desktidy_docx_" +
                                std::to_wstring(GetCurrentProcessId()) + L"_" +
                                std::to_wstring(GetTickCount64());
  RemoveDirectoryRecursive(tmp_root);
  CreateDirectoryW(tmp_root.c_str(), nullptr);

  const std::wstring rels_dir = tmp_root + L"\\_rels";
  const std::wstring word_dir = tmp_root + L"\\word";
  const std::wstring word_rels_dir = word_dir + L"\\_rels";
  const std::wstring word_media_dir = word_dir + L"\\media";
  CreateDirectoryW(rels_dir.c_str(), nullptr);
  CreateDirectoryW(word_dir.c_str(), nullptr);
  CreateDirectoryW(word_rels_dir.c_str(), nullptr);
  CreateDirectoryW(word_media_dir.c_str(), nullptr);

  // 2. Discover and copy images
  std::vector<ImgItem> images;
  auto find_image = [&](const std::wstring& raw_src) -> size_t {
    for (size_t k = 0; k < images.size(); ++k) {
      if (images[k].original_src == raw_src) return k;
    }
    std::wstring disk_p = ProbeImagePath(raw_src, base_dir);
    if (disk_p.empty() || GetFileAttributesW(disk_p.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return static_cast<size_t>(-1);
    }
    ImgItem item;
    item.original_src = raw_src;
    item.disk_path = disk_p;
    item.ext_lower = ExtOfPath(disk_p);
    const size_t idx = images.size() + 1;
    item.rel_id = "rIdImg" + std::to_string(idx);
    item.media_filename = L"image" + std::to_wstring(idx) + L"." + WidenUtf8(item.ext_lower);
    ReadImageSizeGdiplus(disk_p, &item.width_px, &item.height_px);

    std::wstring target_media = word_media_dir + L"\\" + item.media_filename;
    CopyFileW(disk_p.c_str(), target_media.c_str(), FALSE);

    images.push_back(item);
    return images.size() - 1;
  };

  // Pre-scan text for images
  {
    size_t pos = 0;
    while ((pos = md_text.find(L"![", pos)) != std::wstring::npos) {
      size_t alt_end = md_text.find(L"]", pos + 2);
      if (alt_end != std::wstring::npos && alt_end + 1 < md_text.size() && md_text[alt_end + 1] == L'(') {
        size_t src_end = md_text.find(L")", alt_end + 2);
        if (src_end != std::wstring::npos) {
          std::wstring raw_src = md_text.substr(alt_end + 2, src_end - (alt_end + 2));
          size_t sp = raw_src.find(L' ');
          if (sp != std::wstring::npos) raw_src = raw_src.substr(0, sp);
          while (!raw_src.empty() && (raw_src.front() == L' ' || raw_src.front() == L'<' || raw_src.front() == L'"')) raw_src.erase(raw_src.begin());
          while (!raw_src.empty() && (raw_src.back() == L' ' || raw_src.back() == L'>' || raw_src.back() == L'"')) raw_src.pop_back();
          find_image(raw_src);
          pos = src_end + 1;
          continue;
        }
      }
      pos += 2;
    }
  }

  // 3. Write [Content_Types].xml
  {
    std::ofstream ct(tmp_root + L"\\[Content_Types].xml", std::ios::binary);
    ct << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    ct << "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n";
    ct << "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n";
    ct << "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n";
    ct << "  <Default Extension=\"png\" ContentType=\"image/png\"/>\n";
    ct << "  <Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>\n";
    ct << "  <Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>\n";
    ct << "  <Default Extension=\"gif\" ContentType=\"image/gif\"/>\n";
    ct << "  <Default Extension=\"bmp\" ContentType=\"image/bmp\"/>\n";
    ct << "  <Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>\n";
    ct << "</Types>\n";
  }

  // 4. Write _rels/.rels
  {
    std::ofstream rels(rels_dir + L"\\.rels", std::ios::binary);
    rels << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    rels << "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
    rels << "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>\n";
    rels << "</Relationships>\n";
  }

  // 5. Write word/_rels/document.xml.rels
  {
    std::ofstream wrels(word_rels_dir + L"\\document.xml.rels", std::ios::binary);
    wrels << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    wrels << "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
    for (const auto& im : images) {
      wrels << "  <Relationship Id=\"" << im.rel_id
            << "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/"
            << NarrowUtf8(im.media_filename) << "\"/>\n";
    }
    wrels << "</Relationships>\n";
  }

  // 6. Write word/document.xml
  {
    std::ofstream doc(word_dir + L"\\document.xml", std::ios::binary);
    doc << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    doc << "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
           "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
           "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
           "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
           "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">\n";
    doc << "  <w:body>\n";

    // Document Title (Heading 1) if provided
    // Check if the first non-empty markdown line is already an H1 identical to clean_title.
    // If so, do not duplicate the top title.
    bool skip_top_title = clean_title.empty();
    if (!skip_top_title) {
      std::istringstream peek_stream(NarrowUtf8(md_text));
      std::string peek_line;
      while (std::getline(peek_stream, peek_line)) {
        while (!peek_line.empty() && (peek_line.back() == '\r' || peek_line.back() == '\n')) {
          peek_line.pop_back();
        }
        peek_line = DecodeHtmlEntities(peek_line);
        while (!peek_line.empty() && (peek_line.front() == ' ' || peek_line.front() == '\t')) peek_line.erase(peek_line.begin());
        while (!peek_line.empty() && (peek_line.back() == ' ' || peek_line.back() == '\t')) peek_line.pop_back();
        if (peek_line.empty()) continue;

        if (peek_line.rfind("# ", 0) == 0) {
          std::string h1 = peek_line.substr(2);
          while (!h1.empty() && (h1.front() == ' ' || h1.front() == '\t')) h1.erase(h1.begin());
          while (!h1.empty() && (h1.back() == ' ' || h1.back() == '\t')) h1.pop_back();
          std::wstring w_h1 = WidenUtf8(h1);
          if (_wcsicmp(w_h1.c_str(), clean_title.c_str()) == 0) {
            skip_top_title = true;
          }
        }
        break;
      }
    }

    if (!skip_top_title) {
      doc << "    <w:p><w:pPr><w:spacing w:before=\"240\" w:after=\"200\"/><w:jc w:val=\"left\"/></w:pPr>";
      doc << "<w:r><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"Microsoft YaHei\"/><w:b/><w:bCs/><w:sz w:val=\"44\"/><w:szCs w:val=\"44\"/><w:color w:val=\"20242A\"/></w:rPr>";
      doc << "<w:t>" << XmlEscape(NarrowUtf8(clean_title)) << "</w:t></w:r></w:p>\n";
    }

    // Split markdown into lines
    std::istringstream stream(NarrowUtf8(md_text));
    std::string line;
    bool in_code_block = false;
    int doc_pr_id = 1;

    while (std::getline(stream, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
      }

      // Check code fence
      if (line.rfind("```", 0) == 0) {
        in_code_block = !in_code_block;
        continue;
      }

      if (in_code_block) {
        doc << "    <w:p><w:pPr><w:spacing w:before=\"0\" w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>";
        doc << "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"F6F8FA\"/>";
        doc << "<w:ind w:left=\"240\" w:right=\"240\"/></w:pPr>";
        doc << "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\" w:hAnsi=\"Consolas\" w:eastAsia=\"Microsoft YaHei\"/><w:sz w:val=\"19\"/><w:szCs w:val=\"19\"/><w:color w:val=\"24292E\"/></w:rPr>";
        doc << "<w:t xml:space=\"preserve\">" << XmlEscape(line) << "</w:t></w:r></w:p>\n";
        continue;
      }

      // Horizontal rule
      if (line == "---" || line == "***" || line == "___") {
        doc << "    <w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"8\" w:space=\"1\" w:color=\"E1E4E8\"/></w:pBdr><w:spacing w:before=\"120\" w:after=\"120\"/></w:pPr></w:p>\n";
        continue;
      }

      // Empty line -> empty paragraph spacer
      if (line.empty()) {
        doc << "    <w:p><w:pPr><w:spacing w:after=\"120\"/></w:pPr></w:p>\n";
        continue;
      }

      // Decode HTML entities (e.g. &#x20;, &#32;, &nbsp;)
      line = DecodeHtmlEntities(line);

      // Headings
      if (line[0] == '#') {
        int lvl = 0;
        while (lvl < static_cast<int>(line.size()) && line[lvl] == '#') ++lvl;
        if (lvl < static_cast<int>(line.size()) && line[lvl] == ' ') {
          std::string htext = line.substr(lvl + 1);
          int sz = (lvl == 1) ? 36 : (lvl == 2) ? 30 : (lvl == 3) ? 26 : 22;
          int space_before = (lvl == 1) ? 240 : 180;
          int space_after = 120;
          doc << "    <w:p><w:pPr><w:spacing w:before=\"" << space_before << "\" w:after=\"" << space_after << "\"/>";
          if (lvl <= 2) {
            doc << "<w:pBdr><w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"4\" w:color=\"EAECEF\"/></w:pBdr>";
          }
          doc << "</w:pPr>";
          doc << "<w:r><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"Microsoft YaHei\"/><w:b/><w:bCs/><w:sz w:val=\"" << sz << "\"/><w:szCs w:val=\"" << sz << "\"/><w:color w:val=\"1A1F2C\"/></w:rPr>";
          doc << "<w:t>" << XmlEscape(htext) << "</w:t></w:r></w:p>\n";
          continue;
        }
      }

      // Blockquotes
      if (line[0] == '>') {
        std::string qtext = (line.size() > 1 && line[1] == ' ') ? line.substr(2) : line.substr(1);
        doc << "    <w:p><w:pPr><w:pBdr><w:left w:val=\"single\" w:sz=\"18\" w:space=\"8\" w:color=\"2F6FED\"/></w:pBdr><w:ind w:left=\"360\"/><w:spacing w:before=\"60\" w:after=\"60\"/></w:pPr>";
        auto runs = ParseInlineMarkdown(qtext);
        doc << RenderRunsToXml(runs);
        doc << "</w:p>\n";
        continue;
      }

      // Check single image on this line: ![alt](url)
      bool handled_img = false;
      if (line.rfind("![", 0) == 0) {
        size_t alt_end = line.find("]");
        if (alt_end != std::string::npos && alt_end + 1 < line.size() && line[alt_end + 1] == '(') {
          size_t src_end = line.find(")", alt_end + 2);
          if (src_end != std::string::npos) {
            std::string raw_u8 = line.substr(alt_end + 2, src_end - (alt_end + 2));
            size_t sp = raw_u8.find(' ');
            if (sp != std::string::npos) raw_u8 = raw_u8.substr(0, sp);
            while (!raw_u8.empty() && (raw_u8.front() == ' ' || raw_u8.front() == '<' || raw_u8.front() == '"')) raw_u8.erase(raw_u8.begin());
            while (!raw_u8.empty() && (raw_u8.back() == ' ' || raw_u8.back() == '>' || raw_u8.back() == '"')) raw_u8.pop_back();

            const std::wstring raw_w = WidenUtf8(raw_u8);
            for (const auto& im : images) {
              if (im.original_src == raw_w) {
                // Calculate EMU size (1 px = 9525 EMU; max width A4 printable = 5486400)
                long long emu_w = static_cast<long long>(im.width_px) * 9525LL;
                long long emu_h = static_cast<long long>(im.height_px) * 9525LL;
                constexpr long long kMaxEmuWidth = 5486400LL;
                if (emu_w > kMaxEmuWidth && emu_w > 0) {
                  emu_h = emu_h * kMaxEmuWidth / emu_w;
                  emu_w = kMaxEmuWidth;
                }
                if (emu_w <= 0) emu_w = 4000000;
                if (emu_h <= 0) emu_h = 2500000;

                doc << "    <w:p><w:pPr><w:jc w:val=\"center\"/><w:spacing w:before=\"120\" w:after=\"120\"/></w:pPr><w:r>";
                doc << "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">";
                doc << "<wp:extent cx=\"" << emu_w << "\" cy=\"" << emu_h << "\"/>";
                doc << "<wp:docPr id=\"" << doc_pr_id++ << "\" name=\"Picture " << doc_pr_id << "\"/>";
                doc << "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">";
                doc << "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"0\" name=\"Picture\"/><pic:cNvPicPr/></pic:nvPicPr>";
                doc << "<pic:blipFill><a:blip r:embed=\"" << im.rel_id << "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>";
                doc << "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" << emu_w << "\" cy=\"" << emu_h << "\"/></a:xfrm>";
                doc << "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>";
                doc << "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>\n";
                handled_img = true;
                break;
              }
            }
          }
        }
      }
      if (handled_img) {
        continue;
      }

      // Lists
      bool is_list = false;
      std::string lprefix;
      std::string ltext = line;
      if (line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0 || line.rfind("+ ", 0) == 0) {
        is_list = true;
        lprefix = "• ";
        ltext = line.substr(2);
      } else {
        size_t dot = line.find(". ");
        if (dot != std::string::npos && dot > 0 && dot < 5) {
          bool all_digits = true;
          for (size_t d = 0; d < dot; ++d) {
            if (!isdigit(static_cast<unsigned char>(line[d]))) { all_digits = false; break; }
          }
          if (all_digits) {
            is_list = true;
            lprefix = line.substr(0, dot + 2);
            ltext = line.substr(dot + 2);
          }
        }
      }

      if (is_list) {
        doc << "    <w:p><w:pPr><w:ind w:left=\"480\" w:hanging=\"240\"/><w:spacing w:after=\"80\" w:line=\"280\" w:lineRule=\"auto\"/></w:pPr>";
        doc << "<w:r><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:eastAsia=\"Microsoft YaHei\"/><w:b/></w:rPr><w:t xml:space=\"preserve\">"
            << XmlEscape(lprefix) << "</w:t></w:r>";
        auto runs = ParseInlineMarkdown(ltext);
        doc << RenderRunsToXml(runs);
        doc << "</w:p>\n";
        continue;
      }

      // Regular paragraph
      doc << "    <w:p><w:pPr><w:spacing w:after=\"120\" w:line=\"280\" w:lineRule=\"auto\"/></w:pPr>";
      auto runs = ParseInlineMarkdown(line);
      doc << RenderRunsToXml(runs);
      doc << "</w:p>\n";
    }

    // Page margin and geometry (A4 portrait)
    doc << "    <w:sectPr>\n";
    doc << "      <w:pgSz w:w=\"11906\" w:h=\"16838\"/>\n";
    doc << "      <w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>\n";
    doc << "    </w:sectPr>\n";
    doc << "  </w:body>\n";
    doc << "</w:document>\n";
  }

  // 7. Repack directory into target .docx
  DeleteFileW(dst_docx_path.c_str());

  std::wstring temp_zip = tmp_root + L".zip";
  DeleteFileW(temp_zip.c_str());

  bool packed = false;

  // 7.1 Try Windows native tar.exe (fast, ~30ms, no PowerShell / Defender stalls)
  wchar_t sys_dir[MAX_PATH] = {};
  GetSystemDirectoryW(sys_dir, MAX_PATH);
  std::wstring tar_exe = std::wstring(sys_dir) + L"\\tar.exe";
  if (GetFileAttributesW(tar_exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
    std::wstring tar_cmd = L"\"" + tar_exe + L"\" -a -c -f \"" + temp_zip + L"\" -C \"" + tmp_root + L"\" \"[Content_Types].xml\" \"_rels\" \"word\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, tar_cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      const DWORD wait_res = WaitForSingleObject(pi.hProcess, 10000);
      const bool finished = wait_res == WAIT_OBJECT_0;
      if (wait_res == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);

      // 超时被强杀的 tar 可能留下半个 zip；必须等进程正常退出才接受产物。
      if (finished) {
        // Verify temp_zip exists, size > 0 and has PK\x03\x04 zip header
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(temp_zip.c_str(), GetFileExInfoStandard, &fad)) {
          const ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
          if (sz >= 100) {
            HANDLE hFile = CreateFileW(temp_zip.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
              char magic[4] = {};
              DWORD read_bytes = 0;
              if (ReadFile(hFile, magic, 4, &read_bytes, nullptr) && read_bytes == 4) {
                if (magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04) {
                  packed = true;
                }
              }
              CloseHandle(hFile);
            }
          }
        }
      }
    }
  }

  // 7.2 Fallback to PowerShell if tar.exe is unavailable or failed
  if (!packed) {
    DeleteFileW(temp_zip.c_str());
    std::wstring ps_src = tmp_root;
    std::wstring ps_dst = temp_zip;
    for (auto& c : ps_src) if (c == L'/') c = L'\\';
    for (auto& c : ps_dst) if (c == L'/') c = L'\\';

    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command \""
                       L"[Reflection.Assembly]::LoadWithPartialName('System.IO.Compression.FileSystem') | Out-Null; "
                       L"[IO.Compression.ZipFile]::CreateFromDirectory('" + ps_src + L"', '" + ps_dst + L"', [IO.Compression.CompressionLevel]::Optimal, $false)\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
      const DWORD wait_res = WaitForSingleObject(pi.hProcess, 15000);
      const bool finished = wait_res == WAIT_OBJECT_0;
      if (wait_res == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      // 与 tar 分支一致：被强杀的 PowerShell 可能留下截断的 zip。
      if (finished && GetFileAttributesW(temp_zip.c_str()) != INVALID_FILE_ATTRIBUTES) {
        packed = true;
      }
    }
  }

  RemoveDirectoryRecursive(tmp_root);

  if (!packed || GetFileAttributesW(temp_zip.c_str()) == INVALID_FILE_ATTRIBUTES) {
    DeleteFileW(temp_zip.c_str());
    if (err) *err = L"无法完成 DOCX 压缩打包。";
    return false;
  }

  // Move or copy temp_zip to dst_docx_path
  if (!MoveFileExW(temp_zip.c_str(), dst_docx_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
    if (!CopyFileW(temp_zip.c_str(), dst_docx_path.c_str(), FALSE)) {
      DeleteFileW(temp_zip.c_str());
      if (err) *err = L"无法将打包文件写入目标 DOCX 路径。";
      return false;
    }
    DeleteFileW(temp_zip.c_str());
  }

  // Validate output file
  const DWORD attr = GetFileAttributesW(dst_docx_path.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    if (err) *err = L"DOCX 导出文件生成失败。";
    return false;
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(dst_docx_path.c_str(), GetFileExInfoStandard, &fad)) {
    const ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (sz == 0) {
      if (err) *err = L"导出的 DOCX 文件为空。";
      DeleteFileW(dst_docx_path.c_str());
      return false;
    }
  }

  return true;
}
