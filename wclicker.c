/*
 * WClicker - GTK4 Autoclicker for Wayland (Hyprland)
 * Uses /dev/uinput directly — no ydotool needed
 *
 * Compile:
 *   gcc wclicker.c -o wclicker $(pkg-config --cflags --libs gtk4) -lpthread
 *
 * Usage:
 *   ./wclicker            — launch GUI
 *   ./wclicker toggle     — toggle start/stop
 *   ./wclicker start      — start clicking
 *   ./wclicker pause      — stop clicking
 *
 * Hyprland hotkey (hyprland.conf):
 *   bind = , F6, exec, wclicker toggle
 */

#include <gtk/gtk.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/uinput.h>
#include <errno.h>

#define SOCKET_PATH "/tmp/.wclicker.sock"

/* ── uinput ──────────────────────────────────────────────── */
static int uinput_fd = -1;

static int uinput_init(void) {
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (uinput_fd < 0) {
        fprintf(stderr, "Cannot open /dev/uinput: %s\n", strerror(errno));
        fprintf(stderr, "Make sure you are in the 'input' group\n");
        return -1;
    }
    ioctl(uinput_fd, UI_SET_EVBIT,  EV_KEY);
    ioctl(uinput_fd, UI_SET_EVBIT,  EV_SYN);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(uinput_fd, UI_SET_KEYBIT, BTN_MIDDLE);

    struct uinput_setup usetup = {0};
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor  = 0x1234;
    usetup.id.product = 0x5678;
    strncpy(usetup.name, "WClicker Virtual Mouse", UINPUT_MAX_NAME_SIZE);
    ioctl(uinput_fd, UI_DEV_SETUP, &usetup);
    ioctl(uinput_fd, UI_DEV_CREATE);
    usleep(200000);
    return 0;
}

static void uinput_destroy(void) {
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        uinput_fd = -1;
    }
}

static void emit(int type, int code, int val) {
    struct input_event ev = {0};
    ev.type  = type;
    ev.code  = code;
    ev.value = val;
    write(uinput_fd, &ev, sizeof(ev));
}

static int btn_code(int button) {
    switch (button) {
        case 2:  return BTN_MIDDLE;
        case 3:  return BTN_RIGHT;
        default: return BTN_LEFT;
    }
}

static void do_click(int button, int type, int hold_ms) {
    int code = btn_code(button);
    if (type == 1) {
        emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
        emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
        usleep(50000);
        emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
        emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
    } else if (type == 2) {
        emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
        usleep(hold_ms * 1000);
        emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
    } else {
        emit(EV_KEY, code, 1); emit(EV_SYN, SYN_REPORT, 0);
        emit(EV_KEY, code, 0); emit(EV_SYN, SYN_REPORT, 0);
    }
}

/* ── app state ───────────────────────────────────────────── */
typedef struct {
    bool     running;
    bool     clicking;
    int      interval_ms;
    int      random_range_ms;
    int      button;
    int      click_type;
    int      hold_ms;
    int      repeat_count;
    int      clicks_done;

    GtkWidget *btn_toggle;
    GtkWidget *lbl_status;
    GtkWidget *lbl_cps;
    GtkWidget *spin_interval;
    GtkWidget *spin_random;
    GtkWidget *spin_hold;
    GtkWidget *spin_repeat;
    GtkWidget *drop_button;
    GtkWidget *drop_type;

    pthread_t       click_thread;
    pthread_t       socket_thread;
    pthread_mutex_t mutex;
    struct timespec start_time;
} AppState;

static AppState app = {0};

/* ── click thread ────────────────────────────────────────── */
static void *click_loop(void *arg) {
    (void)arg;
    while (app.running) {
        pthread_mutex_lock(&app.mutex);
        bool should = app.clicking;
        int  iv     = app.interval_ms;
        int  rng    = app.random_range_ms;
        int  btn    = app.button;
        int  type   = app.click_type;
        int  hold   = app.hold_ms;
        int  repeat = app.repeat_count;
        pthread_mutex_unlock(&app.mutex);

        if (!should) { usleep(10000); continue; }

        do_click(btn, type, hold);

        pthread_mutex_lock(&app.mutex);
        app.clicks_done++;
        if (repeat > 0 && app.clicks_done >= repeat)
            app.clicking = false;
        pthread_mutex_unlock(&app.mutex);

        int sleep_ms = iv;
        if (rng > 0) sleep_ms += (rand() % (2 * rng + 1)) - rng;
        if (sleep_ms < 1) sleep_ms = 1;
        usleep(sleep_ms * 1000);
    }
    return NULL;
}

