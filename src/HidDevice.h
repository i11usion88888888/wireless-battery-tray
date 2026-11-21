#pragma once

#include <string>
#include <vector>

namespace hid {

struct BatteryInfo {
    std::wstring name;   // product name, e.g. "MX Master 3"
    std::wstring path;   // HID device interface path (useful for debugging)
    int percent = -1;    // 0..100
};

// Maps a raw HID value onto 0..100 using the report's logical range.
// Returns -1 when the range is invalid or the value lies outside it
// (devices use out-of-range values such as 0xFF for "unknown").
int ScaleToPercent(long long value, long long logicalMin, long long logicalMax);

// Enumerates all present HID devices and returns those that report a
// battery level. Safe to call from a worker thread. May block for a while
// if a device answers slowly, so do not call it from the UI thread.
std::vector<BatteryInfo> ReadAllBatteries();

} // namespace hid
