#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------
// GPIO割り当て
//
// GPIO_PWR_SW = 20 を選んだ理由:
//   ESP32-C6 では GPIO 12 = USB D-, GPIO 13 = USB D+ として
//   USB Serial JTAG に使われる。USB接続中はホストがこれらのピンを
//   駆動するため、GPIO出力と競合してLEDが不規則に点滅する。
//   GPIO 20 はUSB機能を持たない汎用GPIOなので競合しない。
//
// フォトカプラ回路 (Board B, アクティブHIGH):
//   GPIO 20 → 200Ω → PC817 LED → GND
//   GPIO HIGH(3.3V): 電流 ≈ (3.3-1.1)/200 = 11mA → LED点灯 → PWR_SW短絡
//   GPIO LOW (0V) : 電流 0mA              → LED消灯 → PWR_SW開放
// -----------------------------------------------------------------------
#define GPIO_PWR_SW   20  // OUTPUT: フォトカプラ経由でPWR SWピンを短絡
#define GPIO_PWR_LED  4   // INPUT:  フォトカプラ経由でPWR LEDピンを読む
                          // 注意: GPIO 4 はLCDのRSTピンと共有。
                          //       pc_get_power_state()の読み値は不正確になる場合がある。

// 0=実機モード, 1=ループバックテストモード
#define PC_LOOPBACK_TEST 0

typedef enum {
    PC_STATE_OFF,
    PC_STATE_ON,
    PC_STATE_TRANSITIONING,
} pc_power_state_t;

void pc_control_init(void);
pc_power_state_t pc_get_power_state(void);

// Matterコールバックから呼ばれる。s_pressing フラグで再入を防ぐ。
// 再送パケットが複数回呼び出しても1回だけ実行される。
esp_err_t pc_execute_command(bool want_on);

// フリーズ検知時に呼ぶ。電源ボタン長押し（強制電源断）をシミュレートする。
esp_err_t pc_execute_force_shutdown(void);

#ifdef __cplusplus
}
#endif
