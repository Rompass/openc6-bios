#pragma once

#include <stdbool.h>

bool hal_thermal_init(void);
bool hal_thermal_read_celsius(float *out_celsius);
