// cutefish_secim_ss.c — slurp+grim tarzı ekran görüntüsü: açılır, sürükle-bırak
// seçimi yapılır, bırakınca seçim PNG olarak panoya kopyalanır. Dialog yok,
// buton yok. ESC veya sağ tık iptal eder.
//
// Yakalama: org.cutefish.Screenshot.captureOutput (compositor D-Bus servisi).
// Seçim katmanı: Xwayland üstünde override-redirect ARGB pencere (cairo ile).
// Panoya: GTK (X11) — seçim başka bir uygulama alınana dek süreç arkada kalır.
//
// Derleme:
//   cc grim-slurp.c -o grim-slurp \
//       $(pkg-config --cflags --libs gtk+-3.0 x11) $(pkg-config --cflags --libs wayland-client)
//
// Kullanım:
//   grim-slurp              # sadece panoya kopyalar
//   grim-slurp dosya.png    # panoya kopyalar ve dosyaya da yazar (denenmedi)
//   grim-slurp --cursor     # fare imlecini de yakalar (denenmedi)

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>

#include <cairo-xlib.h>
#include <gio/gio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glob.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>

#include <wayland-client.h>

// ---------------------------------------------------------------- evdev esc listener ---
typedef struct {
    int fds[16];
    int count;
} EvdevKeyboards;

static void evdev_init(EvdevKeyboards *k) {
    k->count = 0;
    glob_t g;
    if (glob("/dev/input/event*", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && k->count < 16; i++) {
            int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                unsigned long evbits[(EV_MAX + 7) / 8] = {0};
                unsigned long keybits[(KEY_MAX + 7) / 8] = {0};
                if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0 &&
                    (evbits[EV_KEY / 8] & (1 << (EV_KEY % 8)))) {
                    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0 &&
                        (keybits[KEY_ESC / 8] & (1 << (KEY_ESC % 8)))) {
                        k->fds[k->count++] = fd;
                        continue;
                    }
                }
                close(fd);
            }
        }
        globfree(&g);
    }
}

static int evdev_esc_pressed(EvdevKeyboards *k) {
    for (int i = 0; i < k->count; i++) {
        // Doğrudan tuşun fiziksel durumunu kontrol et
        uint8_t keymap[(KEY_MAX + 7) / 8] = {0};
        if (ioctl(k->fds[i], EVIOCGKEY(sizeof(keymap)), keymap) >= 0) {
            if ((keymap[KEY_ESC / 8] >> (KEY_ESC % 8)) & 1)
                return 1;
        }
        // Event akışını da kontrol et
        struct input_event ev[16];
        ssize_t bytes = read(k->fds[i], ev, sizeof(ev));
        if (bytes > 0) {
            int n = bytes / sizeof(struct input_event);
            for (int j = 0; j < n; j++) {
                if (ev[j].type == EV_KEY && ev[j].code == KEY_ESC && (ev[j].value == 1 || ev[j].value == 2))
                    return 1;
            }
        }
    }
    return 0;
}

static void evdev_close(EvdevKeyboards *k) {
    for (int i = 0; i < k->count; i++) {
        close(k->fds[i]);
    }
    k->count = 0;
}

// ---------------------------------------------------------------- outputs ---

typedef struct {
    char name[64];
    int x, y, w, h; // compositor global mantıksal koordinatları
} OutputInfo;

typedef struct {
    OutputInfo outs[16];
    int count;
} OutputList;

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpel,
                            const char *make, const char *model, int32_t transform)
{
    OutputList *list = data;
    OutputInfo *info = &list->outs[list->count];
    info->x = x;
    info->y = y;
}

static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh)
{
    OutputList *list = data;
    list->outs[list->count].w = w;
    list->outs[list->count].h = h;
}

static void output_done(void *data, struct wl_output *o)
{
    OutputList *list = data;
    if (list->count < 15)
        list->count++;
}

static void output_scale(void *data, struct wl_output *o, int32_t scale) {}
static void output_desc(void *data, struct wl_output *o, const char *desc) {}

static void output_name(void *data, struct wl_output *o, const char *name)
{
    OutputList *list = data;
    snprintf(list->outs[list->count].name, sizeof(list->outs[0].name), "%s", name);
}

