#include "dual_network_board.h"
#include "radio/radio_app.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <mutex>
#include <memory>
#include <esp_system.h>

#include "esp_app_desc.h"
#include <driver/i2c_master.h>
#include "driver/spi_common.h"
#include "driver/sdspi_host.h" 
#include "mcp_server.h"
#include "assets/lang_config.h"
#include "display/epd_display.h"
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "power_save_timer.h"
#include "font_emoji.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "led_strip.h"
#include "bq27220.h"
#include "aw32001.h"
#include "esp_epd_gdey027t91.h"
#include <esp_lvgl_port.h>
#include "esp_lcd_touch_ft5x06.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"


static const char *TAG = "XiaoZhi-Card Board";

LV_FONT_DECLARE(font_puhui_16_1);
LV_FONT_DECLARE(font_awesome_16_4);



enum class BoardEvent {
    Shutdown,
    Sleep,
    WakeUp,
    SwitchNetWork,
    ClearWiFiConfig,
};

const char* BoardEventToString(BoardEvent e) {
    switch (e) {
        case BoardEvent::Shutdown:       return "Shutdown";
        case BoardEvent::Sleep:          return "Sleep";
        case BoardEvent::WakeUp:         return "WakeUp";
        case BoardEvent::SwitchNetWork:  return "SwitchNetWork";
        case BoardEvent::ClearWiFiConfig:return "ClearWiFiConfig";
        default:                         return "Unknown";
    }
}

class MovingAverageFilter {
public:
    MovingAverageFilter(int size)
        : size_(size), buffer_(new float[size]), index_(0), count_(0), sum_(0), first_init_(true) 
    {
        memset(buffer_, 0, size * sizeof(float));
    }

    ~MovingAverageFilter() {
        delete[] buffer_;
    }

    float update(float value) {
        if (first_init_) {
            // 首次初始化直接填满 buffer
            for (int i = 0; i < size_; i++) buffer_[i] = value;
            sum_ = value * size_;
            count_ = size_;
            index_ = 0;
            first_init_ = false;
            return value;
        }

        // 移动平均更新
        sum_ -= buffer_[index_];
        buffer_[index_] = value;
        sum_ += value;

        index_ = (index_ + 1) % size_;
        if (count_ < size_) count_++;

        return sum_ / count_;
    }

private:
    int size_;
    float* buffer_;
    int index_;
    int count_;
    float sum_;
    bool first_init_;
};

class XiaozhiCardBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;        // I2C 
    Button user_button_;                     // 用户按键
    EpdDisplay *display_ = nullptr;          // 显示屏
    Aw32001 *charger_ = nullptr;             // 充电管理
    Bq27220 *guage_ = nullptr;               // 电量计
    led_strip_handle_t led_strip_ = nullptr; // 底座指示灯 

    // Display and touch handles
    esp_lcd_panel_io_handle_t panel_io_ = nullptr; 
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_lcd_touch_handle_t touch_ = nullptr;

    // Power management
    PowerSaveTimer *power_save_timer_ = nullptr;
    bool power_save_timer_user_set_ = false;

    // 事件处理 
    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t event_task_handle_ = nullptr; 

    // 状态记录
    NetworkType network_type_;      // 网络类型
    bool modem_powered_on_ = false; // 4G 模组状态
    bool sd_card_present_ = false;  // SD 卡是检测 

    // Private methods
    void InitializeI2c();            // I2C (AW32001，BQ27220，触摸屏)
    void InitializeSpi();            // SPI (显示屏，SD 卡)
    bool charger_ready_ = false;
    MovingAverageFilter battery_filter_{60};
    std::mutex power_sample_mutex_;
    void InitializeCharger();        // 充电 AW32001
    void InitializeGuage();          // 电量 BQ27220
    void InitializeStorage();        // 存储 
    void InitializeDisplay();        // 显示屏
    void InitializeButtons();        // 按键
    void InitializeIndicator();      // 底座指示灯   
    void InitializeTools();          // 
    void InitializePowerSaveTimer(); // 
    void StartUp();                  // 开机 
    void Sleep();                    // 休眠 esp32s3 lightsleep，es8311 sleep 
    void WakeUp();                   // 唤醒

    void PowerOnModem();
    void PowerOffModem();
    void Enable4G(void);
    void Disable4G(void);

    virtual Display *GetDisplay() override;

