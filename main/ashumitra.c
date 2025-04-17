#include <stdio.h>
#include <string.h>
#include <stdlib.h> // Required for atoi
#include <ctype.h>  // Required for toupper
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h" // Required for Mutex
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"      // Required for NVS operations
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"    // Required for JSON handlingfsedf

// WiFi credentials - replace with your own
#define WIFI_SSID      "Delta_Virus_2.4G" // *** REPLACE WITH YOUR WIFI SSID ***
#define WIFI_PASS      "66380115" // *** REPLACE WITH YOUR WIFI PASSWORD ***
#define MAX_RETRY      5

// Servo control parameters (Unchanged)
#define SERVO_GPIO_PIN 2
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_CHANNEL LEDC_CHANNEL_0
#define SERVO_RESOLUTION LEDC_TIMER_13_BIT
#define SERVO_FREQ 50
#define SERVO_MIN_PULSEWIDTH 500
#define SERVO_MAX_PULSEWIDTH 2500

// Number of slots for servo positions (Monday Dose 1/2 ... Saturday Dose 1)
#define NUM_SLOTS 11

// Array to store preset servo positions (in degrees) for each slot
// Slot 0: Mon D1, Slot 1: Mon D2, Slot 2: Tue D1, ..., Slot 10: Sat D1
int servo_positions[NUM_SLOTS] = {0, 17, 34, 52, 69, 86, 103, 121, 138, 155, 172};

// NVS definitions
#define NVS_NAMESPACE "pill_disp"
#define NVS_KEY_FILLED "filled_slots"

// Array to store the filled status of each slot (in RAM, loaded from NVS)
// 0 = empty, 1 = filled
uint8_t filled_slots_status[NUM_SLOTS] = {0}; // Initialize all to empty

// Mutex for protecting access to filled_slots_status (good practice if multiple tasks might access)
static SemaphoreHandle_t nvs_mutex = NULL;

static const char *TAG = "ASHUMITRA_SERVER";

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

// --- NVS Functions ---
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
            memset(filled_slots_status, 0, sizeof(filled_slots_status)); // Reset to default (all empty)
            err = ESP_ERR_NVS_INVALID_LENGTH; // Indicate an issue happened
        } else {
            ESP_LOGI(TAG, "Successfully read filled slots status from NVS.");
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Filled slots status not found in NVS. Initializing to empty.");
        memset(filled_slots_status, 0, sizeof(filled_slots_status));
        // Optionally write the initial empty state back to NVS here
        err = nvs_set_blob(nvs_handle, NVS_KEY_FILLED, filled_slots_status, sizeof(filled_slots_status));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) writing initial empty slots to NVS!", esp_err_to_name(err));
        } else {
             err = nvs_commit(nvs_handle); // Commit changes
             if (err != ESP_OK) {
                 ESP_LOGE(TAG, "Error (%s) committing initial empty slots to NVS!", esp_err_to_name(err));
             } else {
                 ESP_LOGI(TAG, "Initial empty slots saved to NVS.");
                 err = ESP_OK; // Reset error code to OK as we handled the "not found" case
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

    if (!nvs_mutex) return ESP_FAIL; // Should not happen if initialized correctly

    // Lock mutex before accessing NVS
    if (xSemaphoreTake(nvs_mutex, portMAX_DELAY) == pdTRUE) {
        err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) opening NVS handle for writing!", esp_err_to_name(err));
            xSemaphoreGive(nvs_mutex); // Release mutex on error
            return err;
        }

        err = nvs_set_blob(nvs_handle, NVS_KEY_FILLED, filled_slots_status, sizeof(filled_slots_status));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) writing filled slots status to NVS!", esp_err_to_name(err));
        } else {
            err = nvs_commit(nvs_handle); // Commit changes
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) committing filled slots status to NVS!", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Successfully wrote filled slots status to NVS.");
            }
        }

        nvs_close(nvs_handle);
        xSemaphoreGive(nvs_mutex); // Release mutex
    } else {
        ESP_LOGE(TAG, "Could not obtain NVS mutex for writing.");
        err = ESP_FAIL;
    }
    return err;
}


