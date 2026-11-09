#pragma once
#include <windows.h>
#include <string>

// 1. 统一的数据结构
struct DeviceData {
    std::wstring name;
    int battery;          // 0-100, -1 表示休眠/离线
    std::wstring type;    // "mouse", "keyboard", "headset", "other"
    bool isCharging;
};

// 2. 统一的驱动接口 (类似于 Rust 的 trait)
class IDeviceDriver {
public:
    virtual ~IDeviceDriver() = default;

    // 获取 VID/PID
    virtual unsigned short getVid() const = 0;
    virtual unsigned short getPid() const = 0;
    
    // 获取兜底名称和分类
    virtual std::wstring getFallbackName() const = 0;
    virtual std::wstring getDeviceType() const = 0;

    // 核心读取函数
    virtual bool readBattery(HANDLE hDevice, DeviceData& outData) = 0;
};