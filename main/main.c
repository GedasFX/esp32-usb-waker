#include <stdio.h>
#include <string.h>
#include "class/hid/hid_device.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "secrets.h"

#define HOSTNAME "quest-waker"

static volatile bool remote_wakeup_enabled;

enum { REPORT_KEYBOARD = 1, REPORT_SYSTEM = 2 };

// System control report values, in the order of its usages.
enum { SYSTEM_RELEASE = 0, SYSTEM_SLEEP = 2 };

static const uint8_t report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_KEYBOARD)),
    TUD_HID_REPORT_DESC_SYSTEM_CONTROL(HID_REPORT_ID(REPORT_SYSTEM)),
};

static const char *string_descriptor[] = {
    (char[]){0x09, 0x04},
    "Snappi",
    "Quest waker",
    "1",
    "Quest waker keyboard",
};

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(report_descriptor), 0x81, 16, 10),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
}

// The host says, as it suspends the bus, whether it lets this device wake it.
static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_SUSPENDED) {
        remote_wakeup_enabled = event->suspended.remote_wakeup;
    } else if (event->id == TINYUSB_EVENT_RESUMED) {
        remote_wakeup_enabled = false;
    }
}

static void start_usb(void)
{
    // No light sleep: the S3's USB can't wake it, so after a host restart it slept through
    // enumeration and Windows gave up on it
    esp_pm_config_t power = {.max_freq_mhz = 80, .min_freq_mhz = 80, .light_sleep_enable = false};
    ESP_ERROR_CHECK(esp_pm_configure(&power));

    // Describe the keyboard
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(on_usb_event);
    config.descriptor.full_speed_config = configuration_descriptor;
    config.descriptor.string = string_descriptor;
    config.descriptor.string_count = sizeof(string_descriptor) / sizeof(string_descriptor[0]);

    ESP_ERROR_CHECK(tinyusb_driver_install(&config));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    }
}

static void start_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, HOSTNAME));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));

    // Join the network and keep rejoining it
    wifi_config_t config = {.sta = {.ssid = WIFI_SSID, .password = WIFI_PASSWORD, .listen_interval = 10}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // The radio wakes for every 10th beacon, about once a second
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(HOSTNAME));
}

static const char *usb_state(void)
{
    if (!tud_mounted()) {
        return "unmounted";
    }

    return tud_suspended() ? "suspended" : "awake";
}

// Compares in constant time, so the reply's timing leaks nothing of the token.
static bool token_matches(const char *given)
{
    size_t length = strlen(WAKE_TOKEN);
    if (strlen(given) != length) {
        return false;
    }

    unsigned char diff = 0;
    for (size_t i = 0; i < length; i++) {
        diff |= given[i] ^ WAKE_TOKEN[i];
    }

    return diff == 0;
}

static bool authorized(httpd_req_t *request)
{
    char header[160];
    if (httpd_req_get_hdr_value_str(request, "Authorization", header, sizeof(header)) != ESP_OK) {
        return false;
    }

    const char *prefix = "Bearer ";
    if (strncmp(header, prefix, strlen(prefix)) != 0) {
        return false;
    }

    return token_matches(header + strlen(prefix));
}

static esp_err_t reply(httpd_req_t *request, const char *status, const char *body)
{
    httpd_resp_set_status(request, status);

    return httpd_resp_sendstr(request, body);
}

static esp_err_t get_status(httpd_req_t *request)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"usb\":\"%s\",\"remoteWakeup\":%s}\n", usb_state(), remote_wakeup_enabled ? "true" : "false");
    httpd_resp_set_type(request, "application/json");

    return httpd_resp_sendstr(request, body);
}

static esp_err_t post_wake(httpd_req_t *request)
{
    if (!authorized(request)) {
        return reply(request, "401 Unauthorized", "bad token\n");
    }
    if (!tud_mounted()) {
        return reply(request, "409 Conflict", "usb unmounted: quest is off, hibernating or unplugged\n");
    }
    if (!tud_suspended()) {
        return reply(request, "200 OK", "awake\n");
    }
    if (tinyusb_remote_wakeup() != ESP_OK) {
        return reply(request, "409 Conflict", "the host did not enable remote wakeup\n");
    }

    return reply(request, "202 Accepted", "waking\n");
}

// Windows ignores a shorter press; with quest's display off, the first press only lights it.
static esp_err_t post_sleep(httpd_req_t *request)
{
    if (!authorized(request)) {
        return reply(request, "401 Unauthorized", "bad token\n");
    }
    if (!tud_mounted() || tud_suspended() || !tud_hid_ready()) {
        return reply(request, "409 Conflict", "quest is not awake\n");
    }

    // Press and release the sleep button
    uint8_t value = SYSTEM_SLEEP;
    tud_hid_report(REPORT_SYSTEM, &value, sizeof(value));
    vTaskDelay(pdMS_TO_TICKS(500));
    value = SYSTEM_RELEASE;
    tud_hid_report(REPORT_SYSTEM, &value, sizeof(value));

    return reply(request, "202 Accepted", "sleeping\n");
}

static void start_http(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = get_status};
    httpd_uri_t wake = {.uri = "/wake", .method = HTTP_POST, .handler = post_wake};
    httpd_uri_t sleep = {.uri = "/sleep", .method = HTTP_POST, .handler = post_sleep};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wake));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sleep));
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    start_usb();
    start_wifi();
    start_http();
}
