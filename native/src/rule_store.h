#pragma once

#include <cstddef>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Rule model (aligned with Python rule_engine.py + fence_rules.py current
// semantics). A rule maps a pattern to a fence id. Two pattern classes:
//   .ext            extension rule — exact, case-insensitive (Python
//                   match_extension). Evaluated FIRST (extension priority).
//   <other>         filename rule — fnmatch(name, pat) OR pat substring of
//                   name, case-insensitive (Python match_filename). Includes
//                   "*" catch-all and globs like "*.pdf" / "Invoice*" / "?" /
//                   "[ab]".
// Deprecated first-class syntax (name:/name:=/kind:image|doc|video|audio|
// archive|other) is NOT a rule key anymore; LoadRules maps legacy keys to the
// equivalent extension/filename rules for old-config compatibility.
// Insertion-ordered rule map. Iteration order == insertion order (matches
// Python dict insertion semantics, so the FIRST filename rule added wins on a
// multi-rule match — the previous unordered_map had unspecified iteration
// order). operator[] upserts in place, preserving the original position of an
// existing key, so existing callers (rules editor) compile unchanged.
class RuleMap {
 public:
  using value_type = std::pair<std::wstring, std::string>;

  RuleMap() = default;
  RuleMap(std::initializer_list<value_type> init) {
    for (const auto& kv : init) {
      (*this)[kv.first] = kv.second;
    }
  }

  std::string& operator[](const std::wstring& key);

  bool empty() const { return order_.empty(); }
  size_t size() const { return order_.size(); }
  void clear() {
    order_.clear();
    index_.clear();
    folded_.clear();
  }

  auto begin() { return order_.begin(); }
  auto end() { return order_.end(); }
  auto begin() const { return order_.begin(); }
  auto end() const { return order_.end(); }
  auto cbegin() const { return order_.cbegin(); }
  auto cend() const { return order_.cend(); }

  // Case-folded (locale-invariant lowercase) copy of the key at `i`, precomputed
  // on insertion so concurrent const readers never mutate internal buffers.
  const std::wstring& FoldedKey(size_t i) const;

 private:
  std::vector<value_type> order_;
  std::unordered_map<std::wstring, size_t> index_;
  std::vector<std::wstring> folded_;
};

std::wstring RuleFilePath();
RuleMap DefaultRules();
RuleMap LoadRules();
bool SaveRules(const RuleMap& rules);

// U1 filename_rules / category_fence_map (Python organize_targets.py +
// fence_rules.py legacy organize categories). Semantics:
//   category_fence_map : category id -> fence id (target fence per category)
//   filename_rules     : category id -> list of filename glob patterns
// Persisted in a dedicated organize_categories.json (kept separate from the
// pattern->fence `rules` schema so the two never clobber each other).
using CategoryFenceMap = std::unordered_map<std::string, std::string>;
using FilenameRulesMap =
    std::unordered_map<std::string, std::vector<std::wstring>>;

CategoryFenceMap LoadCategoryFenceMap();
FilenameRulesMap LoadFilenameRules();
// Load both maps from organize_categories.json in a single read (the two
// single-map loaders each re-read + re-parse the whole file).
void LoadCategoriesBoth(CategoryFenceMap* fence_map,
                        FilenameRulesMap* filename_rules);
bool SaveCategoryFenceMap(const CategoryFenceMap& map);
bool SaveFilenameRules(const FilenameRulesMap& map);

// U1 settings-UI editable forms (single-line, ';' separated entries so no
// newline escaping is needed across the WebView2 JSON boundary):
//   category_fence_map: "cat=fence; cat2=fence2"
//   filename_rules:     "cat=pat1,pat2; cat2=pat3"
std::wstring SerializeCategoryFenceMap(const CategoryFenceMap& map);
CategoryFenceMap ParseCategoryFenceMapText(const std::wstring& text);
std::wstring SerializeFilenameRules(const FilenameRulesMap& map);
FilenameRulesMap ParseFilenameRulesText(const std::wstring& text);
// Match basename against filename_rules globs → category_fence_map fence id.
// Empty = no category hit (caller falls through to extension RuleMap).
std::string ResolveCategoryFenceId(const std::wstring& path,
                                   const CategoryFenceMap& cat_map,
                                   const FilenameRulesMap& filename_rules);

// Validate a rule pattern + target fence id. Returns "" when acceptable, else a
// user-facing reason (UTF-8) to reject the rule without writing it.
std::string ValidateRule(const std::wstring& pattern, const std::string& fence_id);

// Resolve target fence for a desktop path using pins first, then rules.
// Priority (matches Python rule_engine.match_category): pins → extension exact
// → filename (fnmatch or substring) → fallback (software for links, common
// otherwise).
std::string ResolveFenceId(const std::wstring& path, bool is_link,
                           const std::unordered_map<std::wstring, std::string>& pins,
                           const RuleMap& rules,
                           const std::string& fallback_software,
                           const std::string& fallback_common);

std::wstring ExtOf(const std::wstring& path);
