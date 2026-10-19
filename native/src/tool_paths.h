#pragma once

#include <string>

// Locate helper binaries next to exe, under assets\, or on PATH.
std::wstring FindFfmpegExe();
std::wstring FindFdExe();
std::wstring ExeDir();