public:
    XiaozhiCardBoard();
    ~XiaozhiCardBoard();

    bool IsGuidePageRequired();                         // 是否显示引导页  
    void SetIndicator(uint8_t r, uint8_t g, uint8_t b); // 设置（底座）指示灯 
    void Shutdown();                                    // 关机
    void SetPowerSaveMode(bool en);                     // 省电模式设置
    bool GetPowerSaveMode();
    bool GetPowerStatus(BoardPowerStatus& status) override;
    bool PowerOff() override { return PostEvent(BoardEvent::Shutdown); }                            // 获取省电模式状态 

    /* 事件处理 */
    static void BoardEventTask(void* param);
    void StartBoardEventTask();
    bool PostEvent(const BoardEvent& event);
    void HandleBoardEvent(BoardEvent event); 

    // Display operations
    void ClearDisplay(uint8_t color); 
 
    virtual AudioCodec *GetAudioCodec() override;
    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override;
};

void XiaozhiCardBoard::InitializeI2c()
{
    ESP_LOGI(TAG, "Initialize I2C peripheral");
    i2c_master_bus_config_t i2c_bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SYS_I2C_PIN_SDA,
        .scl_io_num = SYS_I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 0,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
}

void XiaozhiCardBoard::InitializeSpi()
{
    ESP_LOGI(TAG, "Initialize SPI bus");

    // Both devices share SPI; keep their CS lines inactive before mounting NAND.
    gpio_set_direction(EPD_PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_14, 1);
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = EPD_PIN_SCK;
    bus_cfg.mosi_io_num = EPD_PIN_MOSI;
    bus_cfg.miso_io_num = EPD_PIN_MISO;
    bus_cfg.quadwp_io_num = GPIO_NUM_NC;
    bus_cfg.quadhd_io_num = GPIO_NUM_NC;
    bus_cfg.max_transfer_sz = EPD_RES_HEIGHT * EPD_RES_WIDTH; // / 8 + 1;   
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
}

void XiaozhiCardBoard::InitializeCharger()
{
    ESP_LOGI(TAG, "Init Charger AW32001");

    charger_ = new Aw32001(i2c_bus_, I2C_ADDR_AW32001);
    // A failed configuration is retried on the next boot; never use an unverified setting.
    bool ok = charger_->SetWatchdog(0, false) && charger_->SetIntWakeupTime(1) &&
        charger_->SetShippingMode(false) && charger_->SetNtcFunction(false) &&
        charger_->SetDischargeCurrent(2800) && charger_->SetChargeCurrent(260) &&
        charger_->SetChargeVoltage(4200) && charger_->SetPreChargeCurrent(31) &&
        charger_->SetPrechargeToFastchargeThreshold(0) && charger_->SetCharge(true);
    if (!ok) ESP_LOGE(TAG, "Charger configuration failed; playback blocked");
    charger_ready_ = ok;
}

void XiaozhiCardBoard::InitializeGuage()
{
    ESP_LOGI(TAG, "Init Gauge BQ27220");

    guage_ = new Bq27220(i2c_bus_, I2C_ADDR_BQ27220);
}

void XiaozhiCardBoard::InitializeStorage()
{
    ESP_LOGI(TAG, "Init Storage SD Card");
    
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = EPD_SPI_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = GPIO_NUM_14;
    slot_config.host_id = EPD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card = nullptr;
    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret == ESP_OK && card != nullptr) {
        sdmmc_card_print_info(stdout, card);   
        sd_card_present_ = true;
    } else {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    }
}

