#include "pc_control.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "pc_control";

#define DEBOUNCE_MS   50
#define BTN_PRESS_MS  500   // PWRボタンを押し続ける時間 [ms]
                            // ATXマザーボードは通常100ms以上で認識する。
                            // 長くしすぎると強制シャットダウン(4秒以上)になる。

// -----------------------------------------------------------------------
// 再送ブロック設計
//
// Matterプロトコル(MRP)はパケットロス時に自動再送する。
// chip-tool は最大4回、合計約2.7秒以内に再送する。
// これが届くたびにコールバックが呼ばれ、複数回押下になるのを防ぐため
// s_pressing フラグを使う。
//
// タイムライン:
//   t=0ms      : pc_execute_command → s_pressing=true, GPIO HIGH
//   t=500ms    : release_cb        → GPIO LOW, cooldown開始
//   t=3000ms   : cooldown_cb       → s_pressing=false (次のコマンドを受付)
//
// 再送は t=0〜2700ms に集中するため、3000msのブロック窓で全てカバーできる。
// -----------------------------------------------------------------------

static volatile bool s_pressing = false;

// xTaskCreate を使わず esp_timer にした理由:
//   xTaskCreate でタスクを生成すると、FreeRTOS スケジューラの
//   優先度・タイミング次第で複数タスクがGPIOを競合する恐れがある。
//   esp_timer は単一のタイマータスクから順番に呼ばれるため競合しない。
static esp_timer_handle_t s_release_timer  = NULL;
static esp_timer_handle_t s_cooldown_timer = NULL;

static void release_cb(void *arg)
{
    gpio_set_level(GPIO_PWR_SW, 0);
    esp_timer_start_once(s_cooldown_timer, 2500ULL * 1000);
}

static void cooldown_cb(void *arg)
{
    s_pressing = false;
}

void pc_control_init(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << GPIO_PWR_SW),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);
    gpio_set_level(GPIO_PWR_SW, 0);

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << GPIO_PWR_LED),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    esp_timer_create_args_t rel_args = {
        .callback = release_cb,
        .arg      = NULL,
        .name     = "btn_release",
    };
    esp_timer_create_args_t cool_args = {
        .callback = cooldown_cb,
        .arg      = NULL,
        .name     = "btn_cooldown",
    };
    esp_timer_create(&rel_args,  &s_release_timer);
    esp_timer_create(&cool_args, &s_cooldown_timer);

    ESP_LOGI(TAG, "GPIO initialized: PWR_SW=%d (expected 0)", gpio_get_level(GPIO_PWR_SW));
}

pc_power_state_t pc_get_power_state(void)
{
    int first = gpio_get_level(GPIO_PWR_LED);
    vTaskDelay(DEBOUNCE_MS / portTICK_PERIOD_MS);
    int second = gpio_get_level(GPIO_PWR_LED);

    if (first != second) {
        return PC_STATE_TRANSITIONING;
    }
    // フォトカプラON → GPIO LOW → PC_STATE_ON
    return (second == 0) ? PC_STATE_ON : PC_STATE_OFF;
}

esp_err_t pc_execute_command(bool want_on)
{
    if (s_pressing) {
        // Matterの再送や重複呼び出しをここで遮断する
        ESP_LOGI(TAG, "Already pressing, ignored");
        return ESP_OK;
    }
    s_pressing = true;
    ESP_LOGI(TAG, "Pressing power button: %s", want_on ? "ON" : "OFF");

    // GPIO を即座にHIGHにしてからタイマーでLOWに戻す。
    // これにより呼び出し元(Matterコールバック)はすぐにリターンでき、
    // MatterスタックがACKを返せる。
    gpio_set_level(GPIO_PWR_SW, 1);
    esp_timer_start_once(s_release_timer, (uint64_t)BTN_PRESS_MS * 1000);
    return ESP_OK;
}
