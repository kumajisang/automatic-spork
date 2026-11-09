#include <windows.h>
#include <dwmapi.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <sstream>
#include <wrl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#include "drivers/DriverRegistry.h"

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Microsoft::WRL;
using namespace Gdiplus;

#define ID_TRAY_APP_ICON 1001
#define WM_TRAYICON (WM_USER + 1)
#define WM_APP_SCAN_DONE (WM_USER + 2) 

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webView;
HWND g_hMainWnd = nullptr;
HWND g_hTrayWnd = nullptr;
HWND g_hMenuWnd = nullptr; 
HWND g_hTooltipWnd = nullptr;
HICON g_hCurrentTrayIcon = nullptr; 

std::map<std::wstring, NOTIFYICONDATAW> g_pinnedIcons;
std::map<std::wstring, int> g_lastBatteryState; 
std::mutex g_pinnedMutex;
std::mutex g_iconMutex;
UINT g_nextIconId = 2000; 

std::wstring g_iniPath;
std::wstring g_devicesIniPath;
ULONG_PTR g_gdiplusToken = 0;
NOTIFYICONDATAW g_nid = { 0 };
bool g_batteryWarned = false;
bool g_isDarkTheme = false;
bool g_closeDirectly = false; 
bool g_showTrayTip = true; 

ComPtr<ICoreWebView2Environment> g_webviewEnv;
ComPtr<ICoreWebView2Controller> g_menuController;
ComPtr<ICoreWebView2> g_menuWebView;
bool g_menuWebViewReady = false;
bool g_isMenuCreating = false; 

ComPtr<ICoreWebView2Controller> g_tooltipController;
ComPtr<ICoreWebView2> g_tooltipWebView;
bool g_tooltipWebViewReady = false;
bool g_isTooltipCreating = false;

std::atomic<bool> g_isScanning(false);
std::wstring g_latest_json = L"";
std::mutex g_json_mutex;

std::atomic<bool> g_captureActive(false);

struct CustomDeviceProto {
    uint16_t vid;
    uint16_t pid;
    std::wstring name;
    std::wstring type;
    std::vector<uint8_t> request;
    int offset;
    uint8_t mask;
};

bool ReadHidWithTimeoutHelper(HANDLE hDevice, uint8_t* buffer, DWORD bufferSize, DWORD timeoutMs, DWORD& bytesRead) {
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return false;

    BOOL result = ReadFile(hDevice, buffer, bufferSize, &bytesRead, &ov);
    if (!result && GetLastError() == ERROR_IO_PENDING) {
        DWORD waitResult = WaitForSingleObject(ov.hEvent, timeoutMs);
        if (waitResult == WAIT_OBJECT_0) {
            result = GetOverlappedResult(hDevice, &ov, &bytesRead, FALSE);
        } else {
            CancelIo(hDevice); 
        }
    }
    CloseHandle(ov.hEvent);
    return result != FALSE;
}

std::vector<CustomDeviceProto> LoadCustomDevices() {
    std::vector<CustomDeviceProto> list;
    wchar_t sectionNames[2048] = {0};
    DWORD chars = GetPrivateProfileSectionNamesW(sectionNames, 2048, g_devicesIniPath.c_str());
    
    for (wchar_t* section = sectionNames; *section; section += wcslen(section) + 1) {
        CustomDeviceProto proto;
        proto.vid = (uint16_t)GetPrivateProfileIntW(section, L"Vid", 0, g_devicesIniPath.c_str());
        proto.pid = (uint16_t)GetPrivateProfileIntW(section, L"Pid", 0, g_devicesIniPath.c_str());
        if (proto.vid == 0 && proto.pid == 0) continue;
        
        wchar_t buf[256];
        GetPrivateProfileStringW(section, L"Name", L"自定义设备", buf, 256, g_devicesIniPath.c_str());
        proto.name = buf;
        
        GetPrivateProfileStringW(section, L"Type", L"other", buf, 256, g_devicesIniPath.c_str());
        std::wstring typeStr = buf;
        if (typeStr == L"鼠标" || typeStr == L"mouse") proto.type = L"mouse";
        else if (typeStr == L"键盘" || typeStr == L"keyboard") proto.type = L"keyboard";
        else if (typeStr == L"耳机" || typeStr == L"headset") proto.type = L"headset";
        else if (typeStr == L"手柄" || typeStr == L"gamepad" || typeStr == L"joystick") proto.type = L"gamepad";
        else proto.type = L"other";
        
        GetPrivateProfileStringW(section, L"Request", L"", buf, 256, g_devicesIniPath.c_str());
        std::wstring reqStr(buf);
        std::wstring temp;
        for(wchar_t c : reqStr) {
            if (iswspace(c) || c == L',') {
                if (!temp.empty()) {
                    proto.request.push_back((uint8_t)wcstoul(temp.c_str(), nullptr, 16));
                    temp.clear();
                }
            } else {
                temp += c;
            }
        }
        if (!temp.empty()) proto.request.push_back((uint8_t)wcstoul(temp.c_str(), nullptr, 16));
        
        proto.offset = GetPrivateProfileIntW(section, L"Offset", -1, g_devicesIniPath.c_str());
        proto.mask = (uint8_t)GetPrivateProfileIntW(section, L"Mask", 0xFF, g_devicesIniPath.c_str());
        
        if (proto.offset >= 0) {
            list.push_back(proto);
        }
    }
    return list;
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            RegSetValueExW(hKey, L"BatteryMonitor", 0, REG_SZ, (const BYTE*)exePath, (wcslen(exePath) + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hKey, L"BatteryMonitor");
        }
        RegCloseKey(hKey);
    }
}

bool IsAutoStart() {
    HKEY hKey;
    bool exists = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH];
        DWORD size = sizeof(value);
        if (RegQueryValueExW(hKey, L"BatteryMonitor", nullptr, nullptr, (LPBYTE)value, &size) == ERROR_SUCCESS) exists = true;
        RegCloseKey(hKey);
    }
    return exists;
}

bool IsSystemDarkMode() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 1; DWORD size = sizeof(value);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&value, &size);
        RegCloseKey(hKey);
        return value == 0; 
    }
    return false; 
}

