#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <storage/storage.h>

#define JIGGLE_SETTINGS_PATH APP_DATA_PATH("settings.txt")

#define JIGGLE_PERIOD_MS     1000
#define JIGGLE_RANGE_MIN     1
#define JIGGLE_RANGE_MAX     120
#define JIGGLE_RANGE_DEFAULT 40

typedef enum {
    JigglerEventToggle,
    JigglerEventExit,
    JigglerEventBack,
    JigglerEventRangeDec,
    JigglerEventRangeInc,
} JigglerEvent;

typedef enum {
    JigglerScreenMain,
    JigglerScreenSettings,
} JigglerScreen;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriTimer* timer;
    FuriHalUsbInterface* usb_prev;
    JigglerScreen screen;
    bool running;
    bool resume_running;
    uint8_t range_px;
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

static void jiggler_draw_arrow_key(Canvas* canvas, int32_t x, bool enabled, bool points_left) {
    if(enabled) {
        canvas_draw_rbox(canvas, x, 27, 18, 18, 2);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, x, 27, 18, 18, 2);
    }

    jiggler_draw_chevron(canvas, x + 6, 33, points_left);
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
    if(app->last_dx == 0) {
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, "Last move  --");
    } else {
        snprintf(line, sizeof(line), "Last move  %+d px", (int)app->last_dx);
        canvas_draw_str_aligned(canvas, 64, 48, AlignCenter, AlignBottom, line);
    }

    jiggler_draw_hint(canvas, 2, app->running ? "OK Pause" : "OK Start", false);
    jiggler_draw_hint(canvas, 126, "Back", true);
}

static void jiggler_draw_settings(Canvas* canvas, JigglerApp* app) {
    char value[8];

    jiggler_draw_header(canvas, "Settings");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignBottom, "Range");

    snprintf(value, sizeof(value), "%u", app->range_px);
    canvas_set_font(canvas, FontBigNumbers);
    uint16_t value_w = canvas_string_width(canvas, value);
    canvas_draw_str_aligned(canvas, 58, 46, AlignCenter, AlignBottom, value);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 58 + (value_w / 2) + 4, 44, "px");

    jiggler_draw_arrow_key(canvas, 6, app->range_px > JIGGLE_RANGE_MIN, true);
    jiggler_draw_arrow_key(canvas, 104, app->range_px < JIGGLE_RANGE_MAX, false);
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
    } else if((event->key == InputKeyLeft || event->key == InputKeyRight) &&
              (event->type == InputTypeShort || event->type == InputTypeRepeat) &&
              app->screen == JigglerScreenSettings) {
        message = (event->key == InputKeyLeft) ? JigglerEventRangeDec : JigglerEventRangeInc;
        send = true;
    }

    if(send) {
        furi_message_queue_put(app->queue, &message, 0);
    }
}

static void jiggler_timer_callback(void* ctx) {
    JigglerApp* app = ctx;
    int8_t dx;

    if(!app->running) {
        return;
    }

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

static void jiggler_set_running(JigglerApp* app, bool running) {
    app->running = running;
    if(running) {
        furi_timer_start(app->timer, furi_ms_to_ticks(JIGGLE_PERIOD_MS));
    } else {
        furi_timer_stop(app->timer);
    }
    view_port_update(app->view_port);
}

static void jiggler_settings_save(const JigglerApp* app);

static void jiggler_open_settings(JigglerApp* app) {
    app->resume_running = app->running;
    app->screen = JigglerScreenSettings;
    jiggler_set_running(app, false);
}

static void jiggler_close_settings(JigglerApp* app) {
    app->screen = JigglerScreenMain;
    jiggler_set_running(app, app->resume_running);
    jiggler_settings_save(app);
}

static bool jiggler_parse_range(const char* text, uint8_t* out) {
    if(text[0] < '0' || text[0] > '9') {
        return false;
    }

    unsigned value = 0;
    for(const char* cursor = text; *cursor >= '0' && *cursor <= '9'; cursor++) {
        value = value * 10u + (unsigned)(*cursor - '0');
        if(value > JIGGLE_RANGE_MAX) {
            return false;
        }
    }

    if(value < JIGGLE_RANGE_MIN) {
        return false;
    }

    *out = (uint8_t)value;
    return true;
}

static void jiggler_settings_load(JigglerApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(storage_file_open(file, JIGGLE_SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[8] = {0};
        size_t read = storage_file_read(file, buf, sizeof(buf) - 1);
        uint8_t range = 0;
        if(read > 0 && jiggler_parse_range(buf, &range)) {
            app->range_px = range;
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
        char buf[8];
        int length = snprintf(buf, sizeof(buf), "%u\n", app->range_px);
        if(length > 0) {
            storage_file_write(file, buf, (size_t)length);
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void jiggler_change_range(JigglerApp* app, int8_t delta) {
    int16_t next = (int16_t)app->range_px + delta;
    if(next < JIGGLE_RANGE_MIN) {
        next = JIGGLE_RANGE_MIN;
    } else if(next > JIGGLE_RANGE_MAX) {
        next = JIGGLE_RANGE_MAX;
    }

    app->range_px = (uint8_t)next;
    view_port_update(app->view_port);
}

int32_t jiggler_pro_app(void* p) {
    UNUSED(p);

    JigglerApp* app = malloc(sizeof(JigglerApp));
    app->screen = JigglerScreenMain;
    app->running = true;
    app->resume_running = false;
    app->range_px = JIGGLE_RANGE_DEFAULT;
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
                jiggler_set_running(app, !app->running);
            }
            break;
        case JigglerEventBack:
            if(app->screen == JigglerScreenSettings) {
                jiggler_close_settings(app);
            } else {
                jiggler_open_settings(app);
            }
            break;
        case JigglerEventRangeDec:
            if(app->screen == JigglerScreenSettings) {
                jiggler_change_range(app, -1);
            }
            break;
        case JigglerEventRangeInc:
            if(app->screen == JigglerScreenSettings) {
                jiggler_change_range(app, 1);
            }
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
