# wireless-battery-tray

![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg?logo=c%2B%2B)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-lightgrey.svg)
![Build](https://img.shields.io/badge/build-CMake-green.svg)

A tiny Windows tray utility that shows the battery level of your wireless
devices (mice, keyboards, headsets) as a percentage. Pure Win32 API, no
dependencies, no runtime other than the C++ standard library.

- The tray icon shows the **lowest** battery level, colored green / orange / red.
- Hover the icon to see every device with its percentage.
- Right click for the menu (**Refresh**, **Exit**); left click refreshes immediately.
- Refreshes automatically every 5 minutes, runs only one instance at a time.
- HID queries run on a worker thread, so the tray never freezes.

## Build

Requirements: Windows 10/11, CMake 3.16+, and a C++17 compiler (Visual Studio 2019+
Build Tools or MinGW-w64). The Windows SDK provides the HID headers.

```bash
mkdir build && cd build && cmake .. && cmake --build .
```

With Visual Studio (a multi-config generator) build the Release configuration:

```bash
cmake --build . --config Release
```

The executable is `build/wireless-battery-tray.exe` (or
`build/Release/wireless-battery-tray.exe` with Visual Studio).

## Run

Start `wireless-battery-tray.exe`. The icon appears in the notification area
(you may need to drag it out of the overflow "^" menu). To start it with
Windows, put a shortcut to the executable into `shell:startup`.

## How it works

For every present HID device the tool does the following (see `src/HidDevice.cpp`):

1. Enumerates HID interfaces with `SetupDiGetClassDevs` / `SetupDiEnumDeviceInterfaces`.
2. Opens each one with `CreateFile` using access rights `0` (works for keyboards and
   mice that Windows holds exclusively).
3. Reads the report descriptor with `HidD_GetPreparsedData` and looks for a battery
   *feature* value:
   - Battery System page `0x85`, usage `0x65` (AbsoluteStateOfCharge)
   - Battery System page `0x85`, usage `0x64` (RelativeStateOfCharge)
   - Generic Device Controls page `0x06`, usage `0x20` (BatteryStrength)
4. Reads the report with `HidD_GetFeature`, extracts the field with `HidP_GetUsageValue`
   and scales it with the item's logical minimum/maximum to 0-100 %.
5. If the device has no HID battery report, falls back to the Windows device property
   `{104EA319-6EE2-4701-BD47-8DDBF425BBE5},2`, which Windows sets for many Bluetooth LE
   devices (checked on the device node and up to three parents).
6. Takes the device name from `HidD_GetProductString`, or from
   `SetupDiGetDeviceRegistryProperty` (friendly name / description) as a fallback.

### A note on `BATTERY_STATUS`

`hidclass.h` has no `BATTERY_STATUS`. The structure of that name is declared in
`ntddbatt.h` for the battery *class* driver (`IOCTL_BATTERY_QUERY_STATUS`, used by laptop
batteries and UPS devices) and reports capacity in mWh, not a percentage. HID devices
describe their battery in the report descriptor instead, which is what this tool parses.

## Limitations

- Only devices that expose their battery through standard HID usages or the Windows
  battery device property are shown. Many vendor devices (Logitech, Razer, Corsair,
  SteelSeries, most Bluetooth headsets) report the battery over proprietary channels or
  vendor software and will **not** appear.
- Battery values delivered only in *input* reports are not read (only feature reports).
- Not tested on a wide range of hardware yet; reports and pull requests are welcome.

## Project layout

```text
CMakeLists.txt
src/main.cpp        entry point, message loop, refresh scheduling
src/TrayIcon.*      notification-area icon, tooltip, context menu
src/HidDevice.*     HID enumeration and battery reading
```
