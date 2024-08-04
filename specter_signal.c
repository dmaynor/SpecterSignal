#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>
#include <furi_hal.h>
#include <furi_hal_subghz.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <lib/subghz/subghz_tx_rx_worker.h>
#include <dialogs/dialogs.h>
#include <furi/core/log.h>

#define TAG             "SpecterSignal"
#define MAX_FREQUENCIES 5
#define CONFIG_KEY      "specter_signal_config"
#define BASELINE_KEY    "specter_signal_baseline"
#define DWELL_TIME      100
#define FREQUENCY       433920000 // Example frequency in Hz, change as necessary

typedef struct {
    float frequencies[MAX_FREQUENCIES];
    uint32_t num_frequencies;
    float threshold;
    bool visual_alert;
    bool vibration_alert;
} SpecterSignalConfig;

typedef enum {
    ViewMain,
    ViewConfig
} ViewType;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* event_queue;
    FuriTimer* timer;
    ViewType current_view;
    SpecterSignalConfig config;
    float current_signal;
    bool alert_active;
    uint32_t selected_item;
    NotificationApp* notifications;
    uint32_t current_frequency_index;
    SubGhzTxRxWorker* subghz_worker;
    bool settings_changed;
} SpecterSignalState;

typedef enum {
    EventTypeTick,
    EventTypeKey
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} SpecterSignalEvent;

static void specter_signal_draw_main(Canvas* canvas, SpecterSignalState* state) {
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "SpecterSignal");

    char str[64];
    snprintf(str, sizeof(str), "Signal: %.2f dBm", (double)state->current_signal);
    canvas_draw_str(canvas, 2, 30, str);

    snprintf(
        str,
        sizeof(str),
        "Freq: %.2f MHz",
        (double)state->config.frequencies[state->current_frequency_index]);
    canvas_draw_str(canvas, 2, 40, str);

    if(state->alert_active) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 2, 50, "JAMMING DETECTED!");
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 60, "OK:Config  UP:Baseline");
}

static void specter_signal_draw_config(Canvas* canvas, SpecterSignalState* state) {
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Config");

    char str[64];
    for(uint32_t i = 0; i < state->config.num_frequencies; i++) {
        snprintf(
            str,
            sizeof(str),
            "%sFreq %lu: %.2f MHz",
            (state->selected_item == i) ? "> " : "  ",
            i + 1,
            (double)state->config.frequencies[i]);
        canvas_draw_str(canvas, 2, 20 + i * 10, str);
    }

    snprintf(
        str,
        sizeof(str),
        "%sThreshold: %.2f dBm",
        (state->selected_item == state->config.num_frequencies) ? "> " : "  ",
        (double)state->config.threshold);
    canvas_draw_str(canvas, 2, 20 + state->config.num_frequencies * 10, str);

    snprintf(
        str,
        sizeof(str),
        "%sVisual Alert: %s",
        (state->selected_item == state->config.num_frequencies + 1) ? "> " : "  ",
        state->config.visual_alert ? "ON" : "OFF");
    canvas_draw_str(canvas, 2, 30 + state->config.num_frequencies * 10, str);

    snprintf(
        str,
        sizeof(str),
        "%sVibration Alert: %s",
        (state->selected_item == state->config.num_frequencies + 2) ? "> " : "  ",
        state->config.vibration_alert ? "ON" : "OFF");
    canvas_draw_str(canvas, 2, 40 + state->config.num_frequencies * 10, str);

    snprintf(
        str,
        sizeof(str),
        "%sBack",
        (state->selected_item == state->config.num_frequencies + 3) ? "> " : "  ");
    canvas_draw_str(canvas, 2, 50 + state->config.num_frequencies * 10, str);
}

static void save_config(SpecterSignalState* state) {
    FURI_LOG_D(TAG, "Saving configuration");
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_always(file, CONFIG_KEY)) {
        flipper_format_write_header_cstr(file, "SpecterSignal Config", 1);
        flipper_format_write_uint32(file, "NumFrequencies", &state->config.num_frequencies, 1);
        flipper_format_write_float(
            file, "Frequencies", state->config.frequencies, state->config.num_frequencies);
        flipper_format_write_float(file, "Threshold", &state->config.threshold, 1);
        flipper_format_write_bool(file, "VisualAlert", &state->config.visual_alert, 1);
        flipper_format_write_bool(file, "VibrationAlert", &state->config.vibration_alert, 1);
        flipper_format_file_close(file);
    } else {
        FURI_LOG_E(TAG, "Failed to open config file for writing");
    }
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void save_baseline(SpecterSignalState* state) {
    FURI_LOG_D(TAG, "Saving baseline");
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);

    char filename[64];
    snprintf(
        filename,
        sizeof(filename),
        BASELINE_KEY "_%02d%02d%02d_%02d%02d%02d.txt",
        datetime.year,
        datetime.month,
        datetime.day,
        datetime.hour,
        datetime.minute,
        datetime.second);

    if(flipper_format_file_open_always(file, filename)) {
        flipper_format_write_header_cstr(file, "SpecterSignal Baseline", 1);
        flipper_format_write_uint32(file, "NumFrequencies", &state->config.num_frequencies, 1);
        for(uint32_t i = 0; i < state->config.num_frequencies; i++) {
            char key[32];
            snprintf(key, sizeof(key), "Freq_%.2f", (double)state->config.frequencies[i]);
            float signal = furi_hal_subghz_get_rssi();
            flipper_format_write_float(file, key, &signal, 1);
        }
        flipper_format_file_close(file);
    } else {
        FURI_LOG_E(TAG, "Failed to open baseline file for writing");
    }
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
}

