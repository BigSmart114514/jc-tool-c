#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <iostream>
#include <string>
#include <vector>
#include "../service/logging.h"

#ifndef CDS_DISABLE
#define CDS_DISABLE 0x00000008
#endif
#ifndef CDS_ENABLE
#define CDS_ENABLE 0x00000010
#endif

#pragma comment(lib, "setupapi.lib")

static std::vector<std::wstring> EnumDisplayDeviceNames();

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH + 1] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    return path;
}

static std::wstring GetDeviceInstallerPath() {
    return GetExeDir() + L"\\deviceinstaller64.exe";
}

static std::wstring GetUsbmmIdInfPath() {
    return GetExeDir() + L"\\usbmmidd.inf";
}

static std::string narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(),
                         s.data(), len, nullptr, nullptr);
    return s;
}

static bool RunProcess(const std::wstring& exe, const std::wstring& args) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    const std::wstring workingDir = GetExeDir();
    sei.lpDirectory = workingDir.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        WriteLog("[VirtualDisplay] ShellExecuteExW failed: " + std::to_string(GetLastError()));
        return false;
    }
    if (!sei.hProcess) return false;
    const DWORD wait = WaitForSingleObject(sei.hProcess, 30000);
    DWORD ec = 0;
    const bool exited = wait == WAIT_OBJECT_0 && GetExitCodeProcess(sei.hProcess, &ec);
    CloseHandle(sei.hProcess);
    if (!exited || ec != 0) {
        WriteLog("[VirtualDisplay] Installer command failed: " + narrow(args)
                 + " wait=" + std::to_string(wait) + " exit=" + std::to_string(ec));
        return false;
    }
    return true;
}

bool IsUsbmmIdInstalled() {
    HDEVINFO devs = SetupDiGetClassDevsW(
        &GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
        DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        WriteLog("[VirtualDisplay] SetupDiGetClassDevs failed: " + std::to_string(GetLastError()));
        return false;
    }

    bool found = false;
    SP_DEVINFO_DATA did = { sizeof(did) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &did); ++i) {
        wchar_t hwid[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &did, SPDRP_HARDWAREID,
                                                nullptr, (PBYTE)hwid, sizeof(hwid), nullptr))
            continue;
        std::wstring hw(hwid);
        for (auto& c : hw) c = towlower(c);
        if (hw.find(L"usbmmid") != std::wstring::npos ||
            hw.find(L"idd_sample") != std::wstring::npos) {
            found = true;
            WriteLog("[VirtualDisplay] Found usbmmid device");
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

bool InstallUsbmmId() {
    std::wstring exe = GetDeviceInstallerPath();
    std::wstring inf = GetUsbmmIdInfPath();

    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteLog("[VirtualDisplay] deviceinstaller64.exe not found");
        return false;
    }
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteLog("[VirtualDisplay] usbmmidd.inf not found");
        return false;
    }

    WriteLog("[VirtualDisplay] Installing usbmmidd...");
    std::wstring args = L"install \"" + inf + L"\" usbmmidd";
    bool ok = RunProcess(exe, args);
    WriteLog(ok ? "[VirtualDisplay] Install OK" : "[VirtualDisplay] Install FAILED");
    return ok;
}

bool EnableVirtualDisplay(std::wstring& deviceName) {
    deviceName.clear();
    if (!IsUsbmmIdInstalled()) {
        WriteLog("[VirtualDisplay] Not installed, attempting install...");
        if (!InstallUsbmmId()) {
            WriteLog("[VirtualDisplay] Install failed, cannot enable");
            return false;
        }
    }

    std::vector<std::wstring> before = EnumDisplayDeviceNames();

    std::wstring exe = GetDeviceInstallerPath();
    std::wstring args = L"enableidd 1";
    WriteLog("[VirtualDisplay] Enabling usbmmidd (enableidd 1)...");
    if (!RunProcess(exe, args)) return false;

    Sleep(3000);

    std::vector<std::wstring> after = EnumDisplayDeviceNames();

    for (const auto& name : after) {
        bool found = false;
        for (const auto& old : before) {
            if (name == old) { found = true; break; }
        }
        if (!found) {
            WriteLog("[VirtualDisplay] New display device: " + narrow(name));
            deviceName = name;
            return true;
        }
    }

    WriteLog("[VirtualDisplay] No new desktop output visible to service");
    return true;
}

void DisableVirtualDisplay() {
    std::wstring exe = GetDeviceInstallerPath();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteLog("[VirtualDisplay] deviceinstaller64.exe not found, skipping disable");
        return;
    }
    WriteLog("[VirtualDisplay] Deactivating usbmmidd (enableidd 0)...");
    RunProcess(exe, L"enableidd 0");
}

static std::vector<std::wstring> EnumDisplayDeviceNames() {
    std::vector<std::wstring> result;

    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)
            result.push_back(dd.DeviceName);
        dd = {};
        dd.cb = sizeof(dd);
    }
    return result;
}

void DisablePhysicalMonitors(const std::wstring& skipDeviceName) {
    WriteLog("[VirtualDisplay] Disabling physical monitors (skip="
             + (skipDeviceName.empty() ? std::string("none") : narrow(skipDeviceName)) + ")");
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (!skipDeviceName.empty() && wcscmp(dd.DeviceName, skipDeviceName.c_str()) == 0) {
            WriteLog("[VirtualDisplay]  Skipping virtual: " + narrow(dd.DeviceName));
        } else {
            WriteLog("[VirtualDisplay]  Disabling: " + narrow(dd.DeviceName));
            LONG r = ChangeDisplaySettingsExW(dd.DeviceName, NULL, NULL, CDS_DISABLE, NULL);
            if (r != DISP_CHANGE_SUCCESSFUL)
                WriteLog("[VirtualDisplay]  Disable failed: " + std::to_string(r));
        }
        dd.cb = sizeof(dd);
    }
}

void RestorePhysicalMonitors(const std::wstring& skipDeviceName) {
    WriteLog("[VirtualDisplay] Restoring physical monitors (skip="
             + (skipDeviceName.empty() ? std::string("none") : narrow(skipDeviceName)) + ")");
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (!skipDeviceName.empty() && wcscmp(dd.DeviceName, skipDeviceName.c_str()) == 0) {
            WriteLog("[VirtualDisplay]  Skipping virtual: " + narrow(dd.DeviceName));
        } else {
            WriteLog("[VirtualDisplay]  Enabling: " + narrow(dd.DeviceName));
            LONG r = ChangeDisplaySettingsExW(dd.DeviceName, NULL, NULL, CDS_ENABLE, NULL);
            if (r != DISP_CHANGE_SUCCESSFUL)
                WriteLog("[VirtualDisplay]  Enable failed: " + std::to_string(r));
        }
        dd.cb = sizeof(dd);
    }
}
