#include "audio_helper.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

#include "bsp/esp32_s3_touch_amoled_1_75.h"

/*
 * ESP-SR V2 style API
 *
 * This file intentionally does NOT include esp_sr_iface.h.
 * Command recognition is:
 *
 *      I2S/Codec -> AFE(WakeNet9 + VAD) -> MultiNet(mn5q8_en)
 *
 * For mn5q8_en, keep the thermostat command phrases in menuconfig:
 *
 *      ESP Speech Recognition
 *        -> Add English speech commands
 *
 * Use these IDs:
 *      ID1: heat
 *      ID2: cool
 *      ID3: auto
 *      ID4: eco
 *      ID5: off
 *      ID6: up
 *      ID7: down
 *
 * MN5 English runtime API normally expects phonemes, so this file loads
 * commands from sdkconfig instead of calling esp_mn_commands_add("heat").
 */
#include "model_path.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"

#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"

static const char *TAG = "AUDIO_HELPER_SR";

/* =========================================================
 * GLOBAL STATE
 * ========================================================= */

static audio_state_t g_audio_state = AUDIO_STATE_IDLE;
static audio_vad_state_t g_vad_state = AUDIO_VAD_SILENCE;
static kws_result_t g_kws_result = {0};

static TaskHandle_t g_audio_task_handle = NULL;
static bool g_audio_enabled = false;

static esp_codec_dev_handle_t g_mic_handle = NULL;

static bool g_vad_enabled = true;
static float g_vad_confidence = 0.0f;

static bool g_kws_enabled = true;
static float g_kws_confidence_threshold = 0.4f;

/* ESP-SR handles */
static srmodel_list_t *g_sr_models = NULL;

static const esp_afe_sr_iface_t *g_afe_handle = NULL;
static esp_afe_sr_data_t *g_afe_data = NULL;

static esp_mn_iface_t *g_mn_handle = NULL;
static model_iface_data_t *g_mn_data = NULL;

static bool g_mn_session_active = false;

static int g_afe_feed_chunksize = 0;
static int g_afe_feed_channels = 0;
static int g_afe_fetch_chunksize = 0;

static int g_mn_debug_count = 0;
static int g_mn_frame_count = 0;
static int g_mn_speech_frame_count = 0;

/* =========================================================
 * THERMOSTAT COMMAND IDS
 * These MUST match your menuconfig command IDs.
 * ========================================================= */

typedef struct {
    int phrase_id;
    const char *keyword;
    const char *display_name;
} kws_id_map_t;

static const kws_id_map_t g_kws_id_map[] = {
    {1, "heat", "HEAT"},
    {2, "cool", "COOL"},
    {3, "auto", "AUTO"},
    {4, "eco",  "ECO"},
    {5, "off",  "OFF"},
    {6, "up",   "UP"},
    {7, "down", "DOWN"},
};

#define KWS_ID_MAP_COUNT (sizeof(g_kws_id_map) / sizeof(g_kws_id_map[0]))

/* =========================================================
 * AUDIO CONFIG
 * ========================================================= */

#ifndef HW_SAMPLE_RATE
#define HW_SAMPLE_RATE 16000
#endif

#ifndef HW_CHANNELS
#define HW_CHANNELS 2
#endif

#ifndef HW_BIT_DEPTH
#define HW_BIT_DEPTH 16
#endif

#ifndef AUDIO_MIC_GAIN_DB
#define AUDIO_MIC_GAIN_DB 24.0f
#endif

/* AFE input format:
 * "MM" = two interleaved microphone channels.
 * If your board only gives one useful mic channel, change this to "M"
 * and configure I2S/codec as mono.
 */
#ifndef AFE_INPUT_FORMAT
#define AFE_INPUT_FORMAT "MM"
#endif

/* MultiNet create timeout.
 * 6000 ms is a common starting point for command listening after wake word.
 */
#ifndef MN_TIMEOUT_MS
#define MN_TIMEOUT_MS 10000
#endif

/* =========================================================
 * HELPERS
 * ========================================================= */

static const kws_id_map_t *lookup_kws_by_phrase_id(int phrase_id)
{
    for (size_t i = 0; i < KWS_ID_MAP_COUNT; i++) {
        if (g_kws_id_map[i].phrase_id == phrase_id) {
            return &g_kws_id_map[i];
        }
    }
    return NULL;
}