static bool show_save_prompt(void) {
    FURI_LOG_D(TAG, "Showing save prompt");
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "Save Changes?", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        message, "Do you want to save the changes?", 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, "Cancel", "Save", NULL);

    bool should_save = false;
    DialogMessageButton result = dialog_message_show(dialogs, message);
    if(result == DialogMessageButtonRight) {
        should_save = true;
    }

    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
    FURI_LOG_D(TAG, "Save prompt result: %d", should_save);
    return should_save;
}

static void specter_signal_process_input(SpecterSignalState* state, InputEvent input) {
    FURI_LOG_D(TAG, "Processing input: type=%d, key=%d", input.type, input.key);
    if(state->current_view == ViewMain) {
        if(input.type == InputTypeShort && input.key == InputKeyOk) {
            state->current_view = ViewConfig;
            state->selected_item = 0;
            state->settings_changed = false;
        } else if(input.type == InputTypeShort && input.key == InputKeyUp) {
            save_baseline(state);
        }
    } else if(state->current_view == ViewConfig) {
        if(input.type == InputTypeShort && input.key == InputKeyUp) {
            if(state->selected_item > 0) state->selected_item--;
        } else if(input.type == InputTypeShort && input.key == InputKeyDown) {
            if(state->selected_item < state->config.num_frequencies + 3) state->selected_item++;
        } else if(input.type == InputTypeShort && input.key == InputKeyRight) {
            state->settings_changed = true;
            if(state->selected_item < state->config.num_frequencies) {
                state->config.frequencies[state->selected_item] += 0.1f;
            } else if(state->selected_item == state->config.num_frequencies) {
                state->config.threshold += 1.0f;
            } else if(state->selected_item == state->config.num_frequencies + 1) {
                state->config.visual_alert = !state->config.visual_alert;
            } else if(state->selected_item == state->config.num_frequencies + 2) {
                state->config.vibration_alert = !state->config.vibration_alert;
            }
        } else if(input.type == InputTypeShort && input.key == InputKeyLeft) {
            state->settings_changed = true;
            if(state->selected_item < state->config.num_frequencies) {
                state->config.frequencies[state->selected_item] -= 0.1f;
            } else if(state->selected_item == state->config.num_frequencies) {
                state->config.threshold -= 1.0f;
            } else if(state->selected_item == state->config.num_frequencies + 1) {
                state->config.visual_alert = !state->config.visual_alert;
            } else if(state->selected_item == state->config.num_frequencies + 2) {
                state->config.vibration_alert = !state->config.vibration_alert;
            }
        } else if(input.type == InputTypeShort && input.key == InputKeyOk) {
            if(state->selected_item == state->config.num_frequencies + 3) {
                if(state->settings_changed) {
                    if(show_save_prompt()) {
                        save_config(state);
                    }
                }
                state->current_view = ViewMain;
            }
        } else if(input.type == InputTypeShort && input.key == InputKeyBack) {
            if(state->settings_changed) {
                if(show_save_prompt()) {
                    save_config(state);
                }
            }
            state->current_view = ViewMain;
        }
    }
}

static void specter_signal_tick(void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    SpecterSignalEvent event = {.type = EventTypeTick};
    furi_message_queue_put(event_queue, &event, 0);
}

static void specter_signal_input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    SpecterSignalEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void specter_signal_draw_callback(Canvas* canvas, void* ctx) {
    SpecterSignalState* state = ctx;
    furi_mutex_acquire(state->mutex, FuriWaitForever);
    if(state->current_view == ViewMain) {
        specter_signal_draw_main(canvas, state);
    } else {
        specter_signal_draw_config(canvas, state);
    }
    furi_mutex_release(state->mutex);
}