// --- Servo Functions (Unchanged) ---
void servo_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_RESOLUTION,
        .timer_num = SERVO_TIMER,
        .freq_hz = SERVO_FREQ,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t channel_conf = {
        .gpio_num = SERVO_GPIO_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = SERVO_CHANNEL,
        .timer_sel = SERVO_TIMER,
        .duty = 0,
        .hpoint = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));
}

uint32_t servo_angle_to_duty(int angle)
{
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    uint32_t pulse_width = SERVO_MIN_PULSEWIDTH + (((SERVO_MAX_PULSEWIDTH - SERVO_MIN_PULSEWIDTH) * angle) / 180);
    return (pulse_width * ((1 << SERVO_RESOLUTION) - 1)) / (1000000 / SERVO_FREQ);
}

void servo_set_angle(int angle)
{
    uint32_t duty = servo_angle_to_duty(angle);
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, SERVO_CHANNEL));
    ESP_LOGI(TAG, "Setting servo to %d degrees (duty: %lu)", angle, duty);
     // Add a small delay for the servo to physically move
    vTaskDelay(pdMS_TO_TICKS(300)); // 300ms delay, adjust as needed
}

// --- WiFi Functions (Unchanged) ---
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

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta finished.");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// --- Helper Function: Slot to Day/Dose String ---
void slot_to_day_dose_string(int slot, char *out_str, size_t max_len) {
    if (slot < 0 || slot >= NUM_SLOTS) {
        snprintf(out_str, max_len, "Invalid Slot");
        return;
    }
    const char *days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    int day_index = slot / 2;
    int dose_num = (slot % 2) + 1;
    if (day_index < 6) { // Check index bounds
         snprintf(out_str, max_len, "%s Dose %d", days[day_index], dose_num);
    } else {
         snprintf(out_str, max_len, "Error Slot"); // Should not happen with NUM_SLOTS=11
    }

}