static void set_kws_result(const char *keyword, float confidence)
{
    memset(&g_kws_result, 0, sizeof(g_kws_result));

    g_kws_result.detected = true;
    g_kws_result.confidence = confidence;

    strncpy(g_kws_result.keyword, keyword, sizeof(g_kws_result.keyword) - 1);
    g_kws_result.keyword[sizeof(g_kws_result.keyword) - 1] = '\0';

    g_audio_state = AUDIO_STATE_KEYWORD_DETECTED;
}

static esp_err_t sr_init_afe(void)
{
    g_sr_models = esp_srmodel_init("model");
    if (!g_sr_models) {
        ESP_LOGE(TAG, "esp_srmodel_init failed");
        return ESP_FAIL;
    }

    afe_config_t *afe_config = afe_config_init("MM", g_sr_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_config) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }

    // Manually enable NS on SR type
    afe_config->ns_init = true;
    afe_config->ns_model_name = "nsnet2";
    afe_config->afe_ns_mode = AFE_NS_MODE_NET;

    afe_config = afe_config_check(afe_config);

    g_afe_handle = esp_afe_handle_from_config(afe_config);

    g_afe_data = g_afe_handle->create_from_config(afe_config);

    ESP_LOGI(TAG, "AFE NS config: init=%d model=%s mode=%d",
        afe_config->ns_init,
        afe_config->ns_model_name ? afe_config->ns_model_name : "NULL",
        afe_config->afe_ns_mode);


    afe_config_free(afe_config);

    if (!g_afe_data) {
        ESP_LOGE(TAG, "AFE create_from_config failed");
        return ESP_FAIL;
    }

    g_afe_feed_chunksize = g_afe_handle->get_feed_chunksize(g_afe_data);
    g_afe_feed_channels = g_afe_handle->get_feed_channel_num(g_afe_data);
    g_afe_fetch_chunksize = g_afe_handle->get_fetch_chunksize(g_afe_data);

    ESP_LOGI(TAG, "AFE initialized: feed_chunksize=%d, feed_channels=%d, fetch_chunksize=%d",
             g_afe_feed_chunksize,
             g_afe_feed_channels,
             g_afe_fetch_chunksize);

    return ESP_OK;
}

static esp_err_t sr_init_multinet(void)
{
    char *mn_name = esp_srmodel_filter(g_sr_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!mn_name) {
        ESP_LOGE(TAG, "No English MultiNet model found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Using MultiNet model: %s", mn_name);

    g_mn_handle = esp_mn_handle_from_name(mn_name);
    if (!g_mn_handle) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name failed");
        return ESP_FAIL;
    }

    g_mn_data = g_mn_handle->create(mn_name, 3000);
    if (!g_mn_data) {
        ESP_LOGE(TAG, "MultiNet create failed");
        return ESP_FAIL;
    }

    esp_mn_commands_clear();

    esp_mn_commands_add(1, "heat");    // heat
    esp_mn_commands_add(2, "cool");    // cool
    esp_mn_commands_add(3, "auto");    // auto
    esp_mn_commands_add(4, "eco");    // eco
    esp_mn_commands_add(5, "off");     // off
    esp_mn_commands_add(6, "up");     // up
    esp_mn_commands_add(7, "down");    // down

    esp_mn_error_t *err = esp_mn_commands_update();
    if (err) {
        ESP_LOGE(TAG, "esp_mn_commands_update failed");
        return ESP_FAIL;
    }

    g_mn_handle->print_active_speech_commands(g_mn_data);
    // print_active_speech_commands:
    ESP_LOGI(TAG, "==== Active Commands Registered ====");
    for (int i = 0; i < KWS_ID_MAP_COUNT; i++) {
        ESP_LOGI(TAG, "  ID %d: '%s' -> %s",
                g_kws_id_map[i].phrase_id,
                g_kws_id_map[i].keyword,
                g_kws_id_map[i].display_name);
    }
    ESP_LOGI(TAG, "====================================");

    int mn_chunksize = g_mn_handle->get_samp_chunksize(g_mn_data);
    ESP_LOGI(TAG, "MultiNet chunksize=%d, AFE fetch chunksize=%d",
             mn_chunksize,
             g_afe_fetch_chunksize);

    if (mn_chunksize != g_afe_fetch_chunksize) {
        ESP_LOGW(TAG, "MultiNet chunksize does not match AFE fetch chunksize");
    }

    return ESP_OK;
}