int32_t specter_signal_app(void* p) {
    UNUSED(p);
    FURI_LOG_I(TAG, "Application started");

    SpecterSignalState* state = malloc(sizeof(SpecterSignalState));
    if(!state) {
        FURI_LOG_E(TAG, "Failed to allocate memory for state");
        return -1;
    }

    FURI_LOG_D(TAG, "Allocated memory for state");

    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!state->mutex) {
        FURI_LOG_E(TAG, "Failed to allocate mutex");
        free(state);
        return -1;
    }

    FURI_LOG_D(TAG, "Allocated mutex");

    state->event_queue = furi_message_queue_alloc(8, sizeof(SpecterSignalEvent));
    if(!state->event_queue) {
        FURI_LOG_E(TAG, "Failed to allocate event queue");
        furi_mutex_free(state->mutex);
        free(state);
        return -1;
    }

    FURI_LOG_D(TAG, "Allocated event queue");

    state->current_view = ViewMain;
    state->alert_active = false;
    state->selected_item = 0;
    state->current_frequency_index = 0;
    state->settings_changed = false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    if(flipper_format_file_open_existing(file, CONFIG_KEY)) {
        FURI_LOG_D(TAG, "Loading existing configuration");
        flipper_format_read_uint32(file, "NumFrequencies", &state->config.num_frequencies, 1);
        flipper_format_read_float(
            file, "Frequencies", state->config.frequencies, state->config.num_frequencies);
        flipper_format_read_float(file, "Threshold", &state->config.threshold, 1);
        flipper_format_read_bool(file, "VisualAlert", &state->config.visual_alert, 1);
        flipper_format_read_bool(file, "VibrationAlert", &state->config.vibration_alert, 1);
        flipper_format_file_close(file);
    } else {
        FURI_LOG_D(TAG, "Creating default configuration");
        state->config.num_frequencies = 2;
        state->config.frequencies[0] = 433.92f;
        state->config.frequencies[1] = 915.0f;
        state->config.threshold = -60.0f;
        state->config.visual_alert = true;
        state->config.vibration_alert = true;
    }
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);

    FURI_LOG_D(TAG, "Configuration loaded");

    state->notifications = furi_record_open(RECORD_NOTIFICATION);

    FURI_LOG_D(TAG, "Notification record opened");

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, specter_signal_draw_callback, state);
    view_port_input_callback_set(view_port, specter_signal_input_callback, state->event_queue);

    FURI_LOG_D(TAG, "ViewPort allocated and callbacks set");

    state->timer =
        furi_timer_alloc(specter_signal_tick, FuriTimerTypePeriodic, state->event_queue);
    furi_timer_start(state->timer, furi_ms_to_ticks(DWELL_TIME));

    FURI_LOG_D(TAG, "Timer allocated and started");

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    FURI_LOG_D(TAG, "GUI record opened and ViewPort added");

    state->subghz_worker = subghz_tx_rx_worker_alloc();
    if(subghz_tx_rx_worker_start(state->subghz_worker, NULL, FREQUENCY) ==
       true) { // Assuming true is the success value
        FURI_LOG_D(TAG, "SubGhz worker started successfully");
        furi_hal_subghz_start_async_rx(NULL, state);
    } else {
        FURI_LOG_E(TAG, "Failed to start SubGhz worker");
        // Handle error, possibly exit the app
    }

    FURI_LOG_D(TAG, "SubGhz worker allocated and started");

    SpecterSignalEvent event;
    bool running = true;
    while(running) {
        FURI_LOG_D(TAG, "Waiting for event");
        FuriStatus event_status =
            furi_message_queue_get(state->event_queue, &event, FuriWaitForever);

        if(event_status == FuriStatusOk) {
            FURI_LOG_D(TAG, "Event received: type=%d", event.type);
            if(event.type == EventTypeKey) {
                if(event.input.type == InputTypeShort && event.input.key == InputKeyBack &&
                   state->current_view == ViewMain) {
                    running = false;
                } else {
                    specter_signal_process_input(state, event.input);
                }
            } else if(event.type == EventTypeTick) {
                furi_mutex_acquire(state->mutex, FuriWaitForever);
                state->current_signal = furi_hal_subghz_get_rssi();
                state->alert_active = (state->current_signal > state->config.threshold);
                if(state->alert_active) {
                    if(state->config.visual_alert) {
                        notification_message(state->notifications, &sequence_blink_red_100);
                    }
                    if(state->config.vibration_alert) {
                        notification_message(state->notifications, &sequence_single_vibro);
                    }
                }

                if(state->config.num_frequencies > 0) {
                    state->current_frequency_index =
                        (state->current_frequency_index + 1) % state->config.num_frequencies;
                    furi_hal_subghz_idle();
                    furi_hal_subghz_set_frequency(
                        (uint32_t)(state->config.frequencies[state->current_frequency_index] *
                                   1000000));
                    furi_hal_subghz_rx();
                } else {
                    FURI_LOG_E(TAG, "No frequencies configured");
                    // Handle this error condition
                }

                furi_mutex_release(state->mutex);
            }
        }
        view_port_update(view_port);
    }

    FURI_LOG_I(TAG, "Application stopping");

    furi_timer_free(state->timer);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(state->event_queue);
    furi_mutex_free(state->mutex);

    if(state->subghz_worker) {
        subghz_tx_rx_worker_stop(state->subghz_worker);
        subghz_tx_rx_worker_free(state->subghz_worker);
    }

    free(state);

    FURI_LOG_I(TAG, "Application stopped");
    return 0;
}
