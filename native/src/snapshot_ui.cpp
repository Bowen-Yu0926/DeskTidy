#include "snapshot_ui.h"

#include "prompt_text.h"
#include "snapshot.h"

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::wstring SnapshotPreviewText(const std::wstring& path) {
  AppState st;
  if (!LoadSnapshot(path, &st)) {
    return L"（无法读取预览）";
  }
  std::wstring out = L"页=" + std::to_wstring(st.current_page) + L"  分区=" +
                     std::to_wstring(st.fences.size()) + L"\n";
  for (size_t i = 0; i < st.fences.size() && i < 12; ++i) {
    const auto& f = st.fences[i];
    out += L" · " + f.title + L"  (" + std::to_wstring(f.w) + L"x" +
           std::to_wstring(f.h) + L") p" + std::to_wstring(f.page) + L"\n";
  }
  if (st.fences.size() > 12) {
    out += L" · …\n";
  }
  // Sidecar icon count if present.
  const std::wstring icons = path + L".icons.json";
  std::ifstream in(icons);
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();
    int n = 0;
    size_t p = 0;
    while ((p = json.find("\"path\"", p)) != std::string::npos) {
      ++n;
      ++p;
    }
    out += L"壳图标约 " + std::to_wstring(n) + L" 项\n";
  }
  return out;
}

}  // namespace

bool ManageSnapshotsDialog(HWND owner, AppState* applied) {
  if (!applied) {
    return false;
  }
  auto list = ListSnapshots();
  if (list.empty()) {
    MessageBoxW(owner, L"还没有快照。请先「保存布局快照」。", L"布局快照",
                MB_OK | MB_ICONINFORMATION);
    return false;
  }

  std::wstring msg = L"布局快照列表：\n\n";
  for (size_t i = 0; i < list.size(); ++i) {
    msg += std::to_wstring(i + 1) + L". " + list[i].name + L"\n";
  }
  msg += L"\n是 = 恢复选中　否 = 删除选中　取消 = 关闭\n"
         L"（选择编号后会先显示预览）";
  const int choice =
      MessageBoxW(owner, msg.c_str(), L"布局快照", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDCANCEL) {
    return false;
  }

  const std::wstring prompt =
      L"输入编号 (1-" + std::to_wstring(list.size()) + L")：";
  const std::wstring ans = PromptText(owner, L"布局快照", prompt.c_str(), L"1");
  const int idx = _wtoi(ans.c_str()) - 1;
  if (idx < 0 || idx >= static_cast<int>(list.size())) {
    MessageBoxW(owner, L"编号无效。", L"布局快照", MB_OK | MB_ICONWARNING);
    return false;
  }

  const auto& item = list[static_cast<size_t>(idx)];
  const std::wstring preview =
      L"预览：" + item.name + L"\n\n" + SnapshotPreviewText(item.path);

  if (choice == IDNO) {
    if (MessageBoxW(owner, (preview + L"\n确认删除？").c_str(), L"删除快照",
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
      return false;
    }
    if (DeleteFileW(item.path.c_str())) {
      DeleteFileW((item.path + L".icons.json").c_str());
      MessageBoxW(owner, L"已删除。", L"布局快照", MB_OK | MB_ICONINFORMATION);
    } else {
      MessageBoxW(owner, L"删除失败。", L"布局快照", MB_OK | MB_ICONWARNING);
    }
    return false;
  }

  if (MessageBoxW(owner, (preview + L"\n恢复此快照？").c_str(), L"恢复快照",
                  MB_YESNO | MB_ICONQUESTION) != IDYES) {
    return false;
  }

  if (!LoadSnapshotWithIcons(item.path, applied)) {
    MessageBoxW(owner, L"读取快照失败。", L"布局快照", MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}