static const struct wl_output_listener output_listener = {
    output_geometry, output_mode, output_done, output_scale, output_name, output_desc,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
    OutputList *list = data;
    if (strcmp(iface, "wl_output") == 0 && list->count < 16) {
        struct wl_output *output = wl_registry_bind(reg, name, &wl_output_interface, 4);
        wl_output_add_listener(output, &output_listener, list);
    }
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    registry_global, registry_remove,
};

// DISPLAY/XAUTHORITY eksikse (launchpad gibi temiz ortamdan açıldığında) X
// bağlantısını kendi bulur: soketleri ve cutefish yetki dosyalarını tarar.
static Display *open_x_display(char *error, size_t error_size)
{
    Display *display = XOpenDisplay(NULL);
    if (display)
        return display;

    char auth[512];
    auth[0] = '\0';
    // cutefish oturumunun Xwayland yetki dosyaları
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "/run/user/%d/cutefish-xwayland-*/Xauthority", getuid());
    glob_t matches;
    if (glob(pattern, 0, NULL, &matches) == 0 && matches.gl_pathc > 0)
        snprintf(auth, sizeof(auth), "%s", matches.gl_pathv[0]);
    globfree(&matches);
    if (!auth[0]) {
        char *existing = getenv("XAUTHORITY");
        snprintf(auth, sizeof(auth), "%s", existing ? existing : "");
    }

    DIR *dir = opendir("/tmp/.X11-unix");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (strncmp(entry->d_name, "X", 1) != 0)
                continue;
            char *end = NULL;
            long number = strtol(entry->d_name + 1, &end, 10);
            if (!end || *end != '\0' || number < 0 || number > 63)
                continue;
            char name[16];
            snprintf(name, sizeof(name), ":%ld", number);
            setenv("DISPLAY", name, 1);
            if (auth[0])
                setenv("XAUTHORITY", auth, 1);
            display = XOpenDisplay(name);
            if (display)
                break;
        }
        closedir(dir);
    }
    if (!display && error && error_size)
        snprintf(error, error_size,
                 "X ekranı açılamadı (DISPLAY/XAUTHORITY bulunamadı)");
    return display;
}

// WAYLAND_DISPLAY eksikse XDG_RUNTIME_DIR içindeki soketleri dener.
static struct wl_display *open_wayland_display(void)
{
    struct wl_display *display = wl_display_connect(NULL);
    if (display)
        return display;

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime)
        return NULL;
    DIR *dir = opendir(runtime);
    if (!dir)
        return NULL;
    // Tercih sırası: wayland-*, sonra cutefish-*, kilit/kontrol dosyaları hariç.
    char best[128] = "";
    char second[128] = "";
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char path[512];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", runtime, entry->d_name);
        if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
            continue;
        if (strstr(entry->d_name, ".lock") || strstr(entry->d_name, ".control") ||
            strstr(entry->d_name, ".sock"))
            continue;
        if (best[0] == '\0' && strncmp(entry->d_name, "wayland", 7) == 0)
            snprintf(best, sizeof(best), "%s", entry->d_name);
        else if (second[0] == '\0' && strncmp(entry->d_name, "cutefish-", 9) == 0)
            snprintf(second, sizeof(second), "%s", entry->d_name);
    }
    closedir(dir);
    const char *candidates[3] = {best, second, NULL};
    for (int i = 0; i < 2; i++) {
        if (!candidates[i] || !candidates[i][0])
            continue;
        setenv("WAYLAND_DISPLAY", candidates[i], 1);
        display = wl_display_connect(NULL);
        if (display)
            break;
    }
    return display;
}

// Çıkışları toplar; hata olursa 0 döner.
static int list_outputs(OutputList *list)
{
    memset(list, 0, sizeof(*list));
    struct wl_display *display = open_wayland_display();
    if (!display)
        return 0;
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, list);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // isim ve geometri olayları için
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return list->count > 0;
}

// ---------------------------------------------------------------- overlay ---

typedef struct {
    Display *dpy;
    Window window;
    Visual *argb_visual;
    Colormap colormap;
    cairo_surface_t *surface;
    cairo_t *cr;

    int screen_w, screen_h;   // X kök alanı
    double to_logical_x, to_logical_y; // X -> Wayland mantıksal ölçek

    int has_start;
    int start_x, start_y;
    int cur_x, cur_y;
    int dragging;
    int cancelled;
} Overlay;

