#include "http_server.h"
#include "servo.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "HTTP_SERVER";

// NVS definitions
#define NVS_NAMESPACE "pill_disp"
#define NVS_KEY_FILLED "filled_slots"

// Array to store the filled status of each slot
uint8_t filled_slots_status[NUM_SLOTS] = {0};

// Mutex for protecting access to filled_slots_status
static SemaphoreHandle_t nvs_mutex = NULL;

// Event group for WiFi connection status
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

// Function declarations
void slot_to_day_dose(int slot, char *day, size_t day_size, int *dose);
esp_err_t servo_dispense_pill(int slot);

// Convert slot number to day/dose string
void slot_to_day_dose_string(int slot, char *out_str, size_t max_len) {
    const char *days[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    if (slot < 0 || slot >= NUM_SLOTS) {
        snprintf(out_str, max_len, "Invalid Slot");
        return;
    }
    int day_index = slot / 2;
    int dose_num = (slot % 2) + 1;
    if (day_index < 6) {
        snprintf(out_str, max_len, "%s Dose %d", days[day_index], dose_num);
    } else {
        snprintf(out_str, max_len, "Error Slot");
    }
}

// NVS Functions
esp_err_t nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash due to init error: %s", esp_err_to_name(ret));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t nvs_read_filled_slots() {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    size_t required_size = sizeof(filled_slots_status);
    err = nvs_get_blob(nvs_handle, NVS_KEY_FILLED, filled_slots_status, &required_size);

    if (err == ESP_OK) {
        if (required_size != sizeof(filled_slots_status)) {
            ESP_LOGW(TAG, "NVS BLOB size mismatch (%d vs %d expected). Resetting.", (int)required_size, (int)sizeof(filled_slots_status));
            memset(filled_slots_status, 0, sizeof(filled_slots_status));
            err = ESP_ERR_NVS_INVALID_LENGTH;
        } else {
            ESP_LOGI(TAG, "Successfully read filled slots status from NVS.");
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Filled slots status not found in NVS. Initializing to empty.");
        memset(filled_slots_status, 0, sizeof(filled_slots_status));
        err = nvs_set_blob(nvs_handle, NVS_KEY_FILLED, filled_slots_status, sizeof(filled_slots_status));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) writing initial empty slots to NVS!", esp_err_to_name(err));
        } else {
            err = nvs_commit(nvs_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) committing initial empty slots to NVS!", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Initial empty slots saved to NVS.");
                err = ESP_OK;
            }
        }
    } else {
        ESP_LOGE(TAG, "Error (%s) reading filled slots status from NVS!", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return err;
}

esp_err_t nvs_write_filled_slots() {
    nvs_handle_t nvs_handle;
    esp_err_t err;

    if (!nvs_mutex) return ESP_FAIL;

    if (xSemaphoreTake(nvs_mutex, portMAX_DELAY) == pdTRUE) {
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) opening NVS handle for writing!", esp_err_to_name(err));
            xSemaphoreGive(nvs_mutex);
            return err;
        }

        err = nvs_set_blob(nvs_handle, NVS_KEY_FILLED, filled_slots_status, sizeof(filled_slots_status));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) writing filled slots status to NVS!", esp_err_to_name(err));
        } else {
            err = nvs_commit(nvs_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) committing filled slots status to NVS!", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Successfully wrote filled slots status to NVS.");
            }
        }

        nvs_close(nvs_handle);
        xSemaphoreGive(nvs_mutex);
    } else {
        ESP_LOGE(TAG, "Could not obtain NVS mutex for writing.");
        err = ESP_FAIL;
    }
    return err;
}

