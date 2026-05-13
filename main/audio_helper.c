#include "audio_helper.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2s_std.h"

#include "bsp/esp32_s3_touch_amoled_1_75.h"

static const char *TAG = "AUDIO_HELPER_MIC_TEST";

/* =========================================================
 * GLOBAL STATE
 * ========================================================= */

static audio_state_t g_audio_state = AUDIO_STATE_IDLE;
static audio_vad_state_t g_vad_state = AUDIO_VAD_SILENCE;
static kws_result_t g_kws_result = {0};

static TaskHandle_t g_audio_task_handle = NULL;
static bool g_audio_enabled = false;

static esp_codec_dev_handle_t g_mic_handle = NULL;

/* =========================================================
 * MIC TEST CONFIG
 * ========================================================= */

/*
 * Start simple:
 * - 22050 Hz because the BSP default also uses 22050
 * - MONO because bsp_audio_init(NULL) originally configures MONO
 * - 16-bit PCM
 *
 * After this works, we can test stereo / dual mic.
 */
#ifndef HW_SAMPLE_RATE
#define HW_SAMPLE_RATE 22050
#endif

#ifndef HW_CHANNELS
#define HW_CHANNELS 1
#endif

#ifndef HW_BIT_DEPTH
#define HW_BIT_DEPTH 16
#endif

#ifndef AUDIO_MIC_GAIN_DB
#define AUDIO_MIC_GAIN_DB 24.0f
#endif

#define MIC_READ_FRAMES       MONO_FRAME_SAMPLES
#define MIC_READ_SAMPLES      (MIC_READ_FRAMES * HW_CHANNELS)
#define MIC_READ_BYTES        (MIC_READ_SAMPLES * sizeof(int16_t))


/* =========================================================
 * SMALL AUDIO DEBUG HELPER
 * ========================================================= */

static void print_stereo_audio_stats(const int16_t *samples, int sample_count, int loop_count, int bytes_read)
{
    if (sample_count <= 0 || samples == NULL) {
        ESP_LOGI(TAG, "[%d] bytes_read=%d, samples=0", loop_count, bytes_read);
        return;
    }

    int left_peak = 0;
    int right_peak = 0;

    int64_t left_sum_abs = 0;
    int64_t right_sum_abs = 0;

    int64_t left_sum = 0;
    int64_t right_sum = 0;

    int frame_count = sample_count / 2;

    for (int i = 0; i < frame_count; i++) {
        int16_t left = samples[i * 2];
        int16_t right = samples[i * 2 + 1];

        int left_abs = left < 0 ? -left : left;
        int right_abs = right < 0 ? -right : right;

        if (left_abs > left_peak) {
            left_peak = left_abs;
        }

        if (right_abs > right_peak) {
            right_peak = right_abs;
        }

        left_sum_abs += left_abs;
        right_sum_abs += right_abs;

        left_sum += left;
        right_sum += right;
    }

    int left_avg_abs = frame_count > 0 ? (int)(left_sum_abs / frame_count) : 0;
    int right_avg_abs = frame_count > 0 ? (int)(right_sum_abs / frame_count) : 0;

    int left_dc = frame_count > 0 ? (int)(left_sum / frame_count) : 0;
    int right_dc = frame_count > 0 ? (int)(right_sum / frame_count) : 0;

    ESP_LOGI(TAG,
             "[%d] bytes=%d frames=%d | L peak=%d avg=%d dc=%d | R peak=%d avg=%d dc=%d | first LR=[%d,%d,%d,%d,%d,%d,%d,%d]",
             loop_count,
             bytes_read,
             frame_count,
             left_peak,
             left_avg_abs,
             left_dc,
             right_peak,
             right_avg_abs,
             right_dc,
             samples[0],
             samples[1],
             samples[2],
             samples[3],
             samples[4],
             samples[5],
             samples[6],
             samples[7]);
}




/* =========================================================
 * stereo to mono convertor for vad/kws pre-processing
 * ========================================================= */
static void stereo_to_mono_avg(const int16_t *stereo_samples,
                               int16_t *mono_samples,
                               int frame_count)
{
    for (int i = 0; i < frame_count; i++) {
        int32_t left = stereo_samples[i * 2];
        int32_t right = stereo_samples[i * 2 + 1];

        mono_samples[i] = (int16_t)((left + right) / 2);
    }
}

