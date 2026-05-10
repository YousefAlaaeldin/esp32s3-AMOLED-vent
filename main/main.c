#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "lvgl.h"
#include "bsp/display.h"

void app_main(void)
{

        
    // 1. Start the display and LVGL. This initializes the LCD, touch, LVGL task.
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
    printf("ERROR: bsp_display_start() failed!\n");
    while(1) vTaskDelay(1000);
}

    // 2. Turn on the backlight (optional; brightness may be on by default).
    bsp_display_backlight_on();

    // 3. Create a simple LVGL label on the active screen.
    //    bsp_display_lock() / unlock() are required because LVGL is not thread‑safe.
    bsp_display_lock(portMAX_DELAY);
    
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello, LVGL!");
    lv_obj_center(label);
    bsp_display_unlock();

    // 4. Keep the application alive (the LVGL task is running in the background).
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}