static void sr_reset_to_wakenet(void)
{
    g_mn_session_active = false;

    if (g_mn_handle && g_mn_data && g_mn_handle->clean) {
        g_mn_handle->clean(g_mn_data);
    }

    if (g_afe_handle && g_afe_data) {
        g_afe_handle->enable_wakenet(g_afe_data);
        g_afe_handle->reset_vad(g_afe_data);
    }

    g_audio_state = AUDIO_STATE_LISTENING;
}

static void calc_audio_stats(const int16_t *samples,
                             int sample_count,
                             int *peak_out,
                             int *avg_abs_out,
                             int *dc_out)
{
    int peak = 0;
    int64_t sum_abs = 0;
    int64_t sum = 0;

    if (!samples || sample_count <= 0) {
        *peak_out = 0;
        *avg_abs_out = 0;
        *dc_out = 0;
        return;
    }

    for (int i = 0; i < sample_count; i++) {
        int v = samples[i];
        int av = v < 0 ? -v : v;

        if (av > peak) {
            peak = av;
        }

        sum_abs += av;
        sum += v;
    }

    *peak_out = peak;
    *avg_abs_out = (int)(sum_abs / sample_count);
    *dc_out = (int)(sum / sample_count);
}

static void calc_stereo_stats(const int16_t *samples,
                              int frame_count,
                              int *l_peak, int *l_avg,
                              int *r_peak, int *r_avg)
{
    int lp = 0, rp = 0;
    int64_t lsum = 0, rsum = 0;

    for (int i = 0; i < frame_count; i++) {
        int l = samples[i * 2];
        int r = samples[i * 2 + 1];

        int la = l < 0 ? -l : l;
        int ra = r < 0 ? -r : r;

        if (la > lp) lp = la;
        if (ra > rp) rp = ra;

        lsum += la;
        rsum += ra;
    }

    *l_peak = lp;
    *r_peak = rp;
    *l_avg = frame_count > 0 ? (int)(lsum / frame_count) : 0;
    *r_avg = frame_count > 0 ? (int)(rsum / frame_count) : 0;
}

/* =========================================================
 * AUDIO PROCESSING TASK
 * ========================================================= */