std::wstring EscapeJsonString(const std::wstring& input) {
    std::wstring output;
    for (wchar_t c : input) {
        switch (c) {
            case L'\"': output += L"\\\""; break;
            case L'\\': output += L"\\\\"; break;
            case L'\b': output += L"\\b"; break;
            case L'\f': output += L"\\f"; break;
            case L'\n': output += L"\\n"; break;
            case L'\r': output += L"\\r"; break;
            case L'\t': output += L"\\t"; break;
            default: output += c; break;
        }
    }
    return output;
}

bool TryReadStandardHIDBattery(HANDLE hDevice, DeviceData& outDev) {
    PHIDP_PREPARSED_DATA preparsedData;
    if (!HidD_GetPreparsedData(hDevice, &preparsedData)) return false;

    HIDP_CAPS caps;
    if (HidP_GetCaps(preparsedData, &caps) != HIDP_STATUS_SUCCESS) {
        HidD_FreePreparsedData(preparsedData);
        return false;
    }

    bool found = false;
    USHORT numValueCaps = caps.NumberFeatureValueCaps;
    if (numValueCaps > 0) {
        std::vector<HIDP_VALUE_CAPS> valueCaps(numValueCaps);
        if (HidP_GetValueCaps(HidP_Feature, valueCaps.data(), &numValueCaps, preparsedData) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < numValueCaps; i++) {
                if (valueCaps[i].UsagePage == 0x06) { 
                    USAGE usage = valueCaps[i].IsRange ? valueCaps[i].Range.UsageMin : valueCaps[i].NotRange.Usage;
                    if (usage == 0x20) { 
                        uint8_t reportBuffer[64] = { 0 };
                        reportBuffer[0] = (uint8_t)valueCaps[i].ReportID;
                        if (HidD_GetFeature(hDevice, reportBuffer, sizeof(reportBuffer))) {
                            int battery = reportBuffer[1];
                            if (battery >= 0 && battery <= 100) {
                                outDev.battery = battery;
                                outDev.isCharging = false; 
                                if (outDev.name.empty()) outDev.name = L"标准 HID 设备";
                                outDev.type = L"other"; 
                                found = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    HidD_FreePreparsedData(preparsedData);
    return found;
}

std::vector<DeviceData> ScanAllDevices() {
    std::vector<DeviceData> foundDevices;
    auto customProtos = LoadCustomDevices();

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) return foundDevices;

    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    auto drivers = DriverRegistry::getDrivers();
    std::set<std::pair<unsigned short, unsigned short>> uniqueDevices;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, i, &deviceInterfaceData); i++) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &deviceInterfaceData, NULL, 0, &requiredSize, NULL);
        
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(requiredSize);
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &deviceInterfaceData, detailData, requiredSize, NULL, NULL)) {
            HANDLE hDevice = CreateFileW(detailData->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

            if (hDevice != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attributes;
                attributes.Size = sizeof(HIDD_ATTRIBUTES);
                if (HidD_GetAttributes(hDevice, &attributes)) {
                    bool deviceAdded = false;
                    auto deviceKey = std::make_pair(attributes.VendorID, attributes.ProductID);

                    for (const auto& proto : customProtos) {
                        if (attributes.VendorID == proto.vid && attributes.ProductID == proto.pid) {
                            if (uniqueDevices.find(deviceKey) != uniqueDevices.end()) break;

                            if (!proto.request.empty()) {
                                DWORD written = 0;
                                WriteFile(hDevice, proto.request.data(), (DWORD)proto.request.size(), &written, NULL);
                            }
                            
                            uint8_t response[64] = {0};
                            DWORD bytesRead = 0;
                            DWORD timeout = proto.request.empty() ? 1000 : 500;
                            
                            if (ReadHidWithTimeoutHelper(hDevice, response, sizeof(response), timeout, bytesRead)) {
                                if (bytesRead > (DWORD)proto.offset) {
                                    int battery = response[proto.offset] & proto.mask;
                                    if (battery >= 0 && battery <= 100) {
                                        DeviceData dev;
                                        dev.name = proto.name;
                                        dev.type = proto.type;
                                        dev.battery = battery;
                                        dev.isCharging = false;
                                        foundDevices.push_back(dev);
                                        uniqueDevices.insert(deviceKey);
                                        deviceAdded = true;
                                    }
                                }
                            }
                            break;
                        }
                    }

                    if (!deviceAdded) {
                        for (auto& drv : drivers) {
                            if (attributes.VendorID == drv->getVid() && (drv->getPid() == 0x0000 || attributes.ProductID == drv->getPid())) {
                                if (uniqueDevices.find(deviceKey) != uniqueDevices.end()) break; 

                                DeviceData dev;
                                dev.battery = -1;
                                dev.type = drv->getDeviceType();
                                if (drv->readBattery(hDevice, dev)) {
                                    foundDevices.push_back(dev);
                                    uniqueDevices.insert(deviceKey);
                                    deviceAdded = true;
                                }
                                break; 
                            }
                        }
                    }

                    if (!deviceAdded) {
                        DeviceData autoDev;
                        autoDev.battery = -1;
                        wchar_t productName[128] = {0};
                        if (HidD_GetProductString(hDevice, productName, sizeof(productName))) autoDev.name = productName;
                        else autoDev.name = L"未知设备";

                        if (TryReadStandardHIDBattery(hDevice, autoDev)) {
                            if (uniqueDevices.find(deviceKey) == uniqueDevices.end()) {
                                foundDevices.push_back(autoDev);
                                uniqueDevices.insert(deviceKey);
                            }
                        } else {
                            PHIDP_PREPARSED_DATA prepData;
                            if (HidD_GetPreparsedData(hDevice, &prepData)) {
                                HIDP_CAPS caps;
                                if (HidP_GetCaps(prepData, &caps) == HIDP_STATUS_SUCCESS) {
                                    if (caps.UsagePage == 0x01) {
                                        bool isPeripheral = false;
                                        std::wstring guessedType = L"other";

                                        if (caps.Usage == 0x04 || caps.Usage == 0x05 || caps.Usage == 0x08) { isPeripheral = true; guessedType = L"gamepad"; }
                                        else if (caps.Usage == 0x06) { isPeripheral = true; guessedType = L"keyboard"; }
                                        else if (caps.Usage == 0x02) { isPeripheral = true; guessedType = L"mouse"; }

                                        if (isPeripheral && uniqueDevices.find(deviceKey) == uniqueDevices.end()) {
                                            autoDev.type = guessedType;
                                            autoDev.battery = -2;
                                            autoDev.name = autoDev.name + L" (已连接)";
                                            foundDevices.push_back(autoDev);
                                            uniqueDevices.insert(deviceKey);
                                        }
                                    }
                                }
                                HidD_FreePreparsedData(prepData);
                            }
                        }
                    }
                }
                CloseHandle(hDevice);
            }
        }
        free(detailData);
    }
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return foundDevices;
}

