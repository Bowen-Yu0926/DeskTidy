#pragma once

#include <Windows.h>

#include <vector>

struct WindowLedge {
  int x = 0;
  int y = 0;
  int width = 0;
  HWND hwnd = nullptr;
};

// Top edges of visible top-level windows the pet can sit on.
std::vector<WindowLedge> ListWindowLedges(HWND exclude, int max_results = 24);
