// Reads battery levels of HID devices.
//
// IMPORTANT: hidclass.h does not contain a BATTERY_STATUS structure. The only
// BATTERY_STATUS in the Windows headers lives in ntddbatt.h; it belongs to the
// battery class driver (IOCTL_BATTERY_QUERY_STATUS, used for laptop batteries
// and UPS devices) and reports Capacity in mWh, not a percentage. HID devices
// do not use it. A HID device describes its battery in its report descriptor:
//
//   * Battery System page (0x85), usage 0x65 AbsoluteStateOfCharge
//   * Battery System page (0x85), usage 0x64 RelativeStateOfCharge
//   * Generic Device Controls page (0x06), usage 0x20 BatteryStrength
//
// as a *Feature* value. We find that value with HidP_GetValueCaps, read the
// whole report with HidD_GetFeature, extract the field with
// HidP_GetUsageValue and scale it with the item's logical min/max.
//
// Fallback: Windows exposes the battery of many Bluetooth LE devices as the
// device property {104EA319-6EE2-4701-BD47-8DDBF425BBE5},2 (a BYTE, percent)
// on the device node or one of its parents. We check the HID node and up to
// three ancestors.

#include "HidDevice.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <utility>

namespace hid {

namespace {

struct UsageId {
    USHORT page;
    USHORT usage;
};

// Tried in this order; the first one that yields a valid value wins.
constexpr UsageId kBatteryUsages[] = {
    {0x0085, 0x0065}, // Battery System / AbsoluteStateOfCharge
    {0x0085, 0x0064}, // Battery System / RelativeStateOfCharge
    {0x0006, 0x0020}, // Generic Device Controls / BatteryStrength
};

const DEVPROPKEY kBatteryPercentKey = {
    {0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}},
    2};

struct FileHandle {
    HANDLE h;
    explicit FileHandle(HANDLE handle) : h(handle) {}
    ~FileHandle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

struct PreparsedData {
    PHIDP_PREPARSED_DATA data = nullptr;
    ~PreparsedData() {
        if (data) HidD_FreePreparsedData(data);
    }
};

struct DevInfoSet {
    HDEVINFO set;
    explicit DevInfoSet(HDEVINFO s) : set(s) {}
    ~DevInfoSet() {
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    }
    DevInfoSet(const DevInfoSet&) = delete;
    DevInfoSet& operator=(const DevInfoSet&) = delete;
};

std::wstring GetRegistryString(HDEVINFO set, SP_DEVINFO_DATA& data, DWORD property) {
    DWORD type = 0;
    DWORD needed = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &data, property, &type, nullptr, 0, &needed);
    if (needed == 0) return {};

