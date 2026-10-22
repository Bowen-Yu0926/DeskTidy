#pragma once

#include "rule_store.h"

#include <Windows.h>

#include <string>
#include <utility>
#include <vector>

// Modal rules editor. Returns true if the caller should reload rules from *inout.
bool EditRulesDialog(
    HWND owner, RuleMap* inout,
    const std::vector<std::pair<std::string, std::wstring>>& fence_hints);
