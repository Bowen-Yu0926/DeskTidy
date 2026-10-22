#pragma once

#include <string>

// Random toast line for a pet action (pet/sleep/wake/wait/climb/fall/trash).
// character: "hoodie", "voyage", or empty → hoodie defaults.
std::wstring PetActionLine(const std::wstring& character,
                           const char* action);
