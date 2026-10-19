#include "snapshot_ui.h"

#include "prompt_text.h"
#include "snapshot.h"

#include <string>

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
  msg += L"\n是 = 恢复选中　否 = 删除选中　取消 = 关闭";
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

  if (choice == IDNO) {
    if (DeleteFileW(list[static_cast<size_t>(idx)].path.c_str())) {
      MessageBoxW(owner, L"已删除。", L"布局快照", MB_OK | MB_ICONINFORMATION);
    } else {
      MessageBoxW(owner, L"删除失败。", L"布局快照", MB_OK | MB_ICONWARNING);
    }
    return false;
  }

  if (!LoadSnapshot(list[static_cast<size_t>(idx)].path, applied)) {
    MessageBoxW(owner, L"读取快照失败。", L"布局快照", MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}
