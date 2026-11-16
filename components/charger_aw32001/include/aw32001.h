#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif


class Aw32001 {
public:
    Aw32001(i2c_master_bus_handle_t i2c_bus, uint8_t addr);
    bool SetIntWakeupTime(uint8_t sel);
    bool SetWatchdog(uint16_t timeout_s, uint8_t enable_discharge_watchdog);
    bool ResetWatchdog();
    bool SetCharge(bool en);
    bool SetChargeCurrent(uint16_t current_ma);
    bool SetDischargeCurrent(uint16_t current_ma);
    bool SetChargeVoltage(uint16_t voltage_mv);
    bool SetShippingMode(bool en);
    uint8_t GetChargeState();
    bool ReadStatus(uint8_t& status, uint8_t& faults);
    bool SetPreChargeCurrent(uint16_t current_ma);
    bool SetPrechargeToFastchargeThreshold(uint8_t sel);
    bool SetNtcFunction(bool en);
    void DumpRegs(uint8_t start, uint8_t end);
private:
    i2c_master_dev_handle_t i2c_dev_handle;
    bool ReadReg(uint8_t reg, uint8_t& value);
    bool UpdateReg(uint8_t reg, uint8_t mask, uint8_t value);
    bool WriteReg(uint8_t reg, uint8_t value);
};

#ifdef __cplusplus
}
#endif
