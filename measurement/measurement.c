#include "measurement.h"

bool measurement_temperature_c(int16_t raw, float *out)
{
    if (raw < -27315) {
        return false;
    }
    *out = (float)raw / 100.0f;
    return true;
}

bool measurement_humidity_percent(uint16_t raw, float *out)
{
    if (raw > 10000) {
        return false;
    }
    *out = (float)raw / 100.0f;
    return true;
}
