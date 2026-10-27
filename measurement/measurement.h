#ifndef AIRLOOM_MEASUREMENT_H
#define AIRLOOM_MEASUREMENT_H

#include <stdbool.h>
#include <stdint.h>

/* ZCL MeasuredValue attributes (0x0000) from clusters 0x0402 and 0x0405.
 * Returns false for unknown or out-of-range measurements; output is unchanged.
 */
bool measurement_temperature_c(int16_t raw, float *out);
bool measurement_humidity_percent(uint16_t raw, float *out);

#endif