    std::vector<wchar_t> buffer(needed / sizeof(wchar_t) + 2, L'\0');
    if (!SetupDiGetDeviceRegistryPropertyW(set, &data, property, &type,
                                           reinterpret_cast<PBYTE>(buffer.data()), needed,
                                           nullptr)) {
        return {};
    }
    if (type != REG_SZ) return {};
    return std::wstring(buffer.data());
}

// Looks for a battery usage among the device's feature value capabilities and
// reads it with HidD_GetFeature. Returns -1 if the device has none.
int ReadBatteryFromFeatureReport(HANDLE device) {
    PreparsedData preparsed;
    if (!HidD_GetPreparsedData(device, &preparsed.data)) return -1;

    HIDP_CAPS caps{};
    if (HidP_GetCaps(preparsed.data, &caps) != HIDP_STATUS_SUCCESS) return -1;
    if (caps.NumberFeatureValueCaps == 0 || caps.FeatureReportByteLength == 0) return -1;

    USHORT count = caps.NumberFeatureValueCaps;
    std::vector<HIDP_VALUE_CAPS> valueCaps(count);
    if (HidP_GetValueCaps(HidP_Feature, valueCaps.data(), &count, preparsed.data) !=
        HIDP_STATUS_SUCCESS) {
        return -1;
    }
    valueCaps.resize(count);

    for (const UsageId& wanted : kBatteryUsages) {
        for (const HIDP_VALUE_CAPS& vc : valueCaps) {
            if (vc.UsagePage != wanted.page) continue;

            const bool matches = vc.IsRange
                ? (wanted.usage >= vc.Range.UsageMin && wanted.usage <= vc.Range.UsageMax)
                : (vc.NotRange.Usage == wanted.usage);
            if (!matches) continue;

            // Signed fields would need HidP_GetScaledUsageValue; batteries are
            // unsigned in practice, so skip anything else.
            if (vc.LogicalMin < 0) continue;

            // Byte 0 must hold the report ID; the buffer must be exactly the
            // feature report length for the top-level collection.
            std::vector<UCHAR> report(caps.FeatureReportByteLength, 0);
            report[0] = vc.ReportID;
            if (!HidD_GetFeature(device, report.data(), static_cast<ULONG>(report.size()))) {
                continue;
            }

            ULONG raw = 0;
            if (HidP_GetUsageValue(HidP_Feature, vc.UsagePage, vc.LinkCollection, wanted.usage,
                                   &raw, preparsed.data, reinterpret_cast<PCHAR>(report.data()),
                                   static_cast<ULONG>(report.size())) != HIDP_STATUS_SUCCESS) {
                continue;
            }

            const int percent = ScaleToPercent(raw, vc.LogicalMin, vc.LogicalMax);
            if (percent >= 0) return percent;
        }
    }
    return -1;
}

// Bluetooth LE fallback: the shell stores the battery level as a device
// property on the device node (or a parent) rather than in a HID report.
int ReadBatteryFromDeviceProperty(DEVINST node) {
    for (int depth = 0; depth < 4 && node != 0; ++depth) {
        DEVPROPTYPE type = 0;
        BYTE value = 0;
        ULONG size = sizeof(value);
        if (CM_Get_DevNode_PropertyW(node, &kBatteryPercentKey, &type, &value, &size, 0) ==
                CR_SUCCESS &&
            type == DEVPROP_TYPE_BYTE && value <= 100) {
            return value;
        }

        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return -1;
}

} // namespace

int ScaleToPercent(long long value, long long logicalMin, long long logicalMax) {
    if (logicalMax <= logicalMin) return -1;
    if (value < logicalMin || value > logicalMax) return -1;
    const long long span = logicalMax - logicalMin;
    return static_cast<int>(((value - logicalMin) * 100 + span / 2) / span);
}

std::vector<BatteryInfo> ReadAllBatteries() {
    std::vector<BatteryInfo> found;

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    DevInfoSet devices(
        SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (devices.set == INVALID_HANDLE_VALUE) return found;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(devices.set, nullptr, &hidGuid, index, &iface)) break;

        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devices.set, &iface, nullptr, 0, &needed, nullptr);
        if (needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;

        std::vector<BYTE> raw(needed, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(devices.set, &iface, detail, needed, nullptr,
                                              &devInfo)) {
            continue;
        }

        // Access rights 0: we only issue feature-report IOCTLs, and this also
        // works for keyboards and mice, which Windows opens exclusively.
        FileHandle file(CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr));

        std::wstring name;
        int percent = -1;

        if (file.valid()) {
            wchar_t product[128] = {};
            if (HidD_GetProductString(file.h, product, sizeof(product)) && product[0] != L'\0') {
                name = product;
            }
            percent = ReadBatteryFromFeatureReport(file.h);
        }

        if (percent < 0) percent = ReadBatteryFromDeviceProperty(devInfo.DevInst);
        if (percent < 0) continue;

        if (name.empty()) name = GetRegistryString(devices.set, devInfo, SPDRP_FRIENDLYNAME);
        if (name.empty()) name = GetRegistryString(devices.set, devInfo, SPDRP_DEVICEDESC);
        if (name.empty()) name = L"HID device";

        BatteryInfo info;
        info.name = std::move(name);
        info.path = detail->DevicePath;
        info.percent = percent;
        found.push_back(std::move(info));
    }

    // One physical device often exposes several HID interfaces (mouse,
    // keyboard, consumer control...). Collapse entries that look identical.
    std::vector<BatteryInfo> unique;
    for (BatteryInfo& candidate : found) {
        const bool duplicate =
            std::any_of(unique.begin(), unique.end(), [&](const BatteryInfo& existing) {
                return existing.name == candidate.name && existing.percent == candidate.percent;
            });
        if (!duplicate) unique.push_back(std::move(candidate));
    }
    return unique;
}

} // namespace hid