// İmleci "+" (artı) şekline getirir. Ek bir şey çizmeye gerek kalmadığı
// için fare hareketi gecikmesiz takip eder.
static Cursor create_plus_cursor(Display *dpy)
{
    return XCreateFontCursor(dpy, XC_plus);
}


static void overlay_draw(Overlay *ov)
{
    cairo_t *cr = ov->cr;
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);

    // Her yeri karart
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.35);
    cairo_paint(cr);

    int x1 = ov->has_start ? (ov->start_x < ov->cur_x ? ov->start_x : ov->cur_x) : 0;
    int y1 = ov->has_start ? (ov->start_y < ov->cur_y ? ov->start_y : ov->cur_y) : 0;
    int w = ov->has_start ? abs(ov->cur_x - ov->start_x) : 0;
    int h = ov->has_start ? abs(ov->cur_y - ov->start_y) : 0;

    if (ov->has_start && w > 0 && h > 0) {
        // Seçim alanı tertemiz görünsün
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_rectangle(cr, x1, y1, w, h);
        cairo_fill(cr);

        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.9);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, x1 + 0.75, y1 + 0.75, w - 1.5, h - 1.5);
        cairo_stroke(cr);

        // Boyut etiketi
        char label[64];
        snprintf(label, sizeof(label), "%d × %d",
                 (int)(w * ov->to_logical_x + 0.5), (int)(h * ov->to_logical_y + 0.5));
        cairo_font_extents_t extents;
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 13);
        cairo_font_extents(cr, &extents);
        double text_w = 16 + strlen(label) * 8;
        double text_h = extents.height + 10;
        double lx = x1 + w / 2.0 - text_w / 2.0;
        double ly = y1 + h + 10;
        if (ly + text_h > ov->screen_h)
            ly = y1 - text_h - 10;
        cairo_set_source_rgba(cr, 0.1, 0.1, 0.12, 0.85);
        cairo_rectangle(cr, lx, ly, text_w, text_h);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_move_to(cr, lx + 8, ly + extents.ascent + 5);
        cairo_show_text(cr, label);
    } else {
        // İpucu: kenarda küçük bir metin
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.75);
        cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 14);
        cairo_move_to(cr, 12, ov->screen_h - 12);
        cairo_show_text(cr, "Sürükle-bırak ile alan seç · ESC iptal");
    }
    cairo_surface_flush(ov->surface);
}

static void overlay_refresh(Overlay *ov)
{
    overlay_draw(ov);
    XFlush(ov->dpy);
}

static void selection_rect(const Overlay *ov, int *x, int *y, int *w, int *h)
{
    *x = ov->start_x < ov->cur_x ? ov->start_x : ov->cur_x;
    *y = ov->start_y < ov->cur_y ? ov->start_y : ov->cur_y;
    *w = abs(ov->cur_x - ov->start_x);
    *h = abs(ov->cur_y - ov->start_y);
}

// D-Bus ile tam çıkış PNG'sini alır; hata olursa NULL döner.
static GdkPixbuf *capture_output(const char *output_name, int with_cursor, GError **error)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    if (!bus)
        return NULL;
    GVariant *reply = g_dbus_connection_call_sync(
            bus, "org.cutefish.Screenshot", "/Screenshot", "org.cutefish.Screenshot",
            "captureOutput",
            g_variant_new("(sb)", output_name, with_cursor ? TRUE : FALSE),
            G_VARIANT_TYPE("(ay)"), G_DBUS_CALL_FLAGS_NONE, 15000, NULL, error);
    g_object_unref(bus);
    if (!reply)
        return NULL;

    GVariant *array = g_variant_get_child_value(reply, 0);
    gsize length = 0;
    gconstpointer bytes = g_variant_get_fixed_array(array, &length, 1);

    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    GdkPixbuf *pixbuf = NULL;
    if (gdk_pixbuf_loader_write(loader, bytes, length, error) &&
        gdk_pixbuf_loader_close(loader, error))
        pixbuf = g_object_ref(gdk_pixbuf_loader_get_pixbuf(loader));
    g_object_unref(loader);
    g_variant_unref(array);
    g_variant_unref(reply);
    return pixbuf;
}

