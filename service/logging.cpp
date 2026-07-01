#include "logging.h"
#include <windows.h>
#include <cstdio>

void WriteLog(const std::string& msg) {
    HANDLE hFile = CreateFileW(L"C:\\ProgramData\\EasyTier\\service.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    sprintf_s(buf, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string line = buf + msg + "\r\n";
    DWORD written;
    WriteFile(hFile, line.data(), (DWORD)line.size(), &written, NULL);
    CloseHandle(hFile);
}