void XiaozhiCardBoard::InitializeDisplay()
{
    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num = EPD_PIN_DC;
    io_cfg.cs_gpio_num = EPD_PIN_CS;
    io_cfg.pclk_hz = 40 * 1000 * 1000; // 40MHz
    io_cfg.lcd_cmd_bits = 8;    
    io_cfg.lcd_param_bits = 8;  
    io_cfg.spi_mode = 0;
    io_cfg.trans_queue_depth = 8;  
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EPD_SPI_HOST, &io_cfg, &panel_io_));

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = EPD_PIN_RST;
    panel_cfg.rgb_endian = LCD_RGB_ENDIAN_BGR;
    panel_cfg.bits_per_pixel = 1;
    ESP_LOGI(TAG, "Install gdey027t91 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_gdey027t91(panel_io_, &panel_cfg, &panel_));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
    // // TODO: 这里等待耗时，需要优化  
    // ClearDisplay(0x00);
    // ClearDisplay(0xFF);

    // Initialize touch panel
    ESP_LOGI(TAG, "Initialize touch IO (I2C)");
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_config.scl_speed_hz = 100000;
    esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);

    ESP_LOGI(TAG, "Initialize touch controller FT5X06");
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = DISPLAY_WIDTH,
        .y_max = DISPLAY_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .user_data = this 
    };
    esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &touch_);
   
    display_ = new EpdDisplay(panel_io_, panel_, touch_, DISPLAY_WIDTH, DISPLAY_HEIGHT, 
                              DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, 
                              DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, 
                              {
                                  .text_font  = &font_puhui_16_1,
                                  .icon_font  = &font_awesome_16_4,
                                  .emoji_font = font_emoji_64_init(),
                              });

    // 不再提示（引导页） 
    display_->on_click_dont_reming_ = [this]() { 
        nvs_handle_t handle;
        if (nvs_open("app_config", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u8(handle, "dont_remind", 1);  
            nvs_commit(handle);
            nvs_close(handle);
            ESP_LOGI(TAG, "不再显示引导页");
        }
    };   
    // 切换网络
    display_->on_switch_network_ = [this]() { 
        PostEvent(BoardEvent::SwitchNetWork);
    }; 
    // 重置 Wi-Fi 
    display_->on_clear_network_ = [this]() { 
        PostEvent(BoardEvent::ClearWiFiConfig);
    }; 
    // 开启/关闭自动休眠        
    display_->on_auto_sleep_changed_ = [this]() {
        if (power_save_timer_) {
            bool enabled = power_save_timer_->GetState(); 
            power_save_timer_user_set_ = true;
            if (enabled) {
                lv_label_set_text(display_->setup_label_auto_sleep_, "开启自动休眠"); 
            } else {
                lv_label_set_text(display_->setup_label_auto_sleep_, "关闭自动休眠");
            }
            power_save_timer_->SetEnabled(!enabled);
        }
    };
    // 手动休眠 
    display_->on_manual_sleep_ = [this]() { 
        if (power_save_timer_ && power_save_timer_->GetState()) { // 如果开启了省电模式 
            power_save_timer_->ManualSleep();
        } else {
            PostEvent(BoardEvent::Sleep);
        }   
    };    
    // 关机      
    display_->on_shutdown_ = [this]() { 
        PostEvent(BoardEvent::Shutdown);
    }; 
}

void XiaozhiCardBoard::InitializeButtons()
{
    // Ignore the press that woke the PMIC, including a release after a long hold.
    auto ignore_release = std::make_shared<bool>(gpio_get_level(USER_BUTTON_GPIO)==0);
    const TickType_t accept_after=xTaskGetTickCount()+pdMS_TO_TICKS(1000);
    user_button_.OnPressUp([ignore_release,accept_after]() {
        if(*ignore_release){*ignore_release=false;return;}
        if(int32_t(xTaskGetTickCount()-accept_after)<0)return;
        ToggleRadioPower();
    });
}

void XiaozhiCardBoard::InitializeIndicator()
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT, 
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    rmt_config.flags.with_dma = false;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    SetIndicator(0, 0, 50);
}

void XiaozhiCardBoard::SetIndicator(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_strip_ != nullptr) {
        led_strip_set_pixel(led_strip_, 0, r, g, b);
        led_strip_refresh(led_strip_);
    }
}

