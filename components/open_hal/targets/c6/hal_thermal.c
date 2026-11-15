#include "hal_thermal.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "HAL_THERMAL";
static temperature_sensor_handle_t s_temp_sensor = NULL;

bool hal_thermal_init(void)
{
    if (s_temp_sensor != NULL) {
        return true;
    }

    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t err = temperature_sensor_install(&temp_cfg, &s_temp_sensor);

    if (err != ESP_OK) {
        temp_cfg.range_min = -10;
        temp_cfg.range_max = 80;
        err = temperature_sensor_install(&temp_cfg, &s_temp_sensor);
    }

    if (err == ESP_OK) {
        temperature_sensor_enable(s_temp_sensor);
        ESP_LOGI(TAG, "Hardware Temperature Sensor initialized successfully!");
        return true;
    }

    ESP_LOGE(TAG, "Failed to init TSENS: %s", esp_err_to_name(err));
    s_temp_sensor = NULL;
    return false;
}

bool hal_thermal_read_celsius(float *out_celsius)
{
    if (!out_celsius || s_temp_sensor == NULL) {
        return false;
    }
    return temperature_sensor_get_celsius(s_temp_sensor, out_celsius) == ESP_OK;
}