// WiFi Event Handler
static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// WiFi Initialization
void wifi_init_sta(EventGroupHandle_t wifi_event_group)
{
    s_wifi_event_group = wifi_event_group;

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// HTTP Request Handlers
static esp_err_t root_get_handler(httpd_req_t *req)
{
    extern const char index_html_start[] asm("_binary_index_html_start");
    extern const char index_html_end[] asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t script_js_get_handler(httpd_req_t *req)
{
    extern const char script_js_start[] asm("_binary_script_js_start");
    extern const char script_js_end[] asm("_binary_script_js_end");
    const size_t script_js_size = (script_js_end - script_js_start);
    
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, script_js_start, script_js_size);
    return ESP_OK;
}

static esp_err_t add_dose_handler(httpd_req_t *req)
{
    char content[100];
    int ret, remaining = req->content_len;
    
    if (remaining > sizeof(content) - 1) {
        remaining = sizeof(content) - 1;
    }
    
    ret = httpd_req_recv(req, content, remaining);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Invalid JSON");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        return ESP_FAIL;
    }
    
    cJSON *day = cJSON_GetObjectItem(root, "day");
    cJSON *dose = cJSON_GetObjectItem(root, "dose");
    
    if (!day || !dose || !cJSON_IsString(day) || !cJSON_IsNumber(dose)) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Missing or invalid day or dose");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    int slot = day_dose_to_slot(day->valuestring, dose->valueint);
    if (slot < 0) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Invalid day or dose");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    if (filled_slots_status[slot]) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Slot already filled");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    // Move servo to position
    int angle = servo_positions[slot];
    servo_set_angle(angle);
    
    // Mark slot as filled
    filled_slots_status[slot] = 1;
    nvs_write_filled_slots();
    
    // Create success response
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    char msg[100];
    char day_dose_str[50];
    slot_to_day_dose_string(slot, day_dose_str, sizeof(day_dose_str));
    snprintf(msg, sizeof(msg), "Added: %s", day_dose_str);
    cJSON_AddStringToObject(response, "message", msg);
    
    char *response_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_str, strlen(response_str));
    
    free(response_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t remove_dose_handler(httpd_req_t *req)
{
    char content[100];
    int ret, remaining = req->content_len;
    
    if (remaining > sizeof(content) - 1) {
        remaining = sizeof(content) - 1;
    }
    
    ret = httpd_req_recv(req, content, remaining);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Invalid JSON");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        return ESP_FAIL;
    }
    
    cJSON *day = cJSON_GetObjectItem(root, "day");
    cJSON *dose = cJSON_GetObjectItem(root, "dose");
    
    if (!day || !dose) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Missing day or dose");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    int slot = day_dose_to_slot(day->valuestring, dose->valueint);
    if (slot < 0) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Invalid day or dose");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    if (!filled_slots_status[slot]) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Slot already empty");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    filled_slots_status[slot] = 0;
    nvs_write_filled_slots();
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    
    char *response_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_str, strlen(response_str));
    
    free(response_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t get_filled_doses_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *filled_doses = cJSON_CreateArray();
    
    for (int i = 0; i < NUM_SLOTS; i++) {
        if (filled_slots_status[i]) {
            char day_str[20];
            int dose;
            slot_to_day_dose(i, day_str, sizeof(day_str), &dose);
            
            cJSON *dose_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(dose_obj, "day", day_str);
            cJSON_AddNumberToObject(dose_obj, "dose", dose);
            cJSON_AddItemToArray(filled_doses, dose_obj);
        }
    }
    
    cJSON_AddItemToObject(root, "filled_doses", filled_doses);
    
    char *response_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_str, strlen(response_str));
    
    free(response_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t dispense_handler(httpd_req_t *req)
{
    char content[100];
    int ret, remaining = req->content_len;
    
    if (remaining > sizeof(content) - 1) {
        remaining = sizeof(content) - 1;
    }
    
    ret = httpd_req_recv(req, content, remaining);
    if (ret <= 0) {
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    cJSON *root = cJSON_Parse(content);
    if (!root) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Invalid JSON");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        return ESP_FAIL;
    }
    
    cJSON *day = cJSON_GetObjectItem(root, "day");
    cJSON *dose = cJSON_GetObjectItem(root, "dose");
    
    if (!day || !dose) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Missing day or dose");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    int slot = day_dose_to_slot(day->valuestring, dose->valueint);
    if (slot < 0) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Invalid day or dose");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    if (!filled_slots_status[slot]) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Slot is empty");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    esp_err_t err = servo_dispense_pill(slot);
    if (err != ESP_OK) {
        // Return JSON error response
        cJSON *error = cJSON_CreateObject();
        cJSON_AddBoolToObject(error, "success", false);
        cJSON_AddStringToObject(error, "message", "Failed to dispense pill");
        char *error_str = cJSON_PrintUnformatted(error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, error_str, strlen(error_str));
        free(error_str);
        cJSON_Delete(error);
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    filled_slots_status[slot] = 0;
    nvs_write_filled_slots();
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    
    char *response_str = cJSON_PrintUnformatted(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response_str, strlen(response_str));
    
    free(response_str);
    cJSON_Delete(response);
    cJSON_Delete(root);
    return ESP_OK;
}

void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);
        
        httpd_uri_t script = {
            .uri = "/script.js",
            .method = HTTP_GET,
            .handler = script_js_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &script);
        
        httpd_uri_t add_dose = {
            .uri = "/add_dose",
            .method = HTTP_POST,
            .handler = add_dose_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &add_dose);
        
        httpd_uri_t remove_dose = {
            .uri = "/remove_dose",
            .method = HTTP_POST,
            .handler = remove_dose_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &remove_dose);
        
        httpd_uri_t get_filled_doses = {
            .uri = "/get_filled_doses",
            .method = HTTP_GET,
            .handler = get_filled_doses_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &get_filled_doses);
        
        httpd_uri_t dispense = {
            .uri = "/dispense",
            .method = HTTP_POST,
            .handler = dispense_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &dispense);
    }
}
