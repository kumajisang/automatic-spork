#pragma once
#include "IDevice.h"
#include "add/ml/MLMouse.h"              // 你原有的
#include "add/logitech/LogitechHIDPP.h"  // 罗技
#include "add/atk/ATKVxe.h"              // 👈 ATK 也进文件夹了
#include <vector>
#include <memory>

class DriverRegistry {
public:
    static std::vector<std::shared_ptr<IDeviceDriver>> getDrivers() {
        static std::vector<std::shared_ptr<IDeviceDriver>> drivers = {
            std::make_shared<MLMouseDriver>(),
            std::make_shared<LogitechHIDPP>(),
            std::make_shared<ATKVxe>()         
        };
        return drivers;
    }
};