void XiaozhiCardBoard::SetPowerSaveMode(bool en)
{
    (void)en; // Radio reception requires an awake UART and modem.
    if (power_save_timer_) power_save_timer_->SetEnabled(false);
}

bool XiaozhiCardBoard::GetPowerSaveMode(void)
{
    if (!power_save_timer_) return false; 

    return power_save_timer_->GetState();
}

// 物联网初始化，添加对 AI 可见设备
void XiaozhiCardBoard::InitializeTools() 
{
    // TODO:
    // 切换网络
    // 重置 Wi-Fi
    // 底座灯设置
    // 底座 Grove 口
}

bool XiaozhiCardBoard::IsGuidePageRequired(void) 
{  
    nvs_handle_t handle;
    esp_err_t err;
    char stored_ver[32] = {0};
    size_t len = sizeof(stored_ver);
    bool dont_remind = false;
    const char *current_ver = esp_app_get_description()->version;

    err = nvs_open("app_config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE("GUIDE", "NVS open failed");
        return true; // 默认提示
    }

    // 获取保存的版本号和 dont_remind 标志
    bool has_ver = (nvs_get_str(handle, "version", stored_ver, &len) == ESP_OK);
    bool has_flag = (nvs_get_u8(handle, "dont_remind", (uint8_t *)&dont_remind) == ESP_OK);

    if (!has_ver || strcmp(stored_ver, current_ver) != 0) {
        // 版本号不一致或首次运行 → 重置标志
        nvs_set_str(handle, "version", current_ver);
        nvs_set_u8(handle, "dont_remind", 0);  // 重置为 false
        nvs_commit(handle);
        dont_remind = false;
        ESP_LOGI("GUIDE", "Version changed (%s -> %s), reset flag", stored_ver, current_ver);
    }

    nvs_close(handle);
    return !dont_remind;
}