// Pano geçmişi sistemde tutulduğu için sahipliği süreçte bekletmiyoruz;
// yalnız gtk_clipboard_store isteğinin iletilmesi için kısa süre dönüyoruz.
static gboolean clipboard_timeout(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

// Seçim: X koordinatları -> Wayland mantıksal koordinatlar -> çıkış yereli
static GdkPixbuf *crop_selection(const Overlay *ov, const OutputList *outputs,
                                 GdkPixbuf *full, int sx, int sy, int sw, int sh)
{
    double gx = sx * ov->to_logical_x;
    double gy = sy * ov->to_logical_y;
    double gw = sw * ov->to_logical_x;
    double gh = sh * ov->to_logical_y;

    // Seçimi başlattığı çıkışa kırp
    const OutputInfo *out = NULL;
    for (int i = 0; i < outputs->count; i++) {
        const OutputInfo *o = &outputs->outs[i];
        if (gx >= o->x && gx < o->x + o->w && gy >= o->y && gy < o->y + o->h) {
            out = o;
            break;
        }
    }
    if (!out && outputs->count > 0)
        out = &outputs->outs[0];
    if (!out || full == NULL)
        return NULL;

    double scale = out->w > 0 ? (double)gdk_pixbuf_get_width(full) / out->w : 1.0;
    int px = (int)((gx - out->x) * scale + 0.5);
    int py = (int)((gy - out->y) * scale + 0.5);
    int pw = (int)(gw * scale + 0.5);
    int ph = (int)(gh * scale + 0.5);
    px = MAX(0, MIN(px, gdk_pixbuf_get_width(full) - 1));
    py = MAX(0, MIN(py, gdk_pixbuf_get_height(full) - 1));
    pw = MAX(1, MIN(pw, gdk_pixbuf_get_width(full) - px));
    ph = MAX(1, MIN(ph, gdk_pixbuf_get_height(full) - py));

    return gdk_pixbuf_new_subpixbuf(full, px, py, pw, ph);
}

int main(int argc, char **argv)
{
    // Temiz ortamlardan (launchpad vb.) açıldığında Wayland yolu eksik olabilir.
    if (!getenv("XDG_RUNTIME_DIR")) {
        char runtime[64];
        snprintf(runtime, sizeof(runtime), "/run/user/%d", getuid());
        setenv("XDG_RUNTIME_DIR", runtime, 1);
    }

    const char *save_path = NULL;
    int with_cursor = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cursor") == 0)
            with_cursor = 1;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Kullanım: cutefish-secim-ss [dosya.png] [--cursor]\n");
            return 0;
        } else
            save_path = argv[i];
    }

    OutputList outputs;
    if (!list_outputs(&outputs)) {
        fprintf(stderr, "cutefish-secim-ss: çıkış listesi alınamadı\n");
        return 1;
    }

    // --- X üstünde seçim katmanı -------------------------------------------
    Overlay ov;
    memset(&ov, 0, sizeof(ov));
    char xerror[256] = "";
    ov.dpy = open_x_display(xerror, sizeof(xerror));
    if (!ov.dpy) {
        fprintf(stderr, "cutefish-secim-ss: %s\n",
                xerror[0] ? xerror : "X ekranı açılamadı");
        return 1;
    }
    int screen = DefaultScreen(ov.dpy);
    Window root = RootWindow(ov.dpy, screen);
    ov.screen_w = DisplayWidth(ov.dpy, screen);
    ov.screen_h = DisplayHeight(ov.dpy, screen);

    // X alanı ile Wayland mantıksal alanı farklıysa (ölçek) çevir
    double wayland_w = 0, wayland_h = 0;
    for (int i = 0; i < outputs.count; i++) {
        wayland_w = MAX(wayland_w, outputs.outs[i].x + outputs.outs[i].w);
        wayland_h = MAX(wayland_h, outputs.outs[i].y + outputs.outs[i].h);
    }
    ov.to_logical_x = wayland_w > 0 ? wayland_w / ov.screen_w : 1.0;
    ov.to_logical_y = wayland_h > 0 ? wayland_h / ov.screen_h : 1.0;

    XVisualInfo vinfo;
    if (!XMatchVisualInfo(ov.dpy, screen, 32, TrueColor, &vinfo)) {
        fprintf(stderr, "cutefish-secim-ss: ARGB görseli yok\n");
        return 1;
    }
    ov.argb_visual = vinfo.visual;
    ov.colormap = XCreateColormap(ov.dpy, root, ov.argb_visual, AllocNone);

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.override_redirect = True;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.colormap = ov.colormap;
    attrs.event_mask = ExposureMask | PointerMotionMask | ButtonPressMask |
                       ButtonReleaseMask | KeyPressMask;
    ov.window = XCreateWindow(ov.dpy, root, 0, 0, ov.screen_w, ov.screen_h, 0, 32,
                              InputOutput, ov.argb_visual,
                              CWOverrideRedirect | CWBackPixel | CWBorderPixel |
                              CWColormap | CWEventMask, &attrs);

    ov.surface = cairo_xlib_surface_create(ov.dpy, ov.window, ov.argb_visual,
                                           ov.screen_w, ov.screen_h);
    ov.cr = cairo_create(ov.surface);

    Cursor cursor = create_plus_cursor(ov.dpy);
    XDefineCursor(ov.dpy, ov.window, cursor);
    XMapRaised(ov.dpy, ov.window);
    XSync(ov.dpy, False);

    // XGrabKeyboard pencere görünür (viewable) olmadan başarısız olur;
    // XMapWindow eşzamansız olduğu için önce eşleşmeyi bekliyoruz. ESC'nin
    // çalışmamasının sebebi buydu.
    {
        XWindowAttributes wa;
        for (int i = 0; i < 100; i++) {
            if (XGetWindowAttributes(ov.dpy, ov.window, &wa) && wa.map_state == IsViewable)
                break;
            g_usleep(10 * 1000);
            XSync(ov.dpy, False);
        }
    }

    XGrabPointer(ov.dpy, ov.window, True,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, ov.window, cursor, CurrentTime);

    int keyboard_grabbed = 0;
    KeyCode esc_code = XKeysymToKeycode(ov.dpy, XK_Escape);

    // Kök pencerede Escape tuşunu grab et (Wayland/Xwayland odak sorununu çözer)
    if (esc_code != 0) {
        XGrabKey(ov.dpy, esc_code, AnyModifier, root, False, GrabModeAsync, GrabModeAsync);
        XGrabKey(ov.dpy, esc_code, AnyModifier, ov.window, False, GrabModeAsync, GrabModeAsync);
    }

    for (int i = 0; i < 10 && !keyboard_grabbed; i++) {
        if (XGrabKeyboard(ov.dpy, ov.window, True, GrabModeAsync, GrabModeAsync,
                          CurrentTime) == GrabSuccess) {
            keyboard_grabbed = 1;
        } else {
            g_usleep(20 * 1000);
        }
    }
    // Grab yine olmazsa ESC'yi yakalayabilmek için odağı zorla.
    if (!keyboard_grabbed)
        XSetInputFocus(ov.dpy, ov.window, RevertToPointerRoot, CurrentTime);

    overlay_refresh(&ov);

    // Donanım düzeyinde Escape tuşlarını doğrudan yakalamak için evdev başlat
    EvdevKeyboards evdev_kb;
    evdev_init(&evdev_kb);

    // --- Seçim döngüsü ------------------------------------------------------
    int esc_was_down = 0;
    for (;;) {
        int finished = 0;

        while (XPending(ov.dpy)) {
            XEvent event;
            XNextEvent(ov.dpy, &event);
            if (event.type == Expose) {
                overlay_refresh(&ov);
            } else if (event.type == MotionNotify) {
                ov.cur_x = event.xmotion.x_root;
                ov.cur_y = event.xmotion.y_root;
                ov.dragging = ov.has_start;
                overlay_refresh(&ov);
            } else if (event.type == ButtonPress) {
                if (event.xbutton.button == Button1) {
                    ov.has_start = 1;
                    ov.start_x = event.xbutton.x_root;
                    ov.start_y = event.xbutton.y_root;
                    ov.cur_x = ov.start_x;
                    ov.cur_y = ov.start_y;
                    overlay_refresh(&ov);
                } else {
                    ov.cancelled = 1;
                    finished = 1;
                    break;
                }
            } else if (event.type == ButtonRelease) {
                if (event.xbutton.button == Button1 && ov.has_start) {
                    ov.cur_x = event.xbutton.x_root;
                    ov.cur_y = event.xbutton.y_root;
                    finished = 1;
                    break;
                }
            } else if (event.type == KeyPress) {
                KeySym sym = XLookupKeysym(&event.xkey, 0);
                if (sym == XK_Escape || event.xkey.keycode == esc_code) {
                    ov.cancelled = 1;
                    finished = 1;
                    break;
                }
            }
        }
        if (finished)
            break;

        if (evdev_esc_pressed(&evdev_kb)) {
            ov.cancelled = 1;
            break;
        }

        // Klavye grab'ı bir nedenle başarısız olduysa KeyPress olayı bize
        // gelmez; bu yüzden ESC'yi doğrudan fiziksel tuş durumundan da izle.
        if (esc_code != 0) {
            char keys[32];
            XQueryKeymap(ov.dpy, keys);
            int down = (keys[esc_code / 8] >> (esc_code % 8)) & 1;
            if (down && !esc_was_down) {
                ov.cancelled = 1;
                break;
            }
            esc_was_down = down;
        }

        g_usleep(15 * 1000); // ~66 Hz yoklama; boşta CPU'yu yormaz
    }

    int sx, sy, sw, sh;
    selection_rect(&ov, &sx, &sy, &sw, &sh);
    evdev_close(&evdev_kb);
    if (esc_code != 0) {
        XUngrabKey(ov.dpy, esc_code, AnyModifier, root);
        XUngrabKey(ov.dpy, esc_code, AnyModifier, ov.window);
    }
    if (ov.cancelled || !ov.has_start || sw < 4 || sh < 4) {
        XUngrabKeyboard(ov.dpy, CurrentTime);
        XUngrabPointer(ov.dpy, CurrentTime);
        XDestroyWindow(ov.dpy, ov.window);
        cairo_destroy(ov.cr);
        cairo_surface_destroy(ov.surface);
        XCloseDisplay(ov.dpy);
        fprintf(stderr, "cutefish-secim-ss: iptal\n");
        return 0;
    }

    // --- Katmanı kaldır, kompozitör tazeleninceye dek bekle -----------------
    XUngrabKeyboard(ov.dpy, CurrentTime);
    XUngrabPointer(ov.dpy, CurrentTime);
    XUnmapWindow(ov.dpy, ov.window);
    XFlush(ov.dpy);
    g_usleep(220 * 1000);

    // --- Hangi çıkış? -------------------------------------------------------
    double lgx = sx * ov.to_logical_x;
    double lgy = sy * ov.to_logical_y;
    const OutputInfo *out = NULL;
    for (int i = 0; i < outputs.count; i++) {
        const OutputInfo *o = &outputs.outs[i];
        if (lgx >= o->x && lgx < o->x + o->w && lgy >= o->y && lgy < o->y + o->h) {
            out = o;
            break;
        }
    }
    if (!out && outputs.count > 0)
        out = &outputs.outs[0];

    // --- Yakala -------------------------------------------------------------
    GError *error = NULL;
    GdkPixbuf *full = capture_output(out->name, with_cursor, &error);
    if (!full) {
        fprintf(stderr, "cutefish-secim-ss: yakalama başarısız: %s\n",
                error && error->message ? error->message : "?");
        return 1;
    }

    GdkPixbuf *shot = crop_selection(&ov, &outputs, full, sx, sy, sw, sh);
    if (!shot) {
        fprintf(stderr, "cutefish-secim-ss: kırpma başarısız\n");
        return 1;
    }

    if (save_path) {
        GError *save_error = NULL;
        if (!gdk_pixbuf_save(shot, save_path, "png", &save_error, NULL)) {
            fprintf(stderr, "cutefish-secim-ss: kaydedilemedi: %s\n",
                    save_error && save_error->message ? save_error->message : "?");
        }
    }

    // --- Panoya kopyala -----------------------------------------------------
    setenv("GDK_BACKEND", "x11", 1);
    gtk_init(&argc, &argv);
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_image(clipboard, shot);
    gtk_clipboard_store(clipboard);

    printf("cutefish-secim-ss: %dx%d panoya kopyalandı (%s)\n",
           gdk_pixbuf_get_width(shot), gdk_pixbuf_get_height(shot), out->name);
    fflush(stdout);

    // Pano geçmişi sistemde saklandığından sahipliği korumaya gerek yok.
    // Sadece store isteği işlenebilsin diye kısa bir süre ana döngü çevir.
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(800, clipboard_timeout, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    g_object_unref(shot);
    g_object_unref(full);
    return 0;
}
