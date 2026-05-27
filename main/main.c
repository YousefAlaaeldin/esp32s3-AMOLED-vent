#include <stdio.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "lvgl.h"

#include "lvgl_help.h"
#include "audio_helper.h"


/* ============================================
 * Audio Monitor Task
 * ============================================ */


 static void handle_kws_command(const char *keyword);

static void audio_monitor_task(void *arg)
{
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║        Voice Control Listener          ║\n");
    printf("║        Say: heat, cool, auto, eco, off ║\n");
    printf("║        Or:  up, down                   ║\n");
    printf("╚════════════════════════════════════════╝\n\n");

    int silence_count = 0;
    int vad_check_count = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));

        audio_vad_state_t vad = audio_get_vad_state();
        vad_check_count++;

        if (vad == AUDIO_VAD_SPEECH) {
            silence_count = 0;
            
            if (vad_check_count % 10 == 0) {
                printf(".");
                fflush(stdout);
            }
        } else if (vad == AUDIO_VAD_SILENCE) {
            silence_count++;
            
            if (silence_count % 30 == 0 && silence_count > 0) {
                printf(" ");
                fflush(stdout);
            }
        }

        /* CHECK FOR KEYWORD DETECTION */
        if (audio_keyword_detected()) {
            const char *keyword = audio_get_detected_keyword();
            kws_result_t kws_result = audio_get_kws_result();

            printf("\n\n✓ KEYWORD DETECTED!\n");
            printf("  Keyword: '%s'\n", keyword);
            printf("  Confidence: %.1f%%\n", kws_result.confidence * 100.0f);
            printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

            handle_kws_command(keyword);
            audio_reset_kws_result();

            printf("\n");
        }
    }
}

/* ============================================
 * KWS Command Handler
 * ============================================ */

static void handle_kws_command(const char *keyword)
{
    if (!keyword || strlen(keyword) == 0) {
        return;
    }

    bsp_display_lock(portMAX_DELAY);

    printf("\n🎤 Processing voice command: '%s'\n", keyword);

    if (strcmp(keyword, "heat") == 0) {
        printf("  → Setting mode to HEAT\n");
        g_state.mode = MODE_HEAT;
    }
    else if (strcmp(keyword, "cool") == 0) {
        printf("  → Setting mode to COOL\n");
        g_state.mode = MODE_COOL;
    }
    else if (strcmp(keyword, "auto") == 0) {
        printf("  → Setting mode to AUTO\n");
        g_state.mode = MODE_AUTO;
    }
    else if (strcmp(keyword, "eco") == 0) {
        printf("  → Setting mode to ECO\n");
        g_state.mode = MODE_ECO;
    }
    else if (strcmp(keyword, "off") == 0) {
        printf("  → Setting mode to OFF\n");
        g_state.mode = MODE_OFF;
    }
    else if (strcmp(keyword, "up") == 0) {
        g_state.target_temp++;
        if (g_state.target_temp > 90) {
            g_state.target_temp = 90;
        }
        printf("  → Temperature increased to %.0f°F\n", g_state.target_temp);
    }
    else if (strcmp(keyword, "down") == 0) {
        g_state.target_temp--;
        if (g_state.target_temp < 50) {
            g_state.target_temp = 50;
        }
        printf("  → Temperature decreased to %.0f°F\n", g_state.target_temp);
    }
    else {
        printf("  ⚠ Unknown command: '%s'\n", keyword);
    }

    update_ui();
    bsp_display_unlock();
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
    printf("║                                        ║\n");
    printf("║   Voice Commands:                      ║\n");
    printf("║   • Mode: heat, cool, auto, eco, off   ║\n");
    printf("║   • Temp: up, down                     ║\n");
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