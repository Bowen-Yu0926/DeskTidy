#include "pet_lines.h"

#include <Windows.h>

#include <cstring>
#include <vector>

namespace {

const wchar_t* Pick(const std::vector<const wchar_t*>& lines) {
  if (lines.empty()) {
    return L"";
  }
  return lines[static_cast<size_t>(GetTickCount()) % lines.size()];
}

const wchar_t* HoodieLine(const char* action) {
  if (std::strcmp(action, "pet") == 0) {
    return Pick({L"嘿嘿，好啦好啦。", L"谢谢你～", L"再摸摸！"});
  }
  if (std::strcmp(action, "sleep") == 0) {
    return Pick({L"先眯一会儿……", L"Zzz……"});
  }
  if (std::strcmp(action, "wake") == 0) {
    return Pick({L"醒啦！", L"伸个懒腰～"});
  }
  if (std::strcmp(action, "wait") == 0) {
    return Pick({L"趴着打会儿游戏～", L"等你哦～", L"这关马上过！"});
  }
  if (std::strcmp(action, "climb") == 0) {
    return Pick({L"我去边上趴会儿～", L"慢慢挪过去。"});
  }
  if (std::strcmp(action, "fall") == 0) {
    return Pick({L"哇——！", L"落地落地！"});
  }
  if (std::strcmp(action, "trash") == 0) {
    return Pick({L"折个纸飞机～", L"飞走啦！", L"帮你删掉咯。"});
  }
  return L"";
}

const wchar_t* VoyageLine(const char* action) {
  if (std::strcmp(action, "pet") == 0) {
    return Pick({L"耶！好开心！", L"谢谢你～", L"再摸摸！", L"跳起来！"});
  }
  if (std::strcmp(action, "sleep") == 0) {
    return Pick({L"在甲板上眯一会儿……", L"Zzz……"});
  }
  if (std::strcmp(action, "wake") == 0) {
    return Pick({L"醒啦！", L"伸个懒腰～"});
  }
  if (std::strcmp(action, "wait") == 0) {
    return Pick({L"刷刷手机～", L"这条好有趣！", L"等你哦，我再看一眼。"});
  }
  if (std::strcmp(action, "climb") == 0) {
    return Pick({L"我去边上靠会儿～", L"慢慢挪过去。"});
  }
  if (std::strcmp(action, "fall") == 0) {
    return Pick({L"哇——飞起来啦！", L"噗通，落地！"});
  }
  if (std::strcmp(action, "trash") == 0) {
    return Pick({L"揉成一团～", L"进回收站啦。", L"帮你扔掉。"});
  }
  return L"";
}

}  // namespace

std::wstring PetActionLine(const std::wstring& character, const char* action) {
  if (!action || !*action) {
    return {};
  }
  const wchar_t* line = nullptr;
  if (_wcsicmp(character.c_str(), L"voyage") == 0) {
    line = VoyageLine(action);
  } else {
    line = HoodieLine(action);
  }
  return line ? line : L"";
}
