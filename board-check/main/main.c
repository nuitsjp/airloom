#include <inttypes.h>
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"

void app_main(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));
    ESP_LOGI("airloom", "board check v0.1.0; model=%d revision=%d cores=%d flash=%" PRIu32 " bytes reset_reason=%d", chip.model, chip.revision, chip.cores, flash_size, esp_reset_reason());
}