// --- NEW HTML Page with Modes ---
static const char *html_page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ASHUMITRA Pill Dispenser</title>
    <meta name='viewport' content='width=device-width, initial-scale=1'>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f4f0f8; color: #333; margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; }
        .container { background-color: #ffffff; padding: 30px; border-radius: 15px; box-shadow: 0 5px 15px rgba(0, 0, 0, 0.1); text-align: center; max-width: 450px; width: 90%; }
        h1 { color: #6a0dad; margin-bottom: 10px; }
        p { color: #555; margin-bottom: 25px; }
        #dateTime { font-size: 1em; color: #8a2be2; margin-bottom: 20px; padding: 10px; background-color: #e6e6fa; border-radius: 8px; }
        .controls label, .mode-selector label { display: block; margin-bottom: 8px; font-weight: bold; color: #6a0dad; text-align: left; }
        .controls select, .controls button { width: 100%; padding: 12px; margin-bottom: 15px; border-radius: 8px; border: 1px solid #ccc; font-size: 1em; box-sizing: border-box; }
        .controls select { background-color: #f8f8f8; }
        .controls button { background-color: #9370db; color: white; border: none; font-size: 1.1em; font-weight: bold; cursor: pointer; transition: background-color 0.3s ease; box-shadow: 0 4px #6a0dad; position: relative; top: 0; }
        .controls button:hover { background-color: #8a2be2; }
        .controls button:active { background-color: #8a2be2; box-shadow: 0 2px #6a0dad; top: 2px; }
        .controls button.remove-btn { background-color: #dc3545; box-shadow: 0 4px #a71d2a; font-size: 0.9em; padding: 8px; width: auto; margin-left: 10px; }
        .controls button.remove-btn:hover { background-color: #c82333; }
        .controls button.remove-btn:active { background-color: #c82333; box-shadow: 0 2px #a71d2a; top: 2px; }
        .controls button.dispense-btn { background-color: #28a745; box-shadow: 0 4px #1e7e34; margin: 5px; width: calc(50% - 10px); /* Two columns */ display: inline-block; }
        .controls button.dispense-btn:hover { background-color: #218838; }
        .controls button.dispense-btn:active { background-color: #218838; box-shadow: 0 2px #1e7e34; top: 2px; }

        #statusMessage { margin-top: 20px; font-size: 1em; color: #6a0dad; font-weight: bold; min-height: 20px; background-color: #e6e6fa; padding: 10px; border-radius: 8px; display: none; }
        .status-success { color: #155724; background-color: #d4edda; border: 1px solid #c3e6cb; }
        .status-error { color: #721c24; background-color: #f8d7da; border: 1px solid #f5c6cb; }
        .mode-selector { margin-bottom: 25px; background-color: #e6e6fa; padding: 15px; border-radius: 8px; text-align: center; }
        .mode-selector label { display: inline-block; margin: 0 15px; font-weight: normal; color: #333; cursor: pointer; }
        .mode-selector input[type="radio"] { margin-right: 5px; vertical-align: middle; }
        #filledDosesDisplay ul { list-style: none; padding: 0; margin-top: 10px; }
        #filledDosesDisplay li { background-color: #f8f8f8; padding: 8px 12px; margin-bottom: 5px; border-radius: 5px; display: flex; justify-content: space-between; align-items: center; }
        #dispenseControls { margin-top: 15px; }
        #dispenseControls h4, #filledDosesDisplay h4 { color: #6a0dad; margin-bottom: 10px; text-align: left; }
         hr { border: none; border-top: 1px solid #ddd; margin: 25px 0; }
    </style>
</head>
<body>
    <div class="container">
        <h1>ASHUMITRA</h1>
        <p>Smart Pill Dispenser</p>
        <div id="dateTime">Loading date and time...</div>

        <div class="mode-selector">
            <label><input type="radio" name="mode" value="fill" checked onchange="switchMode('fill')"> Filling Mode</label>
            <label><input type="radio" name="mode" value="dispense" onchange="switchMode('dispense')"> Dispense Mode</label>
        </div>

        <div id="statusMessage"></div>

        <!-- Filling Mode Controls -->
        <div id="fillControls" class="controls">
            <label for="daySelect">Select Day:</label>
            <select id="daySelect">
                <option value="monday">Monday</option>
                <option value="tuesday">Tuesday</option>
                <option value="wednesday">Wednesday</option>
                <option value="thursday">Thursday</option>
                <option value="friday">Friday</option>
                <option value="saturday">Saturday</option>
            </select>

            <label for="doseSelect">Select Dose:</label>
            <select id="doseSelect">
                <option value="1">Dose 1</option>
                <option value="2">Dose 2</option>
            </select>

            <button onclick="addDose()">Add Dose to Schedule</button>

            <div id="filledDosesDisplay">
                <h4>Scheduled Doses:</h4>
                <ul id="filledList">
                    <!-- Filled doses will be listed here -->
                </ul>
            </div>
             <hr>
        </div>

        <!-- Dispense Mode Controls -->
        <div id="dispenseControls" class="controls" style="display: none;">
            <h4>Dispense a Scheduled Dose:</h4>
            <div id="dispenseButtons">
                <!-- Buttons for filled doses will be added here -->
            </div>
            <hr>
        </div>


    </div>

    <script>
        let currentMode = 'fill'; // Track current mode

        function updateDateTime() {
            const now = new Date();
            const options = { weekday: 'long', year: 'numeric', month: 'long', day: 'numeric', hour: '2-digit', minute: '2-digit', second: '2-digit' };
            document.getElementById('dateTime').textContent = now.toLocaleDateString('en-US', options);
        }

        function getSlotFromSelection() {
            const day = document.getElementById('daySelect').value;
            const dose = parseInt(document.getElementById('doseSelect').value, 10);
            let slot = -1;

            if (day === 'monday') slot = (dose === 1) ? 0 : 1;
            else if (day === 'tuesday') slot = (dose === 1) ? 2 : 3;
            else if (day === 'wednesday') slot = (dose === 1) ? 4 : 5;
            else if (day === 'thursday') slot = (dose === 1) ? 6 : 7;
            else if (day === 'friday') slot = (dose === 1) ? 8 : 9;
            else if (day === 'saturday') slot = (dose === 1) ? 10 : -1; // Dose 2 invalid

            return slot;
        }

         function slotToDayDoseString(slot) {
            const days = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
            if (slot < 0 || slot >= 11) return "Invalid Slot";
            const dayIndex = Math.floor(slot / 2);
            const doseNum = (slot % 2) + 1;
             if (dayIndex < 6) {
                 return `${days[dayIndex]} Dose ${doseNum}`;
             }
             return "Error Slot";
        }


        function showStatus(message, isError = false) {
            const statusDiv = document.getElementById('statusMessage');
            statusDiv.textContent = message;
            statusDiv.className = isError ? 'status-error' : 'status-success';
            statusDiv.style.display = 'block';
            // Optionally hide after a few seconds
            // setTimeout(() => { statusDiv.style.display = 'none'; }, 5000);
        }

        function clearStatus() {
             const statusDiv = document.getElementById('statusMessage');
             statusDiv.textContent = '';
             statusDiv.style.display = 'none';
             statusDiv.className = '';
        }


        function switchMode(newMode) {
            currentMode = newMode;
            clearStatus();
            if (newMode === 'fill') {
                document.getElementById('fillControls').style.display = 'block';
                document.getElementById('dispenseControls').style.display = 'none';
                 loadFilledDoses(); // Refresh the list view
            } else { // dispense mode
                document.getElementById('fillControls').style.display = 'none';
                document.getElementById('dispenseControls').style.display = 'block';
                loadFilledDoses(); // Load buttons for dispensing
            }
        }

        function handleDayChange() {
            const day = document.getElementById('daySelect').value;
            const doseSelect = document.getElementById('doseSelect');
            const dose2Option = doseSelect.querySelector('option[value="2"]');
            dose2Option.disabled = (day === 'saturday');
            if (day === 'saturday' && doseSelect.value === '2') {
                doseSelect.value = '1';
            }
        }

        function addDose() {
            const slot = getSlotFromSelection();
             clearStatus();

            if (slot === -1) {
                showStatus('Error: Saturday Dose 2 cannot be scheduled.', true);
                return;
            }

            showStatus('Adding dose to schedule...');
            fetch(`/add_dose?slot=${slot}`)
                .then(response => response.text().then(text => ({ ok: response.ok, status: response.status, text })))
                .then(({ ok, status, text }) => {
                    if (!ok) {
                         throw new Error(`HTTP error ${status}: ${text}`);
                    }
                    showStatus(text, false); // Show success message from server
                    loadFilledDoses(); // Refresh the list/buttons
                })
                .catch(error => {
                    console.error('Error adding dose:', error);
                    showStatus(`Error adding dose: ${error.message}`, true);
                });
        }

        function removeDose(slot) {
             clearStatus();
             showStatus('Removing dose from schedule...');
             fetch(`/remove_dose?slot=${slot}`)
                .then(response => response.text().then(text => ({ ok: response.ok, status: response.status, text })))
                 .then(({ ok, status, text }) => {
                     if (!ok) {
                          throw new Error(`HTTP error ${status}: ${text}`);
                     }
                     showStatus(text, false); // Show success message from server
                     loadFilledDoses(); // Refresh the list/buttons
                 })
                 .catch(error => {
                     console.error('Error removing dose:', error);
                     showStatus(`Error removing dose: ${error.message}`, true);
                 });
        }


        function dispensePill(slot) {
            clearStatus();
            if (slot < 0 || slot >= 11) {
                 showStatus('Error: Invalid slot selected.', true);
                 return;
            }
            showStatus(`Dispensing ${slotToDayDoseString(slot)}...`);

            fetch(`/dispense?slot=${slot}`)
                .then(response => response.text().then(text => ({ ok: response.ok, status: response.status, text })))
                 .then(({ ok, status, text }) => {
                     if (!ok) {
                          throw new Error(`HTTP error ${status}: ${text}`);
                     }
                     showStatus(text, false); // Show confirmation from ESP32
                 })
                 .catch(error => {
                     console.error('Error dispensing pill:', error);
                     showStatus(`Error: Could not contact dispenser. ${error.message}`, true);
                 });
        }

        function loadFilledDoses() {
            fetch('/get_filled_doses')
                .then(response => {
                     if (!response.ok) {
                         throw new Error(`HTTP error! Status: ${response.status}`);
                    }
                    return response.json(); // Expecting JSON array like [0, 2, 5]
                })
                .then(filledSlots => {
                    const filledList = document.getElementById('filledList');
                    const dispenseButtonsDiv = document.getElementById('dispenseButtons');

                    // Clear previous entries
                    filledList.innerHTML = '';
                    dispenseButtonsDiv.innerHTML = '';

                    if (filledSlots.length === 0) {
                        filledList.innerHTML = '<li>No doses scheduled yet.</li>';
                         dispenseButtonsDiv.innerHTML = '<p>No scheduled doses available to dispense.</p>';
                         return;
                    }

                    filledSlots.sort((a, b) => a - b); // Sort slots numerically

                    filledSlots.forEach(slot => {
                         const doseText = slotToDayDoseString(slot);

                         // Add to the list in Filling Mode
                        const listItem = document.createElement('li');
                        listItem.textContent = doseText;
                        const removeButton = document.createElement('button');
                        removeButton.textContent = 'Remove';
                        removeButton.className = 'remove-btn';
                        removeButton.onclick = () => removeDose(slot);
                        listItem.appendChild(removeButton);
                        filledList.appendChild(listItem);

                        // Add button in Dispense Mode
                        const dispenseButton = document.createElement('button');
                        dispenseButton.textContent = doseText;
                         dispenseButton.className = 'dispense-btn';
                        dispenseButton.onclick = () => dispensePill(slot);
                        dispenseButtonsDiv.appendChild(dispenseButton);
                    });
                })
                .catch(error => {
                    console.error('Error loading filled doses:', error);
                    showStatus('Error loading scheduled doses: ' + error.message, true);
                     document.getElementById('filledList').innerHTML = '<li>Error loading schedule.</li>';
                     document.getElementById('dispenseButtons').innerHTML = '<p>Error loading schedule.</p>';
                });
        }


        // Initial setup
        window.onload = () => {
            updateDateTime();
            setInterval(updateDateTime, 1000);

            document.getElementById('daySelect').addEventListener('change', handleDayChange);
             handleDayChange(); // Set initial state for Saturday Dose 2

            switchMode(currentMode); // Set initial mode view and load doses
        };

    </script>
</body>
</html>
)rawliteral";


// --- HTTP Handlers ---
// --- HTTP Handlers ---

// Handler for root path (serves the HTML page) - Unchanged
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, html_page);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Handler for adding a dose to the schedule
// Handler for adding a dose to the schedule
static esp_err_t add_dose_handler(httpd_req_t *req)
{
    char buf[50]; // Buffer for query string
    char slot_str[5];
    int slot = -1;
    char resp_str[100];
    char day_dose_buf[50]; // Temporary buffer for day/dose string

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "slot", slot_str, sizeof(slot_str)) == ESP_OK) {
            slot = atoi(slot_str);
            ESP_LOGI(TAG, "Add dose request for slot: %d", slot);

            if (slot >= 0 && slot < NUM_SLOTS) {
                 if (!nvs_mutex) {
                     httpd_resp_send_500(req);
                     return ESP_FAIL;
                 }
                 // Lock mutex before modifying shared data
                 if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                     // Check if already filled first
                     if (filled_slots_status[slot] == 1) {
                         xSemaphoreGive(nvs_mutex); // Release mutex
                         slot_to_day_dose_string(slot, day_dose_buf, sizeof(day_dose_buf)); // Generate string into temp buffer
                         snprintf(resp_str, sizeof(resp_str), "Already Added: %s", day_dose_buf); // Combine prefix and temp buffer
                         httpd_resp_send(req, resp_str, strlen(resp_str));
                         ESP_LOGI(TAG, "%s", resp_str);
                     } else {
                         // Not filled, proceed to add
                         filled_slots_status[slot] = 1; // Mark as filled in RAM
                         xSemaphoreGive(nvs_mutex); // Release mutex *before* NVS write and servo move

                         // --- Move Servo ---
                         int angle = servo_positions[slot];
                         ESP_LOGI(TAG, "Moving servo to %d degrees for filling slot %d", angle, slot);
                         servo_set_angle(angle); // Move servo to the selected slot position
                         // ------------------

                         // Write changes to NVS
                         esp_err_t nvs_err = nvs_write_filled_slots();

                         // Prepare and send response
                         slot_to_day_dose_string(slot, day_dose_buf, sizeof(day_dose_buf)); // Generate string into temp buffer
                         if (nvs_err == ESP_OK) {
                            snprintf(resp_str, sizeof(resp_str), "Added: %s (Moved to %d°)", day_dose_buf, angle); // Combine prefix and temp buffer
                            httpd_resp_send(req, resp_str, strlen(resp_str));
                            ESP_LOGI(TAG, "%s", resp_str);
                         } else {
                            // Still inform user it was added (and moved), but mention NVS error
                            snprintf(resp_str, sizeof(resp_str), "Added: %s (Moved to %d°). NVS Save Error: %s",
                                     day_dose_buf, angle, esp_err_to_name(nvs_err));
                            httpd_resp_set_status(req, "200 OK"); // Send 200 OK, but include error info in message
                            httpd_resp_send(req, resp_str, strlen(resp_str));
                            ESP_LOGE(TAG, "NVS Error saving schedule (%s) but slot added to RAM and servo moved.", esp_err_to_name(nvs_err));
                         }
                     }
                 } else {
                     ESP_LOGE(TAG, "Add dose: Could not obtain mutex");
                     httpd_resp_set_status(req, "503 Service Unavailable");
                     httpd_resp_sendstr(req, "Error: Server busy, please try again.");
                 }

            } else {
                snprintf(resp_str, sizeof(resp_str), "Error: Invalid slot number (%d)", slot);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, resp_str, strlen(resp_str));
                 ESP_LOGW(TAG, "%s", resp_str);
            }
        } else {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "Error: Missing 'slot' parameter.");
            ESP_LOGW(TAG, "Add dose: Missing 'slot' parameter in query: %s", buf);
        }
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Error: Missing query parameters.");
        ESP_LOGW(TAG, "Add dose: No query string");
    }
    return ESP_OK;
}

// Handler for removing a dose from the schedule
static esp_err_t remove_dose_handler(httpd_req_t *req)
{
    char buf[50];
    char slot_str[5];
    int slot = -1;
    char resp_str[100];
    char day_dose_buf[50]; // Temporary buffer for day/dose string

     if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "slot", slot_str, sizeof(slot_str)) == ESP_OK) {
            slot = atoi(slot_str);
            ESP_LOGI(TAG, "Remove dose request for slot: %d", slot);

             if (slot >= 0 && slot < NUM_SLOTS) {
                  if (!nvs_mutex) {
                     httpd_resp_send_500(req);
                     return ESP_FAIL;
                 }
                 // Lock mutex
                 if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                     if (filled_slots_status[slot] == 1) {
                         filled_slots_status[slot] = 0; // Mark as empty in RAM
                         xSemaphoreGive(nvs_mutex); // Release mutex *before* NVS write

                         esp_err_t nvs_err = nvs_write_filled_slots(); // Write changes to NVS
                         if (nvs_err == ESP_OK) {
                             slot_to_day_dose_string(slot, day_dose_buf, sizeof(day_dose_buf)); // Generate string into temp buffer
                             snprintf(resp_str, sizeof(resp_str), "Removed: %s", day_dose_buf); // Combine prefix and temp buffer
                             httpd_resp_send(req, resp_str, strlen(resp_str));
                             ESP_LOGI(TAG, "%s", resp_str);
                         } else {
                            snprintf(resp_str, sizeof(resp_str), "Error saving schedule (NVS Error: %s)", esp_err_to_name(nvs_err));
                            httpd_resp_set_status(req, "500 Internal Server Error");
                            httpd_resp_send(req, resp_str, strlen(resp_str));
                            ESP_LOGE(TAG, "%s", resp_str);
                         }
                     } else {
                         xSemaphoreGive(nvs_mutex); // Release mutex
                         slot_to_day_dose_string(slot, day_dose_buf, sizeof(day_dose_buf)); // Generate string into temp buffer
                         snprintf(resp_str, sizeof(resp_str), "Not Found: %s", day_dose_buf); // Combine prefix and temp buffer
                         httpd_resp_set_status(req, "404 Not Found"); // Or just send a normal OK response
                         httpd_resp_send(req, resp_str, strlen(resp_str));
                         ESP_LOGI(TAG, "%s", resp_str);
                     }
                 } else {
                      ESP_LOGE(TAG, "Remove dose: Could not obtain mutex");
                      httpd_resp_set_status(req, "503 Service Unavailable");
                      httpd_resp_sendstr(req, "Error: Server busy, please try again.");
                 }
             } else {
                 snprintf(resp_str, sizeof(resp_str), "Error: Invalid slot number (%d)", slot);
                 httpd_resp_set_status(req, "400 Bad Request");
                 httpd_resp_send(req, resp_str, strlen(resp_str));
                 ESP_LOGW(TAG, "%s", resp_str);
             }
         } else {
             httpd_resp_set_status(req, "400 Bad Request");
             httpd_resp_sendstr(req, "Error: Missing 'slot' parameter.");
              ESP_LOGW(TAG, "Remove dose: Missing 'slot' parameter in query: %s", buf);
         }
     } else {
         httpd_resp_set_status(req, "400 Bad Request");
         httpd_resp_sendstr(req, "Error: Missing query parameters.");
         ESP_LOGW(TAG, "Remove dose: No query string");
     }
     return ESP_OK;
}


// Handler to get the list of filled doses - Unchanged (already correct)
static esp_err_t get_filled_doses_handler(httpd_req_t *req)
{
    if (!nvs_mutex) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateArray();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON array");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    // Lock mutex for reading shared data
    if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < NUM_SLOTS; i++) {
            if (filled_slots_status[i] == 1) {
                cJSON_AddItemToArray(root, cJSON_CreateNumber(i));
            }
        }
        xSemaphoreGive(nvs_mutex); // Release mutex

        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_str, strlen(json_str));
            ESP_LOGI(TAG, "Sent filled doses: %s", json_str);
            free(json_str); // Free memory allocated by cJSON_Print
        } else {
            ESP_LOGE(TAG, "Failed to print JSON string");
            httpd_resp_send_500(req);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Get filled doses: Could not obtain mutex");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "[]"); // Send empty array on error?
        err = ESP_FAIL;
    }

    cJSON_Delete(root); // Delete JSON object
    return err;
}


// Handler for dispensing pills (now uses slot number)
static esp_err_t dispense_handler(httpd_req_t *req)
{
    char buf[50];
    char slot_str[5];
    int slot = -1;
    char resp_str[100];
    char day_dose_buf[50]; // Temporary buffer for day/dose string
    bool is_filled = false;

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "slot", slot_str, sizeof(slot_str)) == ESP_OK) {
            slot = atoi(slot_str);
            ESP_LOGI(TAG, "Dispense request for slot: %d", slot);

            if (slot >= 0 && slot < NUM_SLOTS) {
                if (!nvs_mutex) {
                    httpd_resp_send_500(req);
                    return ESP_FAIL;
                }
                 // Check if the requested slot is actually filled (read requires mutex)
                 if (xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    is_filled = (filled_slots_status[slot] == 1);
                    xSemaphoreGive(nvs_mutex); // Release mutex after reading
                 } else {
                      ESP_LOGE(TAG, "Dispense: Could not obtain mutex");
                      httpd_resp_set_status(req, "503 Service Unavailable");
                      httpd_resp_sendstr(req, "Error: Server busy, please try again.");
                      return ESP_OK; // Return OK as response sent
                 }


                if (is_filled) {
                    int angle = servo_positions[slot];
                    servo_set_angle(angle); // Move servo

                    // Prepare response string
                    slot_to_day_dose_string(slot, day_dose_buf, sizeof(day_dose_buf)); // Generate string into temp buffer
                    snprintf(resp_str, sizeof(resp_str), "Dispensed: %s (Angle: %d°)", day_dose_buf, angle); // Combine
                    httpd_resp_send(req, resp_str, strlen(resp_str));
                    ESP_LOGI(TAG, "Response: %s", resp_str);

                    // --- Optional: Move servo back to a neutral position after dispensing ---
                     vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second for pill to drop
                     // servo_set_angle(servo_positions[0]); // Example: back to Monday Dose 1 position
                     // ESP_LOGI(TAG, "Servo returned to neutral position.");
                    // --- End Optional ---

                } else {
                     slot_to_day_dose_string(slot, day_dose_buf, sizeof(day_dose_buf)); // Generate string into temp buffer
                     snprintf(resp_str, sizeof(resp_str), "Error: %s is not scheduled/filled.", day_dose_buf); // Combine
                     httpd_resp_set_status(req, "400 Bad Request"); // Or maybe 404 Not Found
                     httpd_resp_send(req, resp_str, strlen(resp_str));
                     ESP_LOGW(TAG, "Dispense failed: Slot %d is not marked as filled.", slot);
                }
            } else {
                snprintf(resp_str, sizeof(resp_str), "Error: Invalid slot number (%d)", slot);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, resp_str, strlen(resp_str));
                 ESP_LOGW(TAG, "%s", resp_str);
            }
        } else {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "Error: Missing 'slot' parameter.");
             ESP_LOGW(TAG, "Dispense: Missing 'slot' parameter in query: %s", buf);
        }
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "Error: Missing query parameters.");
         ESP_LOGW(TAG, "Dispense: No query string");
    }
    return ESP_OK;
}

