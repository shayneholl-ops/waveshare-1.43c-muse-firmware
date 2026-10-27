/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.43C: round 466 px SH8601 AMOLED
 * (CO5300-compatible) with CST820 touch, ES8311 speaker + ES7210 dual mic,
 * an ETA6098 charger with a STAT pin, and a 400 mAh cell on an MX1.25 header.
 * SoC ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB octal PSRAM.
 *
 * There is NO IMU, NO RTC, NO TF slot, NO PMU and NO fuel gauge on this
 * board — unlike the 1.43/1.43-B, the 1.75C and the M5Stack StopWatch, so
 * none of their IMU/RTC/PMU code applies here. In particular, do NOT reuse
 * the StopWatch's 16 MB flash config or its M5PM1 power-chip code: this
 * board charges in pure hardware and reports power only through the ADC
 * divider and the charger's STAT pin (see read_power()).
 *
 * All pins below come from Waveshare's official BSP (bsp_config.h and
 * esp32_s3_touch_amoled_1.43c.cpp in waveshareteam/ESP32-S3-Touch-AMOLED-1.43C,
 * 02_Example/espidf_v5.5.3/01_ADC_Test/components/port_bsp). The panel init
 * commands, the touch read sequence and the battery math are that BSP's,
 * adapted to this SDK's board interface. Structure follows
 * board_waveshare_s3_175c.c (LVGL adapter + muse_lcd_bands) and the direct
 * codec wiring of board_vn_s3_183.c; there is no vendor BSP component for
 * the 1.43C in this SDK, so the panel, touch and codecs are wired here
 * directly instead of through bsp_display_new()/bsp_touch_new().
 *
 * The only button visible to the ESP32 is BOOT (GPIO0): it talks. PWR is a
 * physical power control, not a GPIO, so there is no aux button and no
 * software power_off (muse_board_t: NULL means a physical switch does it,
 * and Settings then shows no Power off).
 */

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

static const char *TAG = "board";

/* Panel: QSPI, 466x466. */
#define LCD_WIDTH 466
#define LCD_HEIGHT 466
#define LCD_D0_PIN GPIO_NUM_9
#define LCD_D1_PIN GPIO_NUM_10
#define LCD_D2_PIN GPIO_NUM_11
#define LCD_D3_PIN GPIO_NUM_12
#define LCD_CS_PIN GPIO_NUM_15
#define LCD_SCK_PIN GPIO_NUM_14
#define LCD_RST_PIN GPIO_NUM_13
#define LCD_TE_PIN GPIO_NUM_8
#define DRAW_BUF_LINES 118      /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_WIDTH * 8 * 2)

/* Touch: CST820 on the codec I2C bus. INT is wired but the official BSP
 * polls over I2C, so it is left unconfigured here. */
#define TP_SDA_PIN GPIO_NUM_47
#define TP_SCL_PIN GPIO_NUM_48
#define TP_RST_PIN GPIO_NUM_16
#define TP_ADDR 0x15

/* Audio: ES8311 out + ES7210 dual mic + NS4150B amp, one duplex I2S bus. */
#define CODEC_I2S_NUM I2S_NUM_0
#define CODEC_MCLK_PIN GPIO_NUM_38
#define CODEC_SCLK_PIN GPIO_NUM_39
#define CODEC_LRCK_PIN GPIO_NUM_40
#define CODEC_DSDIN_PIN GPIO_NUM_41
#define CODEC_DSOUT_PIN GPIO_NUM_42
#define CODEC_AMP_PIN GPIO_NUM_46    /* NS4150B enable, driven by the ES8311 driver */
#define CODEC_LDO_PIN GPIO_NUM_18    /* codec supply enable */

/* Power: battery divider on ADC1 channel 3, charger STAT on GPIO7. */
#define BATT_ADC_CHAN ADC_CHANNEL_3  /* GPIO4 */
#define BATT_STAT_PIN GPIO_NUM_7     /* ETA6098 STAT: LOW while charging */
#define LED_PIN GPIO_NUM_5           /* programmable LED (see init()) */
#define BOOT_PIN GPIO_NUM_0

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tp;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static muse_gpio_button_t s_boot;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_batt_ok;               /* false: ADC unavailable, battery reporting off */

/* Forward: the CST820 read callback, defined below display_start(). */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

/* SH8601 init commands, verbatim from Waveshare's official BSP. The 0x2A
 * window starts at column 6, which is where the panel's visible columns
 * begin; see the gap note in display_start(). */
