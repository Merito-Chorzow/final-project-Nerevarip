#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_http_server.h"

// --- KONFIGURACJA UŻYTKOWNIKA ---
#define EXAMPLE_ESP_WIFI_SSID      "TWOJA_NAZWA_SIECI"
#define EXAMPLE_ESP_WIFI_PASS      "TWOJE_HASLO"
#define LED_GPIO                   2  // Wbudowana dioda na ESP32

// --- DEFINICJE FSM (Maszyna Stanów) ---
typedef enum {
    STATE_INIT,
    STATE_CONNECTING,
    STATE_RUNNING,
    STATE_ERROR
} system_state_t;

static const char *TAG = "APP";
static system_state_t current_state = STATE_INIT;
static bool led_state = false;

// Grupa zdarzeń do obsługi Wi-Fi
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* --- 1. OBSŁUGA STRONY WWW (Endpoints) --- */

// Handler dla strony głównej (HTML + JS)
static esp_err_t root_get_handler(httpd_req_t *req) {
    const char* resp_str = 
        "<!DOCTYPE html>"
        "<html lang='pl'>"
        "<head>"
        "  <meta charset='utf-8'>"
        "  <meta name='viewport' content='width=device-width, initial-scale=1'>"
        "  <title>ESP32 Control</title>"
        "  <style>"
        "    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background-color: #121212; color: #e0e0e0; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }"
        "    .card { background-color: #1e1e1e; padding: 2rem; border-radius: 16px; box-shadow: 0 4px 20px rgba(0,0,0,0.5); text-align: center; width: 300px; }"
        "    h1 { margin-bottom: 0.5rem; font-size: 1.5rem; color: #ffffff; }"
        "    .status-box { background: #2c2c2c; padding: 15px; border-radius: 10px; margin: 20px 0; display: flex; justify-content: space-around; }"
        "    .stat { display: flex; flex-direction: column; }"
        "    .stat-label { font-size: 0.8rem; color: #aaaaaa; }"
        "    .stat-val { font-size: 1.2rem; font-weight: bold; color: #4caf50; }"
        "    .btn-group { display: flex; gap: 10px; justify-content: center; margin-top: 20px; }"
        "    button { border: none; padding: 12px 20px; border-radius: 8px; font-weight: bold; cursor: pointer; transition: transform 0.1s, opacity 0.2s; flex: 1; }"
        "    button:active { transform: scale(0.95); }"
        "    .btn-on { background: #4caf50; color: white; }"
        "    .btn-off { background: #f44336; color: white; }"
        "    .indicator { width: 10px; height: 10px; border-radius: 50%; display: inline-block; margin-right: 5px; }"
        "  </style>"
        "</head>"
        "<body>"
        "  <div class='card'>"
        "    <h1>ESP32 Controller</h1>"
        "    <div class='status-box'>"
        "      <div class='stat'><span class='stat-label'>DIODA</span><span id='st_txt' class='stat-val'>...</span></div>"
        "      <div class='stat'><span class='stat-label'>TEMP</span><span class='stat-val'><span id='tmp'>--</span>&deg;C</span></div>"
        "    </div>"
        "    <div class='btn-group'>"
        "      <button class='btn-on' onclick=\"setLed(1)\">ON</button>"
        "      <button class='btn-off' onclick=\"setLed(0)\">OFF</button>"
        "    </div>"
        "  </div>"

        "  <script>"
        "    function setLed(s) { fetch('/set?led='+s); }"
        "    setInterval(() => {"
        "      fetch('/status').then(r => r.json()).then(d => {"
        "        document.getElementById('st_txt').innerText = d.led ? 'WŁĄCZONA' : 'WYŁĄCZONA';"
        "        document.getElementById('st_txt').style.color = d.led ? '#4caf50' : '#f44336';"
        "        document.getElementById('tmp').innerText = d.temp;"
        "      });"
        "    }, 1000);"
        "  </script>"
        "</body>"
        "</html>";

    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler dla JSON statusu (/status)
static esp_err_t status_get_handler(httpd_req_t *req) {
    char json_resp[100];
    // Symulacja sensora (losowa temperatura 20-30st)
    int temp = 20 + (rand() % 10);
    
    snprintf(json_resp, sizeof(json_resp), "{\"led\":%s, \"temp\":%d, \"uptime\":%lu}",
             led_state ? "true" : "false", temp, xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
             
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler sterowania (/set?led=1)
static esp_err_t set_get_handler(httpd_req_t *req) {
    char buf[10];
    char param[5];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "led", param, sizeof(param)) == ESP_OK) {
            int val = atoi(param);
            led_state = (val == 1);
            gpio_set_level(LED_GPIO, led_state);
            ESP_LOGI(TAG, "Zmiana stanu LED na: %d", led_state);
        }
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Rejestracja adresów URL
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler };
        httpd_uri_t set_uri = { .uri = "/set", .method = HTTP_GET, .handler = set_get_handler };

        httpd_register_uri_handler(server, &root_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &set_uri);
        return server;
    }
    return NULL;
}

/* --- 2. OBSŁUGA WIFI (Driver) --- */

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Ponawiam polaczenie z WiFi...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Dostalem IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = { .ssid = EXAMPLE_ESP_WIFI_SSID, .password = EXAMPLE_ESP_WIFI_PASS, .threshold.authmode = WIFI_AUTH_WPA2_PSK, },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* --- 3. MAIN (Pętla FSM) --- */

void app_main(void) {
    // Inicjalizacja NVS (pamięć flash)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Konfiguracja GPIO (LED)
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    while(1) {
        // --- MASZYNA STANÓW (FSM) ---
        switch (current_state) {
            case STATE_INIT:
                ESP_LOGI(TAG, "[FSM] INIT -> Startuje WiFi");
                wifi_init_sta();
                current_state = STATE_CONNECTING;
                break;

            case STATE_CONNECTING:
                // Czekamy na bit WIFI_CONNECTED_BIT
                if (xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(1000))) {
                    ESP_LOGI(TAG, "[FSM] CONNECTED -> Startuje Webserver");
                    start_webserver();
                    current_state = STATE_RUNNING;
                } else {
                    // Miganie diodą podczas łączenia
                    gpio_set_level(LED_GPIO, !led_state); 
                    led_state = !led_state;
                }
                break;

            case STATE_RUNNING:
                // Normalna praca - system czeka na zapytania HTTP w tle
                // Tutaj np. co jakiś czas logujemy "System OK"
                vTaskDelay(pdMS_TO_TICKS(5000));
                ESP_LOGI(TAG, "[FSM] RUNNING - System dziala, RAM: %lu", esp_get_free_heap_size());
                break;

            case STATE_ERROR:
                // Reset po błędzie
                esp_restart();
                break;
        }
    }
}