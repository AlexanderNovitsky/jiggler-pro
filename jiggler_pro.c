#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <storage/storage.h>

#define JIGGLE_SETTINGS_PATH APP_DATA_PATH("settings.txt")

#define JIGGLE_PERIOD_MS     1000
#define JIGGLE_RANGE_MIN     1
#define JIGGLE_RANGE_MAX     120
#define JIGGLE_RANGE_DEFAULT 40
#define JIGGLE_PAUSE_MIN     1
#define JIGGLE_PAUSE_MAX     180
#define JIGGLE_PAUSE_DEFAULT 10

typedef enum {
    JigglerEventToggle,
    JigglerEventExit,
    JigglerEventBack,
    JigglerEventValueDec,
    JigglerEventValueInc,
    JigglerEventSelectPrev,
    JigglerEventSelectNext,
    JigglerEventPauseDone,
} JigglerEvent;

typedef enum {
    JigglerScreenMain,
    JigglerScreenSettings,
} JigglerScreen;

typedef enum {
    JigglerSettingRange,
    JigglerSettingPause,
    JigglerSettingCount,
} JigglerSetting;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriTimer* timer;
    FuriHalUsbInterface* usb_prev;
    JigglerScreen screen;
    JigglerSetting setting;
    bool running;
    bool resume_running;
    uint8_t range_px;
    uint8_t pause_sec;
    uint8_t pause_left;
    int8_t return_dx;
    int8_t last_dx;
} JigglerApp;

static void jiggler_draw_chevron(Canvas* canvas, int32_t x, int32_t y, bool points_left) {
    if(points_left) {
        canvas_draw_line(canvas, x + 5, y, x, y + 3);
        canvas_draw_line(canvas, x, y + 3, x + 5, y + 6);
    } else {
        canvas_draw_line(canvas, x, y, x + 5, y + 3);
        canvas_draw_line(canvas, x + 5, y + 3, x, y + 6);
    }
}

static void jiggler_draw_hint(Canvas* canvas, int32_t x, const char* text, bool align_right) {
    canvas_set_font(canvas, FontSecondary);
    uint16_t text_w = canvas_string_width(canvas, text);
    uint16_t box_w = text_w + 6;
    int32_t box_x = align_right ? x - box_w : x;

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rbox(canvas, box_x, 52, box_w, 11, 1);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_str(canvas, box_x + 3, 61, text);
    canvas_set_color(canvas, ColorBlack);
}

static void jiggler_draw_header(Canvas* canvas, const char* title) {
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 10, title);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 108, 10, furi_hal_hid_is_connected() ? "USB" : "NO");
    canvas_set_color(canvas, ColorBlack);
}

static void jiggler_draw_main(Canvas* canvas, JigglerApp* app) {
    char line[24];
    const char* status = app->running ? "Running" : "Paused";

    jiggler_draw_header(canvas, "Jiggler Pro");

    canvas_set_font(canvas, FontPrimary);
    uint16_t status_w = canvas_string_width(canvas, status);
    int32_t pill_w = status_w + 16;
    int32_t pill_x = (128 - pill_w) / 2;
    if(app->running) {
        canvas_draw_rbox(canvas, pill_x, 18, pill_w, 16, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignBottom, status);
        canvas_set_color(canvas, ColorBlack);
        jiggler_draw_chevron(canvas, 4, 23, true);
        jiggler_draw_chevron(canvas, 11, 23, true);
        jiggler_draw_chevron(canvas, 112, 23, false);
        jiggler_draw_chevron(canvas, 119, 23, false);
    } else {
        canvas_draw_rframe(canvas, pill_x, 18, pill_w, 16, 2);
        canvas_draw_str_aligned(canvas, 64, 30, AlignCenter, AlignBottom, status);
    }

    canvas_set_font(canvas, FontSecondary);
    if(!app->running && app->pause_left > 0) {
        snprintf(line, sizeof(line), "%u s left", app->pause_left);
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, line);
    } else if(app->last_dx == 0) {
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, "Last move  --");
    } else {
        snprintf(line, sizeof(line), "Last move  %+d px", (int)app->last_dx);
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, line);
    }

    jiggler_draw_hint(canvas, 2, app->running ? "OK Pause" : "OK Start", false);
    jiggler_draw_hint(canvas, 126, "Back", true);
}

