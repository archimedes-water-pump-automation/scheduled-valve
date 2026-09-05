#include "driver/gpio.h"
#include "esp_log.h"

#include "config.h"
#include "valve.h"

static const char *TAG = "valve";

static bool s_open;

static void relay_write(bool energised)
{
    gpio_set_level(PIN_RELAY, RELAY_ACTIVE_LOW ? !energised : energised);
}

void valve_init(void)
{
    /* Latch the pin to the de-asserted level before it becomes an
     * output, so enabling the driver cannot glitch the relay. The
     * external 10k pull-up covers the window before this runs. */
    gpio_set_level(PIN_RELAY, RELAY_ACTIVE_LOW ? 1 : 0);

    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << PIN_RELAY),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&conf));

    relay_write(false);
    s_open = false;
    ESP_LOGI(TAG, "relay on GPIO%d, valve closed", PIN_RELAY);
}

void valve_open(void)
{
    if (s_open) return;
    s_open = true;
    relay_write(true);
    ESP_LOGI(TAG, "open");
}

void valve_close(void)
{
    if (!s_open) return;
    s_open = false;
    relay_write(false);
    ESP_LOGI(TAG, "closed");
}

bool valve_is_open(void)
{
    return s_open;
}
