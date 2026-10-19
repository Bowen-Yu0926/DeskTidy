#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// extension (".pdf", ".lnk", "*") -> fence id
using RuleMap = std::unordered_map<std::wstring, std::string>;

std::wstring RuleFilePath();
RuleMap DefaultRules();
RuleMap LoadRules();
bool SaveRules(const RuleMap& rules);

// Resolve target fence for a desktop path using pins first, then rules.
std::string ResolveFenceId(const std::wstring& path, bool is_link,
                           const std::unordered_map<std::wstring, std::string>& pins,
                           const RuleMap& rules,
                           const std::string& fallback_software,
                           const std::string& fallback_common);

std::wstring ExtOf(const std::wstring& path);
