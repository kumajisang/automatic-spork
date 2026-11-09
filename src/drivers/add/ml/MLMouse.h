#pragma once
#include "../../../IDevice.h"

class MLMouseDriver : public IDeviceDriver {
public:
    unsigned short getVid() const override { return 0xA8A5; }
    unsigned short getPid() const override { return 0x2255; }
    std::wstring getFallbackName() const override { return L"ML MX301"; }
    std::wstring getDeviceType() const override { return L"mouse"; }

    bool readBattery(HANDLE hDevice, DeviceData& outData) override;
};