static void audio_processing_task(void *arg)
{
    ESP_LOGI(TAG, "Audio SR task started");
 
    if (!g_afe_handle || !g_afe_data) {
        ESP_LOGE(TAG, "AFE is not initialized");
        g_audio_state = AUDIO_STATE_ERROR;
        g_audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
 
    const int feed_chunksize = g_afe_handle->get_feed_chunksize(g_afe_data);
    const int feed_channels = g_afe_handle->get_feed_channel_num(g_afe_data);
    const int feed_samples = feed_chunksize * feed_channels;
    const int feed_bytes = feed_samples * sizeof(int16_t);
 
    int16_t *feed_buffer = (int16_t *)calloc(feed_samples, sizeof(int16_t));
    if (feed_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate feed_buffer");
        g_audio_state = AUDIO_STATE_ERROR;
        g_audio_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
 
    ESP_LOGI(TAG, "Ready: listening for wake word");
 
    g_audio_state = AUDIO_STATE_LISTENING;
    vTaskDelay(pdMS_TO_TICKS(300));
 
    while (g_audio_enabled) {
        memset(feed_buffer, 0, feed_bytes);
 
        int ret = esp_codec_dev_read(g_mic_handle, feed_buffer, feed_bytes);
 
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Mic read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
 
        // Feed audio to AFE
        g_afe_handle->feed(g_afe_data, feed_buffer);
 
        int fetch_count = feed_chunksize / g_afe_fetch_chunksize;
        if (fetch_count < 1) {
            fetch_count = 1;
        }
 
        for (int i = 0; i < fetch_count; i++) {
            afe_fetch_result_t *res = g_afe_handle->fetch(g_afe_data);
 
            if (res == NULL || res->ret_value == ESP_FAIL) {
                continue;
            }
 
            // ===== VAD: Detect if voice is present =====
            if (g_vad_enabled) {
                if (res->vad_state == VAD_SPEECH) {
                    if (g_vad_state != AUDIO_VAD_SPEECH) {
                        ESP_LOGI(TAG, "🎤 Voice detected");
                    }
                    g_vad_state = AUDIO_VAD_SPEECH;
                    g_vad_confidence = 1.0f;
 
                    if (!g_mn_session_active) {
                        g_audio_state = AUDIO_STATE_VOICE_DETECTED;
                    }
                } else {
                    if (g_vad_state == AUDIO_VAD_SPEECH) {
                        ESP_LOGI(TAG, "🔇 Silence detected");
                    }
                    g_vad_state = AUDIO_VAD_SILENCE;
                    g_vad_confidence = 0.0f;
 
                    if (!g_mn_session_active) {
                        g_audio_state = AUDIO_STATE_LISTENING;
                    }
                }
            }
 
            // ===== WAKENET: Detect the wake word =====
            if (g_kws_enabled && res->wakeup_state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "════════════════════════════════════════");
                ESP_LOGI(TAG, "⭐ WAKE WORD DETECTED - Listening for command");
                ESP_LOGI(TAG, "════════════════════════════════════════");
 
                g_mn_session_active = true;
                g_audio_state = AUDIO_STATE_VOICE_DETECTED;

                // After line 452
                ESP_LOGI(TAG, "DEBUG: WakeNet detected, starting MN session");
                ESP_LOGI(TAG, "  g_kws_enabled=%d", g_kws_enabled);
                ESP_LOGI(TAG, "  g_mn_handle=%p", (void*)g_mn_handle);
                ESP_LOGI(TAG, "  g_mn_data=%p", (void*)g_mn_data);
                ESP_LOGI(TAG, "  AFE handle=%p", (void*)g_afe_handle);
 
                // Reset counters
                g_mn_debug_count = 0;
                g_mn_frame_count = 0;
                g_mn_speech_frame_count = 0;
 
                // Clean MultiNet state
                if (g_mn_handle && g_mn_data && g_mn_handle->clean) {
                    g_mn_handle->clean(g_mn_data);
                }
 
                // Disable WakeNet while listening for command
                g_afe_handle->disable_wakenet(g_afe_data);

                ESP_LOGI(TAG,
         "MN session started: WakeNet disabled, waiting for command frames");
 
                // Skip this frame (wake-word frame itself)
                continue;
            }
 
            // ===== MULTINET: Detect the command word =====
            if (g_mn_session_active && ((g_mn_frame_count % 25) == 0)) {
                ESP_LOGI(TAG,
                        "MN gate: kws=%d session=%d handle=%p data=%p pcm=%p",
                        g_kws_enabled,
                        g_mn_session_active,
                        g_mn_handle,
                        g_mn_data,
                        res->data);
            }
    
            if (g_kws_enabled &&
                g_mn_session_active &&
                g_mn_handle &&
                g_mn_data &&
                res->data) {

                g_mn_frame_count++;

                if (res->vad_state == VAD_SPEECH) {
                    g_mn_speech_frame_count++;
                }

                if ((g_mn_frame_count % 10) == 0) {
                ESP_LOGI(TAG, "DEBUG MN frame %d: calling detect() with data=%p", 
                        g_mn_frame_count, (void*)res->data);
                
                // Check if res->data is valid
                if (res->data) {
                    int16_t *ptr = (int16_t*)res->data;
                    int peak = 0;
                    for (int i = 0; i < 512 && i < 1024; i++) {
                        int val = ptr[i] < 0 ? -ptr[i] : ptr[i];
                        if (val > peak) peak = val;
                    }
                    ESP_LOGI(TAG, "  Audio signal peak: %d (valid if > 100)", peak);
                }
            }

            esp_mn_state_t mn_state = g_mn_handle->detect(g_mn_data, res->data);

            if ((g_mn_frame_count % 10) == 0) {
                ESP_LOGI(TAG, "DEBUG: detect() returned state=%d", (int)mn_state);
            }

                // Clean periodic proof that MultiNet is alive
                if ((g_mn_frame_count % 25) == 0) {
                    ESP_LOGI(TAG,
                            "MN listening: frame=%d speech=%d vad=%s state=%d",
                            g_mn_frame_count,
                            g_mn_speech_frame_count,
                            res->vad_state == VAD_SPEECH ? "speech" : "silence",
                            mn_state);
                }

                if (mn_state == ESP_MN_STATE_DETECTED) {
                    esp_mn_results_t *mn_result = g_mn_handle->get_results(g_mn_data);

                    if (mn_result == NULL) {
                        ESP_LOGW(TAG,
                                "MN detected but result=NULL: frames=%d speech=%d",
                                g_mn_frame_count,
                                g_mn_speech_frame_count);
                    } else if (mn_result->num <= 0) {
                        ESP_LOGW(TAG,
                                "MN detected but result empty: num=%d frames=%d speech=%d",
                                mn_result->num,
                                g_mn_frame_count,
                                g_mn_speech_frame_count);
                    } else {
                        int phrase_id = mn_result->phrase_id[0];
                        float prob = mn_result->prob[0];
 
                        const kws_id_map_t *cmd = lookup_kws_by_phrase_id(phrase_id);
 
                        // ===== NEW DEBUG OUTPUT =====
                        ESP_LOGI(TAG, "");
                        ESP_LOGI(TAG, "═══════════════════════════════════════════");
                        ESP_LOGI(TAG, "📊 DETECTION ANALYSIS");
                        ESP_LOGI(TAG, "═══════════════════════════════════════════");
                        ESP_LOGI(TAG, "Phrase ID detected: %d", phrase_id);
                        ESP_LOGI(TAG, "Confidence: %.4f (threshold: %.4f)", prob, g_kws_confidence_threshold);
                        ESP_LOGI(TAG, "Frames processed: %d (speech frames: %d)", 
                                g_mn_frame_count, g_mn_speech_frame_count);
                        
                        // Print all results (not just top 1)
                        ESP_LOGI(TAG, "Total results returned: %d", mn_result->num);
                        for (int j = 0; j < mn_result->num && j < 5; j++) {
                            const kws_id_map_t *alt_cmd = lookup_kws_by_phrase_id(mn_result->phrase_id[j]);
                            ESP_LOGI(TAG, "  [%d] ID=%d, prob=%.4f %s",
                                    j,
                                    mn_result->phrase_id[j],
                                    mn_result->prob[j],
                                    alt_cmd ? alt_cmd->display_name : "UNKNOWN");
                        }
                        
                        if (cmd != NULL) {
                            ESP_LOGI(TAG, "Mapped to command: %s (phoneme: %s)", 
                                    cmd->display_name, cmd->keyword);
                        } else {
                            ESP_LOGI(TAG, "⚠️ Phrase ID %d not found in command map!", phrase_id);
                        }
                        ESP_LOGI(TAG, "═══════════════════════════════════════════");
                        ESP_LOGI(TAG, "");
                        // ===== END DEBUG OUTPUT =====
 
                        if (cmd != NULL && prob >= g_kws_confidence_threshold) {
                            set_kws_result(cmd->keyword, prob);
 
                            ESP_LOGI(TAG,
                                    "✅ COMMAND ACCEPTED: %s keyword=%s prob=%.2f",
                                    cmd->display_name,
                                    cmd->keyword,
                                    prob);
 
                        } else if (cmd == NULL) {
                            ESP_LOGW(TAG,
                                    "⚠️ COMMAND REJECTED: unknown phrase_id=%d prob=%.2f",
                                    phrase_id,
                                    prob);
 
                        } else {
                            ESP_LOGW(TAG,
                                    "⚠️ COMMAND REJECTED: %s prob=%.2f < threshold=%.2f",
                                    cmd->keyword,
                                    prob,
                                    g_kws_confidence_threshold);
                        }
                    }
 
                    sr_reset_to_wakenet();
                    ESP_LOGI(TAG, "Ready: listening for next wake word\n");
 
                } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
                    ESP_LOGI(TAG,
                            "⏱️  MN timeout: frames=%d speech=%d",
                            g_mn_frame_count,
                            g_mn_speech_frame_count);
                    
                    // Also print intermediate results if available
                    if (g_mn_handle && g_mn_data) {
                        esp_mn_results_t *partial_result = g_mn_handle->get_results(g_mn_data);
                        if (partial_result && partial_result->num > 0) {
                            ESP_LOGI(TAG, "Last partial result before timeout:");
                            for (int j = 0; j < partial_result->num && j < 5; j++) {
                                const kws_id_map_t *cmd = lookup_kws_by_phrase_id(partial_result->phrase_id[j]);
                                ESP_LOGI(TAG, "  [%d] ID=%d, prob=%.4f %s",
                                        j,
                                        partial_result->phrase_id[j],
                                        partial_result->prob[j],
                                        cmd ? cmd->display_name : "UNKNOWN");
                            }
                        }
                    }
 
                    sr_reset_to_wakenet();
                    ESP_LOGI(TAG, "Ready: listening for next wake word\n");
                }
            }
        }
 
        vTaskDelay(pdMS_TO_TICKS(1));
    }
 
    free(feed_buffer);
 
    ESP_LOGI(TAG, "Audio SR task stopped");
 
    g_audio_state = AUDIO_STATE_IDLE;
    g_audio_task_handle = NULL;
    vTaskDelete(NULL);
}

