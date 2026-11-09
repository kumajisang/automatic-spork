#pragma once
#include "IDevice.h" // 👈 直接引用，不再需要 ../
#include <hidsdi.h>

extern bool ReadHidWithTimeoutHelper(HANDLE hDevice, uint8_t* buffer, DWORD bufferSize, DWORD timeoutMs, DWORD& bytesRead);

class LogitechHIDPP : public IDeviceDriver {
public:
    unsigned short getVid() const override { return 0x046D; }
    unsigned short getPid() const override { return 0x0000; }
    
    std::wstring getDeviceType() const override { return L"mouse"; }
    std::wstring getFallbackName() const override { return L"罗技无线设备"; }

    bool readBattery(HANDLE hDevice, DeviceData& outData) override {
        uint8_t buffer[20] = {0};
        DWORD written = 0, bytesRead = 0;

        buffer[0] = 0x10; buffer[1] = 0xFF; buffer[2] = 0x07; 
        if (WriteFile(hDevice, buffer, 7, &written, NULL)) {
            Sleep(15);
            if (ReadHidWithTimeoutHelper(hDevice, buffer, sizeof(buffer), 50, bytesRead) && bytesRead >= 7) {
                if (buffer[0] == 0x10 && buffer[2] == 0x07 && buffer[3] == 0x00) {
                    outData.battery = buffer[4];
                    int status = buffer[5];
                    outData.isCharging = (status == 1 || status == 2 || status == 4);
                    if (outData.battery >= 0 && outData.battery <= 100) return true;
                }
            }
        }

        buffer[0] = 0x10; buffer[1] = 0xFF; buffer[2] = 0x00; buffer[3] = 0x00; buffer[4] = 0x10; buffer[5] = 0x00;
        if (WriteFile(hDevice, buffer, 7, &written, NULL)) {
            Sleep(15);
            if (ReadHidWithTimeoutHelper(hDevice, buffer, sizeof(buffer), 50, bytesRead) && bytesRead >= 7) {
                uint8_t featureIndex = buffer[4];
                if (featureIndex > 0 && featureIndex != 0xFF) {
                    buffer[0] = 0x10; buffer[1] = 0xFF; buffer[2] = featureIndex; buffer[3] = 0x10; 
                    buffer[4] = 0x00; buffer[5] = 0x00; buffer[6] = 0x00;
                    if (WriteFile(hDevice, buffer, 7, &written, NULL)) {
                        Sleep(15);
                        if (ReadHidWithTimeoutHelper(hDevice, buffer, sizeof(buffer), 50, bytesRead) && bytesRead >= 7) {
                            outData.battery = buffer[4];
                            if (outData.battery > 100) outData.battery = buffer[5];
                            int status = buffer[5];
                            outData.isCharging = (status == 1 || status == 2 || status == 4);
                            if (outData.battery >= 0 && outData.battery <= 100) return true;
                        }
                    }
                }
            }
        }
        return false;
    }
};