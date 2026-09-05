#include "nvs_flash.h"
#include "esp_log.h"

#include "control.h"
#include "schedule.h"
#include "telemetry.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    telemetry_start();   /* brings up wifi, which SNTP needs */
    schedule_init();
    control_start();

    ESP_LOGI(TAG, "boot complete");
}
