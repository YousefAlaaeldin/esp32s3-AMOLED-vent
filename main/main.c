#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "lvgl.h"

#include "lvgl_help.h"
#include "audio_helper.h"

/* ============================================
 * Audio Monitor Task
 * ============================================ */

static void audio_monitor_task(void *arg)
{
    printf("\n=== Audio System Started ===\n");
    printf("Listening for voice activity...\n\n");
    
    int silence_count = 0;
    
    while (1) {
        // Shorter delay - check VAD state frequently but don't block
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Check VAD state
        audio_vad_state_t vad = audio_get_vad_state();
        
        if (vad == AUDIO_VAD_SPEECH) {
            silence_count = 0;
            printf(".");  // Dot while speaking
            fflush(stdout);
        } else if (vad == AUDIO_VAD_SILENCE) {
            silence_count++;
            // Print silence indicator less frequently
            if (silence_count % 30 == 0) {  // Every 3 seconds of silence
                printf(" ");
                fflush(stdout);
            }
        }
        
        // Check if keyword detected (for future use)
        if (audio_keyword_detected()) {
            printf("\n\n🎤 KEYWORD DETECTED: %s\n", 
                   audio_get_detected_keyword());
            audio_reset_kws_result();
        }
    }
}

/* ============================================
 * Main Application
 * ============================================ */
void app_main(void)
{
    printf("\n\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║   ESP32-S3 Smart Thermostat System     ║\n");
    printf("║   With Voice Control & LVGL Display    ║\n");
    printf("╚════════════════════════════════════════╝\n\n");

    /* ===== Initialize Display (LVGL) ===== */
    printf("[1/4] Initializing display...\n");
    
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        printf("ERROR: bsp_display_start() failed!\n");
        while(1) vTaskDelay(1000);
    }

    bsp_display_backlight_on();
    printf("  ✓ Display initialized\n");

    // Create the thermostat UI
    bsp_display_lock(portMAX_DELAY);
    create_ui();
    bsp_display_unlock();
    printf("  ✓ Thermostat UI created\n");

    /* ===== Initialize Audio System ===== */
    printf("[2/4] Initializing audio system...\n");
    
    esp_err_t err = audio_init();
    if (err != ESP_OK) {
        printf("ERROR: Failed to initialize audio: %s\n", esp_err_to_name(err));
        while(1) vTaskDelay(1000);
    }
    printf("  ✓ Audio system initialized\n");

    /* ===== Start Audio Processing ===== */
    printf("[3/4] Starting audio processing...\n");
    
    err = audio_start_processing(1);  // Priority 1
    if (err != ESP_OK) {
        printf("ERROR: Failed to start audio processing: %s\n", esp_err_to_name(err));
        while(1) vTaskDelay(1000);
    }
    printf("  ✓ Audio processing started\n");

    /* ===== Create Audio Monitor Task ===== */
    printf("[4/4] Creating audio monitor task...\n");
    
    xTaskCreate(
        audio_monitor_task,
        "audio_monitor",
        2048,
        NULL,
        2,
        NULL);
    
    printf("  ✓ Audio monitor task created\n");
    
    printf("\n✓ System Ready!\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n");

    /* ===== Main Loop: Simulate Temperature Changes ===== */
    printf("Simulating temperature changes...\n");
    
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
}