static void print_mono_audio_stats(const int16_t *samples,
                                   int sample_count,
                                   int loop_count,
                                   int bytes_read)
{
    if (sample_count <= 0 || samples == NULL) {
        ESP_LOGI(TAG, "[%d] mono samples=0", loop_count);
        return;
    }

    int peak = 0;
    int64_t sum_abs = 0;
    int64_t sum = 0;

    for (int i = 0; i < sample_count; i++) {
        int v = samples[i];
        int av = v < 0 ? -v : v;

        if (av > peak) {
            peak = av;
        }

        sum_abs += av;
        sum += v;
    }

    int avg_abs = (int)(sum_abs / sample_count);
    int dc_offset = (int)(sum / sample_count);

    ESP_LOGI(TAG,
             "[%d] stereo_bytes=%d mono_samples=%d | MONO peak=%d avg=%d dc=%d first=[%d,%d,%d,%d,%d,%d,%d,%d]",
             loop_count,
             bytes_read,
             sample_count,
             peak,
             avg_abs,
             dc_offset,
             samples[0],
             sample_count > 1 ? samples[1] : 0,
             sample_count > 2 ? samples[2] : 0,
             sample_count > 3 ? samples[3] : 0,
             sample_count > 4 ? samples[4] : 0,
             sample_count > 5 ? samples[5] : 0,
             sample_count > 6 ? samples[6] : 0,
             sample_count > 7 ? samples[7] : 0);
}

/* =========================================================
 * AUDIO PROCESSING TASK - MIC TEST ONLY
 * ========================================================= */

static void audio_processing_task(void *arg)
{
    ESP_LOGI(TAG, "Mic test task started");

    static int16_t stereo_buffer[MIC_READ_SAMPLES];
    static int16_t mono_buffer[MONO_FRAME_SAMPLES];

    int loop_count = 0;
    int zero_read_count = 0;
    int error_read_count = 0;
    int good_read_count = 0;

    g_audio_state = AUDIO_STATE_LISTENING;

    /*
     * Give codec and I2S a short time after opening.
     */
    vTaskDelay(pdMS_TO_TICKS(300));

    while (g_audio_enabled) {
        loop_count++;

        memset(stereo_buffer, 0, sizeof(stereo_buffer));

        int ret = esp_codec_dev_read(g_mic_handle, stereo_buffer, MIC_READ_BYTES);

        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int bytes_read = MIC_READ_BYTES;
        int samples_read = bytes_read / sizeof(int16_t);

        int frame_count = samples_read / HW_CHANNELS;

        if (frame_count > MONO_FRAME_SAMPLES) {
            frame_count = MONO_FRAME_SAMPLES;
        }

        stereo_to_mono_avg(stereo_buffer, mono_buffer, frame_count);

        /*
         * Print every successful read for first few reads,
         * 
         */
        good_read_count++;

        if (good_read_count <= 50 || good_read_count % 50 == 0) {
            print_mono_audio_stats(mono_buffer, samples_read, loop_count, bytes_read);
        }

        /*
         * Small delay only to reduce log spam.
         * esp_codec_dev_read may already block depending on lower driver behavior.
         */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Mic test task stopped");

    g_audio_state = AUDIO_STATE_IDLE;
    g_audio_task_handle = NULL;
    vTaskDelete(NULL);
}

/* =========================================================
 * INITIALIZATION - MIC ONLY
 * ========================================================= */

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "Initializing MIC-ONLY audio test");
    ESP_LOGI(TAG, "Target format: %d Hz, %d channel(s), %d-bit",
             HW_SAMPLE_RATE, HW_CHANNELS, HW_BIT_DEPTH);

    /*
     * IMPORTANT:
     * We explicitly configure I2S instead of using bsp_audio_init(NULL),
     * because the previous code opened the mic as stereo while the BSP default
     * is mono. For this first test, keep everything MONO.
     */
    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(HW_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT,
            I2S_SLOT_MODE_STEREO
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

    /*
     * MIC ONLY.
     * Do NOT initialize speaker here.
     * We want to isolate the RX path first.
     */
    g_mic_handle = bsp_audio_codec_microphone_init();
    if (g_mic_handle == NULL) {
        ESP_LOGE(TAG, "bsp_audio_codec_microphone_init failed");
        g_audio_state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Microphone codec handle created");

    esp_codec_dev_sample_info_t mic_info = {
        .sample_rate = HW_SAMPLE_RATE,
        .channel = HW_CHANNELS,
        .bits_per_sample = HW_BIT_DEPTH,
        .channel_mask = 0x03,  // Try enabling both channels if codec supports it, even if we're only reading mono. This is because some codecs (like ES7210) have internal MIC1/MIC2 but expose only MONO externally.
        .mclk_multiple = 0,
    };

    int ret = esp_codec_dev_open(g_mic_handle, &mic_info);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open mic failed: %d", ret);
        g_audio_state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Mic codec opened");

    /*
     * Set input gain.
     * If this returns an error, do not fail the whole test.
     */
    ret = esp_codec_dev_set_in_gain(g_mic_handle, AUDIO_MIC_GAIN_DB);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_in_gain failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Mic input gain set to %.1f dB", AUDIO_MIC_GAIN_DB);
    }

    /*
     * For ES7210, try enabling gain on both possible mic channels.
     * Even though this first test reads MONO, the codec may expose MIC1/MIC2 internally.
     */
    ret = esp_codec_dev_set_in_channel_gain(g_mic_handle, 0x03, AUDIO_MIC_GAIN_DB);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_in_channel_gain failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Mic channel gain set on mask 0x03 to %.1f dB", AUDIO_MIC_GAIN_DB);
    }

    /*
     * Critical: unmute input.
     */
    ret = esp_codec_dev_set_in_mute(g_mic_handle, false);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_in_mute(false) failed: %d", ret);
    } else {
        ESP_LOGI(TAG, "Mic input unmuted");
    }

    /*
     * Stabilization delay.
     */
    vTaskDelay(pdMS_TO_TICKS(1000));

    g_audio_enabled = true;
    g_audio_state = AUDIO_STATE_IDLE;

    ESP_LOGI(TAG, "MIC-ONLY audio test initialized successfully");
    return ESP_OK;
}

