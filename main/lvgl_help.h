#ifndef THERMOSTAT_UI_H
#define THERMOSTAT_UI_H

#include "lvgl.h"        // LVGL types are needed for the state struct
#include <stdbool.h>     // for bool
#include "lvgl_help.h"    // for thermostat_mode_t

/* ---- Mode enum (needed by main if you want to check/set mode) ---- */
typedef enum {
    MODE_OFF = 0,
    MODE_AUTO,
    MODE_HEAT,
    MODE_COOL,
    MODE_ECO
} thermostat_mode_t;

/* ---- Full state struct (same as in lvgl_help.c) ---- */
typedef struct {
    float current_temp;
    float target_temp;
    thermostat_mode_t mode;
    bool use_celsius;
    bool dimmed;
} thermostat_state_t;

/* ---- Global state – so main.c can read/write it directly ---- */
extern thermostat_state_t g_state;

/* ---- Public functions ---- */
void create_ui(void);           // builds the whole screen
void update_ui(void);           // refreshes temps, arc, mode

#endif