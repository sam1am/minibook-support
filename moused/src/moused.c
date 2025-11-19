#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "debug.h"
#include "device.h"
#include "server.h"
#include "vdevice.h"

#define MINIBOOK_INPUT_DEVICE "/dev/input/by-id/usb-0603_0003-event-mouse"
#define MINIBOOKX_INPUT_DEVICE                                                 \
    "/dev/input/by-path/"                                                      \
    "pci-0000:00:15.3-platform-i2c_designware.3-event-mouse"

#define VERSION "moused 1.3.0"

server_t *server_addr = NULL;

int input = -1, output = -1;
// int is_enabled_debug = 0;
int is_enabled_passthrough = 1;
int is_enabled_calibration = 0;

// Position filtering to reduce jitter and lift noise
int last_abs_x = -1;
int last_abs_y = -1;
int prev_delta_x = 0;
int prev_delta_y = 0;
double smoothed_x = -1.0;
double smoothed_y = -1.0;

// Adaptive smoothing thresholds
#define DEADZONE 2              // Ignore movements smaller than this (sub-pixel jitter)
#define SMALL_MOVEMENT 10       // Movements below this get heavy smoothing
#define MEDIUM_MOVEMENT 25      // Movements below this get moderate smoothing
#define LIFT_NOISE_THRESHOLD 15 // Suppress sudden jumps larger than this (likely lift noise)
#define SMOOTHING_HEAVY 0.3     // Heavy smoothing factor (70% old, 30% new)
#define SMOOTHING_MODERATE 0.6  // Moderate smoothing factor (40% old, 60% new)
#define SMOOTHING_LIGHT 0.85    // Light smoothing factor (15% old, 85% new)

// Multi-touch tracking for scrolling
#define MAX_SLOTS 10
typedef struct {
    int tracking_id;  // -1 means slot is empty
    int x;
    int y;
} touch_slot_t;

touch_slot_t touch_slots[MAX_SLOTS] = {{-1, 0, 0}};
int current_slot = 0;
int active_fingers = 0;
int prev_active_fingers = 0;  // Track previous state to detect transitions
int stable_finger_count = 0;  // Debounced finger count
int last_scroll_y = -1;
double scroll_velocity = 0.0;

// Scroll parameters
#define SCROLL_THRESHOLD 3       // Minimum movement to trigger scroll (pixels)
#define SCROLL_SCALE 3.0         // Scale factor for scroll sensitivity (higher = faster)
#define INERTIA_DECAY 0.92       // Velocity decay factor per frame (higher = longer inertia)
#define INERTIA_MIN_VELOCITY 0.5 // Stop when velocity drops below this

// Recovery the device
void recovery_device() {
    // Enable the pointer
    if (input != -1) {
        ioctl(input, EVIOCGRAB, 0);
        close(input);
    }
    if (output != -1) {
        ioctl(output, UI_DEV_DESTROY);
        close(output);
    }
}

// Signal handler
void sigint_handler(int signum) {
    if (server_addr != NULL) {
        stop_server(server_addr);
    }
    recovery_device();
    exit(EXIT_SUCCESS);
}

int new_device() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("Cannot open the output device");
        recovery_device();
        exit(EXIT_FAILURE);
    }

    // Clone the enabled event types and codes of the device
    clone_enabled_event_types_and_codes(input, fd);

    // Enable scroll wheel events for two-finger scrolling
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);

    // Set device properties to match physical trackpad (PROP=5)
    // This tells libinput it's a buttonpad-style touchpad, enabling proper gesture support
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);     // 0x00
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_BUTTONPAD);   // 0x02

    // Setup the device
    struct uinput_setup uisetup = {0};
    memset(&uisetup, 0, sizeof(uisetup));
    strcpy(uisetup.name, "MiniBookSupport Virtual Touchpad");
    uisetup.id.bustype = BUS_USB;
    uisetup.id.vendor = 0x1234;
    uisetup.id.product = 0x5678;
    uisetup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &uisetup) < 0) {
        perror("Failed to setup the virtual device");
        recovery_device();
        exit(EXIT_FAILURE);
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("Failed to create the virtual device");
        recovery_device();
        exit(EXIT_FAILURE);
    }
    return fd;
}

// Print the help message
void print_help() {
    printf("Usage: ./moused [-d] [-c] [-h] [--version]\n");
    printf("Options:\n");
    printf("  -d: Enable debug mode\n");
    printf("  -c: Enable calibration mode\n");
    printf("  -h: Print this help message\n");
    printf("  --version: Print the version\n");
}

// Parse the command line arguments
void parse_args(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            enable_debug();
        } else if (strcmp(argv[i], "-c") == 0) {
            is_enabled_calibration = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", VERSION);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "-h") == 0) {
            print_help();
            exit(EXIT_SUCCESS);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help();
            exit(EXIT_FAILURE);
        }
    }
}

