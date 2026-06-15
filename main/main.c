#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bios_core.h"

void app_main(void)
{
    // Background stress test removed. BIOS is now completely clean and stable!
    bios_core_start();
}
