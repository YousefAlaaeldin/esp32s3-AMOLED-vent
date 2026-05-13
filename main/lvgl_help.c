#include "lvgl.h"
#include "bsp/display.h"    // for bsp_display_lock / unlock
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"


/* =========================================================
 * CONFIG (new UI)
 * ========================================================= */

#define SCREEN_SIZE            466

#define TEMP_MIN_F             50
#define TEMP_MAX_F             90

#define ARC_SIZE               360
#define ARC_WIDTH              8

#define TEMP_FONT              &lv_font_montserrat_48
#define MODE_FONT              &lv_font_montserrat_28
#define BUTTON_FONT            &lv_font_montserrat_36

/* =========================================================
 * MODES
 * ========================================================= */

typedef enum {
    MODE_OFF = 0,
    MODE_AUTO,
    MODE_HEAT,
    MODE_COOL,
    MODE_ECO
} thermostat_mode_t;

/* =========================================================
 * STATE
 * ========================================================= */

typedef struct {
    float current_temp;
    float target_temp;

    thermostat_mode_t mode;

    bool use_celsius;
    bool dimmed;
} thermostat_state_t;

thermostat_state_t g_state = {
    .current_temp = 68.0f,
    .target_temp = 72.0f,
    .mode = MODE_COOL,
    .use_celsius = false,
    .dimmed = false
};

/* =========================================================
 * GLOBAL OBJECTS
 * ========================================================= */

static lv_obj_t *root;
static lv_obj_t *arc;

static lv_obj_t *current_label;
static lv_obj_t *target_label;
static lv_obj_t *divider_label;

static lv_obj_t *minus_btn;
static lv_obj_t *plus_btn;

static lv_obj_t *mode_container;
static lv_obj_t *mode_label;

/* =========================================================
 * COLORS
 * ========================================================= */

static lv_color_t mode_color(void)
{
    switch(g_state.mode) {

        case MODE_HEAT:
            return lv_color_hex(0xff7a00);

        case MODE_COOL:
            return lv_color_hex(0x3aa6ff);

        case MODE_ECO:
            return lv_color_hex(0x4cd964);

        case MODE_AUTO:
            return lv_color_hex(0xffffff);

        default:
            return lv_color_hex(0xffffff);
    }
}

/* =========================================================
 * HELPERS
 * ========================================================= */

static int temp_to_arc(float temp)
{
    float normalized =
        (temp - TEMP_MIN_F) /
        (TEMP_MAX_F - TEMP_MIN_F);

    if(normalized < 0.0f) normalized = 0.0f;
    if(normalized > 1.0f) normalized = 1.0f;

    return (int)(normalized * 100.0f);
}

/* =========================================================
 * UI UPDATE
 * ========================================================= */

static void update_temperatures(void)
{
    char buf[16];

    snprintf(buf, sizeof(buf),
             "%.0f°",
             g_state.current_temp);

    lv_label_set_text(current_label, buf);

    snprintf(buf, sizeof(buf),
             "%.0f°",
             g_state.target_temp);

    lv_label_set_text(target_label, buf);
}

static void update_arc(void)
{
    lv_color_t c = mode_color();

    if(g_state.mode == MODE_OFF) {

        lv_obj_fade_out(arc, 250, 0);

        return;
    }

    lv_obj_fade_in(arc, 250, 0);

    lv_obj_set_style_arc_color(
        arc,
        c,
        LV_PART_INDICATOR);

    lv_arc_set_value(
        arc,
        temp_to_arc(g_state.target_temp));
}

static void update_mode_label(void)
{
    switch(g_state.mode) {

        case MODE_OFF:
            lv_label_set_text(mode_label, "< OFF >");
            break;

        case MODE_AUTO:
            lv_label_set_text(mode_label, " AUTO ");
            break;

        case MODE_HEAT:
            lv_label_set_text(mode_label, " HEAT ");
            break;

        case MODE_COOL:
            lv_label_set_text(mode_label, " COOL ");
            break;

        case MODE_ECO:
            lv_label_set_text(mode_label, " ECO ");
            break;
    }

    lv_obj_set_style_text_color(
        mode_label,
        mode_color(),
        0);
}

void update_ui(void)
{
    update_temperatures();
    update_arc();
    update_mode_label();
}

/* =========================================================
 * EVENTS
 * ========================================================= */

static void increase_temp(lv_event_t *e)
{
    LV_UNUSED(e);

    g_state.target_temp++;

    if(g_state.target_temp > TEMP_MAX_F)
        g_state.target_temp = TEMP_MAX_F;

    update_ui();
}