/* =========================================================
 * INITIALIZATION
 * ========================================================= */

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "Initializing audio + ESP-SR");
    ESP_LOGI(TAG, "Target codec format: %d Hz, %d channel(s), %d-bit",
             HW_SAMPLE_RATE,
             HW_CHANNELS,
             HW_BIT_DEPTH);

    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(HW_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            HW_CHANNELS == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        g_audio_state = AUDIO_STATE_ERROR;
        return err;
    }

    ESP_LOGI(TAG, "BSP audio/I2S initialized");

    g_mic_handle = bsp_audio_codec_microphone_init();
    if (g_mic_handle == NULL) {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
        g_audio_state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t mic_info = {
        .sample_rate = HW_SAMPLE_RATE,
        .channel = HW_CHANNELS,
        .bits_per_sample = HW_BIT_DEPTH,
        .channel_mask = HW_CHANNELS == 1 ? 0x01 : 0x03,
        .mclk_multiple = 0,
    };

    int codec_ret = esp_codec_dev_open(g_mic_handle, &mic_info);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open mic failed: %d", codec_ret);
        g_audio_state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }

    codec_ret = esp_codec_dev_set_in_gain(g_mic_handle, AUDIO_MIC_GAIN_DB);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_in_gain failed: %d", codec_ret);
    }

    codec_ret = esp_codec_dev_set_in_channel_gain(g_mic_handle,
                                                  HW_CHANNELS == 1 ? 0x01 : 0x03,
                                                  AUDIO_MIC_GAIN_DB);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_in_channel_gain failed: %d", codec_ret);
    }

    codec_ret = esp_codec_dev_set_in_mute(g_mic_handle, false);
    if (codec_ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_in_mute(false) failed: %d", codec_ret);
    }

    ESP_LOGI(TAG, "Mic codec opened and unmuted");

    err = sr_init_afe();
    if (err != ESP_OK) {
        g_audio_state = AUDIO_STATE_ERROR;
        return err;
    }

    err = sr_init_multinet();
    if (err != ESP_OK) {
        g_audio_state = AUDIO_STATE_ERROR;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));

    g_audio_enabled = true;
    g_audio_state = AUDIO_STATE_IDLE;

    ESP_LOGI(TAG, "Audio + ESP-SR initialized successfully");
    return ESP_OK;
}

