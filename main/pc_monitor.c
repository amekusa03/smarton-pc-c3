#include "pc_monitor.h"
#include "pc_control.h"

#include <string.h>
#include <mdns.h>
#include <ping/ping_sock.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

static const char *TAG = "pc_monitor";

#define PING_INTERVAL_MS     5000
#define PING_TIMEOUT_MS      2000
#define MDNS_TIMEOUT_MS      3000
#define IP_FAIL_THRESHOLD    3
#define MDNS_FAIL_THRESHOLD  2
#define BOOT_TIMEOUT_MS      60000   // 実測値により変更する
#define SHUTDOWN_TIMEOUT_MS  30000

static volatile pc_monitor_state_t s_state   = PC_MONITOR_IDLE;
static volatile TickType_t         s_state_at = 0;
static char     s_hostname[64];
static uint32_t s_cached_ip = 0;

static SemaphoreHandle_t s_ping_sem;
static volatile bool     s_ping_result;

static void on_ping_end(esp_ping_handle_t h, void *arg)
{
    uint32_t received = 0;
    esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &received, sizeof(received));
    s_ping_result = (received > 0);
    xSemaphoreGive(s_ping_sem);
}

static bool ping_once(uint32_t ip)
{
    char ipstr[16];
    inet_ntop(AF_INET, &ip, ipstr, sizeof(ipstr));

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr.u_addr.ip4.addr = ip;
    cfg.target_addr.type = ESP_IPADDR_TYPE_V4;
    cfg.count = 1;
    cfg.timeout_ms = PING_TIMEOUT_MS;

    esp_ping_callbacks_t cbs = { .on_ping_end = on_ping_end };

    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        ESP_LOGE(TAG, "ping session failed: %s", ipstr);
        return false;
    }

    s_ping_result = false;
    esp_ping_start(ping);
    BaseType_t ok = xSemaphoreTake(s_ping_sem, pdMS_TO_TICKS(PING_TIMEOUT_MS + 1000));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);

    ESP_LOGI(TAG, "ping %s -> %s", ipstr, s_ping_result ? "OK" : (ok ? "NG" : "TIMEOUT"));
    return s_ping_result;
}

static bool resolve_mdns(void)
{
    char fqdn[80];
    snprintf(fqdn, sizeof(fqdn), "%s.local", s_hostname);

    struct addrinfo hints = { .ai_family = AF_INET };
    struct addrinfo *res = NULL;

    if (getaddrinfo(fqdn, NULL, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "mDNS failed: %s", fqdn);
        return false;
    }

    s_cached_ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    char buf[16];
    inet_ntop(AF_INET, &s_cached_ip, buf, sizeof(buf));
    ESP_LOGI(TAG, "mDNS: %s -> %s", fqdn, buf);
    freeaddrinfo(res);
    return true;
}

static void monitor_task(void *arg)
{
    while (!resolve_mdns()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    int ip_fails        = 0;
    int unreachable_rounds = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));

        pc_monitor_state_t state = s_state;
        uint32_t elapsed_ms = (xTaskGetTickCount() - s_state_at) * portTICK_PERIOD_MS;

        if (state == PC_MONITOR_BOOTING) {
            if (elapsed_ms >= BOOT_TIMEOUT_MS) {
                ESP_LOGW(TAG, "Boot timeout, resuming monitor");
                s_state = PC_MONITOR_IDLE;
            } else if (resolve_mdns() && ping_once(s_cached_ip)) {
                ESP_LOGI(TAG, "PC booted (%.1fs)", elapsed_ms / 1000.0f);
                s_state = PC_MONITOR_IDLE;
            }
            ip_fails = unreachable_rounds = 0;
            continue;
        }

        if (state == PC_MONITOR_SHUTTING_DOWN) {
            if (elapsed_ms >= SHUTDOWN_TIMEOUT_MS || !ping_once(s_cached_ip)) {
                ESP_LOGI(TAG, "PC shut down");
                s_state = PC_MONITOR_IDLE;
            }
            ip_fails = unreachable_rounds = 0;
            continue;
        }

        // IDLE: 死活監視
        if (ping_once(s_cached_ip)) {
            ip_fails = unreachable_rounds = 0;
            continue;
        }

        if (++ip_fails < IP_FAIL_THRESHOLD) {
            ESP_LOGW(TAG, "Ping fail %d/%d", ip_fails, IP_FAIL_THRESHOLD);
            continue;
        }

        // IP連続失敗 → mDNSで再確認
        ip_fails = 0;
        bool mdns_ok = resolve_mdns();
        bool ping_ok = mdns_ok && ping_once(s_cached_ip);
        ESP_LOGW(TAG, "Unreachable check: mDNS=%s ping=%s (round %d/%d)",
                 mdns_ok ? "OK" : "NG", ping_ok ? "OK" : "NG",
                 unreachable_rounds + 1, MDNS_FAIL_THRESHOLD);

        if (ping_ok) {
            ESP_LOGI(TAG, "PC alive (IP may have changed)");
            unreachable_rounds = 0;
            continue;
        }

        if (++unreachable_rounds < MDNS_FAIL_THRESHOLD) {
            continue;
        }

        // フリーズ確定
        ESP_LOGE(TAG, "Freeze detected! Force shutdown.");
        unreachable_rounds = 0;
        pc_execute_force_shutdown();
        pc_monitor_set_state(PC_MONITOR_BOOTING);
    }
}

void pc_monitor_set_state(pc_monitor_state_t state)
{
    static const char *names[] = {"IDLE", "BOOTING", "SHUTTING_DOWN"};
    ESP_LOGI(TAG, "-> %s", names[state]);
    s_state    = state;
    s_state_at = xTaskGetTickCount();
}

esp_err_t pc_monitor_init(const char *hostname)
{
    strlcpy(s_hostname, hostname, sizeof(s_hostname));
    s_ping_sem = xSemaphoreCreateBinary();
    if (!s_ping_sem) return ESP_ERR_NO_MEM;

    xTaskCreate(monitor_task, "pc_monitor", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "Started for %s.local", hostname);
    return ESP_OK;
}
