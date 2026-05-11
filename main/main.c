#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "lvgl.h"
#include "lvgl_help.h"



/* ============================================
 * Main Application
 * ============================================ */
void app_main(void)
{


    /*

    // 1. Start the display and LVGL
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        printf("ERROR: bsp_display_start() failed!\n");
        while(1) vTaskDelay(1000);
    }

    // 2. Turn on the backlight
    bsp_display_backlight_on();

    // 3. Create the thermostat UI (new design)
    bsp_display_lock(portMAX_DELAY);
    create_ui();
    bsp_display_unlock();

    // 4. Simulate temperature changes (for demo purposes)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        // Use BSP mutex to safely update the state and UI
        bsp_display_lock(portMAX_DELAY);

        if (g_state.current_temp < g_state.target_temp) {
            g_state.current_temp += 0.3f;
            if (g_state.current_temp > g_state.target_temp) {
                g_state.current_temp = g_state.target_temp;
            }
        } else if (g_state.current_temp > g_state.target_temp) {
            g_state.current_temp -= 0.2f;
            if (g_state.current_temp < g_state.target_temp) {
                g_state.current_temp = g_state.target_temp;
            }
        }

        update_ui();   // refresh the entire UI after state change
        bsp_display_unlock();
    }
    
    */
}