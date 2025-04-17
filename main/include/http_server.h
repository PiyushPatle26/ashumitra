#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/event_groups.h"

// WiFi credentials
#define WIFI_SSID      "Delta_Virus_2.4G"
#define WIFI_PASS      "66380115"
#define MAX_RETRY      5

// Number of slots
#define NUM_SLOTS 11

// Function declarations
void wifi_init_sta(EventGroupHandle_t wifi_event_group);
void start_webserver(void);
int day_dose_to_slot(const char *day, int dose);
void slot_to_day_dose(int slot, char *day, size_t day_size, int *dose);
void slot_to_day_dose_string(int slot, char *out_str, size_t max_len);

#endif // HTTP_SERVER_H