static void decrease_temp(lv_event_t *e)
{
    LV_UNUSED(e);

    g_state.target_temp--;

    if(g_state.target_temp < TEMP_MIN_F)
        g_state.target_temp = TEMP_MIN_F;

    update_ui();
}

static void mode_swipe_event(lv_event_t *e)
{
    LV_UNUSED(e);

    printf("DEBUG: Gesture event detected!\n");

    lv_dir_t dir = lv_indev_get_gesture_dir(
        lv_indev_active());

    printf("DEBUG: Gesture direction = %d (LEFT=1, RIGHT=2)\n", dir);

    if(dir == LV_DIR_LEFT) {
        printf("DEBUG: Swipe LEFT detected\n");

        g_state.mode++;

        if(g_state.mode > MODE_ECO)
            g_state.mode = MODE_OFF;
    }

    else if(dir == LV_DIR_RIGHT) {
        printf("DEBUG: Swipe RIGHT detected\n");

        g_state.mode--;

        if(g_state.mode < MODE_OFF)
            g_state.mode = MODE_ECO;
    }

    update_ui();
}

static void unit_toggle_event(lv_event_t *e)
{
    LV_UNUSED(e);

    g_state.use_celsius =
        !g_state.use_celsius;
}

/* =========================================================
 * CREATE HELPERS
 * ========================================================= */

static lv_obj_t *create_circle_button(
    lv_obj_t *parent,
    const char *txt)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, 56, 56);

    lv_obj_set_style_radius(
        btn,
        LV_RADIUS_CIRCLE,
        0);

    lv_obj_set_style_bg_color(
        btn,
        lv_color_hex(0x1c1c1c),
        0);

    lv_obj_set_style_bg_opa(
        btn,
        LV_OPA_70,
        0);

    lv_obj_set_style_border_width(
        btn,
        0,
        0);

    lv_obj_t *label =
        lv_label_create(btn);

    lv_label_set_text(label, txt);

    lv_obj_set_style_text_font(
        label,
        BUTTON_FONT,
        0);

    lv_obj_center(label);

    return btn;
}

/* =========================================================
 * CREATE UI
 * ========================================================= */

static void create_background(void)
{
    root = lv_screen_active();

    lv_obj_set_style_bg_color(
        root,
        lv_color_black(),
        0);

    lv_obj_set_style_bg_opa(
        root,
        LV_OPA_COVER,
        0);
}

static void create_arc_widget(void)
{
    arc = lv_arc_create(root);

    lv_obj_set_size(
        arc,
        ARC_SIZE,
        ARC_SIZE);

    lv_obj_center(arc);

    lv_obj_set_y(arc, -20);

    lv_arc_set_range(arc, 0, 100);

    lv_arc_set_bg_angles(
        arc,
        135,
        45);

    lv_obj_remove_style(
        arc,
        NULL,
        LV_PART_KNOB);

    lv_obj_set_style_arc_width(
        arc,
        ARC_WIDTH,
        LV_PART_MAIN);

    lv_obj_set_style_arc_width(
        arc,
        ARC_WIDTH,
        LV_PART_INDICATOR);

    lv_obj_set_style_arc_color(
        arc,
        lv_color_hex(0x202020),
        LV_PART_MAIN);

    lv_obj_set_style_arc_rounded(
        arc,
        true,
        LV_PART_INDICATOR);

    // Ensure arc doesn't capture any touches
    lv_obj_clear_flag(
        arc,
        LV_OBJ_FLAG_CLICKABLE);

    // Send to back (lowest z-order)
    lv_obj_move_to_index(arc, 0);

    printf("DEBUG: Arc created - not clickable\n");
}

static void create_temperature_labels(void)
{
    current_label =
        lv_label_create(root);

    lv_obj_set_style_text_font(
        current_label,
        TEMP_FONT,
        0);

    lv_obj_set_style_text_color(
        current_label,
        lv_color_white(),
        0);

    lv_obj_align(
        current_label,
        LV_ALIGN_CENTER,
        -70,
        -10);

    divider_label =
        lv_label_create(root);

    lv_label_set_text(
        divider_label,
        "|");

    lv_obj_set_style_text_font(
        divider_label,
        &lv_font_montserrat_28,
        0);

    lv_obj_set_style_text_color(
        divider_label,
        lv_color_hex(0x404040),
        0);

    lv_obj_align(
        divider_label,
        LV_ALIGN_CENTER,
        0,
        -10);

    target_label =
        lv_label_create(root);

    lv_obj_set_style_text_font(
        target_label,
        TEMP_FONT,
        0);

    lv_obj_set_style_text_color(
        target_label,
        lv_color_white(),
        0);

    lv_obj_align(
        target_label,
        LV_ALIGN_CENTER,
        70,
        -10);
}

