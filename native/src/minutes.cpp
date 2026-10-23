#include "minutes.h"

#include "i18n.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "tool_paths.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring CfgPath() { return JoinPath(NativeDataDir(), L"minutes.json"); }

std::wstring ReadFolderCfg() {
  std::ifstream in(CfgPath());
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  const size_t p = json.find("\"folder\"");
  if (p == std::string::npos) {
    return {};
  }
  size_t q = json.find('"', json.find(':', p) + 1);
  if (q == std::string::npos) {
    return {};
  }
  ++q;
  std::string val;
  while (q < json.size() && json[q] != '"') {
    if (json[q] == '\\' && q + 1 < json.size()) {
      ++q;
    }
    val.push_back(json[q++]);
  }
  return WidenUtf8(val);
}

void WriteFolderCfg(const std::wstring& folder) {
  std::ofstream out(CfgPath(), std::ios::binary);
  if (!out) {
    return;
  }
  std::string s = NarrowUtf8(folder);
  std::string esc;
  for (char c : s) {
    if (c == '\\' || c == '"') {
      esc.push_back('\\');
    }
    esc.push_back(c);
  }
  out << "{\n  \"folder\":\"" << esc << "\"\n}\n";
}

int NextSerial(const std::wstring& folder, int year, int month, int day) {
  wchar_t prefix[32];
  swprintf_s(prefix, L"%04d%02d%02d_", year, month, day);
  const size_t plen = wcslen(prefix);
  int max_n = 0;
  const wchar_t* pats[] = {L"*.md", L"*.rtf", L"*.docx"};
  for (const wchar_t* pat : pats) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(folder, pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (_wcsnicmp(fd.cFileName, prefix, plen) != 0) {
        continue;
      }
      const wchar_t* p = fd.cFileName + plen;
      int n = _wtoi(p);
      if (n > max_n) {
        max_n = n;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return max_n + 1;
}

uint32_t Crc32(const std::string& data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char c : data) {
    crc ^= c;
    for (int i = 0; i < 8; ++i) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

void Put16(std::string& out, uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void Put32(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// Minimal stored (uncompressed) ZIP writer — enough for a valid .docx package.
bool WriteStoredZip(
    const std::wstring& path,
    const std::vector<std::pair<std::string, std::string>>& entries) {
  std::string out;
  struct CD {
    uint32_t crc;
    uint32_t offset;
    uint32_t size;
    const std::string* name;
  };
  std::vector<CD> cds;
  for (const auto& e : entries) {
    const uint32_t crc = Crc32(e.second);
    const uint32_t off = static_cast<uint32_t>(out.size());
    Put32(out, 0x04034b50u);            // local file header
    Put16(out, 20);                      // version needed
    Put16(out, 0);                       // flags
    Put16(out, 0);                       // method = stored
    Put16(out, 0);                       // mod time
    Put16(out, 0x21);                    // mod date (1980-01-01 style)
    Put32(out, crc);
    Put32(out, static_cast<uint32_t>(e.second.size()));
    Put32(out, static_cast<uint32_t>(e.second.size()));
    Put16(out, static_cast<uint16_t>(e.first.size()));
    Put16(out, 0);                       // extra len
    out += e.first;
    out += e.second;
    cds.push_back({crc, off, static_cast<uint32_t>(e.second.size()), &e.first});
  }
  const uint32_t cd_start = static_cast<uint32_t>(out.size());
  for (const auto& cd : cds) {
    Put32(out, 0x02014b50u);            // central directory header
    Put16(out, 20);                      // version made by
    Put16(out, 20);                      // version needed
    Put16(out, 0);                       // flags
    Put16(out, 0);                       // method
    Put16(out, 0);                       // time
    Put16(out, 0x21);                    // date
    Put32(out, cd.crc);
    Put32(out, cd.size);                 // compressed size (stored)
    Put32(out, cd.size);                 // uncompressed size
    Put16(out, static_cast<uint16_t>(cd.name->size()));
    Put16(out, 0);                       // extra
    Put16(out, 0);                       // comment
    Put16(out, 0);                       // disk
    Put16(out, 0);                       // internal attrs
    Put32(out, 0);                       // external attrs
    Put32(out, cd.offset);               // local header offset
    out += *cd.name;
  }
  const uint32_t cd_size = static_cast<uint32_t>(out.size()) - cd_start;
  Put32(out, 0x06054b50u);              // end of central directory
  Put16(out, 0);                         // disk
  Put16(out, 0);                         // cd start disk
  Put16(out, static_cast<uint16_t>(cds.size()));
  Put16(out, static_cast<uint16_t>(cds.size()));
  Put32(out, cd_size);
  Put32(out, cd_start);
  Put16(out, 0);                         // comment len

  std::ofstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  f.write(out.data(), static_cast<std::streamsize>(out.size()));
  return static_cast<bool>(f);
}

std::string XmlEscape(const std::string& s) {
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
    }
  }
  return o;
}

const char* kFonts =
    "<w:rFonts w:ascii=\"微软雅黑\" w:hAnsi=\"微软雅黑\" w:eastAsia=\"微软雅黑\" "
    "w:cs=\"微软雅黑\"/>";

std::string RunP(const std::string& text, int half_pts, bool bold) {
  std::string o = "<w:r><w:rPr>";
  if (bold) {
    o += "<w:b/>";
  }
  o += "<w:sz w:val=\"" + std::to_string(half_pts) + "\"/><w:szCs w:val=\"" +
       std::to_string(half_pts) + "\"/>";
  o += kFonts;
  o += "</w:rPr><w:t xml:space=\"preserve\">" + XmlEscape(text) + "</w:t></w:r>";
  return o;
}

// Build the WordprocessingML body matching Python meeting_minutes.py
// (Heading 1 / Heading 2 paragraph styles, not bold-only Normal runs).
bool WriteMinutesDocx(const std::wstring& path, int year, int month, int day,
                      const std::string& stem) {
  char date[64];
  sprintf_s(date, "%04d年%02d月%02d日", year, month, day);
  std::string body;
  // Title → Heading 1 (centered).
  body += "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/>"
          "<w:jc w:val=\"center\"/></w:pPr>";
  body += RunP("会议纪要", 36, true);
  body += "</w:p>";
  // Meta line: Normal (date bold + serial regular).
  body += "<w:p><w:pPr><w:pStyle w:val=\"Normal\"/></w:pPr>";
  body += RunP(std::string("日期：") + date, 22, true);
  body += RunP(std::string("　　编号：") + stem, 22, false);
  body += "</w:p>";
  static const char* kSections[][2] = {
      {"一、出席人员", "（请填写）"}, {"二、会议议题", "（请填写）"},
      {"三、讨论内容", "（请填写）"}, {"四、决议事项", "（请填写）"},
      {"五、待办事项", "（请填写）"},
  };
  for (const auto& sec : kSections) {
    body += "<w:p><w:pPr><w:pStyle w:val=\"Heading2\"/></w:pPr>";
    body += RunP(sec[0], 28, true);
    body += "</w:p>";
    body += "<w:p><w:pPr><w:pStyle w:val=\"Normal\"/></w:pPr>";
    body += RunP(sec[1], 22, false);
    body += "</w:p>";
  }
  body += "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
          "<w:pgMar w:top=\"1440\" w:right=\"1800\" w:bottom=\"1440\" "
          "w:left=\"1800\" w:header=\"851\" w:footer=\"992\" w:gutter=\"0\"/>"
          "</w:sectPr>";

  const std::string doc =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<w:document "
      "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
      "<w:body>" +
      body + "</w:body></w:document>";

  // P3-39: real Word heading styles (outline levels) so TOC / Navigation work.
  const std::string styles =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<w:styles "
      "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
      "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
      "<w:name w:val=\"Normal\"/><w:qFormat/>"
      "<w:rPr>" +
      std::string(kFonts) +
      "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/>"
      "<w:color w:val=\"000000\"/></w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\">"
      "<w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:next w:val=\"Normal\"/><w:uiPriority w:val=\"9\"/><w:qFormat/>"
      "<w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr>"
      "<w:rPr>" +
      std::string(kFonts) +
      "<w:b/><w:sz w:val=\"36\"/><w:szCs w:val=\"36\"/>"
      "<w:color w:val=\"000000\"/></w:rPr></w:style>"
      "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\">"
      "<w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/>"
      "<w:next w:val=\"Normal\"/><w:uiPriority w:val=\"9\"/><w:qFormat/>"
      "<w:pPr><w:outlineLvl w:val=\"1\"/></w:pPr>"
      "<w:rPr>" +
      std::string(kFonts) +
      "<w:b/><w:sz w:val=\"28\"/><w:szCs w:val=\"28\"/>"
      "<w:color w:val=\"000000\"/></w:rPr></w:style>"
      "</w:styles>";

  const std::string content_types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<Types "
      "xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" "
      "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/word/document.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument."
      "wordprocessingml.document.main+xml\"/>"
      "<Override PartName=\"/word/styles.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument."
      "wordprocessingml.styles+xml\"/>"
      "</Types>";

  const std::string package_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<Relationships "
      "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" "
      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
      "relationships/officeDocument\" Target=\"word/document.xml\"/>"
      "</Relationships>";

  const std::string doc_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<Relationships "
      "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" "
      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
      "relationships/styles\" Target=\"styles.xml\"/>"
      "</Relationships>";

  return WriteStoredZip(path, {{"[Content_Types].xml", content_types},
                               {"_rels/.rels", package_rels},
                               {"word/document.xml", doc},
                               {"word/_rels/document.xml.rels", doc_rels},
                               {"word/styles.xml", styles}});
}

}  // namespace

std::wstring MinutesFolder(bool ensure) {
  std::wstring folder = ReadFolderCfg();
  // Corrupt / empty-quoted values from older builds → treat as unset.
  while (!folder.empty() &&
         (folder.front() == L'"' || folder.front() == L' ' ||
          folder.front() == L'\t')) {
    folder.erase(folder.begin());
  }
  while (!folder.empty() &&
         (folder.back() == L'"' || folder.back() == L' ' ||
          folder.back() == L'\t')) {
    folder.pop_back();
  }
  if (folder.empty() || folder == L"." || folder == L"\\\"\\\"") {
    folder = JoinPath(ExeDir(), L"纪要");
  }
  if (ensure) {
    CreateDirectoryW(folder.c_str(), nullptr);
  }
  return folder;
}

std::wstring MinutesFolderOverride() { return ReadFolderCfg(); }

void ConfigureMinutesFolder(HWND owner) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  const std::wstring browse_title = Tr("minutes_browse");
  bi.lpszTitle = browse_title.c_str();
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (!pidl) {
    return;
  }
  wchar_t path[MAX_PATH] = {};
  SHGetPathFromIDListW(pidl, path);
  CoTaskMemFree(pidl);
  if (path[0]) {
    WriteFolderCfg(path);
    MessageBoxW(owner, path, Tr("minutes_folder_set").c_str(),
                MB_OK | MB_ICONINFORMATION);
  }
}

void ResetMinutesFolder() { WriteFolderCfg(L""); }

void SetMinutesFolder(const std::wstring& folder) { WriteFolderCfg(folder); }

std::wstring CreateAndOpenMinutes(HWND owner) {
  const std::wstring folder = MinutesFolder(true);
  SYSTEMTIME st{};
  GetLocalTime(&st);
  const int serial =
      NextSerial(folder, st.wYear, st.wMonth, st.wDay);
  wchar_t name[64];
  swprintf_s(name, L"%04d%02d%02d_%03d.md", st.wYear, st.wMonth, st.wDay,
             serial);
  const std::wstring path = JoinPath(folder, name);
  {
    std::ofstream out(path, std::ios::binary);
    out << "# 会议纪要\n\n"
        << "- 日期：" << st.wYear << "-" << st.wMonth << "-" << st.wDay << "\n"
        << "- 编号：" << serial << "\n"
        << "- 参与人：\n"
        << "- 议程：\n"
        << "- 决议：\n"
        << "- 待办：\n";
  }
  ShellExecuteW(owner, L"open", L"notepad.exe", path.c_str(), nullptr,
                SW_SHOWNORMAL);
  return path;
}

void OpenMinutesFolder(HWND owner) {
  const std::wstring folder = MinutesFolder(true);
  ShellExecuteW(owner, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring CreateAndOpenMinutesDocx(HWND owner) {
  const std::wstring folder = MinutesFolder(true);
  SYSTEMTIME st{};
  GetLocalTime(&st);
  const int serial = NextSerial(folder, st.wYear, st.wMonth, st.wDay);
  wchar_t name[64];
  swprintf_s(name, L"%04d%02d%02d_%03d.docx", st.wYear, st.wMonth, st.wDay,
             serial);
  const std::wstring path = JoinPath(folder, name);
  wchar_t stem[32];
  swprintf_s(stem, L"%04d%02d%02d_%03d", st.wYear, st.wMonth, st.wDay, serial);
  if (!WriteMinutesDocx(path, st.wYear, st.wMonth, st.wDay, NarrowUtf8(stem))) {
    MessageBoxW(owner, L"无法写入纪要文件。", L"会议纪要", MB_OK | MB_ICONERROR);
    return std::wstring();
  }
  ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return path;
}

std::wstring CreateAndOpenMinutesRtf(HWND owner) {
  // Word-friendly output is now a real .docx (was RTF). Kept the symbol for
  // callers that still reference the "Word" menu action.
  return CreateAndOpenMinutesDocx(owner);
}

bool OpenRecentMinutes(HWND owner) {
  const std::wstring folder = MinutesFolder(true);
  std::vector<std::wstring> files;
  const wchar_t* pats[] = {L"*.md", L"*.rtf", L"*.docx"};
  for (const wchar_t* pat : pats) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(folder, pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        files.push_back(fd.cFileName);
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(files.begin(), files.end());
  std::reverse(files.begin(), files.end());
  if (files.size() > 40) {
    files.resize(40);
  }
  if (files.empty()) {
    MessageBoxW(owner, Tr("minutes_empty").c_str(), Tr("minutes_title").c_str(),
                MB_OK | MB_ICONINFORMATION);
    return false;
  }

  std::wstring picked;
  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user_data_subdir = L"webview2-minutes";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\pick_list.html");
  if (dlg.html_path.empty()) {
    MessageBoxW(owner, L"找不到 pick_list.html", Tr("minutes_title").c_str(),
                MB_OK | MB_ICONWARNING);
    return false;
  }
  dlg.build_state = [&]() {
    std::wstring json = L"{\"type\":\"state\",\"title\":\"";
    json += MistJsonEscape(Tr("minutes_recent"));
    json += L"\",\"hint\":\"";
    json += MistJsonEscape(folder);
    json += L"\",\"open_label\":\"";
    json += MistJsonEscape(Tr("search_open"));
    json += L"\",\"allow_new\":false,\"sel\":0,\"items\":[";
    for (size_t i = 0; i < files.size(); ++i) {
      if (i) {
        json += L',';
      }
      json += L'"';
      json += MistJsonEscape(files[i]);
      json += L'"';
    }
    json += L"]}";
    return json;
  };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"open") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(files.size())) {
        picked = JoinPath(folder, files[static_cast<size_t>(index)]);
        return true;
      }
      return false;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, Tr("minutes_recent").c_str(), 520, 420);
  if (picked.empty()) {
    return false;
  }
  ShellExecuteW(owner, L"open", picked.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return true;
}
