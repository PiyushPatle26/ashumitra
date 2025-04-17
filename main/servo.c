#include "servo.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "SERVO";

// Array to store preset servo positions (in degrees) for each slot
const int servo_positions[NUM_SLOTS] = {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160};

esp_err_t servo_init(void)
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
    
    return ESP_OK;
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
    vTaskDelay(pdMS_TO_TICKS(300)); // 300ms delay for servo movement
}

esp_err_t servo_dispense_pill(int slot)
{
    if (slot < 0 || slot >= NUM_SLOTS) {
        ESP_LOGE(TAG, "Invalid slot number: %d", slot);
        return ESP_ERR_INVALID_ARG;
    }

    // Move to the specified slot
    servo_set_angle(servo_positions[slot]);
    vTaskDelay(pdMS_TO_TICKS(500)); // Wait for servo to reach position

    // Return to home position
    servo_set_angle(servo_positions[0]);
    return ESP_OK;
}

int day_dose_to_slot(const char *day, int dose)
{
    if (!day || dose < 1 || dose > 2) {
        return -1;
    }

    int day_num;
    if (strcmp(day, "monday") == 0) day_num = 0;
    else if (strcmp(day, "tuesday") == 0) day_num = 1;
    else if (strcmp(day, "wednesday") == 0) day_num = 2;
    else if (strcmp(day, "thursday") == 0) day_num = 3;
    else if (strcmp(day, "friday") == 0) day_num = 4;
    else if (strcmp(day, "saturday") == 0) day_num = 5;
    else if (strcmp(day, "sunday") == 0) day_num = 6;
    else return -1;

    // For days 0-4 (Monday to Friday), we have 2 doses each (slots 0-9)
    // For days 5-6 (Saturday and Sunday), we have 1 dose each (slots 10-11)
    if (day_num <= 4) {
        return (day_num * 2) + (dose - 1);
    } else {
        // For Saturday and Sunday, only dose 1 is available
        if (dose == 1) {
            return 10 + (day_num - 5);
        }
        return -1;
    }
}

void slot_to_day_dose(int slot, char *day, size_t day_size, int *dose)
{
    if (slot < 0 || slot >= NUM_SLOTS || !day || !dose) {
        if (day && day_size > 0) day[0] = '\0';
        if (dose) *dose = 0;
        return;
    }

    if (slot < 10) {
        // Slots 0-9: Monday to Friday (2 doses each)
        int day_num = slot / 2;
        *dose = (slot % 2) + 1;
        const char *days[] = {"monday", "tuesday", "wednesday", "thursday", "friday"};
        strncpy(day, days[day_num], day_size - 1);
    } else {
        // Slots 10-11: Saturday and Sunday (1 dose each)
        int day_num = 5 + (slot - 10);
        *dose = 1;
        const char *days[] = {"saturday", "sunday"};
        strncpy(day, days[slot - 10], day_size - 1);
    }
    day[day_size - 1] = '\0';
}