// Manage the press / release of the key
int pressing_keys[KEY_MAX] = {0};
void press_key(int key) {
    debug_printf("Press key: %d\n", key);
    pressing_keys[key] = 1;
}
void release_key(int key) {
    debug_printf("Release key: %d\n", key);
    pressing_keys[key] = 0;
}
void release_unreleased_keys() {
    for (int i = 0; i < KEY_MAX; i++) {
        if (pressing_keys[i]) {
            release_key(i);
            // Release the key
            emit(output, EV_KEY, i, 0);
            emit(output, EV_SYN, SYN_REPORT, 0);
        }
    }
}

// Server callback
uint8_t server_callback(uint8_t type, uint8_t data) {
    debug_printf("Server callback: %d %d\n", type, data);
    switch (type) {
    case 0:
        debug_printf("Set passthrough: %d\n", data);
        is_enabled_passthrough = data;
        if (!is_enabled_passthrough) {
            release_unreleased_keys();
        }
        return 0;
    case 1:
        return (uint8_t)is_enabled_passthrough;
    default:
        break;
    }
    return 0;
}

// Main
int main(int argc, char *argv[]) {
    // Parse the command line arguments
    parse_args(argc, argv);

    // Check the device model
    char device_model[256] = {0};
    get_device_model(device_model, sizeof(device_model));
    debug_printf("Device model: %s\n", device_model);
    if (strstr(device_model, "MiniBook") == NULL && strstr(device_model, "FreeBook") == NULL) {
        fprintf(stderr, "This device is not supported\n");
        recovery_device();
        return (EXIT_FAILURE);
    }
    if (strncmp(device_model, "MiniBook X", 10) == 0 || strncmp(device_model, "FreeBook", 8) == 0) {
        input = open(MINIBOOKX_INPUT_DEVICE, O_RDWR);
        is_enabled_calibration = 0;
    } else {
        input = open(MINIBOOK_INPUT_DEVICE, O_RDWR);
    }
    if (input == -1) {
        perror("Cannot open the input device");
        return (EXIT_FAILURE);
    }
    output = new_device();
    if (output == -1) {
        perror("Cannot create the output device");
        recovery_device();
        return (EXIT_FAILURE);
    }

    // Wait until the all keys are released
    debug_printf("Wait until all keys are released\n");
    // Set the input device to non-blocking mode
    fcntl(input, F_SETFL, O_NONBLOCK);
    int pressing_count = 0;
    int count = 0;
    while (pressing_count != 0 || count < 10) {
        struct input_event event;
        ssize_t result = read(input, &event, sizeof(event));
        if (result == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                count++;
                usleep(100000); // 100ms
                continue;
            }
            perror("read");
            recovery_device();
            exit(EXIT_FAILURE);
        } else if (result == sizeof(event)) {
            if (event.type == EV_KEY) {
                if (event.value > 0) {
                    if (pressing_keys[event.code] == 0) {
                        press_key(event.code);
                        pressing_count++;
                    }
                } else if (event.value == 0) {
                    if (pressing_keys[event.code] == 1) {
                        release_key(event.code);
                        pressing_count--;
                    }
                }
            }
        }
        count++;
        usleep(100000); // 100ms
    }
    debug_printf("All keys are released\n");
    // Set the input device to blocking mode
    fcntl(input, F_SETFL, 0);

    // Disable the input device
    ioctl(input, EVIOCGRAB, 1);
    // Register the signal handler
    signal(SIGINT, sigint_handler);

    // Create the server
    server_t server;
    server_addr = &server;
    // Setup the server
    setup_server(&server, "/var/run/moused.sock", server_callback);

    // Start the server
    if (start_server(&server) == 1) {
        perror("Cannot start the server");
        recovery_device();
        return (EXIT_FAILURE);
    }

    // Main loop
    while (1) {
        struct input_event event;

        ssize_t result = read(input, &event, sizeof(event));
        if (result == -1) {
            perror("Stopping");
            recovery_device();
            exit(EXIT_FAILURE);
        } else if (result != sizeof(event)) {
            perror("Invalid event size");
            recovery_device();
            exit(EXIT_FAILURE);
        }
        switch (event.type) {
        case EV_SYN:
            debug_printf("EV_SYN: %d %d\n", event.code, event.value);

            // Stabilize finger count at frame boundaries
            if (event.code == SYN_REPORT) {
                // If finger count changed, reset scroll position to avoid jumps
                if (active_fingers != prev_active_fingers) {
                    last_scroll_y = -1;
                    debug_printf("Finger count changed: %d -> %d\n", prev_active_fingers, active_fingers);
                    prev_active_fingers = active_fingers;
                }
                stable_finger_count = active_fingers;
            }

            if (is_enabled_passthrough) {
                emit(output, event.type, event.code, event.value);
            }
            break;
        case EV_MSC:
            debug_printf("EV_MSC: %d %d\n", event.code, event.value);
            if (is_enabled_passthrough) {
                emit(output, event.type, event.code, event.value);
            }
            break;
        case EV_KEY:
            debug_printf("EV_KEY: %d %d\n", event.code, event.value);
            // passthrough
            if (is_enabled_passthrough) {
                if (event.value > 0) {
                    press_key(event.code);
                } else if (event.value == 0) {
                    release_key(event.code);
                    // Reset position tracking when finger lifts
                    if (event.code == BTN_TOUCH) {
                        last_abs_x = -1;
                        last_abs_y = -1;
                        prev_delta_x = 0;
                        prev_delta_y = 0;
                        smoothed_x = -1.0;
                        smoothed_y = -1.0;
                        debug_printf("Reset position tracking (finger lifted)\n");
                    }
                }
                emit(output, event.type, event.code, event.value);
            }
            break;
        case EV_REL:
            // Calibrate the pointer
            if (event.code == REL_X) {
                int rel_x = event.value;
                debug_printf("REL_X: %d\n", event.value);
                if (is_enabled_calibration) {
                    if (event.value == -1) {
                        rel_x = 0;
                    } else {
                        rel_x = event.value + 1;
                    }
                    debug_printf("REL_X (calibrated): %d\n", rel_x);
                } else {
                    rel_x = event.value;
                }
                if (is_enabled_passthrough) {
                    emit(output, event.type, event.code, rel_x);
                }
            } else if (event.code == REL_Y) {
                int rel_y = event.value;
                debug_printf("REL_Y: %d\n", event.value);

                if (is_enabled_calibration) {
                    if (event.value == -1) {
                        rel_y = 0;
                    } else {
                        rel_y = event.value + 1;
                    }
                    debug_printf("REL_Y (calibrated): %d\n", rel_y);
                } else {
                    rel_y = event.value;
                }
                if (is_enabled_passthrough) {
                    emit(output, event.type, event.code, rel_y);
                }
            }
            break;
        case EV_ABS:
            debug_printf("EV_ABS: %d %d\n", event.code, event.value);
            if (is_enabled_passthrough) {
                // Track multi-touch for two-finger scrolling
                if (event.code == ABS_MT_SLOT) {
                    current_slot = event.value;
                    if (current_slot >= MAX_SLOTS) current_slot = MAX_SLOTS - 1;
                    debug_printf("MT_SLOT: %d\n", current_slot);
                } else if (event.code == ABS_MT_TRACKING_ID) {
                    if (event.value == -1) {
                        // Finger lifted
                        if (touch_slots[current_slot].tracking_id != -1) {
                            active_fingers--;
                            debug_printf("Finger lifted. Active fingers: %d\n", active_fingers);
                        }
                        touch_slots[current_slot].tracking_id = -1;
                        if (active_fingers == 0) {
                            last_scroll_y = -1;  // Reset scroll tracking
                        }
                    } else {
                        // New finger down
                        if (touch_slots[current_slot].tracking_id == -1) {
                            active_fingers++;
                            debug_printf("Finger down. Active fingers: %d\n", active_fingers);
                        }
                        touch_slots[current_slot].tracking_id = event.value;
                    }
                } else if (event.code == ABS_MT_POSITION_X) {
                    touch_slots[current_slot].x = event.value;
                } else if (event.code == ABS_MT_POSITION_Y) {
                    touch_slots[current_slot].y = event.value;

                    // If we have 2 fingers (use stable count to avoid mid-frame transitions), calculate scroll
                    if (stable_finger_count == 2) {
                        // Calculate average Y position of all active fingers
                        int sum_y = 0, count = 0;
                        for (int i = 0; i < MAX_SLOTS; i++) {
                            if (touch_slots[i].tracking_id != -1) {
                                sum_y += touch_slots[i].y;
                                count++;
                            }
                        }
                        int avg_y = sum_y / count;

                        if (last_scroll_y != -1) {
                            int delta_y = avg_y - last_scroll_y;
                            int abs_delta = delta_y < 0 ? -delta_y : delta_y;
                            if (abs_delta > SCROLL_THRESHOLD) {
                                // Generate scroll event (negative for natural scrolling)
                                // Scale and round to get smooth scrolling
                                double scroll_amount_float = -delta_y * SCROLL_SCALE / 10.0;
                                int scroll_amount = scroll_amount_float >= 0
                                    ? (int)(scroll_amount_float + 0.5)
                                    : (int)(scroll_amount_float - 0.5);

                                if (scroll_amount != 0) {
                                    emit(output, EV_REL, REL_WHEEL, scroll_amount);
                                    emit(output, EV_SYN, SYN_REPORT, 0);
                                    debug_printf("Scroll: %d (delta_y=%d)\n", scroll_amount, delta_y);

                                    // Track velocity for inertia
                                    scroll_velocity = scroll_amount_float;
                                }
                            }
                        }
                        last_scroll_y = avg_y;

                        // DON'T emit position events when scrolling - this prevents pinch zoom gestures
                        break;
                    }
                }

                // When scrolling (2+ fingers), suppress ALL multi-touch events to prevent gesture confusion
                if (stable_finger_count >= 2) {
                    // Only allow non-MT events through
                    if (event.code != ABS_MT_SLOT &&
                        event.code != ABS_MT_TRACKING_ID &&
                        event.code != ABS_MT_POSITION_X &&
                        event.code != ABS_MT_POSITION_Y &&
                        event.code != ABS_MT_TOUCH_MAJOR &&
                        event.code != ABS_MT_TOUCH_MINOR &&
                        event.code != ABS_MT_PRESSURE) {
                        emit(output, event.type, event.code, event.value);
                    }
                    break;
                }

                // Emit MT tracking events for single finger
                if (event.code == ABS_MT_SLOT || event.code == ABS_MT_TRACKING_ID) {
                    emit(output, event.type, event.code, event.value);
                }

                // Apply adaptive exponential smoothing for cursor position (single finger only)
                if ((event.code == ABS_X || event.code == ABS_Y ||
                    event.code == ABS_MT_POSITION_X || event.code == ABS_MT_POSITION_Y) &&
                    stable_finger_count == 1) {
                    // Use appropriate smoothing variables based on event code
                    int is_x = (event.code == ABS_X || event.code == ABS_MT_POSITION_X);
                    double *smoothed = is_x ? &smoothed_x : &smoothed_y;
                    int *last_pos = is_x ? &last_abs_x : &last_abs_y;

                    // Initialize smoothed position on first touch
                    if (*smoothed < 0.0) {
                        *smoothed = (double)event.value;
                        *last_pos = event.value;
                        emit(output, event.type, event.code, event.value);
                        break;
                    }

                    // Calculate delta from raw position
                    int delta = event.value - *last_pos;
                    int abs_delta = (delta < 0) ? -delta : delta;

                    // Get previous delta for jump detection
                    int *prev_delta = is_x ? &prev_delta_x : &prev_delta_y;
                    int prev_abs_delta = (*prev_delta < 0) ? -*prev_delta : *prev_delta;

                    // Deadzone: ignore sub-pixel jitter when nearly still
                    if (abs_delta < DEADZONE) {
                        debug_printf("Deadzone: ignoring delta=%d\n", delta);
                        break;  // Don't emit, don't update
                    }

                    // Lift noise detection: suppress sudden large jumps after small movements
                    if (abs_delta > LIFT_NOISE_THRESHOLD && prev_abs_delta < SMALL_MOVEMENT) {
                        debug_printf("Lift noise: suppressing jump delta=%d (prev=%d)\n", delta, *prev_delta);
                        break;  // Don't emit, don't update
                    }

                    // Adaptive smoothing based on movement size
                    double alpha;
                    if (abs_delta < SMALL_MOVEMENT) {
                        alpha = SMOOTHING_HEAVY;      // Heavy smoothing for small jittery movements
                        debug_printf("Heavy smoothing: delta=%d\n", delta);
                    } else if (abs_delta < MEDIUM_MOVEMENT) {
                        alpha = SMOOTHING_MODERATE;   // Moderate smoothing for medium movements
                        debug_printf("Moderate smoothing: delta=%d\n", delta);
                    } else {
                        alpha = SMOOTHING_LIGHT;      // Light smoothing for large intentional movements
                        debug_printf("Light smoothing: delta=%d\n", delta);
                    }

                    // Apply exponential moving average: smoothed = alpha * new + (1-alpha) * old
                    *smoothed = alpha * (double)event.value + (1.0 - alpha) * (*smoothed);

                    // Emit the smoothed position
                    int smoothed_value = (int)(*smoothed + 0.5);  // Round to nearest integer
                    *last_pos = event.value;  // Track raw position for delta calculation
                    *prev_delta = delta;      // Track for jump detection

                    emit(output, event.type, event.code, smoothed_value);
                } else {
                    emit(output, event.type, event.code, event.value);
                }
            }
            break;
        default:
            debug_printf("Other: %d %d\n", event.code, event.value);
            if (is_enabled_passthrough) {
                emit(output, event.type, event.code, event.value);
            }
            break;
        }
    }
    // Never reach here...
    ioctl(output, UI_DEV_DESTROY);
    // Enable the pointer
    ioctl(input, EVIOCGRAB, 0);
    close(input);
    close(output);
    return (EXIT_SUCCESS);
}
