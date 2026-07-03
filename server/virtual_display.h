#ifndef VIRTUAL_DISPLAY_H
#define VIRTUAL_DISPLAY_H

#include <string>
#include <vector>

// Check if usbmmid_v2 is installed via SetupAPI
bool IsUsbmmIdInstalled();

// Install usbmmid_v2 driver (requires bundled inf/cat in exe dir)
bool InstallUsbmmId();

// Enable virtual display — returns the DeviceName (e.g. L"\\\\.\\DISPLAY4")
// Used by EasyTierService to pass to DirectDesktop
std::wstring EnableVirtualDisplay();

// Disable/remove virtual display
void DisableVirtualDisplay();

// Query all display device names (for matching against DXGI outputs)
std::vector<std::wstring> EnumDisplayDeviceNames();

// Power management (call from user session only)
// Pass the virtual display DeviceName to skip it (e.g. L"\\\\.\\DISPLAY3")
// Empty string = disable/enable all displays
void DisablePhysicalMonitors(const std::wstring& skipDeviceName = L"");
void RestorePhysicalMonitors(const std::wstring& skipDeviceName = L"");

#endif
