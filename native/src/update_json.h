#pragma once

#include <functional>
#include <string>

// Hand-rolled scanning of the GitHub `/releases/latest` JSON body.
//
// Kept free of Win32/network/UI so it can be unit-tested against the exact
// response shapes GitHub returns — the updater cannot depend on a JSON library
// here, and a wrong assumption about the shape only shows up as "no installer
// in this release" at download time.

// Value of `"key": "..."` in `json` (first occurrence). Unescapes backslashes
// one level; returns "" when the key or its string value is absent.
std::string JsonStringField(const std::string& json, const char* key);

// Value of `"key": <digits>` (first occurrence) as text, "" when absent.
std::string JsonNumberField(const std::string& json, const char* key);

// Index of the '}' that closes the object starting at `start`, honouring
// nesting and string literals. std::string::npos when unterminated.
size_t JsonObjectEnd(const std::string& json, size_t start);

// Walk the top-level objects of the array that follows the `"key"` field,
// calling `fn` with each element as a slice. Stops at the end of that array.
//
// Searching for '{' from the start of the document instead finds the ROOT
// object first, whose matching '}' spans the whole body — every asset then
// collapses into one "element" and only the first (or none) is ever seen.
void ForEachArrayObject(const std::string& json, const char* key,
                        const std::function<void(const std::string&)>& fn);

// Pick the release's Windows installer out of the response body: the largest
// asset whose name is `DeskTidy_Setup_*.exe`. Returns false (leaving outputs
// untouched) when there is none.
bool FindExeAsset(const std::string& json, std::string* name,
                  std::wstring* url, long long* size);

// Pick a sibling asset by exact name (e.g. "<Setup>.exe.manifest" /
// "<Setup>.exe.sig"). Returns false when absent.
bool FindAssetByName(const std::string& json, const std::string& want_name,
                     std::wstring* url);
