#ifndef VIRTUAL_DISPLAY_H
#define VIRTUAL_DISPLAY_H

#include <string>

// Check if usbmmid_v2 is installed via SetupAPI
bool IsUsbmmIdInstalled();

// Install usbmmid_v2 driver (requires bundled inf/cat in exe dir)
bool InstallUsbmmId();

// Enable the driver. deviceName may be empty if the display is not visible in Session 0.
bool EnableVirtualDisplay(std::wstring& deviceName);

// Disable the output, leaving the driver installed.
void DisableVirtualDisplay();

// Power management (call from user session only)
// Pass the virtual display DeviceName to skip it (e.g. L"\\\\.\\DISPLAY3")
// Empty string = disable/enable all displays
void DisablePhysicalMonitors(const std::wstring& skipDeviceName = L"");
void RestorePhysicalMonitors(const std::wstring& skipDeviceName = L"");

#endif
