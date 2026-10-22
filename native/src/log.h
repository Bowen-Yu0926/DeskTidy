#pragma once

// Minimal UTF-8 file logger + crash hooks for DeskTidy Native.
// Log path: %LOCALAPPDATA%\DeskTidyNative\logs\desktidy.log
// Rotates to desktidy.1.log when the active log exceeds 2 MB.

void LogInit();
void LogShutdown();
// printf-style wide logging; timestamp + [TID] + newline are added for you.
void LogLine(const wchar_t* fmt, ...);
// printf-style narrow (UTF-8) logging; same prefix handling.
void LogLineA(const char* fmt, ...);
// Installs UnhandledExceptionFilter / std::set_terminate / signal handlers.
void LogInstallCrashHandler();
