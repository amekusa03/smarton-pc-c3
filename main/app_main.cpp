#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_netif.h>
#include <driver/gpio.h>

#include <esp_matter.h>
#include "pc_control.h"
#include "pc_monitor.h"
#include "wifi_creds.h"

#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

#define GPIO_FACTORY_RESET    GPIO_NUM_9
#define FACTORY_RESET_HOLD_MS 3000

static void factory_reset_task(void *)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_9),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    while (true) {
        if (gpio_get_level(GPIO_FACTORY_RESET) == 0) {
            int held = 0;
            while (gpio_get_level(GPIO_FACTORY_RESET) == 0 && held < FACTORY_RESET_HOLD_MS) {
                vTaskDelay(100 / portTICK_PERIOD_MS);
                held += 100;
            }
            if (held >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGI(TAG, "Factory reset triggered");
                esp_matter::factory_reset();
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged: {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t info;
            if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
                char ip[20];
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
                ESP_LOGI(TAG, "IP: %s", ip);
                static bool s_monitor_started = false;
                if (!s_monitor_started) {
                    s_monitor_started = true;
                    pc_monitor_init(PC_HOSTNAME);
                }
            }
        }
        break;
    }
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        break;
    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                        uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                          uint32_t cluster_id, uint32_t attribute_id,
                                          esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != PRE_UPDATE) {
        return ESP_OK;
    }
    if (cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id) {
        return ESP_OK;
    }
    // pc_execute_command は s_pressing フラグで再入を防ぐ。
    // Matter再送が複数回呼んでも1回しかボタンを押さない。
    pc_monitor_set_state(val->val.b ? PC_MONITOR_BOOTING : PC_MONITOR_SHUTTING_DOWN);
    return pc_execute_command(val->val.b);
}

static void store_wifi_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open_from_partition("nvs", "CHIP_KVS", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open CHIP_KVS namespace");
        return;
    }
    nvs_set_blob(nvs, "wifi-ssid", WIFI_SSID, strlen(WIFI_SSID));
    nvs_set_blob(nvs, "wifi-pass", WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi credentials stored: %s", WIFI_SSID);
}

extern "C" void app_main()
{
    nvs_flash_init();
    pc_control_init();
    store_wifi_credentials();

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    on_off_plug_in_unit::config_t plug_config;
    plug_config.on_off.on_off = false;
    endpoint_t *endpoint = on_off_plug_in_unit::create(node, &plug_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create endpoint");
        return;
    }

    ESP_LOGI(TAG, "Endpoint ID: %d", endpoint::get_id(endpoint));

    esp_err_t err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork));

    xTaskCreate(factory_reset_task, "factory_reset", 4096, nullptr, 1, nullptr);
}