HICON CreateCustomIcon(int battery = -1, bool isCharging = false) {
    std::lock_guard<std::mutex> lock(g_iconMutex);
    Bitmap bitmap(32, 32, PixelFormat32bppARGB);
    Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias); 
    
    Color bgColor;
    if (battery == -1 || battery == -2 || isCharging || battery >= 50) bgColor = Color(255, 52, 199, 89); 
    else if (battery >= 20) bgColor = Color(255, 255, 179, 0); 
    else bgColor = Color(255, 255, 71, 87); 

    SolidBrush bgBrush(bgColor); 
    GraphicsPath path;
    int radius = 8; 
    path.AddArc(0, 0, radius, radius, 180, 90);
    path.AddArc(32 - radius, 0, radius, radius, 270, 90);
    path.AddArc(32 - radius, 32 - radius, radius, radius, 0, 90);
    path.AddArc(0, 32 - radius, radius, radius, 90, 90);
    path.CloseFigure();
    graphics.FillPath(&bgBrush, &path);

    if (battery == -1 || battery == -2) {
        Pen batteryPen(Color(255, 255, 255, 255), 2.0f);
        graphics.DrawRectangle(&batteryPen, 8, 12, 16, 10); 
        SolidBrush capBrush(Color(255, 255, 255, 255));
        graphics.FillRectangle(&capBrush, 24, 15, 2, 4); 
        graphics.FillRectangle(&capBrush, 10, 14, 12, 6); 
    } else {
        SolidBrush textBrush(Color(255, 255, 255, 255));
        if (isCharging) {
             Point points[] = { Point(20, 6), Point(14, 18), Point(18, 18), Point(12, 28), Point(22, 16), Point(18, 16) };
             graphics.FillPolygon(&textBrush, points, 6);
        } else {
             std::wstring text = std::to_wstring(battery);
             FontFamily fontFamily(L"Microsoft YaHei");
             float fontSize = 18.0f; 
             if (battery == 100) fontSize = 15.0f; 
             else if (battery < 10) fontSize = 22.0f;
             Font font(&fontFamily, fontSize, FontStyleBold, UnitPixel);
             StringFormat format;
             format.SetAlignment(StringAlignmentCenter);
             format.SetLineAlignment(StringAlignmentCenter);
             format.SetFormatFlags(StringFormatFlagsNoWrap); 
             RectF rect(-10, -1, 52, 32); 
             graphics.DrawString(text.c_str(), -1, &font, rect, &format, &textBrush);
        }
    }
    HICON hIcon = NULL;
    bitmap.GetHICON(&hIcon); 
    return hIcon;
}

void CheckLowBattery(const std::vector<DeviceData>& devices, int lowBatt, bool& warned, NOTIFYICONDATAW& nid) {
    if (lowBatt <= 0) return;
    bool needWarn = false; std::wstring lowestName; int lowestPct = 101;
    for (const auto& d : devices) {
        if (d.battery >= 0 && d.battery <= lowBatt) {
            needWarn = true;
            if (d.battery < lowestPct) { lowestPct = d.battery; lowestName = d.name; }
        }
    }
    if (needWarn && !warned) {
        nid.uFlags |= NIF_INFO; wcscpy_s(nid.szInfoTitle, L"低电量警告");
        std::wstring msg = lowestName + L" 电量仅剩 " + std::to_wstring(lowestPct) + L"%！";
        wcscpy_s(nid.szInfo, msg.c_str()); nid.dwInfoFlags = NIIF_WARNING;
        Shell_NotifyIconW(NIM_MODIFY, &nid); nid.uFlags &= ~NIF_INFO; warned = true;
    } else if (!needWarn) {
        bool allHigh = true;
        for (const auto& d : devices) if (d.battery >= 0 && d.battery <= lowBatt + 5) allHigh = false;
        if (allHigh) warned = false;
    }
}

