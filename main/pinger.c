#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "esp_attr.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "nvs_flash.h"

#include "apps/sntp/sntp.h"

#include "esp_http_client.h"
#include "esp_ping.h"
#include "ping.h"

#define DEFAULT_SSID "Wadsl-Mia"
#define DEFAULT_PWD "KernelCacheCompressionIsBoringButFun"
#define PING_COUNT 10
#define URL "http://192.168.10.104:3000"
#define TAG "Pinger"

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

typedef struct _ping_data_t {
    char timestamp[64];
    int total;
    int count;
    int max;
    int min;
    int resp;
    int timeouts;
} ping_data_t;

QueueHandle_t queue;

void ping_data_to_json(ping_data_t ping_data[], size_t ping_data_len, char json_string[])
{
    memset(json_string, 0, strlen(json_string));

    strcat(json_string, "[");

    for (size_t i = 0; i < ping_data_len; i++) {
        char tmp_str[256] = { 0 };

        sprintf(tmp_str,
            "{\"timestamp\":\"%s\", \"total\":%d, "
            "\"count\":%d,\"max\":%d,\"min\":%d, \"resp\":%d,\"timeouts\":%d},",
            ping_data[i].timestamp,
            ping_data[i].total,
            ping_data[i].count,
            ping_data[i].max,
            ping_data[i].min,
            ping_data[i].resp,
            ping_data[i].timeouts);
        strcat(json_string, tmp_str);

        memset(tmp_str, 0, strlen(tmp_str));
    }

    json_string[strlen(json_string) - 1] = ']';
}

static esp_err_t
event_handler(EventGroupHandle_t wifi_event_group, system_event_t* event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "Lost wifi connection");
        /* This is a workaround as ESP32 WiFi libs don't currently
         auto-reassociate. */
        // esp_wifi_connect();

        ESP_LOGI(TAG, "Not reconnecting, deiniting ping");
        ping_deinit();

        // Unset the CONNECTED_BIT on the wifi_event_group
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t
initialise_wifi(EventGroupHandle_t wifi_event_group)
{
    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, wifi_event_group));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = DEFAULT_SSID,
            .password = DEFAULT_PWD,
        },
    };

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t
_http_event_handler(esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG,
            "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
            evt->header_key,
            evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (!esp_http_client_is_chunked_response(evt->client)) {
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}

static esp_err_t
send_data(char* post_str)
{
    esp_http_client_config_t config = {
        .url = URL,
        .timeout_ms = 1000,
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_url(client, URL);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(
        client, post_str, strlen(post_str));
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
            "HTTP POST Status = %d, content_length = %d",
            esp_http_client_get_status_code(client),
            esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return ESP_OK;
}

static esp_err_t IRAM_ATTR
ping_results_callback(ping_target_id_t msgType, esp_ping_found* pf)
{
    // Get the time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Set timezone to Italy
    setenv("TZ", "CET-1CEST-2,M3.5.0/02:00:00,M10.5.0/03:00:00", 1);
    tzset();
    localtime_r(&now, &timeinfo);

    // Print the time
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    ESP_LOGI(TAG, "[%s]\tpf->send_count: %d", strftime_buf, pf->send_count);

    ping_data_t ping_data = { 0 };
    memset(ping_data.timestamp, 0, strlen(ping_data.timestamp));
    strcpy(ping_data.timestamp, strftime_buf);
    ping_data.total = pf->total_time;
    ping_data.count = pf->send_count;
    ping_data.max = pf->max_time;
    ping_data.min = pf->min_time;
    ping_data.resp = pf->resp_time;
    ping_data.timeouts = pf->timeout_count;

    // TODO: handle errQUEUE_FULL
    xQueueSendFromISR(queue, &ping_data, pdFALSE);

    return ESP_OK;
}
void ping_task(QueueHandle_t queue)
{
    ESP_LOGI(TAG, "Ping Task was started");
    while (1) {
        ip4_addr_t ip;

        // TODO: add config
        ip4addr_aton("1.1.1.1", &ip);
        uint32_t ping_count = PING_COUNT; // how many pings per report
        uint32_t ping_timeout = 750; // mS till we consider it timed out
        uint32_t ping_delay = 100; // mS between pings

        ESP_ERROR_CHECK(esp_ping_set_target(
            PING_TARGET_IP_ADDRESS_COUNT, &ping_count, sizeof(uint32_t)));
        ESP_ERROR_CHECK(esp_ping_set_target(
            PING_TARGET_RCV_TIMEO, &ping_timeout, sizeof(uint32_t)));
        ESP_ERROR_CHECK(esp_ping_set_target(
            PING_TARGET_DELAY_TIME, &ping_delay, sizeof(uint32_t)));
        ESP_ERROR_CHECK(
            esp_ping_set_target(PING_TARGET_IP_ADDRESS, &ip, sizeof(uint32_t)));
        ESP_ERROR_CHECK(esp_ping_set_target(PING_TARGET_RES_FN,
            &ping_results_callback,
            sizeof(ping_results_callback)));

        ping_init();

        // This should be enough for all the pings
        // TODO: add config
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
void http_client_task(QueueHandle_t queue)
{
    // The buffer is long PING_COUNT + 1 because ping_result will call the last result twice.
    ping_data_t ping_data_buffer[PING_COUNT + 1];
    size_t i = 0;

    ESP_LOGI(TAG, "HTTP Client Task was started");
    while (1) {
        // TODO: handle errQUEUE_EMPTY
        xQueueReceive(queue, &ping_data_buffer[i], portMAX_DELAY);
        ESP_LOGI(TAG, "received some data, i = %d", i);
        i++;

        if (i == PING_COUNT) {
            char post_data_buffer[4096];
            ping_data_to_json(ping_data_buffer, i, post_data_buffer);
            ESP_LOGI(TAG, "%s", post_data_buffer);
            ESP_ERROR_CHECK(send_data(post_data_buffer));
            i = 0;
        }
    }
}
void app_main()
{
    // Initialize NVS, Wifi and SNTP
    {
        ESP_LOGI(TAG, "Initializing NVS");
        ESP_ERROR_CHECK(nvs_flash_init());

        ESP_LOGI(TAG, "Initializing WIFI");
        EventGroupHandle_t wifi_event_group = xEventGroupCreate();
        ESP_ERROR_CHECK(initialise_wifi(wifi_event_group));
        xEventGroupWaitBits(
            wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

        ESP_LOGI(TAG, "Initializing SNTP");
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, "pool.ntp.org");
        sntp_init();
        time_t now = 0;
        struct tm timeinfo = { 0 };
        int retry = 0;
        const int retry_count = 10;
        while (timeinfo.tm_year < (2018 - 1900) && ++retry < retry_count) {
            ESP_LOGI(TAG,
                "Waiting for system time to be set... (%d/%d)",
                retry,
                retry_count);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            time(&now);
            localtime_r(&now, &timeinfo);
        }

        if (timeinfo.tm_year < (2018 - 1900)) {
            ESP_LOGE(TAG, "Could not set time after 10 retries");
            exit(1);
        }
    }
    queue = xQueueCreate(100, sizeof(ping_data_t));

    ESP_LOGI(TAG, "Starting Ping Task");
    xTaskCreate(&ping_task, "ping_task", 8192, queue, 5, NULL);

    ESP_LOGI(TAG, "Starting HTTP Client Task");
    xTaskCreate(&http_client_task, "http_client_task", 8192, queue, 5, NULL);
}