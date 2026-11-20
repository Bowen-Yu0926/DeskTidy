#include "docx_export.h"

#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <fstream>
#include <string>

static ULONG_PTR g_gdi_token = 0;

void InitGdiplus() {
  Gdiplus::GdiplusStartupInput input;
  Gdiplus::GdiplusStartup(&g_gdi_token, &input, nullptr);
}

void ShutdownGdiplus() {
  if (g_gdi_token) {
    Gdiplus::GdiplusShutdown(g_gdi_token);
  }
}

int main() {
  printf("Running docx export tests...\n");
  InitGdiplus();

  wchar_t tmp_dir[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp_dir);
  std::wstring work_dir = std::wstring(tmp_dir) + L"test_docx_unit_" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(work_dir.c_str(), nullptr);

  // 1. 生成测试 PNG 图片
  std::wstring img_path = work_dir + L"\\test_sample.png";
  {
    Gdiplus::Bitmap bmp(100, 100, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    Gdiplus::SolidBrush b(Gdiplus::Color(255, 47, 111, 237));
    g.FillRectangle(&b, 0, 0, 100, 100);
    CLSID pngClsid{};
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &pngClsid);
    bmp.Save(img_path.c_str(), &pngClsid, nullptr);
  }

  // 2. 准备 Markdown 测试文本（包含实体字符与标题）
  std::wstring md =
      L"# 测试笔记标题.md\n\n"
      L"## 二级标题：特性展示\n\n"
      L"这是一段普通的正文内容，&#x20;包含 HTML 实体空格&#32;测试&nbsp;以及 **加粗文字**，*斜体文字*，以及 `inline_code()` 和 ~~删除线~~。\n\n"
      L"> 这是引用的段落文字，应当有左侧竖线和缩进格式。\n\n"
      L"- 无序列表项目一\n"
      L"- 无序列表项目二\n\n"
      L"1. 有序列表第一项\n"
      L"2. 有序列表第二项\n\n"
      L"```cpp\n"
      L"int main() {\n"
      L"  printf(\"Hello, DOCX!\\n\");\n"
      L"  return 0;\n"
      L"}\n"
      L"```\n\n"
      L"---\n\n"
      L"下面是一张内嵌图片：\n\n"
      L"![测试图片](test_sample.png)\n";

  std::wstring docx_out = work_dir + L"\\output_note.docx";
  DeleteFileW(docx_out.c_str());

  std::wstring err;
  bool ok = ExportMarkdownToDocx(md, L"测试笔记标题.md", work_dir, docx_out, &err);
  if (!ok) {
    printf("FAIL: ExportMarkdownToDocx failed: %ls\n", err.c_str());
    return 1;
  }

  // 3. 校验导出的 .docx
  DWORD attr = GetFileAttributesW(docx_out.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    printf("FAIL: output file not found\n");
    return 1;
  }

  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW(docx_out.c_str(), GetFileExInfoStandard, &fad)) {
    printf("FAIL: GetFileAttributesExW failed\n");
    return 1;
  }
  const ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
  if (sz <= 500) {
    printf("FAIL: DOCX file size too small: %llu\n", sz);
    return 1;
  }
  printf("OK: ExportMarkdownToDocx generated valid .docx file (size=%llu bytes)\n", sz);

  // 清理临时文件
  DeleteFileW(img_path.c_str());
  DeleteFileW(docx_out.c_str());
  RemoveDirectoryW(work_dir.c_str());

  ShutdownGdiplus();
  printf("all docx export tests passed!\n");
  return 0;
}
