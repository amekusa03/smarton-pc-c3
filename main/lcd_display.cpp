#include "lcd_display.hpp"
#include <LovyanGFX.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <cmath>
#include <cstring>
#include <cstdio>

static const char *TAG = "lcd";

// ── LGFX hardware config (Waveshare ESP32-C6-GEEK / ST7789) ──────────────
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI      _bus;
    lgfx::Light_PWM    _light;
public:
    LGFX() {
        { auto c = _bus.config();
          c.spi_host   = SPI2_HOST;
          c.spi_mode   = 3;
          c.freq_write = 40000000;
          c.freq_read  = 16000000;
          c.pin_sclk   = 1;
          c.pin_mosi   = 2;
          c.pin_miso   = -1;
          c.pin_dc     = 3;
          _bus.config(c); _panel.setBus(&_bus); }

        { auto c = _panel.config();
          c.pin_cs        = 5;
          c.pin_rst       = 4;
          c.memory_width  = 240;
          c.memory_height = 320;
          c.panel_width   = 135;
          c.panel_height  = 240;
          c.offset_x      = 52;
          c.offset_y      = 40;
          _panel.config(c); }

        { auto c = _light.config();
          c.pin_bl     = 6;
          c.freq       = 44100;
          c.pwm_channel = 0;
          _light.config(c); _panel.setLight(&_light); }

        setPanel(&_panel);
    }
};

static LGFX lcd;
static LGFX_Sprite spr(&lcd);

// ── Display state ─────────────────────────────────────────────────────────
typedef enum {
    STATE_WIFI_CONNECTING,
    STATE_COMMISSIONING,
    STATE_PC_OFF,
    STATE_PC_ON,
} disp_state_t;

static volatile disp_state_t g_state = STATE_WIFI_CONNECTING;
static volatile bool  g_flash        = false;
static volatile float g_pulse        = 0.0f;
static char g_ip[24]                 = "";

static const int W        = 240;
static const int H        = 135;
static const int HEADER_H = 20;
static const int FOOTER_H = 20;
static const int MAIN_H   = H - HEADER_H - FOOTER_H;  // 95px

// ── Colors (RGB888 → RGB565) ──────────────────────────────────────────────
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(b >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (r >> 3);
}

// ── WiFi signal bars ──────────────────────────────────────────────────────
static void draw_wifi(LGFX_Sprite &s, int x, int y, bool ok) {
    for (int i = 0; i < 3; i++) {
        int bh = (i + 1) * 4 + 1;
        uint16_t col = ok ? rgb(0x00, 0x66, 0xFF) : rgb(0x33, 0x33, 0x33);
        s.fillRect(x + i * 6, y + (13 - bh), 5, bh, col);
    }
}