static void create_buttons(void)
{
    // Minus button - under left temperature
    minus_btn =
        create_circle_button(
            root,
            "-");

    lv_obj_align(
        minus_btn,
        LV_ALIGN_CENTER,
        -70,
        60);

    // Plus button - under right temperature
    plus_btn =
        create_circle_button(
            root,
            "+");

    lv_obj_align(
        plus_btn,
        LV_ALIGN_CENTER,
        70,
        60);

    lv_obj_add_event_cb(
        minus_btn,
        decrease_temp,
        LV_EVENT_CLICKED,
        NULL);

    lv_obj_add_event_cb(
        plus_btn,
        increase_temp,
        LV_EVENT_CLICKED,
        NULL);
}

static void mode_prev_btn(lv_event_t *e)
{
    LV_UNUSED(e);

    printf("DEBUG: Mode PREV button clicked\n");

    g_state.mode--;

    if(g_state.mode < MODE_OFF)
        g_state.mode = MODE_ECO;

    update_ui();
}

static void mode_next_btn(lv_event_t *e)
{
    LV_UNUSED(e);

    printf("DEBUG: Mode NEXT button clicked\n");

    g_state.mode++;

    if(g_state.mode > MODE_ECO)
        g_state.mode = MODE_OFF;

    update_ui();
}

static void create_mode_carousel(void)
{
    // Main container with flex layout (row)
    mode_container =
        lv_obj_create(root);

    lv_obj_remove_style_all(
        mode_container);

    lv_obj_set_size(
        mode_container,
        LV_PCT(100),
        60);

    lv_obj_align(
        mode_container,
        LV_ALIGN_BOTTOM_MID,
        0,
        -20);

    // Layout: [<] [MODE] [>] horizontally centered
    lv_obj_set_flex_flow(mode_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mode_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Left button: PREVIOUS mode
    lv_obj_t *btn_prev = lv_button_create(mode_container);
    lv_obj_set_size(btn_prev, 50, 50);
    lv_obj_set_style_radius(btn_prev, 8, 0);
    lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x1c1c1c), 0);
    lv_obj_set_style_bg_opa(btn_prev, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn_prev, 0, 0);

    lv_obj_t *btn_prev_label = lv_label_create(btn_prev);
    lv_label_set_text(btn_prev_label, "<");
    lv_obj_set_style_text_font(btn_prev_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(btn_prev_label, lv_color_white(), 0);
    lv_obj_center(btn_prev_label);

    lv_obj_add_event_cb(btn_prev, mode_prev_btn, LV_EVENT_CLICKED, NULL);

    // Center: Mode label
    mode_label =
        lv_label_create(mode_container);

    lv_obj_set_style_text_font(
        mode_label,
        MODE_FONT,
        0);

    lv_label_set_text(mode_label, " COOL ");

    // Right button: NEXT mode
    lv_obj_t *btn_next = lv_button_create(mode_container);
    lv_obj_set_size(btn_next, 50, 50);
    lv_obj_set_style_radius(btn_next, 8, 0);
    lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x1c1c1c), 0);
    lv_obj_set_style_bg_opa(btn_next, LV_OPA_70, 0);
    lv_obj_set_style_border_width(btn_next, 0, 0);

    lv_obj_t *btn_next_label = lv_label_create(btn_next);
    lv_label_set_text(btn_next_label, ">");
    lv_obj_set_style_text_font(btn_next_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(btn_next_label, lv_color_white(), 0);
    lv_obj_center(btn_next_label);

    lv_obj_add_event_cb(btn_next, mode_next_btn, LV_EVENT_CLICKED, NULL);

    printf("DEBUG: Mode control created with LEFT < > RIGHT buttons\n");
}

/* =========================================================
 * BURN-IN PROTECTION
 * ========================================================= */

static void burn_in_task(void *arg)
{
    LV_UNUSED(arg);

    int8_t drift_x = 1;
    int8_t drift_y = 1;

    while(1) {

        vTaskDelay(pdMS_TO_TICKS(120000));

        // Protect LVGL call with BSP mutex
        bsp_display_lock(portMAX_DELAY);
        lv_obj_set_pos(
            root,
            drift_x,
            drift_y);
        bsp_display_unlock();

        drift_x = -drift_x;
        drift_y = -drift_y;
    }
}

/* =========================================================
 * PUBLIC CREATE UI
 * ========================================================= */

void create_ui(void)
{
    create_background();

    create_arc_widget();

    create_temperature_labels();

    create_buttons();

    create_mode_carousel();

    update_ui();

    // Start burn-in protection task
    xTaskCreate(
        burn_in_task,
        "burnin",
        2048,
        NULL,
        1,
        NULL);
} 