void TriggerAsyncScan(HWND hwnd) {
    if (g_isScanning.load()) return; 
    g_isScanning.store(true);

    std::thread([hwnd]() {
        auto devices = ScanAllDevices();
        int lowBatt = GetPrivateProfileIntW(L"Settings", L"LowBatteryThreshold", 10, g_iniPath.c_str());
        CheckLowBattery(devices, lowBatt, g_batteryWarned, g_nid);

        {
            std::lock_guard<std::mutex> lock(g_pinnedMutex);
            for (auto& pair : g_pinnedIcons) {
                const std::wstring& devName = pair.first;
                NOTIFYICONDATAW& pinNid = pair.second;
                bool found = false;
                for (const auto& dev : devices) {
                    if (dev.name.find(devName) != std::wstring::npos || devName.find(dev.name) != std::wstring::npos) {
                        int prevBattery = g_lastBatteryState[devName];
                        if (prevBattery != dev.battery || pinNid.hIcon == nullptr) {
                            if (pinNid.hIcon) DestroyIcon(pinNid.hIcon);
                            pinNid.hIcon = CreateCustomIcon(dev.battery, dev.isCharging);
                            wcscpy_s(pinNid.szTip, devName.c_str());
                            pinNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; 
                            Shell_NotifyIconW(NIM_MODIFY, &pinNid);
                            g_lastBatteryState[devName] = dev.battery;
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (g_lastBatteryState[devName] != -1 || pinNid.hIcon == nullptr) {
                        if (pinNid.hIcon) DestroyIcon(pinNid.hIcon);
                        pinNid.hIcon = CreateCustomIcon(-1, false);
                        wcscpy_s(pinNid.szTip, (devName + L" (未连接)").c_str());
                        pinNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; 
                        Shell_NotifyIconW(NIM_MODIFY, &pinNid);
                        g_lastBatteryState[devName] = -1;
                    }
                }
            }
        }

        std::wstring json = L"{\"devices\": [";
        for (size_t i = 0; i < devices.size(); i++) {
            if (i > 0) json += L",";
            std::wstring safeName = EscapeJsonString(devices[i].name);
            std::wstring safeType = EscapeJsonString(devices[i].type);
            std::wstring protocol = (devices[i].name.find(L"蓝牙") != std::wstring::npos || devices[i].name.find(L"Bluetooth") != std::wstring::npos) ? L"蓝牙连接" : L"2.4G 无线";
            json += L"{\"name\": \"" + safeName + L"\", \"battery\": " + std::to_wstring(devices[i].battery) + 
                    L", \"type\": \"" + safeType + L"\", \"isCharging\": " + (devices[i].isCharging ? L"true" : L"false") + 
                    L", \"protocol\": \"" + protocol + L"\"}"; 
        }
        json += L"]}";

        { std::lock_guard<std::mutex> lock(g_json_mutex); g_latest_json = json; }
        PostMessageW(hwnd, WM_APP_SCAN_DONE, 0, 0);
        g_isScanning.store(false);
    }).detach();
}

class MenuWebMessageHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2WebMessageReceivedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR rawMsg = nullptr; args->TryGetWebMessageAsString(&rawMsg);
        if (rawMsg) {
            std::wstring msg(rawMsg);
            if (msg == L"exit-app") { DestroyWindow(g_hMainWnd); PostQuitMessage(0); }
            else if (msg == L"show-main") { ShowWindow(g_hMenuWnd, SW_HIDE); ShowWindow(g_hMainWnd, SW_SHOW); SetForegroundWindow(g_hMainWnd); }
            else if (msg.find(L"toggle_startup:") == 0) {
                bool enable = (msg.substr(15) == L"1"); SetAutoStart(enable);
                if (g_webView) { std::wstring toggleJson = std::wstring(L"{\"startup\": ") + (enable ? L"true" : L"false") + L"}"; g_webView->PostWebMessageAsJson(toggleJson.c_str()); }
            }
            else if (msg == L"menu-ready") {
                if (g_menuWebView) {
                    std::wstring initJson = std::wstring(L"{\"action\": \"setTheme\", \"isDark\": ") + (g_isDarkTheme ? L"true" : L"false") + L", \"startup\": " + (IsAutoStart() ? L"true" : L"false") + L"}";
                    g_menuWebView->PostWebMessageAsJson(initJson.c_str());
                }
            }
            CoTaskMemFree(rawMsg);
        }
        return S_OK;
    }
};

class MenuControllerCreatedHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HWND hwnd;
    MenuControllerCreatedHandler(HWND h) : hwnd(h) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        if (FAILED(result) || !controller) { g_isMenuCreating = false; return result; }
        g_menuController = controller;
        g_menuController->get_CoreWebView2(g_menuWebView.GetAddressOf());
        RECT bounds = { 0, 0, 180, 180 }; g_menuController->put_Bounds(bounds);
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(g_menuController.As(&controller2))) controller2->put_DefaultBackgroundColor({ 0, 0, 0, 0 }); 
        EventRegistrationToken token; auto msgHandler = Make<MenuWebMessageHandler>(); g_menuWebView->add_WebMessageReceived(msgHandler.Get(), &token);
        wchar_t exePath[MAX_PATH]; GetModuleFileNameW(NULL, exePath, MAX_PATH); std::wstring exeDir(exePath); exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/")); 
        std::wstring htmlPath = L"file:///" + exeDir + L"/ui/tray.html"; for (auto& c : htmlPath) { if (c == L'\\') c = L'/'; }
        g_menuWebView->Navigate(htmlPath.c_str());
        g_menuWebViewReady = true; g_isMenuCreating = false;
        return S_OK;
    }
};

class TooltipWebMessageHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2WebMessageReceivedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR rawMsg = nullptr; args->TryGetWebMessageAsString(&rawMsg);
        if (rawMsg) {
            std::wstring msg(rawMsg);
            if (msg == L"tooltip-ready" && g_tooltipWebView) {
                std::wstring themeJson = L"{\"action\": \"setTheme\", \"isDark\": " + std::wstring(g_isDarkTheme ? L"true" : L"false") + L"}";
                g_tooltipWebView->PostWebMessageAsJson(themeJson.c_str());
                std::wstring json_copy; { std::lock_guard<std::mutex> lock(g_json_mutex); json_copy = g_latest_json; }
                if (!json_copy.empty()) g_tooltipWebView->PostWebMessageAsJson(json_copy.c_str());
            }
            CoTaskMemFree(rawMsg);
        }
        return S_OK;
    }
};

class TooltipControllerCreatedHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HWND hwnd;
    TooltipControllerCreatedHandler(HWND h) : hwnd(h) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        if (FAILED(result) || !controller) { g_isTooltipCreating = false; return result; }
        g_tooltipController = controller;
        g_tooltipController->get_CoreWebView2(g_tooltipWebView.GetAddressOf());
        RECT bounds = { 0, 0, 240, 180 }; g_tooltipController->put_Bounds(bounds);
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(g_tooltipController.As(&controller2))) controller2->put_DefaultBackgroundColor({ 0, 0, 0, 0 });
        EventRegistrationToken token; auto msgHandler = Make<TooltipWebMessageHandler>(); g_tooltipWebView->add_WebMessageReceived(msgHandler.Get(), &token);
        wchar_t exePath[MAX_PATH]; GetModuleFileNameW(NULL, exePath, MAX_PATH); std::wstring exeDir(exePath); exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/")); 
        std::wstring htmlPath = L"file:///" + exeDir + L"/ui/tooltip.html"; for (auto& c : htmlPath) { if (c == L'\\') c = L'/'; }
        g_tooltipWebView->Navigate(htmlPath.c_str());
        g_tooltipWebViewReady = true; g_isTooltipCreating = false;
        return S_OK;
    }
};

class WebMessageReceivedHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2WebMessageReceivedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) override {
        LPWSTR rawMsg = nullptr; args->TryGetWebMessageAsString(&rawMsg);
        if (rawMsg) {
            std::wstring msg(rawMsg);
            if (msg == L"ready" || msg == L"refresh") TriggerAsyncScan(g_hMainWnd); 
            else if (msg == L"close-window") ShowWindow(g_hMainWnd, SW_HIDE);
            else if (msg == L"exit-app") { DestroyWindow(g_hMainWnd); PostQuitMessage(0); }
            else if (msg == L"drag-window") { ReleaseCapture(); SendMessage(g_hMainWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
            else if (msg.find(L"hw_accel:") == 0) {
                WritePrivateProfileStringW(L"Settings", L"HardwareAcceleration", msg.substr(9).c_str(), g_iniPath.c_str());
                std::wstring toastJson = L"{\"action\": \"showToast\", \"message\": \"设置已保存，重启软件后生效！\"}";
                g_webView->PostWebMessageAsJson(toastJson.c_str());
            }
            else if (msg.find(L"low_batt:") == 0) WritePrivateProfileStringW(L"Settings", L"LowBatteryThreshold", msg.substr(9).c_str(), g_iniPath.c_str());
            else if (msg.find(L"set_interval:") == 0) {
                int sec = std::stoi(msg.substr(13));
                if (sec > 0) { SetTimer(g_hMainWnd, 1, sec * 1000, NULL); WritePrivateProfileStringW(L"Settings", L"ScanInterval", std::to_wstring(sec).c_str(), g_iniPath.c_str()); }
            }
            else if (msg.find(L"theme_changed:") == 0) {
                g_isDarkTheme = (msg.substr(14) == L"dark");
                std::wstring themeJson = L"{\"action\": \"setTheme\", \"isDark\": " + std::wstring(g_isDarkTheme ? L"true" : L"false") + L"}";
                if (g_menuWebView) g_menuWebView->PostWebMessageAsJson(themeJson.c_str());
                if (g_tooltipWebView) g_tooltipWebView->PostWebMessageAsJson(themeJson.c_str());
            }
            else if (msg.find(L"toggle_startup:") == 0) {
                SetAutoStart(msg.substr(15) == L"1");
                if (g_menuWebView) { std::wstring toggleJson = std::wstring(L"{\"startup\": ") + (msg.substr(15) == L"1" ? L"true" : L"false") + L"}"; g_menuWebView->PostWebMessageAsJson(toggleJson.c_str()); }
            }
            else if (msg.find(L"set_close_directly:") == 0) {
                g_closeDirectly = (msg.substr(19) == L"1");
                WritePrivateProfileStringW(L"Settings", L"CloseDirectly", g_closeDirectly ? L"1" : L"0", g_iniPath.c_str());
            }
            else if (msg.find(L"set_tray_tip:") == 0) {
                g_showTrayTip = (msg.substr(13) == L"1");
                WritePrivateProfileStringW(L"Settings", L"ShowTrayTip", g_showTrayTip ? L"1" : L"0", g_iniPath.c_str());
            }
            else if (msg.find(L"save_custom:") == 0) {
                std::wstring json = msg.substr(12);
                auto extract = [](const std::wstring& str, const std::wstring& key) {
                    size_t pos = str.find(L"\"" + key + L"\":");
                    if (pos == std::wstring::npos) return std::wstring();
                    pos += key.length() + 3;
                    if (str[pos] == L'\"') pos++;
                    size_t end = str.find_first_of(L"\",}", pos);
                    return str.substr(pos, end - pos);
                };

                std::wstring vid = extract(json, L"vid");
                std::wstring pid = extract(json, L"pid");
                std::wstring name = extract(json, L"name");
                std::wstring type = extract(json, L"type");
                std::wstring offset = extract(json, L"offset");

                if (!vid.empty() && !pid.empty()) {
                    std::wstring section = name.empty() ? L"CustomDevice_" + vid + pid : name;
                    WritePrivateProfileStringW(section.c_str(), L"Vid", vid.c_str(), g_devicesIniPath.c_str());
                    WritePrivateProfileStringW(section.c_str(), L"Pid", pid.c_str(), g_devicesIniPath.c_str());
                    WritePrivateProfileStringW(section.c_str(), L"Name", name.c_str(), g_devicesIniPath.c_str());
                    WritePrivateProfileStringW(section.c_str(), L"Type", type.c_str(), g_devicesIniPath.c_str());
                    WritePrivateProfileStringW(section.c_str(), L"Offset", offset.c_str(), g_devicesIniPath.c_str());
                    WritePrivateProfileStringW(section.c_str(), L"Mask", L"255", g_devicesIniPath.c_str());
                    WritePrivateProfileStringW(section.c_str(), L"Request", L"", g_devicesIniPath.c_str());
                }
            }
            // 👈 接收前端抓包指令 (全新 tshark 一步到位版)
            else if (msg.find(L"start_capture:") == 0) {
                if (g_captureActive.load()) return S_OK; 
                std::wstring type = msg.substr(14);
                g_captureActive.store(true);

                std::thread([type]() {
                    auto SendResult = [&](const std::wstring& hexStr) {
                        std::wstring json = L"{\"action\": \"capture_result\", \"type\": \"" + type + L"\", \"hex\": \"" + hexStr + L"\"}";
                        if (g_webView) g_webView->PostWebMessageAsJson(json.c_str());
                        g_captureActive.store(false);
                    };

                    std::wstring tsPath = L"C:\\Program Files\\Wireshark\\tshark.exe";
                    if (GetFileAttributesW(tsPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        tsPath = L"C:\\Program Files (x86)\\Wireshark\\tshark.exe";
                        if (GetFileAttributesW(tsPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                            SendResult(L"ERROR_NO_TSHARK"); return;
                        }
                    }

                    std::wstring tempFile = L"C:\\Windows\\Temp\\battery_capture.pcap";
                    DeleteFileW(tempFile.c_str()); 

                    // 1. 直接使用 tshark 抓包，自带 3 秒超时，避免被强制关闭导致文件损坏
                    std::wstring cmdCap = L"\"" + tsPath + L"\" -i \\\\.\\USBPcap1 -a duration:3 -w \"" + tempFile + L"\"";
                    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi;
                    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
                    
                    if (CreateProcessW(NULL, const_cast<LPWSTR>(cmdCap.c_str()), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                        // 等待最多 4 秒，让 tshark 自然结束
                        WaitForSingleObject(pi.hProcess, 4000);
                        TerminateProcess(pi.hProcess, 0);
                        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                    } else {
                        SendResult(L"ERROR_START_CAP"); return;
                    }

                    if (!g_captureActive.load()) { SendResult(L""); return; }

                    if (GetFileAttributesW(tempFile.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        SendResult(L"ERROR_NO_PCAP_FILE"); return;
                    }

                    // 2. 解析文件：提取所有可能的数据段，不局限于 capdata
                    std::wstring cmdParse = L"\"" + tsPath + L"\" -r \"" + tempFile + L"\" -T fields -e usb.capdata -e usb.data_fragment -e usb.data";
                    std::wstring resultHex = L"";
                    
                    SECURITY_ATTRIBUTES saAttr = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
                    HANDLE hRead, hWrite;
                    if (CreatePipe(&hRead, &hWrite, &saAttr, 0)) {
                        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
                        STARTUPINFOW si2 = { sizeof(si2) };
                        si2.dwFlags = STARTF_USESTDHANDLES;
                        si2.hStdOutput = hWrite; si2.hStdError = hWrite;
                        PROCESS_INFORMATION pi2;
                        if (CreateProcessW(NULL, const_cast<LPWSTR>(cmdParse.c_str()), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si2, &pi2)) {
                            CloseHandle(hWrite);
                            char buffer[4096] = {0}; DWORD bytesRead;
                            std::string out;
                            while (ReadFile(hRead, buffer, sizeof(buffer)-1, &bytesRead, NULL) && bytesRead > 0) {
                                buffer[bytesRead] = '\0'; out += buffer;
                                if (!g_captureActive.load()) break; 
                            }
                            WaitForSingleObject(pi2.hProcess, 1000);
                            TerminateProcess(pi2.hProcess, 0);
                            CloseHandle(pi2.hProcess); CloseHandle(pi2.hThread);
                            
                            // 清理并提取第一行有效数据
                            std::string cleanHex = "";
                            for (char c : out) {
                                if (isxdigit(c)) {
                                    cleanHex += c;
                                } else if (c == '\n' || c == '\t') {
                                    if (cleanHex.length() >= 6) break; // 找到一行有效数据就停
                                    cleanHex.clear();
                                }
                            }
                            
                            // 格式化：每 2 个字符加个空格
                            for (size_t i = 0; i < cleanHex.length(); i += 2) {
                                resultHex += (wchar_t)cleanHex[i];
                                if (i+1 < cleanHex.length()) resultHex += (wchar_t)cleanHex[i+1];
                                resultHex += L' ';
                            }
                        }
                        CloseHandle(hRead);
                    }

                    if (!g_captureActive.load()) { SendResult(L""); return; }

                    if (resultHex.empty()) {
                        SendResult(L"ERROR_EMPTY_DATA"); // 没抓到任何数据
                    } else {
                        while (!resultHex.empty() && resultHex.back() == L' ') resultHex.pop_back();
                        SendResult(resultHex);
                    }
                }).detach();
            }
            else if (msg.find(L"add_pin:") == 0) {
                std::wstring devName = msg.substr(8);
                {
                    std::lock_guard<std::mutex> lock(g_pinnedMutex);
                    if (g_pinnedIcons.find(devName) == g_pinnedIcons.end()) {
                        NOTIFYICONDATAW pinNid = {};
                        pinNid.cbSize = sizeof(NOTIFYICONDATAW);
                        pinNid.hWnd = g_hTrayWnd;
                        pinNid.uID = g_nextIconId++;
                        pinNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                        pinNid.uCallbackMessage = WM_TRAYICON;
                        pinNid.uVersion = NOTIFYICON_VERSION_4;
                        pinNid.hIcon = CreateCustomIcon(-1, false); 
                        wcscpy_s(pinNid.szTip, devName.c_str());
                        Shell_NotifyIconW(NIM_ADD, &pinNid);
                        Shell_NotifyIconW(NIM_SETVERSION, &pinNid);
                        g_pinnedIcons[devName] = pinNid;
                        g_lastBatteryState[devName] = -99;
                    }
                }
                TriggerAsyncScan(g_hMainWnd); 
            }
            else if (msg.find(L"remove_pin:") == 0) {
                std::wstring devName = msg.substr(11);
                std::lock_guard<std::mutex> lock(g_pinnedMutex);
                auto it = g_pinnedIcons.find(devName);
                if (it != g_pinnedIcons.end()) {
                    Shell_NotifyIconW(NIM_DELETE, &it->second);
                    if (it->second.hIcon) DestroyIcon(it->second.hIcon);
                    g_pinnedIcons.erase(it);
                    g_lastBatteryState.erase(devName);
                }
            }
            CoTaskMemFree(rawMsg);
        }
        return S_OK;
    }
};

class ControllerCreatedHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
public:
    HWND hwnd;
    ControllerCreatedHandler(HWND h) : hwnd(h) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller* controller) override {
        if (FAILED(result) || !controller) return result;
        g_controller = controller; g_controller->get_CoreWebView2(g_webView.GetAddressOf());
        RECT bounds; GetClientRect(hwnd, &bounds); g_controller->put_Bounds(bounds);
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(g_controller.As(&controller2))) controller2->put_DefaultBackgroundColor({ 255, 245, 245, 240 });

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(g_webView->get_Settings(&settings))) settings->put_AreDefaultContextMenusEnabled(FALSE);
        EventRegistrationToken token; auto msgHandler = Make<WebMessageReceivedHandler>(); g_webView->add_WebMessageReceived(msgHandler.Get(), &token);
        wchar_t exePath[MAX_PATH]; GetModuleFileNameW(NULL, exePath, MAX_PATH); std::wstring exeDir(exePath); exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/")); 
        std::wstring htmlPath = L"file:///" + exeDir + L"/ui/index.html"; for (auto& c : htmlPath) { if (c == L'\\') c = L'/'; }
        g_webView->Navigate(htmlPath.c_str());
        
        std::wstring pinnedJson = L"[";
        bool first = true;
        {
            std::lock_guard<std::mutex> lock(g_pinnedMutex);
            for (auto& pair : g_pinnedIcons) {
                if (!first) pinnedJson += L",";
                pinnedJson += L"\"" + EscapeJsonString(pair.first) + L"\"";
                first = false;
            }
        }
        pinnedJson += L"]";

        std::wstring initJson = L"{\"hwAccel\": " + std::to_wstring(GetPrivateProfileIntW(L"Settings", L"HardwareAcceleration", 1, g_iniPath.c_str())) + 
            L", \"lowBatt\": " + std::to_wstring(GetPrivateProfileIntW(L"Settings", L"LowBatteryThreshold", 10, g_iniPath.c_str())) + 
            L", \"systemDark\": " + std::wstring(IsSystemDarkMode() ? L"true" : L"false") + 
            L", \"startup\": " + std::wstring(IsAutoStart() ? L"true" : L"false") +
            L", \"closeDirectly\": " + std::wstring(g_closeDirectly ? L"true" : L"false") + 
            L", \"trayTip\": " + std::wstring(g_showTrayTip ? L"true" : L"false") + 
            L", \"pinnedDevices\": " + pinnedJson + L"}";
        g_webView->PostWebMessageAsJson(initJson.c_str());
        return S_OK;
    }
};

class EnvironmentCreatedHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
public:
    HWND hwnd; EnvironmentCreatedHandler(HWND h) : hwnd(h) {}
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Environment* env) override {
        if (FAILED(result)) return result;
        g_webviewEnv = env; auto mainHandler = Make<ControllerCreatedHandler>(hwnd); env->CreateCoreWebView2Controller(hwnd, mainHandler.Get());
        return S_OK;
    }
};

LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_ERASEBKGND: return 1; 
    case WM_ACTIVATE: if (LOWORD(wParam) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE); break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_TRAYICON) {
        UINT uID = wParam; 
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt); int winW = 180, winH = 180; int winX = pt.x - winW + 20; int winY = pt.y - winH - 20;
            if (winX < 0) winX = 0; if (winY < 0) winY = pt.y + 20;
            if (!g_menuWebViewReady) {
                if (!g_isMenuCreating && g_webviewEnv) {
                    g_isMenuCreating = true; SetWindowPos(g_hMenuWnd, HWND_TOPMOST, winX, winY, winW, winH, SWP_SHOWWINDOW);
                    auto menuHandler = Make<MenuControllerCreatedHandler>(g_hMenuWnd); g_webviewEnv->CreateCoreWebView2Controller(g_hMenuWnd, menuHandler.Get());
                }
                return 0;
            }
            SetForegroundWindow(g_hMenuWnd); SetWindowPos(g_hMenuWnd, HWND_TOPMOST, winX, winY, winW, winH, SWP_SHOWWINDOW);
            if (g_menuWebView) { 
                std::wstring startupJson = std::wstring(L"{\"startup\": ") + (IsAutoStart() ? L"true" : L"false") + L"}"; 
                g_menuWebView->PostWebMessageAsJson(startupJson.c_str()); 
            }
            return 0; 
        } 
        else if (LOWORD(lParam) == WM_LBUTTONUP) { 
            ShowWindow(g_hMainWnd, SW_SHOW); 
            SetForegroundWindow(g_hMainWnd); 
            return 0; 
        }
        else if (LOWORD(lParam) == NIN_POPUPOPEN) {
            if (!g_showTrayTip) return 0;
            POINT pt; GetCursorPos(&pt); int winW = 240, winH = 180; int winX = pt.x - (winW / 2); int winY = pt.y - winH - 10; 
            if (winX < 0) winX = 0; if (winY < 0) winY = pt.y + 20;
            if (!g_tooltipWebViewReady) {
                if (!g_isTooltipCreating && g_webviewEnv) {
                    g_isTooltipCreating = true; SetWindowPos(g_hTooltipWnd, HWND_TOPMOST, winX, winY, winW, winH, SWP_SHOWWINDOW);
                    auto tooltipHandler = Make<TooltipControllerCreatedHandler>(g_hTooltipWnd); g_webviewEnv->CreateCoreWebView2Controller(g_hTooltipWnd, tooltipHandler.Get());
                }
                return 0;
            }
            SetWindowPos(g_hTooltipWnd, HWND_TOPMOST, winX, winY, winW, winH, SWP_SHOWWINDOW | SWP_NOACTIVATE); 
            ShowWindow(g_hTooltipWnd, SW_SHOW);
            if (g_tooltipWebView) { 
                std::wstring json_copy; 
                { std::lock_guard<std::mutex> lock(g_json_mutex); json_copy = g_latest_json; } 
                if (!json_copy.empty()) g_tooltipWebView->PostWebMessageAsJson(json_copy.c_str()); 
            }
            return 0;
        }
        else if (LOWORD(lParam) == NIN_POPUPCLOSE) { 
            if (g_hTooltipWnd) ShowWindow(g_hTooltipWnd, SW_HIDE); 
            return 0; 
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_SIZE: if (g_controller) { RECT bounds; GetClientRect(hwnd, &bounds); g_controller->put_Bounds(bounds); } break;
    case WM_NCCALCSIZE: if (wParam == TRUE) return 0; break;
    case WM_GETMINMAXINFO: { MINMAXINFO* mmi = (MINMAXINFO*)lParam; mmi->ptMinTrackSize.x = 600; mmi->ptMinTrackSize.y = 450; } break;
    case WM_TIMER: if (wParam == 1) { TriggerAsyncScan(hwnd); } break;
    case WM_APP_SCAN_DONE: if (g_webView) { std::wstring json_copy; { std::lock_guard<std::mutex> lock(g_json_mutex); json_copy = g_latest_json; } if (!json_copy.empty()) g_webView->PostWebMessageAsJson(json_copy.c_str()); } break;
    case WM_CLOSE: if (g_closeDirectly) DestroyWindow(hwnd); else ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_DESTROY: PostQuitMessage(0); break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"BatteryMonitor_SingleInstance_Mutex");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hExistingWnd = FindWindowW(L"BatteryMonitorMain", nullptr);
        if (hExistingWnd) { ShowWindow(hExistingWnd, SW_SHOW); SetForegroundWindow(hExistingWnd); }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gdiplusStartupInput; GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    wchar_t exePath[MAX_PATH]; GetModuleFileNameW(NULL, exePath, MAX_PATH); 
    std::wstring exeDir(exePath); exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));
    
    g_iniPath = exeDir + L"\\config.ini";
    g_devicesIniPath = exeDir + L"\\devices.ini";

    int hwAccel = GetPrivateProfileIntW(L"Settings", L"HardwareAcceleration", 1, g_iniPath.c_str());
    g_closeDirectly = GetPrivateProfileIntW(L"Settings", L"CloseDirectly", 0, g_iniPath.c_str()) != 0;
    g_showTrayTip = GetPrivateProfileIntW(L"Settings", L"ShowTrayTip", 1, g_iniPath.c_str()) != 0; 

    const wchar_t MENU_CLASS[] = L"TrayMenuWindow"; WNDCLASS wcMenu = {}; wcMenu.lpfnWndProc = MenuWndProc; wcMenu.hInstance = hInstance; wcMenu.lpszClassName = MENU_CLASS; RegisterClass(&wcMenu);
    g_hMenuWnd = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, MENU_CLASS, L"Menu", WS_POPUP, 0, 0, 180, 180, nullptr, nullptr, hInstance, nullptr);

    const wchar_t TOOLTIP_CLASS[] = L"TrayTooltipWindow"; WNDCLASS wcTooltip = {}; wcTooltip.lpfnWndProc = MenuWndProc; wcTooltip.hInstance = hInstance; wcTooltip.lpszClassName = TOOLTIP_CLASS; RegisterClass(&wcTooltip);
    g_hTooltipWnd = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, TOOLTIP_CLASS, L"Tooltip", WS_POPUP, 0, 0, 240, 180, nullptr, nullptr, hInstance, nullptr);

    const wchar_t TRAY_CLASS[] = L"TrayWindow"; WNDCLASS wcTray = {}; wcTray.lpfnWndProc = TrayWndProc; wcTray.hInstance = hInstance; wcTray.lpszClassName = TRAY_CLASS; RegisterClass(&wcTray);
    g_hTrayWnd = CreateWindowEx(0, TRAY_CLASS, L"Tray", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    g_hCurrentTrayIcon = CreateCustomIcon(-1, false); 
    HICON hMainIcon = CreateCustomIcon(-1, false); 
    
    g_nid.cbSize = sizeof(NOTIFYICONDATAW); 
    g_nid.hWnd = g_hTrayWnd; g_nid.uID = ID_TRAY_APP_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; 
    g_nid.uCallbackMessage = WM_TRAYICON; 
    g_nid.hIcon = g_hCurrentTrayIcon; 
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    wcscpy_s(g_nid.szTip, L"设备电量监控"); 
    
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

    const wchar_t CLASS_NAME[] = L"BatteryMonitorMain"; WNDCLASS wc = {}; wc.lpfnWndProc = MainWndProc; wc.hInstance = hInstance; wc.lpszClassName = CLASS_NAME; wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH); RegisterClass(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN); int screenH = GetSystemMetrics(SM_CYSCREEN);
    int winW = 800; int winH = 620; int winX = (screenW - winW) / 2; int winY = (screenH - winH) / 2;
    g_hMainWnd = CreateWindowEx(WS_EX_APPWINDOW, CLASS_NAME, L"设备电量", WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX, winX, winY, winW, winH, nullptr, nullptr, hInstance, nullptr);

    SendMessage(g_hMainWnd, WM_SETICON, ICON_BIG, (LPARAM)hMainIcon);
    SendMessage(g_hMainWnd, WM_SETICON, ICON_SMALL, (LPARAM)hMainIcon);

    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hMainWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
    DwmSetWindowAttribute(g_hMenuWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));

    ComPtr<ICoreWebView2EnvironmentOptions> options = Make<CoreWebView2EnvironmentOptions>();
    if (options) options->put_AdditionalBrowserArguments(hwAccel == 0 ? L"--disable-gpu" : L"");
    
    wchar_t appDataPath[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", appDataPath, MAX_PATH);
    std::wstring userDataFolder = std::wstring(appDataPath) + L"\\BatteryMonitor";
    
    auto envHandler = Make<EnvironmentCreatedHandler>(g_hMainWnd);
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), options.Get(), envHandler.Get());
    
    ShowWindow(g_hMainWnd, SW_SHOW);
    UpdateWindow(g_hMainWnd);

    int scanInterval = GetPrivateProfileIntW(L"Settings", L"ScanInterval", 15, g_iniPath.c_str());
    if (scanInterval <= 0) scanInterval = 15;
    SetTimer(g_hMainWnd, 1, scanInterval * 1000, NULL);

    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    Shell_NotifyIconW(NIM_DELETE, &g_nid); 
    for (auto& pair : g_pinnedIcons) {
        Shell_NotifyIconW(NIM_DELETE, &pair.second);
        if (pair.second.hIcon) DestroyIcon(pair.second.hIcon);
    }
    if (g_hCurrentTrayIcon) DestroyIcon(g_hCurrentTrayIcon); if (hMainIcon) DestroyIcon(hMainIcon);
    GdiplusShutdown(g_gdiplusToken); CoUninitialize();

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}