void XiaozhiCardBoard::StartUp()
{
    // Do not power the modem/display repeatedly when the battery is exhausted.
    for (;;) {
        BoardPowerStatus power;
        if (GetPowerStatus(power) && power.millivolts >= 3500) return;
        uint8_t state=0, faults=0;
        bool external=charger_->ReadStatus(state,faults) && (state&0x02);
        if (!external && charger_ready_) {
            for (int attempt=0;attempt<3;++attempt) {
                if(charger_->SetShippingMode(true))vTaskDelay(pdMS_TO_TICKS(1500));
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        // Charging/unknown telemetry retries slowly; unplugged low battery stays asleep.
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        if(external || !charger_ready_)esp_sleep_enable_timer_wakeup(60ULL*1000000);
        if(gpio_get_level(USER_BUTTON_GPIO)!=0)esp_sleep_enable_ext1_wakeup(BIT64(USER_BUTTON_GPIO),ESP_EXT1_WAKEUP_ANY_LOW);
        esp_deep_sleep_start();
    }
}

void XiaozhiCardBoard::Shutdown()
{
    ESP_LOGI(TAG, "Shutdown");

    uint8_t power_status = 0, faults = 0;
    if (!charger_->ReadStatus(power_status, faults)) {
        ESP_LOGE(TAG, "Charger status unavailable; shutdown cancelled");
        CancelRadioShutdown("电源读取失败，关机取消");
        return;
    }
    if (power_status & 0x02) {
        ESP_LOGI(TAG, "底座供电中保持开机");

        ShowRadioChargingStandby();
        return;
    }

    esp_task_wdt_delete(event_task_handle_);

    NetworkType network_type = GetNetworkType();
    if (network_type == NetworkType::ML307) {
        // auto& board = Board::GetInstance();
        // auto& dual_board = static_cast<DualNetworkBoard&>(board);
        // auto& ml307_board = static_cast<Ml307Board&>(dual_board.GetCurrentBoard());
        // for (int i = 0; i < 5; i++) {
        //     if (ml307_board.PowerOff()) {
        //         modem_powered_on_ = false;
        //         ESP_LOGI(TAG, "4G POWER OFF SUCCESS"); 
        //         break; 
        //     } else {
        //         vTaskDelay(pdMS_TO_TICKS(100));
        //     }
        // }
        PowerOffModem();
    } else if (network_type == NetworkType::WIFI) { // 确认 4G 模组已关机 
        uint8_t i = 0; 
        while (i++ < 10) {
            if (modem_powered_on_) { 
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
  
    // A newly attached dock must not leave a powered device showing a shutdown page.
    if(charger_->ReadStatus(power_status,faults) && (power_status&0x02)){esp_restart();return;}
    ShowRadioPoweredOff();
    vTaskDelay(pdMS_TO_TICKS(1500));
    for (int attempt=0;attempt<3;++attempt) {
        bool sent=charger_->SetShippingMode(true);
        ESP_LOGI(TAG,"Shipping command attempt=%d sent=%d",attempt+1,sent);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    ESP_LOGE(TAG,"Shipping did not remove power; entering deep sleep fallback");
    // Avoid immediately waking while the shutdown key is still held.
    for(int i=0;i<100 && gpio_get_level(USER_BUTTON_GPIO)==0;i++)vTaskDelay(pdMS_TO_TICKS(50));
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if(gpio_get_level(USER_BUTTON_GPIO)!=0)esp_sleep_enable_ext1_wakeup(BIT64(USER_BUTTON_GPIO),ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();  
}

void XiaozhiCardBoard::InitializePowerSaveTimer()
{
    power_save_timer_ = new PowerSaveTimer(-1, 3*60, -1); // 3分钟自动休眠 
    power_save_timer_->OnEnterSleepMode([this]() {
        ESP_LOGI(TAG, "On Enter Sleep Mode");
        auto& board = Board::GetInstance();
        auto* xz_board = static_cast<XiaozhiCardBoard*>(&board);
        xz_board->PostEvent(BoardEvent::Sleep);
    });
    power_save_timer_->OnExitSleepMode([this]() {
        ESP_LOGI(TAG, "On Exit Sleep Mode");
        auto& board = Board::GetInstance();
        auto* xz_board = static_cast<XiaozhiCardBoard*>(&board);
        xz_board->PostEvent(BoardEvent::WakeUp);
    });
    power_save_timer_->OnShutdownRequest([this]() {
        ESP_LOGI(TAG, "On Shutdown Request");
    });
    power_save_timer_->SetEnabled(false);
}

void XiaozhiCardBoard::Sleep()
{
    return; // Radio reception requires an awake UART and modem.
    ESP_LOGI(TAG, "Sleep");
 
    auto& app = Application::GetInstance();
    // Disable wake word detection
    auto& audio_service = app.GetAudioService();
    bool is_wake_word_running_ = audio_service.IsWakeWordRunning();
    if (is_wake_word_running_) {
        audio_service.EnableWakeWordDetection(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Disable audio input and output
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec) {
        codec->EnableInput(false);
        codec->EnableOutput(false);
    }
    audio_service.Stop();

    // 进入休眠页面 
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();   // 保存休眠前页面  
    lv_screen_load(display_->scr_sleep_); // 进入休眠页面 
    lv_refr_now(NULL);                    // 立即触发刷新
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(100));

    while (1) {
        esp_task_wdt_reset();
        esp_sleep_enable_timer_wakeup(5 * 60 * 1000000); // 休眠期间 5 分钟定时唤醒一次 
        esp_err_t err = esp_light_sleep_start();
        ESP_LOGI(TAG, "Woke up, err = %s", esp_err_to_name(err));
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) { 
            ESP_LOGI(TAG, "定时器唤醒");
            constexpr int SAMPLE_COUNT = 5;
            int low_count = 0;
            for (int i = 0; i < SAMPLE_COUNT; ++i) {
                float bat_vol = guage_->getVolt(VOLT_MODE::VOLT) / 1000.0f;
                ESP_LOGI(TAG, "[%d] 电压采样：%.2f V", i + 1, bat_vol);
                if (bat_vol <= BAT_VOL_EMPTY && !charger_->GetChargeState()) { // 电压低且未充电
                    low_count++;
                }
                vTaskDelay(pdMS_TO_TICKS(20));  
            }
            if (low_count >= SAMPLE_COUNT) {
                ESP_LOGW(TAG, "检测到 5 次电压均低于 %.2fV，执行关机", BAT_VOL_EMPTY);
                Shutdown();
                return;
            }
            ESP_LOGI(TAG, "电压正常，重新进入休眠");
            continue;
        } 
        // 退出 
        break;
    }

    ESP_LOGI(TAG, "唤醒");
    lvgl_port_lock(0);
    lv_screen_load(scr); // 重新加载原页面
    lvgl_port_unlock();

    if (codec) {
        codec->EnableInput(true);
        codec->EnableOutput(true);
    }
    audio_service.Start();

    // Enable wake word detection
    if (is_wake_word_running_) {
        audio_service.EnableWakeWordDetection(true);
    }
    // 
    if (power_save_timer_) {
        power_save_timer_->WakeUp();
    }
}

void XiaozhiCardBoard::WakeUp()
{
    ESP_LOGI(TAG, "WakeUp");
    // 
}

void XiaozhiCardBoard::BoardEventTask(void* param) 
{
    auto* self = static_cast<XiaozhiCardBoard*>(param);
    BoardEvent event;

    // esp_task_wdt_config_t twdt_config = {
    //     .timeout_ms = 6000,
    //     .idle_core_mask = (1 << 0) | (1 << 1),
    //     .trigger_panic = true, 
    // };
    // esp_task_wdt_init(&twdt_config);
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    ESP_ERROR_CHECK(esp_task_wdt_status(NULL));
    while (1) {
        esp_task_wdt_reset();
        if (xQueueReceive(self->event_queue_, &event, pdMS_TO_TICKS(1000))) {
            ESP_LOGI(TAG, "Received event: %s", BoardEventToString(event));
            self->HandleBoardEvent(event);
        }
    }
}

void XiaozhiCardBoard::StartBoardEventTask()
{
    event_queue_ = xQueueCreate(8, sizeof(BoardEvent));
    if (!event_queue_) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        BoardEventTask,      // 静态任务函数
        "BoardEventTask",    // 名字
        8192,                // 栈大小
        this,                // 参数（this 指针）
        10,                  // 优先级
        &event_task_handle_, // 任务句柄 
        0                    // 核心（CORE1）
    );
    if(created!=pdPASS){vQueueDelete(event_queue_);event_queue_=nullptr;ESP_LOGE(TAG,"Board event task creation failed");}
}

bool XiaozhiCardBoard::PostEvent(const BoardEvent& event)
{
    if (!event_queue_) {
        ESP_LOGW(TAG, "Event queue not initialized");
        return false;
    }
    return xQueueSend(event_queue_, &event, 0) == pdTRUE;   
}

void XiaozhiCardBoard::HandleBoardEvent(BoardEvent event) 
{
    switch (event) {
        case BoardEvent::Shutdown: {
                Shutdown();
            }    
            break;
        case BoardEvent::Sleep: {
                Sleep();
            }
            break;
        case BoardEvent::WakeUp: {
                WakeUp();
            }
            break;
        case BoardEvent::SwitchNetWork: {
                SwitchNetworkType();
            }
            break;
        case BoardEvent::ClearWiFiConfig: {
                auto& app = Application::GetInstance();
                if (GetNetworkType() == NetworkType::WIFI) {
                    // cast to WifiBoard
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.ResetWifiConfiguration();
                }
            }
            break;
        default: {
                ESP_LOGW(TAG, "Unknown event type: %d", static_cast<int>(event));
            }  
            break;
    }
}

void XiaozhiCardBoard::PowerOnModem() 
{
    ESP_LOGI(TAG, "Power On Modem");

    gpio_num_t pwr_pin = ML307R_PIN_PWR;
    gpio_reset_pin(pwr_pin);
    gpio_set_direction(pwr_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pwr_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    // 关机状态下，PWR 拉低 2s ~ 3.5s 开机 （pwr 逻辑反转）
    gpio_set_level(pwr_pin, 1); 
    vTaskDelay(pdMS_TO_TICKS(2500));
    gpio_set_level(pwr_pin, 0);
}

void XiaozhiCardBoard::PowerOffModem() 
{
    ESP_LOGI(TAG, "Power Off Modem");

    gpio_num_t pwr_pin = ML307R_PIN_PWR;
    gpio_reset_pin(pwr_pin);
    gpio_set_direction(pwr_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pwr_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    // 开机状态下，PWR 拉低 3.5 ~ 4.0s 关机 （pwr 逻辑反转）
    gpio_set_level(pwr_pin, 1); 
    vTaskDelay(pdMS_TO_TICKS(3750));
    gpio_set_level(pwr_pin, 0);
}

void XiaozhiCardBoard::Enable4G(void)
{
    ESP_LOGI(TAG, "Enable4G");

    struct TaskWrapper {
        static void run(void *param) {
            auto *self = static_cast<XiaozhiCardBoard*>(param);
            self->PowerOnModem();
            self->modem_powered_on_ = true;
            vTaskDelete(NULL);
        }
    };

    xTaskCreate(TaskWrapper::run, "enable_4g_task", 4096, this, 6, NULL);
}

void XiaozhiCardBoard::Disable4G(void)
{
    ESP_LOGI(TAG, "Disable4G");

    struct TaskWrapper {
        static void run(void *param) {
            auto* self = static_cast<XiaozhiCardBoard*>(param);
            vTaskDelay(pdMS_TO_TICKS(3000)); // 等待模组启动完成
            self->PowerOffModem();
            // auto modem = AtModem::Detect(ML307R_PIN_TX, ML307R_PIN_RX, ML307R_PIN_DTR, 115200);
            // if (!modem) { // Wi-Fi 切换到 4G 时 AtModem: Failed to send AT+CGMM command，这里做判断处理
            //     ESP_LOGE(TAG, "Failed to detect modem");
            //     self->PowerOffModem();
            // } else {
            //     auto uart = modem->GetAtUart();
            //     for (int i = 0; i < 5; i++) {
            //         if (uart->SendCommand("AT+MPOF=0", 1000)) {
            //             std::string response = uart->GetResponse();
            //             if (response == "POWER OFF") {
            //                 ESP_LOGI(TAG, "4G POWER OFF SUCCESS"); 
            //                 break;
            //             }
            //         }
            //         if (i == 4) {
            //             self->PowerOffModem();
            //             break;
            //         }
            //     }
            // }
            self->modem_powered_on_ = false;  
            vTaskDelete(NULL);
        }
    };

    xTaskCreate(TaskWrapper::run, "disable_4g_task", 4096, this, 5, NULL);
}

XiaozhiCardBoard::XiaozhiCardBoard() : DualNetworkBoard(ML307R_PIN_TX, ML307R_PIN_RX, ML307R_PIN_DTR, 1, true),
         user_button_(USER_BUTTON_GPIO, false, 2000, 400) // 双击间隔为 400ms 内 
{
    // Initialize hardware components
    InitializeI2c();
    InitializeCharger();
    InitializeGuage();
    InitializeSpi();
    // The radio does not require SD storage.
    InitializeDisplay(); 
    InitializeButtons();
    InitializeIndicator();
    InitializePowerSaveTimer();
    StartBoardEventTask();

    StartUp();
    if (IsGuidePageRequired()) {
        lv_screen_load(display_->scr_startup_);  
    } else {
        lv_screen_load(display_->scr_main_);  
    }

    Enable4G(); // 启动 4G 模组 

    network_type_ = GetNetworkType();
    ESP_LOGI(TAG, "Current network type: %s", 
                    network_type_ == NetworkType::WIFI ? "WiFi" : 
                    network_type_ == NetworkType::ML307 ? "ML307" : "Unknown");
    // Keep ML307 powered for mobile radio.

    InitializeTools();

    // 设置唤醒源，按键，触摸唤醒 
    uint64_t wakeup_pins = BIT64(USER_BUTTON_GPIO) | BIT64(TOUCH_INT_GPIO);
    esp_sleep_enable_ext1_wakeup(wakeup_pins, ESP_EXT1_WAKEUP_ANY_LOW); 
    esp_sleep_enable_gpio_wakeup();
}

XiaozhiCardBoard::~XiaozhiCardBoard()
{
    if (charger_) {
        delete charger_;
    }
    if (guage_) {
        delete guage_;
    }
    if (display_) {
        delete display_;
    }
    if (power_save_timer_) {
        delete power_save_timer_;
    }
}

Display *XiaozhiCardBoard::GetDisplay()
{
    return display_;
}

bool XiaozhiCardBoard::GetBatteryLevel(int& level,bool& charging,bool& discharging) {
    BoardPowerStatus status;
    if(!GetPowerStatus(status))return false;
    level=status.percent;charging=status.charging;discharging=!status.external_power;
    return true;
}

bool XiaozhiCardBoard::GetPowerStatus(BoardPowerStatus& status) {
    std::lock_guard<std::mutex> lock(power_sample_mutex_);
    uint16_t mv = 0, soc = 0, full_capacity = 0;
    int16_t ma = 0;
    uint8_t charger_status = 0, faults = 0;
    if (!charger_ready_ || !guage_ || !charger_ || !guage_->ReadPowerSample(mv, soc, ma, full_capacity) ||
        !charger_->ReadStatus(charger_status, faults)) return false;
    status = {};
    status.millivolts = mv;
    status.milliamps = ma;
    status.full_capacity_mah = full_capacity;
    status.external_power = (charger_status & 0x02) != 0;
    const unsigned state = (charger_status >> 3) & 3;
    status.charging = status.external_power && (state == 1 || state == 2);
    status.full = status.external_power && state == 3;
    status.thermal_regulation = (charger_status & 1) != 0;
    status.input_limited = (charger_status & 0x04) != 0;
    status.fault_flags = (faults & 0x3f) | (charger_status & 0x80);
    // Match the supplied XiaozhiCard firmware: voltage scale, 60-sample average, rounding.
    const float volts=mv/1000.0f;
    float raw=volts>=BAT_VOL_FULL?100.0f:(volts-BAT_VOL_EMPTY)/(BAT_VOL_FULL-BAT_VOL_EMPTY)*100.0f;
    raw=std::max(0.0f,std::min(100.0f,raw));
    status.percent=std::max(0,std::min(100,int(battery_filter_.update(raw)+0.5f)));
    status.estimated=true; // The official algorithm remains voltage-derived; UI omits the tilde as requested.
    SetIndicator(0, 0, status.charging ? 35 : 0);
    return true;
}

void XiaozhiCardBoard::ClearDisplay(uint8_t color)
{
    static uint8_t *buf = nullptr;
    static size_t buf_size = 0;
    if (panel_ == nullptr) {
        printf("ClearDisplay: panel_ is null!\n");
        return;
    }
    buf_size = EPD_RES_WIDTH * EPD_RES_HEIGHT;
    if (buf == nullptr) {
        buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (!buf) {
            printf("ClearDisplay: failed to allocate %u bytes in SPIRAM!\n", (unsigned)buf_size);
            return;
        }
    }
    memset(buf, color, buf_size);
    panel_gdey027t91_draw_bitmap_full(panel_, 0, 0, EPD_RES_WIDTH, EPD_RES_HEIGHT, buf);
}

AudioCodec *XiaozhiCardBoard::GetAudioCodec()
{
    static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                        AUDIO_I2S_PIN_MCLK, AUDIO_I2S_PIN_BCLK, AUDIO_I2S_PIN_WS, AUDIO_I2S_PIN_DOUT,
                                        AUDIO_I2S_PIN_DIN, AUDIO_PIN_PA, AUDIO_CODEC_ES8311_ADDR);
    return &audio_codec;
}

DECLARE_BOARD(XiaozhiCardBoard);
 