/* ── socket command handler ──────────────────────────────── */
static void handle_command(const char *cmd) {
    pthread_mutex_lock(&app.mutex);
    if (strcmp(cmd, "toggle") == 0) {
        app.clicking = !app.clicking;
        if (app.clicking) {
            app.clicks_done = 0;
            clock_gettime(CLOCK_MONOTONIC, &app.start_time);
        }
    } else if (strcmp(cmd, "start") == 0 && !app.clicking) {
        app.clicking    = true;
        app.clicks_done = 0;
        clock_gettime(CLOCK_MONOTONIC, &app.start_time);
    } else if (strcmp(cmd, "pause") == 0) {
        app.clicking = false;
    }
    pthread_mutex_unlock(&app.mutex);
}

static void *socket_loop(void *arg) {
    (void)arg;
    unlink(SOCKET_PATH);

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) return NULL;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    bind(server, (struct sockaddr *)&addr, sizeof(addr));
    listen(server, 5);

    while (app.running) {
        int client = accept(server, NULL, NULL);
        if (client < 0) continue;
        char buf[64] = {0};
        read(client, buf, sizeof(buf) - 1);
        buf[strcspn(buf, "\n\r")] = 0;
        handle_command(buf);
        close(client);
    }
    close(server);
    unlink(SOCKET_PATH);
    return NULL;
}

/* ── UI update timer ─────────────────────────────────────── */
static gboolean update_ui(gpointer data) {
    (void)data;
    pthread_mutex_lock(&app.mutex);
    bool clicking    = app.clicking;
    int  clicks_done = app.clicks_done;
    struct timespec st = app.start_time;
    pthread_mutex_unlock(&app.mutex);

    if (clicking) {
        gtk_button_set_label(GTK_BUTTON(app.btn_toggle), "⏹ Stop");
        gtk_label_set_text(GTK_LABEL(app.lbl_status), "🟢 Active");
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - st.tv_sec) +
                         (now.tv_nsec - st.tv_nsec) / 1e9;
        if (elapsed > 0.1) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f clicks/s | total: %d",
                     clicks_done / elapsed, clicks_done);
            gtk_label_set_text(GTK_LABEL(app.lbl_cps), buf);
        }
    } else {
        gtk_button_set_label(GTK_BUTTON(app.btn_toggle), "▶ Start");
        gtk_label_set_text(GTK_LABEL(app.lbl_status), "🔴 Stopped");
    }
    return G_SOURCE_CONTINUE;
}

/* ── button callback ─────────────────────────────────────── */
static void on_toggle(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    pthread_mutex_lock(&app.mutex);
    app.clicking = !app.clicking;
    if (app.clicking) {
        app.clicks_done     = 0;
        app.interval_ms     = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(app.spin_interval));
        app.random_range_ms = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(app.spin_random));
        app.hold_ms         = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(app.spin_hold));
        app.repeat_count    = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(app.spin_repeat));
        app.button          = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(app.drop_button)) + 1;
        app.click_type      = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(app.drop_type));
        clock_gettime(CLOCK_MONOTONIC, &app.start_time);
    }
    pthread_mutex_unlock(&app.mutex);
}

/* ── build UI ────────────────────────────────────────────── */
static void build_ui(GtkApplication *gapp, gpointer d) {
    (void)d;
    GtkWidget *win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(win), "WClicker");
    gtk_window_set_default_size(GTK_WINDOW(win), 360, 400);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span font='16' weight='bold'>🖱 WClicker</span>");
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *hstatus = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(hstatus, GTK_ALIGN_CENTER);
    app.lbl_status = gtk_label_new("🔴 Stopped");
    app.lbl_cps    = gtk_label_new("");
    gtk_box_append(GTK_BOX(hstatus), app.lbl_status);
    gtk_box_append(GTK_BOX(hstatus), app.lbl_cps);
    gtk_box_append(GTK_BOX(box), hstatus);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_box_append(GTK_BOX(box), grid);

    int row = 0;
