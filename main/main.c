#include "http_server.h"
#include "servo.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"

static const char *TAG = "ASHUMITRA_SERVER";

// Event group for WiFi connection status
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT        BIT1

// Mutex for NVS access
SemaphoreHandle_t nvs_mutex;

// Function declarations
esp_err_t nvs_init(void);
esp_err_t nvs_read_filled_slots(void);

void app_main(void)
{
    // Initialize NVS first
    ESP_ERROR_CHECK(nvs_init());

    // Create Mutex for shared NVS/RAM data access
    nvs_mutex = xSemaphoreCreateMutex();
    if (!nvs_mutex) {
        ESP_LOGE(TAG, "Failed to create NVS mutex!");
        return;
    }

    // Create WiFi event group
    EventGroupHandle_t wifi_event_group = xEventGroupCreate();
    if (!wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create WiFi event group!");
        return;
    }

    // Load initial filled slots status from NVS into RAM
    if (nvs_read_filled_slots() != ESP_OK) {
        ESP_LOGW(TAG, "Issues reading initial NVS data, proceeding with default (empty).");
    }

    // Initialize servo
    servo_init();
    ESP_LOGI(TAG, "Servo initialized on GPIO %d", SERVO_GPIO_PIN);

    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_init_sta(wifi_event_group);

    // Start the web server only if WiFi connected successfully
    if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Starting web server...");
        start_webserver();
        // Set servo to initial/home position
        vTaskDelay(pdMS_TO_TICKS(500));
        servo_set_angle(servo_positions[0]);
        ESP_LOGI(TAG, "Servo set to initial position: %d degrees (Slot 0)", servo_positions[0]);
    } else {
        ESP_LOGE(TAG, "WiFi connection failed. Web server not started.");
    }

    ESP_LOGI(TAG, "ASHUMITRA application started.");
}
