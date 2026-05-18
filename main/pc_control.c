#include "pc_control.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

static const char *TAG = "pc_control";

#define DEBOUNCE_MS        50
#define BTN_PRESS_MS       500   // 短押し [ms]（ATXは100ms以上で認識。4秒以上で強制断になるため注意）
#define BTN_LONGPRESS_MS   4500  // 長押し [ms]（強制電源断）

// フォトカプラの極性設定
// 1 = アクティブHIGH: GPIO HIGH → LED点灯 → PWR_SW短絡（Board B 標準配線）
// 0 = アクティブLOW : GPIO LOW  → LED点灯 → PWR_SW短絡（逆配線・逆極性）
// 接続後にPCが即ONになる場合は 0 に変更して試す
#define PWR_SW_ACTIVE_HIGH  0

#if PWR_SW_ACTIVE_HIGH
#  define LEVEL_PRESS   1
#  define LEVEL_IDLE    0
#else
#  define LEVEL_PRESS   0
#  define LEVEL_IDLE    1
#endif

// -----------------------------------------------------------------------
// 再送ブロック設計
//
// Matterプロトコル(MRP)はパケットロス時に自動再送する。
// chip-tool は最大4回、合計約2.7秒以内に再送する。
// これが届くたびにコールバックが呼ばれ複数回押下になるのを防ぐため
// s_pressing フラグを使う。
//
// タイムライン:
//   t=0ms    : pc_execute_command → s_pressing=true, GPIO=LEVEL_PRESS
//   t=500ms  : release_cb        → GPIO=LEVEL_IDLE, cooldown開始
//   t=3000ms : cooldown_cb       → s_pressing=false (次のコマンドを受付)
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
    gpio_set_level(GPIO_PWR_SW, LEVEL_IDLE);
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
    gpio_set_level(GPIO_PWR_SW, LEVEL_IDLE);

    // gpio_config_t in_cfg = {
    //     .pin_bit_mask = (1ULL << GPIO_PWR_LED),
    //     .mode         = GPIO_MODE_INPUT,
    //     .pull_up_en   = GPIO_PULLUP_ENABLE,
    //     .pull_down_en = GPIO_PULLDOWN_DISABLE,
    //     .intr_type    = GPIO_INTR_DISABLE,
    // };
    // gpio_config(&in_cfg);

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

    ESP_LOGI(TAG, "GPIO initialized: PWR_SW=%d (expected %d)", gpio_get_level(GPIO_PWR_SW), LEVEL_IDLE);
}

// pc_power_state_t pc_get_power_state(void)  // 未使用: 電源状態検知はPing監視に切り替えたため不要
// {
//     int first = gpio_get_level(GPIO_PWR_LED);
//     vTaskDelay(DEBOUNCE_MS / portTICK_PERIOD_MS);
//     int second = gpio_get_level(GPIO_PWR_LED);
//
//     if (first != second) {
//         return PC_STATE_TRANSITIONING;
//     }
//     return (second == 0) ? PC_STATE_ON : PC_STATE_OFF;
// }

esp_err_t pc_execute_command(bool want_on)
{
    if (s_pressing) {
        ESP_LOGI(TAG, "Already pressing, ignored");
        return ESP_OK;
    }
    s_pressing = true;
    ESP_LOGI(TAG, "Pressing power button: %s", want_on ? "ON" : "OFF");

    // GPIO を即座に LEVEL_PRESS にしてからタイマーで LEVEL_IDLE に戻す。
    // 呼び出し元(Matterコールバック)がすぐにリターンでき、ACKを返せる。
    gpio_set_level(GPIO_PWR_SW, LEVEL_PRESS);
    esp_timer_start_once(s_release_timer, (uint64_t)BTN_PRESS_MS * 1000);
    return ESP_OK;
}

esp_err_t pc_execute_force_shutdown(void)
{
    if (s_pressing) {
        ESP_LOGI(TAG, "Already pressing, ignored");
        return ESP_OK;
    }
    s_pressing = true;
    ESP_LOGI(TAG, "Long pressing power button (force shutdown)");
    gpio_set_level(GPIO_PWR_SW, LEVEL_PRESS);
    esp_timer_start_once(s_release_timer, (uint64_t)BTN_LONGPRESS_MS * 1000);
    return ESP_OK;
}