/* =========================================================
 * START / STOP / DEINIT
 * ========================================================= */

esp_err_t audio_start_processing(uint8_t task_priority)
{
    if (g_mic_handle == NULL) {
        ESP_LOGE(TAG, "audio_start_processing called before audio_init");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_audio_task_handle != NULL) {
        ESP_LOGW(TAG, "Mic test task already running");
        return ESP_OK;
    }

    g_audio_enabled = true;

    BaseType_t result = xTaskCreatePinnedToCore(
        audio_processing_task,
        "mic_test",
        AUDIO_TASK_STACK_SIZE,
        NULL,
        task_priority,
        &g_audio_task_handle,
        1
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mic test task");
        g_audio_task_handle = NULL;
        g_audio_state = AUDIO_STATE_ERROR;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Mic test task created with priority %d", task_priority);
    return ESP_OK;
}

esp_err_t audio_stop_processing(void)
{
    ESP_LOGI(TAG, "Stopping mic test task");

    g_audio_enabled = false;

    int wait_count = 0;
    while (g_audio_task_handle != NULL && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }

    if (g_audio_task_handle != NULL) {
        ESP_LOGW(TAG, "Mic test task did not stop cleanly within timeout");
    }

    g_audio_state = AUDIO_STATE_IDLE;
    return ESP_OK;
}

esp_err_t audio_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing mic-only audio test");

    audio_stop_processing();

    if (g_mic_handle != NULL) {
        esp_codec_dev_set_in_mute(g_mic_handle, true);
        esp_codec_dev_close(g_mic_handle);
        g_mic_handle = NULL;
    }

    memset(&g_kws_result, 0, sizeof(g_kws_result));
    g_vad_state = AUDIO_VAD_SILENCE;
    g_audio_state = AUDIO_STATE_IDLE;

    ESP_LOGI(TAG, "Mic-only audio test deinitialized");
    return ESP_OK;
}

/* =========================================================
 * COMPATIBILITY FUNCTIONS
 * These remain so main.c does not break.
 * VAD/KWS are intentionally disabled for this test.
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
    return 0.0f;
}

kws_result_t audio_get_kws_result(void)
{
    return g_kws_result;
}

void audio_reset_kws_result(void)
{
    memset(&g_kws_result, 0, sizeof(g_kws_result));
}

bool audio_keyword_detected(void)
{
    return false;
}

const char* audio_get_detected_keyword(void)
{
    return "";
}

void audio_set_vad_enabled(bool enabled)
{
    ESP_LOGI(TAG, "VAD is disabled in mic-only test build. Requested: %s",
             enabled ? "enabled" : "disabled");
}

void audio_set_kws_enabled(bool enabled)
{
    ESP_LOGI(TAG, "KWS is disabled in mic-only test build. Requested: %s",
             enabled ? "enabled" : "disabled");
}

void audio_set_vad_threshold(float threshold)
{
    ESP_LOGI(TAG, "VAD threshold ignored in mic-only test build: %.2f", threshold);
}

void audio_set_kws_threshold(float threshold)
{
    ESP_LOGI(TAG, "KWS threshold ignored in mic-only test build: %.2f", threshold);
}