static void jiggler_draw_setting_row(
    Canvas* canvas,
    int32_t y,
    const char* label,
    const char* value,
    bool selected) {
    if(selected) {
        canvas_draw_rbox(canvas, 2, y, 124, 16, 2);
        canvas_set_color(canvas, ColorWhite);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 8, y + 11, label);
    canvas_draw_str_aligned(canvas, 120, y + 11, AlignRight, AlignBottom, value);
    canvas_set_color(canvas, ColorBlack);
}

static void jiggler_draw_settings(Canvas* canvas, JigglerApp* app) {
    char range_value[16];
    char pause_value[16];
    bool range_selected = app->setting == JigglerSettingRange;

    if(range_selected) {
        snprintf(range_value, sizeof(range_value), "< %u px >", app->range_px);
        snprintf(pause_value, sizeof(pause_value), "%u s", app->pause_sec);
    } else {
        snprintf(range_value, sizeof(range_value), "%u px", app->range_px);
        snprintf(pause_value, sizeof(pause_value), "< %u s >", app->pause_sec);
    }

    jiggler_draw_header(canvas, "Settings");
    jiggler_draw_setting_row(canvas, 16, "Range", range_value, range_selected);
    jiggler_draw_setting_row(canvas, 34, "Pause", pause_value, !range_selected);
    jiggler_draw_hint(canvas, 126, "Back", true);
}

static void jiggler_draw_callback(Canvas* canvas, void* ctx) {
    JigglerApp* app = ctx;

    canvas_clear(canvas);
    if(app->screen == JigglerScreenSettings) {
        jiggler_draw_settings(canvas, app);
    } else {
        jiggler_draw_main(canvas, app);
    }
}

static void jiggler_input_callback(InputEvent* event, void* ctx) {
    JigglerApp* app = ctx;
    JigglerEvent message;
    bool send = false;
    bool repeatable = event->type == InputTypeShort || event->type == InputTypeRepeat;

    if(event->key == InputKeyBack && event->type == InputTypeShort) {
        message = JigglerEventBack;
        send = true;
    } else if(event->key == InputKeyBack && event->type == InputTypeLong) {
        message = JigglerEventExit;
        send = true;
    } else if(event->key == InputKeyOk && event->type == InputTypeShort &&
              app->screen == JigglerScreenMain) {
        message = JigglerEventToggle;
        send = true;
    } else if(app->screen == JigglerScreenSettings && repeatable) {
        if(event->key == InputKeyLeft) {
            message = JigglerEventValueDec;
            send = true;
        } else if(event->key == InputKeyRight) {
            message = JigglerEventValueInc;
            send = true;
        } else if(event->key == InputKeyUp) {
            message = JigglerEventSelectPrev;
            send = true;
        } else if(event->key == InputKeyDown) {
            message = JigglerEventSelectNext;
            send = true;
        }
    }

    if(send) {
        furi_message_queue_put(app->queue, &message, 0);
    }
}

static void jiggler_step(JigglerApp* app) {
    int8_t dx;

    if(app->return_dx != 0) {
        dx = app->return_dx;
        app->return_dx = 0;
    } else {
        dx = (int8_t)app->range_px;
        app->return_dx = (int8_t)-dx;
    }

    furi_hal_hid_mouse_move(dx, 0);
    app->last_dx = dx;
    view_port_update(app->view_port);
}

