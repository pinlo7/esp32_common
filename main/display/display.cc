#include "display.h"
#include "config.h"
#include "esp_log.h"
#include <esp_lcd_panel_vendor.h>

#define TAG "Display"

Display::Display() {
    // Constructor
}

Display::~Display() {
    // Destructor
}

void Display::Init() {
    InitSpi();
    initDisplay();
}

void Display::InitSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
}
void Display::initDisplay() {
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    // 打开背光（config.h 定义背光引脚；适配器不负责背光，需应用自行点亮）
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << DISPLAY_BACKLIGHT_PIN;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    bl_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    bl_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    bl_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&bl_cfg);
    gpio_set_level(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT ? 0 : 1);

    // 液晶屏控制IO初始化
    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = DISPLAY_CS_PIN;
    io_config.dc_gpio_num = DISPLAY_DC_PIN;
    io_config.spi_mode = DISPLAY_SPI_MODE;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

    // 初始化液晶屏驱动芯片
    ESP_LOGD(TAG, "Install LCD driver");
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = DISPLAY_RST_PIN;
    panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
    
    esp_lcd_panel_reset(panel);

    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    // 步骤 1: 初始化适配器
    // esp_lv_adapter 默认配置宏是 C 风格设计化初始化（只填部分字段，其余靠编译器补零），
    // C++ 模式下 -Werror=missing-field-initializers 会报错，这里局部忽略该警告
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg));

    // 步骤 2: 注册显示设备(根据接口类型选择相应的默认配置宏)
    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
        panel,           // LCD 面板句柄
        panel_io,        // LCD 面板 IO 句柄(某些接口可为 NULL)
        DISPLAY_WIDTH,             // 水平分辨率
        DISPLAY_HEIGHT,             // 垂直分辨率
        ESP_LV_ADAPTER_ROTATE_0 // 旋转角度
    );
#pragma GCC diagnostic pop
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    assert(disp != NULL);

    // 步骤 3: (可选) 注册输入设备
    // 使用 esp_lcd_touch API 创建触摸句柄(此处省略具体实现)
    // esp_lcd_touch_handle_t touch_handle = /* ... */;
    // esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
    // lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_cfg);
    // assert(touch != NULL);

    // 步骤 4: 启动适配器任务
    ESP_ERROR_CHECK(esp_lv_adapter_start());

    // 步骤 5: 使用 LVGL API 绘制界面(需要先加锁)
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        lv_obj_t *label = lv_label_create(lv_scr_act());
        lv_label_set_text(label, "Hello LVGL!");
        lv_obj_center(label);
        esp_lv_adapter_unlock();
    }
}