/* =========================================================
 * START / STOP / DEINIT
 * ========================================================= */

esp_err_t audio_start_processing(uint8_t task_priority)
{
    if (g_mic_handle == NULL || g_afe_data == NULL) {
        ESP_LOGE(TAG, "audio_start_processing called before audio_init");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_audio_task_handle != NULL) {
        ESP_LOGW(TAG, "Audio task already running");
        return ESP_OK;
    }

    g_audio_enabled = true;

    BaseType_t result = xTaskCreatePinnedToCore(
        audio_processing_task,
        "audio_sr",
        AUDIO_TASK_STACK_SIZE,
        NULL,
        task_priority,
        &g_audio_task_handle,
        1
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio_sr task");
        g_audio_task_handle = NULL;
        g_audio_state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio SR task created with priority %d", task_priority);
    return ESP_OK;
}

esp_err_t audio_stop_processing(void)
{
    ESP_LOGI(TAG, "Stopping audio SR task");

    g_audio_enabled = false;

    int wait_count = 0;
    while (g_audio_task_handle != NULL && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }

    if (g_audio_task_handle != NULL) {
        ESP_LOGW(TAG, "Audio SR task did not stop cleanly within timeout");
    }

    g_audio_state = AUDIO_STATE_IDLE;
    return ESP_OK;
}

esp_err_t audio_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing audio + ESP-SR");

    audio_stop_processing();

    if (g_mic_handle != NULL) {
        esp_codec_dev_set_in_mute(g_mic_handle, true);
        esp_codec_dev_close(g_mic_handle);
        g_mic_handle = NULL;
    }

    if (g_mn_handle && g_mn_data) {
        g_mn_handle->destroy(g_mn_data);
        g_mn_data = NULL;
        g_mn_handle = NULL;
    }

    if (g_afe_handle && g_afe_data) {
        g_afe_handle->destroy(g_afe_data);
        g_afe_data = NULL;
        g_afe_handle = NULL;
    }

    g_sr_models = NULL;

    memset(&g_kws_result, 0, sizeof(g_kws_result));
    g_vad_state = AUDIO_VAD_SILENCE;
    g_audio_state = AUDIO_STATE_IDLE;
    g_mn_session_active = false;

    ESP_LOGI(TAG, "Audio + ESP-SR deinitialized");
    return ESP_OK;
}

