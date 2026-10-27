#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char sensor_id[24];
    bool has_temperature;
    float temperature_c;
    int64_t temperature_received_us;
    bool has_humidity;
    float humidity_percent;
    int64_t humidity_received_us;
    bool has_battery_percent;
    float battery_percent;
    int64_t battery_percent_received_us;
    bool has_battery_voltage;
    float battery_voltage_v;
    int64_t battery_voltage_received_us;
} airloom_snapshot_t;

typedef bool (*airloom_snapshot_fn)(airloom_snapshot_t *snapshot);