static const sh8601_lcd_init_cmd_t s_lcd_init_cmds[] = {
    { 0xFE, (uint8_t []){ 0x00 }, 1, 0 },
    { 0xC4, (uint8_t []){ 0x80 }, 1, 0 },
    { 0x3A, (uint8_t []){ 0x55 }, 1, 0 },
    { 0x35, (uint8_t []){ 0x00 }, 1, 0 },
    { 0x53, (uint8_t []){ 0x20 }, 1, 0 },
    { 0x51, (uint8_t []){ 0x00 }, 1, 0 },   /* dark start; the UI takes over brightness after boot */
    { 0x36, (uint8_t []){ 0xC0 }, 1, 0 },
    { 0x63, (uint8_t []){ 0xFF }, 1, 0 },
    { 0x2A, (uint8_t []){ 0x00, 0x06, 0x01, 0xD7 }, 4, 0 },
    { 0x2B, (uint8_t []){ 0x00, 0x00, 0x01, 0xD1 }, 4, 0 },
    { 0x11, (uint8_t []){ 0x00 }, 0, 100 },
    { 0x29, (uint8_t []){ 0x00 }, 0, 0 },
};

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TP_SDA_PIN,
        .scl_io_num = TP_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_boot, BOOT_PIN), TAG, "boot button");

    /* Programmable LED: initialized off. muse_board_t has no LED hook, so
     * nothing in the app drives it yet; a later status-light step may claim
     * it. Initializing it here only keeps the pin from floating. */
    const gpio_config_t led = {
        .pin_bit_mask = 1ULL << LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&led), TAG, "led");
    gpio_set_level(LED_PIN, 0);

    /* Codec supply enable. muse_audio calls codec_power(true) before
     * audio_init, but configuring the pin here keeps that call to a level
     * set. TODO: verify on hardware that the LDO enable is active high; if
     * the codecs do not probe, flip codec_power() first. */
    const gpio_config_t ldo = {
        .pin_bit_mask = 1ULL << CODEC_LDO_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ldo), TAG, "codec ldo");
    gpio_set_level(CODEC_LDO_PIN, 0);

    /* Charger STAT: input with pull-up, exactly as the official BSP sets it. */
    const gpio_config_t stat = {
        .pin_bit_mask = 1ULL << BATT_STAT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&stat), TAG, "charge stat");

    /* Battery ADC, mirroring the official BSP: ADC1, 12-bit, 12 dB
     * attenuation, curve-fitting calibration. Unlike the 1.75C's PMU, a
     * failed ADC here must not take the board down: warn and keep going
     * with battery reporting disabled (read_power() then reports -1/0). */
    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&adc_cfg, &s_adc) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC unavailable: battery status disabled");
        return ESP_OK;
    }
    const adc_oneshot_chan_cfg_t ch_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, BATT_ADC_CHAN, &ch_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC channel unavailable: battery status disabled");
        return ESP_OK;
    }
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC calibration unavailable: battery status disabled");
        return ESP_OK;
    }
    s_batt_ok = true;
    return ESP_OK;
}

/* The SH8601 needs even-aligned update windows. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

/*
 * bsp_display_start(), less its draw buffers: the BSP's are PSRAM, and every
 * flush from them needs a fresh 46 KB internal DMA bounce buffer, which can't
 * be had once Wi-Fi and BLE are up. The bands go out through two fixed 7 KB
 * internal buffers instead; 9 KB ones left 1 KB free while Wi-Fi joined.
 * (Same reasoning as board_waveshare_s3_175c.c.)
 */
static lv_display_t *display_start(lv_indev_t **touch)
{
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCK_PIN,
        .data0_io_num = LCD_D0_PIN,
        .data1_io_num = LCD_D1_PIN,
        .data2_io_num = LCD_D2_PIN,
        .data3_io_num = LCD_D3_PIN,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    if (spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_PIN,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    if (esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &s_io) != ESP_OK) {
        return NULL;
    }
    sh8601_vendor_config_t vendor_config = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    if (esp_lcd_new_panel_sh8601(s_io, &panel_config, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);

    /* The panel driver applies this gap to every LVGL flush. The 2A window
     * start (column 6) is the panel's GRAM window, not the driver gap:
     * Waveshare's examples and factory firmware set the gap to 8. */
    esp_lcd_panel_set_gap(s_panel, 8, 0);
    muse_lcd_bands_set_gap(8, 0);
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_WIDTH,
            .ver_res = LCD_HEIGHT,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);
    /* Tearing effect on (V-blank only), as the init commands already ask. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x35 << 8), (uint8_t[]){ 0x00 }, 1);
    muse_lcd_bands_set_te(LCD_TE_PIN);

    /* Touch reset pulse, exactly as the official BSP does it. */
    const gpio_config_t tp_rst = {
        .pin_bit_mask = 1ULL << TP_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&tp_rst) != ESP_OK) {
        return NULL;
    }
    gpio_set_level(TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(TP_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    const i2c_device_config_t tp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_i2c, &tp_cfg, &s_tp) != ESP_OK) {
        return NULL;
    }
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    *touch = indev;
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