/* =========================================================
 * PUBLIC GETTERS / SETTERS
 * ========================================================= */

audio_state_t audio_get_state(void)
{
    return g_audio_state;
}

audio_vad_state_t audio_get_vad_state(void)
{
    return g_vad_state;
}

float audio_get_vad_confidence(void)
{
    return g_vad_confidence;
}

kws_result_t audio_get_kws_result(void)
{
    return g_kws_result;
}

void audio_reset_kws_result(void)
{
    memset(&g_kws_result, 0, sizeof(g_kws_result));

    if (g_audio_state == AUDIO_STATE_KEYWORD_DETECTED) {
        g_audio_state = AUDIO_STATE_LISTENING;
    }
}

bool audio_keyword_detected(void)
{
    return g_kws_result.detected;
}

const char* audio_get_detected_keyword(void)
{
    if (g_kws_result.detected && strlen(g_kws_result.keyword) > 0) {
        return g_kws_result.keyword;
    }
    return "";
}

void audio_set_vad_enabled(bool enabled)
{
    g_vad_enabled = enabled;

    if (g_afe_handle && g_afe_data) {
        if (enabled) {
            g_afe_handle->enable_vad(g_afe_data);
        } else {
            g_afe_handle->disable_vad(g_afe_data);
            g_vad_state = AUDIO_VAD_SILENCE;
            g_vad_confidence = 0.0f;
        }
    }

    ESP_LOGI(TAG, "VAD %s", enabled ? "enabled" : "disabled");
}

void audio_set_kws_enabled(bool enabled)
{
    g_kws_enabled = enabled;

    if (!enabled) {
        g_mn_session_active = false;

        if (g_afe_handle && g_afe_data) {
            g_afe_handle->disable_wakenet(g_afe_data);
        }
    } else {
        if (g_afe_handle && g_afe_data) {
            g_afe_handle->enable_wakenet(g_afe_data);
        }
    }

    ESP_LOGI(TAG, "KWS/WakeNet %s", enabled ? "enabled" : "disabled");
}

void audio_set_vad_threshold(float threshold)
{
    /*
     * AFE VAD thresholds are controlled through afe_config before create_from_config().
     * This function is kept for compatibility with your existing main.c.
     */
    ESP_LOGI(TAG, "AFE VAD threshold runtime setter not used: %.2f", threshold);
}

void audio_set_kws_threshold(float threshold)
{
    if (threshold < 0.0f || threshold > 1.0f) {
        ESP_LOGW(TAG, "Invalid KWS threshold %.2f, ignoring", threshold);
        return;
    }

    g_kws_confidence_threshold = threshold;
    ESP_LOGI(TAG, "KWS confidence threshold set to %.2f", threshold);
}
