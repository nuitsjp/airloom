#include "measurement.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static void expect_temperature(int16_t raw, float expected)
{
    float value = 999.0f;
    assert(measurement_temperature_c(raw, &value));
    assert(fabsf(value - expected) < 0.001f);
}

static void expect_humidity(uint16_t raw, float expected)
{
    float value = 999.0f;
    assert(measurement_humidity_percent(raw, &value));
    assert(fabsf(value - expected) < 0.001f);
}

int main(void)
{
    float value = 999.0f;

    expect_temperature(-27315, -273.15f);
    expect_temperature(-125, -1.25f);
    expect_temperature(0, 0.0f);
    expect_temperature(2345, 23.45f);
    expect_temperature(INT16_MAX, 327.67f);
    assert(!measurement_temperature_c(INT16_MIN, &value));
    assert(value == 999.0f);
    assert(!measurement_temperature_c(-27316, &value));
    assert(value == 999.0f);

    expect_humidity(0, 0.0f);
    expect_humidity(6524, 65.24f);
    expect_humidity(10000, 100.0f);
    assert(!measurement_humidity_percent(10001, &value));
    assert(value == 999.0f);
    assert(!measurement_humidity_percent(UINT16_MAX, &value));
    assert(value == 999.0f);

    puts("measurement tests passed");
    return 0;
}
