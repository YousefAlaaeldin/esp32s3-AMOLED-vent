#ifndef AUDIO_HELPER_H
#define AUDIO_HELPER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* =========================================================
 * AUDIO CONFIGURATION
 * ========================================================= */

/* VAD target (after resampling) */
#define VAD_SAMPLE_RATE       16000
#define VAD_FRAME_SIZE        480      // 30 ms mono @ 16 kHz
#define VAD_FRAME_MS          30

#define VAD_START_TRIGGER_FRAMES    2
#define VAD_END_TRIGGER_FRAMES      15

/* Hardware audio interface (ES7210 + I2S) */
#define HW_SAMPLE_RATE        16000
#define HW_CHANNELS           2
#define HW_BIT_DEPTH          16

#define MONO_FRAME_SAMPLES    VAD_FRAME_SIZE       // 30 ms of mono audio at 16 kHz

#define AUDIO_TASK_STACK_SIZE 8192
#define AUDIO_TASK_PRIORITY   1

#define AUDIO_MIC_GAIN_DB     24.0f


/* =========================================================
 * STATE ENUMS
 * ========================================================= */

typedef enum {
    AUDIO_STATE_IDLE,
    AUDIO_STATE_LISTENING,
    AUDIO_STATE_VOICE_DETECTED,
    AUDIO_STATE_KEYWORD_DETECTED,
    AUDIO_STATE_ERROR
} audio_state_t;

typedef enum {
    AUDIO_VAD_SILENCE,
    AUDIO_VAD_SPEECH,
    AUDIO_VAD_PROCESSING
} audio_vad_state_t;

/* =========================================================
 * RESULT STRUCTURES
 * ========================================================= */

typedef struct {
    audio_vad_state_t state;
    float confidence;      // 0.0 - 1.0
} vad_result_t;

typedef struct {
    bool detected;
    float confidence;      // 0.0 - 1.0
    char keyword[64];      // Detected keyword name
} kws_result_t;

typedef struct {
    audio_state_t state;
    vad_result_t vad;
    kws_result_t kws;
} audio_result_t;

/* =========================================================
 * PUBLIC FUNCTIONS
 * ========================================================= */

/**
 * @brief Initialize audio system
 * Sets up I2S, microphone codec, VAD, and KWS models
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t audio_init(void);

/**
 * @brief Deinitialize audio system
 * Stops audio processing task and frees resources
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t audio_deinit(void);

/**
 * @brief Start audio processing task
 * Must be called after audio_init()
 * 
 * @param task_priority Priority of the processing task
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t audio_start_processing(uint8_t task_priority);

/**
 * @brief Stop audio processing task
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t audio_stop_processing(void);

/**
 * @brief Get current audio state
 * 
 * @return Current audio processing state
 */
audio_state_t audio_get_state(void);

/**
 * @brief Get VAD state
 * 
 * @return VAD detection result
 */
audio_vad_state_t audio_get_vad_state(void);

/**
 * @brief Get last VAD confidence
 * 
 * @return Confidence value 0.0 - 1.0
 */
float audio_get_vad_confidence(void);

/**
 * @brief Get last KWS result
 * 
 * @return KWS detection result
 */
kws_result_t audio_get_kws_result(void);

/**
 * @brief Reset KWS result (clear last detection)
 */
void audio_reset_kws_result(void);

/**
 * @brief Check if keyword was detected since last reset
 * 
 * @return true if keyword detected, false otherwise
 */
bool audio_keyword_detected(void);

/**
 * @brief Get detected keyword name
 * 
 * @return Pointer to keyword string
 */
const char* audio_get_detected_keyword(void);

/**
 * @brief Enable/disable VAD processing
 * 
 * @param enabled true to enable, false to disable
 */
void audio_set_vad_enabled(bool enabled);

/**
 * @brief Enable/disable KWS processing
 * 
 * @param enabled true to enable, false to disable
 */
void audio_set_kws_enabled(bool enabled);

/**
 * @brief Set VAD detection threshold
 * Default is 0.5, higher = less sensitive
 * 
 * @param threshold Value 0.0 - 1.0
 */
void audio_set_vad_threshold(float threshold);

/**
 * @brief Set KWS detection threshold
 * Default is 0.5, higher = less sensitive
 * 
 * @param threshold Value 0.0 - 1.0
 */
void audio_set_kws_threshold(float threshold);

#endif // AUDIO_HELPER_H