// Start the HTTP server - Unchanged
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240; // Increased stack size further for HTML page and JSON
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // URI handler for the root page
        httpd_uri_t root_uri = { "/", HTTP_GET, root_get_handler, NULL };
        httpd_register_uri_handler(server, &root_uri);

        // URI handler for adding a dose
        httpd_uri_t add_dose_uri = { "/add_dose", HTTP_GET, add_dose_handler, NULL };
        httpd_register_uri_handler(server, &add_dose_uri);

        // URI handler for removing a dose
        httpd_uri_t remove_dose_uri = { "/remove_dose", HTTP_GET, remove_dose_handler, NULL };
        httpd_register_uri_handler(server, &remove_dose_uri);

        // URI handler for getting filled doses
        httpd_uri_t get_filled_uri = { "/get_filled_doses", HTTP_GET, get_filled_doses_handler, NULL };
        httpd_register_uri_handler(server, &get_filled_uri);

        // URI handler for the dispense action (using slot)
        httpd_uri_t dispense_uri = { "/dispense", HTTP_GET, dispense_handler, NULL };
        httpd_register_uri_handler(server, &dispense_uri);

        ESP_LOGI(TAG, "Web server started successfully with new handlers.");
        return server;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}
// --- Main Application ---
void app_main(void)
{
    // Initialize NVS first
    ESP_ERROR_CHECK(nvs_init());

    // Create Mutex for shared NVS/RAM data access
    nvs_mutex = xSemaphoreCreateMutex();
    if (!nvs_mutex) {
         ESP_LOGE(TAG, "Failed to create NVS mutex!");
         // Handle error - perhaps restart?
         return;
    }

    // Load initial filled slots status from NVS into RAM
    if (nvs_read_filled_slots() != ESP_OK) {
        // If reading failed critically (not just 'not found'), log it.
        // The function already initializes to empty on 'not found' or size mismatch.
        ESP_LOGW(TAG, "Issues reading initial NVS data, proceeding with default (empty).");
    }


    // Initialize servo
    servo_init();
    ESP_LOGI(TAG, "Servo initialized on GPIO %d", SERVO_GPIO_PIN);

    // Initialize WiFi
    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_init_sta();

    // Start the web server only if WiFi connected successfully
    if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) {
         ESP_LOGI(TAG, "Starting web server...");
         start_webserver();
         // Set servo to initial/home position (optional, e.g., slot 0)
         // Use a small delay to ensure webserver task is running before potential servo movement
         vTaskDelay(pdMS_TO_TICKS(500));
         servo_set_angle(servo_positions[0]);
         ESP_LOGI(TAG, "Servo set to initial position: %d degrees (Slot 0)", servo_positions[0]);
    } else {
         ESP_LOGE(TAG, "WiFi connection failed. Web server not started.");
    }

    ESP_LOGI(TAG, "ASHUMITRA application started.");
    // Tasks (WiFi, HTTP server) are running. app_main can exit or loop.
}