#define LABEL(txt, r) { \
    GtkWidget *_l = gtk_label_new(txt); \
    gtk_widget_set_halign(_l, GTK_ALIGN_START); \
    gtk_grid_attach(GTK_GRID(grid), _l, 0, r, 1, 1); }
#define SPIN(w, mn, mx, val, r) \
    w = gtk_spin_button_new_with_range(mn, mx, 1); \
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), val); \
    gtk_widget_set_hexpand(w, TRUE); \
    gtk_grid_attach(GTK_GRID(grid), w, 1, r, 1, 1);

    LABEL("Interval (ms):", row); SPIN(app.spin_interval, 1,    60000, 100, row); row++;
    LABEL("Random ± (ms):", row); SPIN(app.spin_random,   0,    5000,  0,   row); row++;
    LABEL("Hold (ms):",     row); SPIN(app.spin_hold,     1,    5000,  100, row); row++;
    LABEL("Repeat (0=∞):",  row); SPIN(app.spin_repeat,   0, 999999,   0,   row); row++;

    LABEL("Button:", row);
    const char *btns[] = {"Left", "Middle", "Right", NULL};
    app.drop_button = gtk_drop_down_new_from_strings(btns);
    gtk_widget_set_hexpand(app.drop_button, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app.drop_button, 1, row, 1, 1); row++;

    LABEL("Click type:", row);
    const char *types[] = {"Single", "Double", "Hold", NULL};
    app.drop_type = gtk_drop_down_new_from_strings(types);
    gtk_widget_set_hexpand(app.drop_type, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app.drop_type, 1, row, 1, 1);

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    app.btn_toggle = gtk_button_new_with_label("▶ Start");
    gtk_widget_add_css_class(app.btn_toggle, "suggested-action");
    gtk_widget_set_size_request(app.btn_toggle, -1, 48);
    g_signal_connect(app.btn_toggle, "clicked", G_CALLBACK(on_toggle), NULL);
    gtk_box_append(GTK_BOX(box), app.btn_toggle);

    GtkWidget *hint = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hint),
        "<span size='small' color='gray'>"
        "Hotkey (hyprland.conf):\n"
        "bind = , F6, exec, wclicker toggle"
        "</span>");
    gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(box), hint);

    g_timeout_add(150, update_ui, NULL);
    gtk_window_present(GTK_WINDOW(win));
}

/* ── send command to running instance ───────────────────── */
static int send_command(const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "wclicker is not running\n");
        close(fd);
        return 1;
    }
    write(fd, cmd, strlen(cmd));
    close(fd);
    printf("Sent: %s\n", cmd);
    return 0;
}

/* ── main ────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    if (argc >= 2) {
        const char *cmd = argv[1];
        if (!strcmp(cmd, "toggle") || !strcmp(cmd, "start") || !strcmp(cmd, "pause"))
            return send_command(cmd);
        fprintf(stderr, "Usage: wclicker [toggle|start|pause]\n");
        return 1;
    }

    srand(time(NULL));
    pthread_mutex_init(&app.mutex, NULL);
    app.running     = true;
    app.interval_ms = 100;
    app.button      = 1;

    if (uinput_init() < 0) return 1;

    pthread_create(&app.click_thread,  NULL, click_loop,  NULL);
    pthread_create(&app.socket_thread, NULL, socket_loop, NULL);

    GtkApplication *gapp = gtk_application_new(
        "ua.wclicker.app", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gapp, "activate", G_CALLBACK(build_ui), NULL);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);

    app.running  = false;
    app.clicking = false;
    pthread_cancel(app.click_thread);
    pthread_cancel(app.socket_thread);
    pthread_mutex_destroy(&app.mutex);
    uinput_destroy();
    unlink(SOCKET_PATH);
    g_object_unref(gapp);
    return status;
}
