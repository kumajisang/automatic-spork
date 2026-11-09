#include "MLMouse.h"
#include <hidsdi.h>
#include <vector>
#include <string>

#define LOG(msg) OutputDebugStringW((std::wstring(L"[ML] ") + msg + L"\n").c_str())

bool MLMouseDriver::readBattery(HANDLE hDevice, DeviceData& outData) {
    // 1. 接口拦截
    PHIDP_PREPARSED_DATA preparsedData;
    HIDP_CAPS caps;
    if (!HidD_GetPreparsedData(hDevice, &preparsedData)) return false;
    bool isVendorInterface = false;
    if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS) {
        if (caps.UsagePage == 0xFF00 || caps.UsagePage == 0xFF01) isVendorInterface = true;
    }
    HidD_FreePreparsedData(preparsedData);
    if (!isVendorInterface) return false; 

    // 2. 获取名字
    wchar_t productName[128] = {0};
    if (HidD_GetProductString(hDevice, productName, sizeof(productName))) outData.name = productName;
    else outData.name = getFallbackName();
    outData.type = getDeviceType();

    // 👈 核心修复：发指令前，先强行排空接收器的旧缓存！
    for (int i = 0; i < 10; i++) {
        BYTE dummy[65] = { 0 };
        DWORD dummyRead = 0;
        OVERLAPPED olDummy = { 0 };
        olDummy.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        ReadFile(hDevice, dummy, 65, &dummyRead, &olDummy);
        WaitForSingleObject(olDummy.hEvent, 1);
        CancelIoEx(hDevice, &olDummy);
        CloseHandle(olDummy.hEvent);
    }

    // 3. 发送查询指令 (55 30)
    BYTE packet[65] = { 0 };
    packet[0] = 0x00; 
    packet[1] = 0x55; 
    packet[2] = 0x30; 

    OVERLAPPED olWrite = {0};
    olWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    DWORD bytesWritten = 0;
    WriteFile(hDevice, packet, 65, &bytesWritten, &olWrite);
    WaitForSingleObject(olWrite.hEvent, 20);
    CloseHandle(olWrite.hEvent);
    
    Sleep(30);

    // 4. 重新读取最新回复
    for (int retry = 0; retry < 30; retry++) {
        OVERLAPPED olRead = {0};
        olRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        BYTE readBuffer[65] = {0};
        DWORD bytesRead = 0;
        
        ReadFile(hDevice, readBuffer, 65, &bytesRead, &olRead);
        
        if (WaitForSingleObject(olRead.hEvent, 10) == WAIT_OBJECT_0) {
            GetOverlappedResult(hDevice, &olRead, &bytesRead, FALSE);
            CloseHandle(olRead.hEvent);
            
            if (bytesRead >= 10) {
                for (DWORD i = 0; i < bytesRead - 10; ++i) {
                    if (readBuffer[i] == 0xAA && readBuffer[i + 1] == 0x30) {
                        
                        int level = readBuffer[i + 8]; 
                        int chargeCurrent = readBuffer[i + 3];
                        
                        if (level >= 0 && level <= 100) {
                            outData.battery = level;
                            // 如果不是合法的充电电流（比如关机脏数据），强制视为未充电
                            outData.isCharging = (chargeCurrent > 0 && chargeCurrent < 200);
                            return true;
                        }
                    }
                }
            }
        } else {
            CancelIoEx(hDevice, &olRead);
            WaitForSingleObject(olRead.hEvent, 5);
            CloseHandle(olRead.hEvent);
        }
    }
    
    // 如果没读到新数据，返回 false 让主程序显示休眠
    return false;
}