/* CST820 touch, read over I2C exactly as the official BSP reads it:
 * register 0x02 reports the touch count and event, 0x03 the coordinates,
 * and both axes are mirrored (LCD_WIDTH - x, LCD_HEIGHT - y). */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint8_t gesture[2] = { 0, 0 };
    uint8_t pos[4] = { 0, 0, 0, 0 };
    (void)indev;
    if (i2c_master_transmit_receive(s_tp, (uint8_t[]){ 0x02 }, 1,
                                    gesture, 2, pdMS_TO_TICKS(50)) != ESP_OK) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    uint8_t event = gesture[1] >> 6;
    if (gesture[0] && event != 0x01) {
        if (i2c_master_transmit_receive(s_tp, (uint8_t[]){ 0x03 }, 1,
                                        pos, 4, pdMS_TO_TICKS(50)) != ESP_OK) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        int x = ((pos[0] & 0x0f) << 8) | pos[1];
        int y = ((pos[2] & 0x0f) << 8) | pos[3];
        data->point.x = LCD_WIDTH - x;
        data->point.y = LCD_HEIGHT - y;
        if (data->point.x >= LCD_WIDTH) {
            data->point.x = LCD_WIDTH - 1;
        }
        if (data->point.y >= LCD_HEIGHT) {
            data->point.y = LCD_HEIGHT - 1;
        }
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void send_brightness(void *level)
{
    /* SH8601 "write display brightness" (0x51), as the BSP sends it. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), level, 1);
}

static void set_brightness(int pct)
{
    uint8_t level = (uint8_t)(pct * 255 / 100);
    muse_lcd_bands_run(send_brightness, &level);
}

static void send_sleep(void *sleep)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

/* Plain SLPIN/SLPOUT over the QSPI command path. Unlike the 1.75C, the
 * panel reset (GPIO13) and the touch reset (GPIO16) are separate pins here,
 * so sleeping the panel cannot pulse the touch reset line. */
static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));   /* settle before the next command */
}

/* Touch reset pulse, as the official BSP does it (also used at init).
 * The pin is configured as an output by init; this only pulses levels. */
