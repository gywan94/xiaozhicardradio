#include "aw32001.h"
#include "esp_log.h"
#define I2C_MASTER_TIMEOUT_MS 200
static const char* TAG="AW32001";
Aw32001::Aw32001(i2c_master_bus_handle_t bus,uint8_t addr) {
    i2c_device_config_t cfg={};cfg.dev_addr_length=I2C_ADDR_BIT_LEN_7;cfg.device_address=addr;cfg.scl_speed_hz=400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&cfg,&i2c_dev_handle));
}
bool Aw32001::ReadReg(uint8_t reg,uint8_t& value) {
    value=0;
    auto err=i2c_master_transmit_receive(i2c_dev_handle,&reg,1,&value,1,I2C_MASTER_TIMEOUT_MS);
    if(err!=ESP_OK)ESP_LOGE(TAG,"Read %02x: %s",reg,esp_err_to_name(err));
    return err==ESP_OK;
}
bool Aw32001::WriteReg(uint8_t reg,uint8_t value) {
    uint8_t bytes[]={reg,value};auto err=i2c_master_transmit(i2c_dev_handle,bytes,2,I2C_MASTER_TIMEOUT_MS);
    if(err!=ESP_OK)ESP_LOGE(TAG,"Write %02x: %s",reg,esp_err_to_name(err));
    return err==ESP_OK;
}
bool Aw32001::UpdateReg(uint8_t reg,uint8_t mask,uint8_t value) {
    uint8_t old=0,actual=0;
    if(!ReadReg(reg,old)||!WriteReg(reg,(old&~mask)|(value&mask))||!ReadReg(reg,actual))return false;
    if((actual&mask)!=(value&mask)){ESP_LOGE(TAG,"Register %02x verification failed",reg);return false;}
    return true;
}
bool Aw32001::SetIntWakeupTime(uint8_t sel){return UpdateReg(0x22,0x08,sel?0x08:0);}
bool Aw32001::SetWatchdog(uint16_t seconds,uint8_t discharge){
    uint8_t bits=0;
    switch(seconds){case 0:break;case 40:bits=1;break;case 80:bits=2;break;case 160:bits=3;break;default:return false;}
    return UpdateReg(0x05,0xe0,(discharge?0x80:0)|(bits<<5));
}
bool Aw32001::ResetWatchdog(){uint8_t value=0;return ReadReg(0x02,value)&&WriteReg(0x02,value|0x40);}
bool Aw32001::SetCharge(bool en){return UpdateReg(0x01,0x0f,en?0x07:0x0f);}
bool Aw32001::SetChargeCurrent(uint16_t ma){if(ma>456)ma=456;return UpdateReg(0x02,0x3f,ma/8);}
bool Aw32001::SetDischargeCurrent(uint16_t ma){if(ma<200)ma=200;if(ma>3200)ma=3200;return UpdateReg(0x03,0xf0,(ma/200-1)<<4);}
bool Aw32001::SetChargeVoltage(uint16_t mv){if(mv<3600)mv=3600;if(mv>4545)mv=4545;return UpdateReg(0x04,0xfc,((mv-3600)/15)<<2);}
bool Aw32001::SetPreChargeCurrent(uint16_t ma){if(ma<1)ma=1;if(ma>31)ma=31;return UpdateReg(0x0b,0x3f,0x20|((ma/2)<<1));}
bool Aw32001::SetPrechargeToFastchargeThreshold(uint8_t sel){return sel<=1&&UpdateReg(0x04,0x03,sel?2:0);}
bool Aw32001::SetNtcFunction(bool en){return UpdateReg(0x06,0x80,en?0x80:0);}
bool Aw32001::SetShippingMode(bool en){
    // INT wake remains enabled; preserve unrelated control bits.
    if(!UpdateReg(0x0c,0x04,0))return false;
    if(en&&!UpdateReg(0x09,0xc0,0))return false;
    uint8_t value=0;
    if(!ReadReg(0x06,value))return false;
    // Power may disappear before a readback is possible.
    return WriteReg(0x06,en?(value|0x20):(value&~0x20));
}
uint8_t Aw32001::GetChargeState(){uint8_t value=0;return ReadReg(0x08,value)?((value>>3)&3):0;}
bool Aw32001::ReadStatus(uint8_t& status,uint8_t& faults){return ReadReg(0x08,status)&&ReadReg(0x09,faults);}
void Aw32001::DumpRegs(uint8_t start,uint8_t end){if(start>end||end>0x0c)return;for(unsigned reg=start;reg<=end;reg++){uint8_t value=0;if(ReadReg(reg,value))ESP_LOGI(TAG,"Reg %02x=%02x",reg,value);}}
