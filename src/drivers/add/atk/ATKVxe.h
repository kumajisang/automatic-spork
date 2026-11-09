#pragma once
#include "IDevice.h" // 👈 直接引用
#include <hidsdi.h>

extern bool ReadHidWithTimeoutHelper(HANDLE hDevice, uint8_t* buffer, DWORD bufferSize, DWORD timeoutMs, DWORD& bytesRead);

class ATKVxe : public IDeviceDriver {
public:
    unsigned short getVid() const override { return 0x373B; }
    unsigned short getPid() const override { return 0x0000; }

    std::wstring getDeviceType() const override { return L"mouse"; }
    std::wstring getFallbackName() const override { return L"ATK/VXE 系列"; }

    bool readBattery(HANDLE hDevice, DeviceData& outData) override {
        HIDD_ATTRIBUTES attrs;
        attrs.Size = sizeof(attrs);
        if (HidD_GetAttributes(hDevice, &attrs)) {
            wchar_t productName[128] = {0};
            if (HidD_GetProductString(hDevice, productName, sizeof(productName))) {
                outData.name = productName;
            } else {
                if (attrs.ProductID == 0x1084 || attrs.ProductID == 0x1103) outData.name = L"ATK 烈空系列鼠标";
                else if (attrs.ProductID == 0x1042 || attrs.ProductID == 0x1044) outData.name = L"VXE 游戏鼠标";
                else if (attrs.ProductID == 0x1111) outData.name = L"Kysona 鼠标";
                else outData.name = getFallbackName();
            }
        } else outData.name = getFallbackName();
        
        outData.type = getDeviceType();

        uint8_t request[] = { 0x08, 0x7D, 0x72, 0x02, 0x00, 0x01, 0x07, 0x01 };
        DWORD written = 0, bytesRead = 0;
        
        if (WriteFile(hDevice, request, sizeof(request), &written, NULL)) {
            uint8_t response[64] = {0};
            Sleep(15);
            if (ReadHidWithTimeoutHelper(hDevice, response, sizeof(response), 50, bytesRead) && bytesRead >= 7) {
                outData.battery = response[6];
                outData.isCharging = false; 
                if (outData.battery >= 0 && outData.battery <= 100) return true;
            }
        }
        return false;
    }
};