static void touch_reset_pulse(void)
{
    gpio_set_level(TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(TP_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(TP_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

/* Screen off: LVGL stops and the CST820 hibernates (register 0xA5 <- 0x03,
 * hibernate until reset, from wupsbr's 1.43C board file) to cut idle
 * current. Resume must reset the touch: hibernate only ends on reset. */
static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        if (s_tp) {
            const uint8_t sleep_cmd[2] = { 0xA5, 0x03 };
            if (i2c_master_transmit(s_tp, sleep_cmd, sizeof(sleep_cmd),
                                    pdMS_TO_TICKS(50)) != ESP_OK) {
                ESP_LOGW(TAG, "CST820 sleep command failed, touch keeps scanning");
            }
        }
    } else {
        touch_reset_pulse();
        esp_lv_adapter_resume();
    }
}

/* ES8311 out and ES7210 dual mic on one duplex I2S bus, wired as in
 * board_vn_s3_183.c. The codecs share the touch I2C bus. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CODEC_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = CODEC_MCLK_PIN,
            .bclk = CODEC_SCLK_PIN,
            .ws = CODEC_LRCK_PIN,
            .dout = CODEC_DSDIN_PIN,
            .din = CODEC_DSOUT_PIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = CODEC_I2S_NUM, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t adc_i2c = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        /* The driver owns the amp enable from here on, so audio_power stays
         * NULL below. GPIO46 is a strapping pin, but the driver only drives
         * it after boot, so strapping is unaffected. */
        .pa_pin = CODEC_AMP_PIN,
        .use_mclk = true,
        /* NS4150B amp rail is 5.0 V per Waveshare's examples/schematic
         * (wupsbr's 1.43C board file); pa_voltage only feeds the volume
         * mapping, it does not drive a voltage. */
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    /* ES7210 PGA steps are 3 dB; snap so the UI shows what's applied. */
    db = (db / 3) * 3;
    /* esp_codec_dev rounds 33 dB down to 30; the next real step up is 34.5. */
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

/* The codecs' own supply: the codec LDO enable. muse_audio calls this
 * before audio_init (the first time too), and after both codecs close. */
static void codec_power(bool on)
{
    gpio_set_level(CODEC_LDO_PIN, on ? 1 : 0);
}

static unsigned poll_buttons(void)
{
    /* BOOT talks; there is no second button on this board. */
    return muse_gpio_button_poll(&s_boot);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

/*
 * Battery power, from the official BSP's bsp_batt_get_voltage() and
 * bsp_batt_get_status() — NOT from a PMU: this board has none.
 *
 * Voltage: ADC1 channel 3 (GPIO4) behind a 1/2 divider, so the calibrated
 * ADC millivolts are doubled to get battery millivolts. Percent comes from
 * a resting-voltage curve for a 3.7 V nominal Li-ion under light load,
 * linearly interpolated: it is an estimate, not a fuel gauge, and while
 * charging the terminal voltage reads high, so the percent is indicative
 * only until the charger stops.
 *
 * Charging: the ETA6098's STAT pin on GPIO7, input with pull-up. STAT is
 * open-drain: LOW while charging, high-impedance (pulled high) when done.
 * A HIGH reading therefore means "full" or "no charger attached" — the two
 * are told apart by the voltage (near 4.2 V and not falling means full;
 * falling means no charger).
 *
 * USB: the S3's USB-Serial/JTAG block has no VBUS register, so USB presence
 * is INFERRED, not sensed: STAT low means the charger is running, which
 * only happens with a charger (USB) attached.
 *
 * No battery: a disconnected divider reads near zero, while even a deeply
 * discharged cell rests above ~2.5 V, so anything under 500 mV reports as
 * no battery (percent -1, millivolts 0, per muse_power_t).
 */
static esp_err_t read_power(muse_power_t *out)
{
    static const struct { int mv, pct; } levels[] = {
        { 3000, 0 }, { 3400, 5 }, { 3600, 10 }, { 3700, 20 }, { 3750, 30 },
        { 3800, 40 }, { 3850, 50 }, { 3900, 60 }, { 3950, 70 }, { 4000, 80 },
        { 4100, 90 }, { 4200, 100 },
    };

    out->charging = gpio_get_level(BATT_STAT_PIN) == 0;
    out->usb = out->charging;
    if (!s_batt_ok) {
        out->battery_pct = -1;
        out->battery_mv = 0;
        return ESP_OK;
    }

    int sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw, cal_mv;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHAN, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(s_cali, raw, &cal_mv) != ESP_OK) {
            return ESP_ERR_TIMEOUT;
        }
        sum += cal_mv;
    }
    int mv = (sum / 8) * 2;
    if (mv < 500) {
        out->battery_pct = -1;
        out->battery_mv = 0;
        return ESP_OK;
    }
    int pct = mv <= levels[0].mv ? 0 : 100;
    for (int i = 0; i + 1 < (int)(sizeof(levels) / sizeof(levels[0])); i++) {
        if (mv >= levels[i].mv && mv < levels[i + 1].mv) {
            pct = levels[i].pct + (mv - levels[i].mv) * (levels[i + 1].pct - levels[i].pct)
                                 / (levels[i + 1].mv - levels[i].mv);
            break;
        }
    }
    out->battery_mv = mv;
    out->battery_pct = pct;
    return ESP_OK;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.43C",
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .round = true,
    .touch = true,
    .diagonal_in = 1.43f,
    .talk_button = "boot",
    /* No aux button: PWR is a physical power control, not a GPIO.
     * Hint at the bottom center, next to the physical BOOT key (wupsbr's
     * 1.43C layout); confirm against the key on hardware. */
    .talk_hint = { LV_ALIGN_CENTER, 0, 190 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .draw_frame = muse_lcd_bands_frame,
    .audio_init = audio_init,
    .mic_slot = -1,             /* mix both ES7210 mics, as on the 1.75C */
    .set_mic_gain = set_mic_gain,
    .codec_power = codec_power,
    /* audio_power stays NULL: the amp enable is owned by the ES8311 driver
     * through pa_pin. power_off stays NULL: no software power-off path is
     * known on this board (see the header comment). */
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