// ── Draw one complete frame ───────────────────────────────────────────────
static void draw_frame(disp_state_t state, float pulse) {
    spr.createSprite(W, H);

    // ── Background ──────────────────────────────────────────────────────
    uint16_t bg;
    switch (state) {
    case STATE_PC_ON:          bg = rgb(0x05, 0x05, 0x22); break;
    case STATE_PC_OFF:         bg = rgb(0x14, 0x10, 0x10); break;
    default:                   bg = rgb(0x08, 0x08, 0x10); break;
    }
    spr.fillScreen(bg);

    // ── Header bar ──────────────────────────────────────────────────────
    spr.fillRect(0, 0, W, HEADER_H, rgb(0x0A, 0x0A, 0x0A));
    spr.drawFastHLine(0, HEADER_H, W, rgb(0x22, 0x22, 0x22));

    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextColor(rgb(0x88, 0x88, 0x88));
    spr.setTextDatum(textdatum_t::middle_left);
    spr.drawString("Smart PC", 7, HEADER_H / 2);

    bool wifi_ok = (state != STATE_WIFI_CONNECTING);
    draw_wifi(spr, W - 24, 3, wifi_ok);

    // ── Main area ────────────────────────────────────────────────────────
    int cy = HEADER_H + MAIN_H / 2;

    if (state == STATE_WIFI_CONNECTING) {
        spr.setFont(&fonts::FreeSansBold12pt7b);
        spr.setTextSize(1.0f);
        spr.setTextDatum(textdatum_t::middle_center);
        spr.setTextColor(rgb(0x00, 0x99, 0xFF));
        spr.drawString("connecting...", W / 2, cy);

    } else if (state == STATE_COMMISSIONING) {
        spr.setFont(&fonts::FreeSansBold24pt7b);
        spr.setTextSize(1.6f);
        spr.setTextDatum(textdatum_t::middle_center);
        spr.setTextColor(rgb(0x00, 0xAA, 0xFF));
        spr.drawString("Ready", W / 2, cy);

    } else {
        // Big ON / OFF with pulse
        float bright = (state == STATE_PC_ON) ? (0.75f + 0.25f * pulse) : 1.0f;
        uint16_t text_col;
        if (state == STATE_PC_ON) {
            text_col = rgb(0, (uint8_t)(0xEE * bright), (uint8_t)(0x55 * bright));
        } else {
            text_col = rgb((uint8_t)(0xDD * bright), 0x11, 0x11);
        }

        spr.setFont(&fonts::FreeSansBold24pt7b);
        spr.setTextSize(1.6f);
        spr.setTextDatum(textdatum_t::middle_center);
        spr.setTextColor(text_col);
        spr.drawString((state == STATE_PC_ON) ? "ON" : "OFF", W / 2, cy);
    }

    // ── Footer bar ───────────────────────────────────────────────────────
    spr.drawFastHLine(0, H - FOOTER_H, W, rgb(0x22, 0x22, 0x22));
    spr.fillRect(0, H - FOOTER_H + 1, W, FOOTER_H - 1, rgb(0x0A, 0x0A, 0x0A));

    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextSize(1);
    spr.setTextDatum(textdatum_t::middle_center);
    if (g_ip[0]) {
        char foot[48];
        snprintf(foot, sizeof(foot), "%s  ·  Matter", g_ip);
        spr.setTextColor(rgb(0x55, 0x55, 0x55));
        spr.drawString(foot, W / 2, H - FOOTER_H / 2);
    } else {
        spr.setTextColor(rgb(0x33, 0x33, 0x33));
        spr.drawString("no ip", W / 2, H - FOOTER_H / 2);
    }

    spr.pushSprite(0, 0);
    spr.deleteSprite();
}

// ── Flash effect then switch state ───────────────────────────────────────
static void do_flash(disp_state_t new_state) {
    lcd.fillScreen(lcd.color888(0xFF, 0xFF, 0xFF));
    vTaskDelay(80 / portTICK_PERIOD_MS);
    g_state = new_state;
}

// ── Poll IP address and auto-update state ─────────────────────────────────
static void poll_ip(void) {
    if (g_state != STATE_WIFI_CONNECTING && g_ip[0]) return;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) return;

    char ip[20];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    strlcpy(g_ip, ip, sizeof(g_ip));
    if (g_state == STATE_WIFI_CONNECTING) {
        g_state = STATE_COMMISSIONING;
        ESP_LOGI(TAG, "WiFi connected, IP=%s", ip);
    }
}

// ── Animation task (20 fps) ───────────────────────────────────────────────
static void lcd_task(void *) {
    float angle     = 0.0f;
    int   ip_ticker = 0;
    while (true) {
        angle += 0.05f;
        if (angle > 2.0f * M_PI) angle -= 2.0f * M_PI;
        g_pulse = sinf(angle);

        // IPアドレスを1秒ごとにポーリング
        if (++ip_ticker >= 20) {
            ip_ticker = 0;
            poll_ip();
        }

        draw_frame(g_state, g_pulse);
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ── Public API ────────────────────────────────────────────────────────────
void lcd_init(void) {
    lcd.init();
    lcd.setRotation(1);
    lcd.setBrightness(220);
    lcd.fillScreen(0);
    xTaskCreate(lcd_task, "lcd", 16384, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "LCD initialized 240x135");
}

void lcd_notify_wifi_connecting(void) {
    g_state = STATE_WIFI_CONNECTING;
}

void lcd_notify_wifi_connected(const char *ip) {
    strlcpy(g_ip, ip, sizeof(g_ip));
    if (g_state == STATE_WIFI_CONNECTING) {
        g_state = STATE_COMMISSIONING;
    }
}

void lcd_notify_commissioning(void) {
    g_state = STATE_COMMISSIONING;
}

void lcd_notify_pc_on(void) {
    do_flash(STATE_PC_ON);
}

void lcd_notify_pc_off(void) {
    do_flash(STATE_PC_OFF);
}