static void jiggler_timer_callback(void* ctx) {
    JigglerApp* app = ctx;

    if(app->screen != JigglerScreenMain) {
        return;
    }

    if(app->running) {
        jiggler_step(app);
        return;
    }

    if(app->pause_left == 0) {
        return;
    }

    app->pause_left--;
    if(app->pause_left == 0) {
        JigglerEvent message = JigglerEventPauseDone;
        furi_message_queue_put(app->queue, &message, 0);
    }
    view_port_update(app->view_port);
}

static void jiggler_sync_timer(JigglerApp* app) {
    bool need_timer = app->screen == JigglerScreenMain && (app->running || app->pause_left > 0);

    // furi_timer_stop only queues the stop, so is_running can still be true
    // when it returns. Always start when the timer is needed.
    if(need_timer) {
        furi_timer_start(app->timer, furi_ms_to_ticks(JIGGLE_PERIOD_MS));
    } else {
        furi_timer_stop(app->timer);
    }
}

static void jiggler_play_resume_tune(void) {
    const float notes[] = {523.25f, 659.25f, 783.99f, 1046.50f, 783.99f, 246.94f};
    const uint8_t hold_ms[] = {70, 70, 70, 110, 50, 180};

    if(!furi_hal_speaker_acquire(50)) {
        return;
    }

    for(size_t i = 0; i < COUNT_OF(notes); i++) {
        furi_hal_speaker_start(notes[i], 0.6f);
        furi_delay_ms(hold_ms[i]);
        furi_hal_speaker_stop();
        furi_delay_ms(25);
    }

    furi_hal_speaker_release();
}

static void jiggler_settings_save(const JigglerApp* app);

static void jiggler_open_settings(JigglerApp* app) {
    app->resume_running = app->running;
    app->screen = JigglerScreenSettings;
    app->running = false;
    jiggler_sync_timer(app);
    view_port_update(app->view_port);
}

static void jiggler_close_settings(JigglerApp* app) {
    app->screen = JigglerScreenMain;
    app->running = app->resume_running;
    if(app->running) {
        app->pause_left = 0;
    }
    jiggler_sync_timer(app);
    view_port_update(app->view_port);
    jiggler_settings_save(app);
}

static const char* jiggler_parse_uint(
    const char* text,
    unsigned min_value,
    unsigned max_value,
    uint8_t* out) {
    while(*text == ' ') {
        text++;
    }
    if(*text < '0' || *text > '9') {
        return NULL;
    }

    unsigned value = 0;
    while(*text >= '0' && *text <= '9') {
        value = value * 10u + (unsigned)(*text - '0');
        if(value > max_value) {
            return NULL;
        }
        text++;
    }

    if(value < min_value) {
        return NULL;
    }

    *out = (uint8_t)value;
    return text;
}

