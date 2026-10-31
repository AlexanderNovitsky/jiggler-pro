#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>

#define JIGGLE_PERIOD_MS 1000
#define JIGGLE_MIN_PX    30
#define JIGGLE_MAX_PX    50

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* queue;
    FuriTimer* timer;
    FuriHalUsbInterface* usb_prev;
    int8_t return_dx;
    int8_t last_dx;
} JigglerApp;

static void jiggler_draw_callback(Canvas* canvas, void* ctx) {
    JigglerApp* app = ctx;
    char line[32];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 12, "Jiggler Pro");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(
        canvas,
        2,
        28,
        furi_hal_hid_is_connected() ? "USB connected" : "Waiting for USB...");

    if(app->last_dx == 0) {
        canvas_draw_str(canvas, 2, 42, "Move: --");
    } else {
        snprintf(line, sizeof(line), "Move: %+d px", (int)app->last_dx);
        canvas_draw_str(canvas, 2, 42, line);
    }

    canvas_draw_str(canvas, 2, 60, "Back to exit");
}

static void jiggler_input_callback(InputEvent* event, void* ctx) {
    JigglerApp* app = ctx;

    if(event->key == InputKeyBack &&
       (event->type == InputTypeShort || event->type == InputTypeLong)) {
        bool exit = true;
        furi_message_queue_put(app->queue, &exit, 0);
    }
}

static void jiggler_timer_callback(void* ctx) {
    JigglerApp* app = ctx;
    int8_t dx;

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

int32_t jiggler_pro_app(void* p) {
    UNUSED(p);

    JigglerApp* app = malloc(sizeof(JigglerApp));
    app->return_dx = 0;
    app->last_dx = 0;
    app->queue = furi_message_queue_alloc(4, sizeof(bool));
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

    bool exit = false;
    while(!exit) {
        furi_message_queue_get(app->queue, &exit, FuriWaitForever);
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
