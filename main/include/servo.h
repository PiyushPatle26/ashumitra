#ifndef SERVO_H
#define SERVO_H

#include <esp_err.h>

// Servo control parameters
#define SERVO_GPIO_PIN 2
#define SERVO_TIMER LEDC_TIMER_0
#define SERVO_CHANNEL LEDC_CHANNEL_0
#define SERVO_RESOLUTION LEDC_TIMER_13_BIT
#define SERVO_FREQ 50
#define SERVO_MIN_PULSEWIDTH 500
#define SERVO_MAX_PULSEWIDTH 2500

// Number of slots for servo positions (11 slots total)
#define NUM_SLOTS 11

// Servo positions for each slot (in degrees)
extern const int servo_positions[NUM_SLOTS];

// Function declarations
esp_err_t servo_init(void);
void servo_set_angle(int angle);
uint32_t servo_angle_to_duty(int angle);
esp_err_t servo_dispense_pill(int slot);

// Utility functions for slot management
int day_dose_to_slot(const char *day, int dose);
void slot_to_day_dose(int slot, char *day, size_t day_size, int *dose);

#endif // SERVO_H