static void jiggler_settings_load(JigglerApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, JIGGLE_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[16] = {0};
        size_t read = storage_file_read(file, buf, sizeof(buf) - 1);
        if(read > 0) {
            uint8_t range = 0;
            const char* cursor =
                jiggler_parse_uint(buf, JIGGLE_RANGE_MIN, JIGGLE_RANGE_MAX, &range);
            if(cursor != NULL) {
                app->range_px = range;
                uint8_t pause_sec = 0;
                cursor = jiggler_parse_uint(
                    cursor, JIGGLE_PAUSE_MIN, JIGGLE_PAUSE_MAX, &pause_sec);
                if(cursor != NULL) {
                    app->pause_sec = pause_sec;
                }
            }
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void jiggler_settings_save(const JigglerApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, JIGGLE_SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[16];
        int length = snprintf(buf, sizeof(buf), "%u %u\n", app->range_px, app->pause_sec);
        if(length > 0) {
            storage_file_write(file, buf, (size_t)length);
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void jiggler_change_selected(JigglerApp* app, int8_t delta) {
    int16_t next;
    uint8_t min_value;
    uint8_t max_value;
    uint8_t* field;

    if(app->setting == JigglerSettingRange) {
        field = &app->range_px;
        min_value = JIGGLE_RANGE_MIN;
        max_value = JIGGLE_RANGE_MAX;
    } else {
        field = &app->pause_sec;
        min_value = JIGGLE_PAUSE_MIN;
        max_value = JIGGLE_PAUSE_MAX;
    }

    next = (int16_t)(*field) + delta;
    if(next < min_value) {
        next = min_value;
    } else if(next > max_value) {
        next = max_value;
    }

    *field = (uint8_t)next;
    view_port_update(app->view_port);
}

static void jiggler_toggle_pause(JigglerApp* app) {
    if(app->running) {
        app->running = false;
        app->pause_left = app->pause_sec;
    } else {
        app->running = true;
        app->pause_left = 0;
    }

    jiggler_sync_timer(app);
    view_port_update(app->view_port);
}

static void jiggler_finish_pause(JigglerApp* app) {
    if(app->screen != JigglerScreenMain || app->running) {
        return;
    }

    jiggler_play_resume_tune();
    app->running = true;
    app->pause_left = 0;
    furi_timer_stop(app->timer);
    furi_timer_start(app->timer, furi_ms_to_ticks(JIGGLE_PERIOD_MS));
    jiggler_step(app);
}

int32_t jiggler_pro_app(void* p) {
    UNUSED(p);

    JigglerApp* app = malloc(sizeof(JigglerApp));
    app->screen = JigglerScreenMain;
    app->setting = JigglerSettingRange;
    app->running = true;
    app->resume_running = false;
    app->range_px = JIGGLE_RANGE_DEFAULT;
    app->pause_sec = JIGGLE_PAUSE_DEFAULT;
    app->pause_left = 0;
    app->return_dx = 0;
    app->last_dx = 0;
    jiggler_settings_load(app);
    app->queue = furi_message_queue_alloc(8, sizeof(JigglerEvent));
    app->view_port = view_port_alloc();
    app->timer = furi_timer_alloc(jiggler_timer_callback, FuriTimerTypePeriodic, app);

    view_port_draw_callback_set(app->view_port, jiggler_draw_callback, app);
    view_port_input_callback_set(app->view_port, jiggler_input_callback, app);

    app->usb_prev = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    furi_check(furi_hal_usb_set_config(&usb_hid, NULL));

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    furi_timer_start(app->timer, furi_ms_to_ticks(JIGGLE_PERIOD_MS));

    JigglerEvent event;
    bool exit = false;
    while(!exit) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        switch(event) {
        case JigglerEventToggle:
            if(app->screen == JigglerScreenMain) {
                jiggler_toggle_pause(app);
            }
            break;
        case JigglerEventBack:
            if(app->screen == JigglerScreenSettings) {
                jiggler_close_settings(app);
            } else {
                jiggler_open_settings(app);
            }
            break;
        case JigglerEventValueDec:
            if(app->screen == JigglerScreenSettings) {
                jiggler_change_selected(app, -1);
            }
            break;
        case JigglerEventValueInc:
            if(app->screen == JigglerScreenSettings) {
                jiggler_change_selected(app, 1);
            }
            break;
        case JigglerEventSelectPrev:
            if(app->setting > JigglerSettingRange) {
                app->setting--;
                view_port_update(app->view_port);
            }
            break;
        case JigglerEventSelectNext:
            if(app->setting + 1 < JigglerSettingCount) {
                app->setting++;
                view_port_update(app->view_port);
            }
            break;
        case JigglerEventPauseDone:
            jiggler_finish_pause(app);
            break;
        case JigglerEventExit:
            exit = true;
            break;
        }
    }

    furi_timer_stop(app->timer);
    jiggler_settings_save(app);
    furi_hal_usb_set_config(app->usb_prev, NULL);

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    furi_timer_free(app->timer);
    view_port_free(app->view_port);
    furi_message_queue_free(app->queue);
    free(app);

    return 0;
}
