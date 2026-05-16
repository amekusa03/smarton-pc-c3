#pragma once
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PC_MONITOR_IDLE,
    PC_MONITOR_BOOTING,
    PC_MONITOR_SHUTTING_DOWN,
} pc_monitor_state_t;

// Matter起動後に呼ぶ。hostnameはmDNSホスト名（".local"なし）
esp_err_t pc_monitor_init(const char *hostname);
void pc_monitor_set_state(pc_monitor_state_t state);

#ifdef __cplusplus
}
#endif
