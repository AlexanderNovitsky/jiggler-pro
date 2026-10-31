#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>

#define JIGGLE_PERIOD_MS 1000
#define JIGGLE_MIN_PX    30
#define JIGGLE_MAX_PX    50

typedef enum {
    JigglerEventToggle,
    JigglerEventExit,
} JigglerEvent;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriTimer* timer;
    FuriHalUsbInterface* usb_prev;
    bool running;
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

static void jiggler_draw_callback(Canvas* canvas, void* ctx) {
    JigglerApp* app = ctx;
    char line[24];
    const char* status = app->running ? "Running" : "Paused";

    canvas_clear(canvas);

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 13);
    canvas_set_color(canvas, ColorWhite);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 3, 10, "Jiggler Pro");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 108, 10, furi_hal_hid_is_connected() ? "USB" : "NO");
    canvas_set_color(canvas, ColorBlack);

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
    jiggler_draw_hint(canvas, 126, "hold Back", true);
}

static void jiggler_input_callback(InputEvent* event, void* ctx) {
    JigglerApp* app = ctx;
    JigglerEvent message;

    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        message = JigglerEventToggle;
        furi_message_queue_put(app->queue, &message, 0);
    } else if(event->key == InputKeyBack && event->type == InputTypeLong) {
        message = JigglerEventExit;
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
        uint32_t span = JIGGLE_MAX_PX - JIGGLE_MIN_PX + 1;
        dx = (int8_t)(JIGGLE_MIN_PX + (furi_hal_random_get() % span));
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

int32_t jiggler_pro_app(void* p) {
    UNUSED(p);

    JigglerApp* app = malloc(sizeof(JigglerApp));
    app->running = true;
    app->return_dx = 0;
    app->last_dx = 0;
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
            jiggler_set_running(app, !app->running);
            break;
        case JigglerEventExit:
            exit = true;
            break;
        }
    }

    furi_timer_stop(app->timer);
    furi_hal_usb_set_config(app->usb_prev, NULL);

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    furi_timer_free(app->timer);
    view_port_free(app->view_port);
    furi_message_queue_free(app->queue);
    free(app);

    return 0;
}
