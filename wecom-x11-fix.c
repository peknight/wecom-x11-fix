/*
 * wecom-x11-fix - X11 workarounds for Deepin-Wine WeCom
 *
 * Two layout bugs are handled:
 *
 * 1. WeCom draws parts of its client-side chrome in transparent
 *    override-redirect helper windows (the title bar strip with the
 *    minimize/maximize/close buttons, and small widgets such as the "more"
 *    button) and raises them above every other window on the screen.  Because
 *    those windows have a full input shape, clicks aimed at whatever is really
 *    on top at that place are swallowed by WeCom, and because they sit above
 *    the windows they belong under, they bleed through dialogs.  Their input
 *    and bounding regions are trimmed by the areas covered by windows stacked
 *    above the WeCom window they belong to, so clicks reach the window the user
 *    actually sees, the drawn buttons stop bleeding through dialogs, and the
 *    uncovered part keeps working as WeCom's own chrome.
 *
 * 2. WeCom draws its drop shadow into a separate 32 bit window that is larger
 *    than the main window on every side.  During an interactive move or
 *    resize the shadow is reconfigured one pointer event late, which shows up
 *    as a white border trailing the window.  The shadow is glued to the main
 *    window as soon as the window manager reports a new geometry.
 *
 * 3. That shadow window paints the inner 8 pixels of its hole - the band that
 *    sits on top of the window's own edge - as fully opaque white (dialogs)
 *    or as an opaque copy of the window background (main window).  On dialogs
 *    such as the assistant panel this reads as a white frame around the
 *    window.  The shadow's bounding shape is intersected with the area outside
 *    its owner window, so only the soft outer shadow is kept.
 *
 *    Mail windows live in a separate WeMail.exe process.  Its shadow is an
 *    override-redirect ARGB window without WM_TRANSIENT_FOR, and Wine keeps it
 *    above every other top-level, so the same inner band also floats above
 *    windows that cover the mail window.  Shadow candidates therefore include
 *    override-redirect windows, and the shadow shape is clipped by the windows
 *    stacked above its owner as well as by the owner itself.
 *
 * Nothing is written to the Wine prefix, the application data or any
 * configuration file.  The program only adjusts X11 window geometry and
 * input shapes; it restores the original shapes when it exits.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

/*
 * 两个发行线路跑的是同一个程序、带同一套 bug，但窗口的 WM_CLASS 不同：
 * deepin 的 deb 包由它自己的 run.sh 导出 com.qq.weixin.work.deepin，Windows exe
 * 那条线路用 wecom-win（见 wecom-win-bin 的启动器）。两条线路都会被修复，所以
 * 这里认一个集合而不是单值。
 *
 * 不经过启动器、手工 wine 起来的实例没有 WINE_WMCLASS，WM_CLASS 会退回
 * ("wxwork.exe", "Wine")，按类名就漏掉了；这种实例同样有那两套 bug，所以再按
 * 可执行文件名认一次。wxwork.exe 是主程序，wemail.exe 是邮件窗口所在的
 * 独立进程；只认这两个名字，不会波及其它 Wine 程序。
 */
static const char *const wecom_classes[] = {
    "com.qq.weixin.work.deepin",
    "wecom-win",
    "wecom-win11",
};
static const char *const wecom_names[] = {
    "wxwork.exe",
    "wemail.exe",
};
#define MAX_TARGETS 8
#define RING_PAD_MIN 2
#define RING_PAD_MAX 40
#define MAIN_GROW_MAX 640
#define GROW_SETTLE_MS 800.0
#define STRIP_MAX_HEIGHT 64
#define OVERLAY_MAX_HEIGHT 64
#define OVERLAY_SLACK 64
#define OVERLAY_MIN_AREA 64
#define MAX_OVERLAYS 8
#define MAX_HOLES 64
#define SCAN_INTERVAL_MS 10000.0
#define SCAN_DEBOUNCE_MS 250.0
#define GLUE_INTERVAL_MS 50.0
#define IDLE_GLUE_INTERVAL_MS 250.0
#define HOT_WINDOW_MS 600.0
#define SHAPE_INTERVAL_MS 30.0
#define SHAPE_WATCHDOG_MS 250.0
#define SHAPE_ECHO_MS 150.0

typedef struct {
    int x, y, w, h;
} Rect;

/*
 * An ARGB helper window that WeCom uses to draw part of its own chrome or
 * shadow.  Older Wine versions created these as override-redirect windows;
 * newer ones make the title bar shadow a managed dialog, so both forms are
 * tracked.  base/base_input are the shapes Wine gave the window; they are kept
 * so that shaping only ever removes pixels and the original shapes can be
 * restored on exit.
 */
typedef struct {
    Window win;
    Window frame;
    XRectangle *base;
    int base_count;
    XRectangle *base_input;
    int base_input_count;
    int base_w;
    int base_h;
    XRectangle *applied;
    int applied_count;
    int seen;
    int managed;
    int click_through;
    double applied_ms;
} Overlay;

/*
 * The shadow window of a target.  base is the shape Wine gave it; the shape we
 * want is base minus the area covered by the owner window itself, which drops
 * the opaque inner band while keeping the outer shadow gradient.
 */
typedef struct {
    Window win;
    XRectangle *base;
    int base_count;
    XRectangle *base_input;
    int base_input_count;
    XRectangle *applied;
    int applied_count;
    int captured;
    double applied_ms;
} RingShape;

typedef struct {
    Window main;
    Window frame;
    Window ring;
    Window ring_frame;
    RingShape ring_shape;
    Overlay overlays[MAX_OVERLAYS];
    int overlay_count;
    int ring_pad;
    int glue_dx, glue_dy, glue_dw, glue_dh;
    int glue_stuck;
    int min_w, min_h;
    double last_grow_ms;
    int stale_bw, stale_bh;
    double stale_since_ms;
    double last_config_ms;
    unsigned long pid;
    int valid;
} Target;

static Display *dpy;
static Window root;
static int verbose;
static volatile sig_atomic_t running = 1;
static double hot_until;
static int shape_dirty = 1;

static Atom atom_wm_pid;
static Atom atom_wm_state;
static Atom atom_wm_state_maximized_vert;
static Atom atom_wm_state_maximized_horz;
static Atom atom_wm_window_type;
static Atom atom_wm_window_type_normal;
static Atom atom_wm_window_type_popup_menu;
static Atom atom_wm_window_type_dropdown_menu;
static Atom atom_wm_window_type_tooltip;
static Atom atom_client_list;

static Target targets[MAX_TARGETS];

static int shape_event_base;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;

    if (!verbose) {
        return;
    }
    va_start(ap, fmt);
    fputs("wecom-x11-fix: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int ignore_x_error(Display *d, XErrorEvent *e)
{
    char text[128];

    if (e->error_code == BadWindow || e->error_code == BadDrawable ||
        e->error_code == BadMatch || e->error_code == BadValue) {
        return 0;
    }
    XGetErrorText(d, e->error_code, text, sizeof(text));
    logmsg("X error: %s (request %d)", text, e->request_code);
    return 0;
}

static int win_rect(Window win, Rect *out)
{
    Window child;
    int x = 0, y = 0;
    unsigned int w = 0, h = 0, border = 0, depth = 0;

    if (win == None) {
        return 0;
    }
    if (!XGetGeometry(dpy, win, &child, &x, &y, &w, &h, &border, &depth)) {
        return 0;
    }
    if (!XTranslateCoordinates(dpy, win, root, 0, 0, &x, &y, &child)) {
        return 0;
    }
    out->x = x;
    out->y = y;
    out->w = (int)w;
    out->h = (int)h;
    return out->w > 0 && out->h > 0;
}

static unsigned long win_pid(Window win)
{
    Atom type;
    int format;
    unsigned long count = 0, after = 0;
    unsigned char *data = NULL;
    unsigned long pid = 0;

    if (XGetWindowProperty(dpy, win, atom_wm_pid, 0, 1, False, XA_CARDINAL,
                           &type, &format, &count, &after, &data) == Success &&
        data != NULL) {
        if (count == 1) {
            pid = *(unsigned long *)data;
        }
        XFree(data);
    }
    return pid;
}

static int win_is_wecom(Window win)
{
    XClassHint hint = {0};
    int match = 0;

    if (!XGetClassHint(dpy, win, &hint)) {
        return 0;
    }
    if (hint.res_class != NULL) {
        size_t i;

        for (i = 0; i < sizeof(wecom_classes) / sizeof(wecom_classes[0]); i++) {
            if (strcmp(hint.res_class, wecom_classes[i]) == 0) {
                match = 1;
                break;
            }
        }
    }
    if (!match && hint.res_name != NULL) {
        size_t i;

        for (i = 0; i < sizeof(wecom_names) / sizeof(wecom_names[0]); i++) {
            if (strcasecmp(hint.res_name, wecom_names[i]) == 0) {
                match = 1;
                break;
            }
        }
    }
    if (hint.res_name != NULL) {
        XFree(hint.res_name);
    }
    if (hint.res_class != NULL) {
        XFree(hint.res_class);
    }
    return match;
}

static int win_has_window_type(Window win, Atom wanted)
{
    Atom type;
    int format;
    unsigned long count = 0, after = 0;
    unsigned char *data = NULL;
    int found = 0;

    if (XGetWindowProperty(dpy, win, atom_wm_window_type, 0, 8, False, XA_ATOM,
                           &type, &format, &count, &after, &data) == Success &&
        data != NULL) {
        Atom *atoms = (Atom *)data;
        for (unsigned long i = 0; i < count; i++) {
            if (atoms[i] == wanted) {
                found = 1;
                break;
            }
        }
        XFree(data);
    }
    return found;
}

static int win_is_maximized(Window win)
{
    Atom type;
    int format;
    unsigned long count = 0, after = 0;
    unsigned char *data = NULL;
    int maximized = 0;

    if (XGetWindowProperty(dpy, win, atom_wm_state, 0, 8, False, XA_ATOM,
                           &type, &format, &count, &after, &data) == Success &&
        data != NULL) {
        Atom *atoms = (Atom *)data;

        for (unsigned long i = 0; i < count; i++) {
            if (atoms[i] == atom_wm_state_maximized_vert ||
                atoms[i] == atom_wm_state_maximized_horz) {
                maximized = 1;
                break;
            }
        }
        XFree(data);
    }
    return maximized;
}

static Window win_top_level(Window win)
{
    Window current = win;

    for (int depth = 0; depth < 8; depth++) {
        Window root_ret, parent_ret, *children = NULL;
        unsigned int nchildren = 0;

        if (!XQueryTree(dpy, current, &root_ret, &parent_ret, &children,
                        &nchildren)) {
            return current;
        }
        if (children != NULL) {
            XFree(children);
        }
        if (parent_ret == root || parent_ret == None) {
            return current;
        }
        current = parent_ret;
    }
    return current;
}

static int win_is_known_overlay(Window win)
{
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];

        if (!t->valid) {
            continue;
        }
        for (int j = 0; j < t->overlay_count; j++) {
            if (t->overlays[j].win == win || t->overlays[j].frame == win) {
                return 1;
            }
        }
    }
    return 0;
}

static Overlay *find_overlay(Window win)
{
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];

        if (!t->valid) {
            continue;
        }
        for (int j = 0; j < t->overlay_count; j++) {
            if (t->overlays[j].win == win) {
                return &t->overlays[j];
            }
        }
    }
    return NULL;
}

/*
 * Fill a hole list with the parts of a window rectangle that are covered by
 * windows stacked above anchor.  XQueryTree returns root children bottom to
 * top, so only entries after the anchor's own top-level frame can occlude it.
 * Anchoring at the window being shaped (rather than at the main frame) keeps
 * windows between main and the shadow/overlay from being treated as if they
 * were above it.  Coordinates are rect-local.
 */
static int collect_occluders(Target *t, Window anchor, const Rect *rect,
                             XRectangle *holes, int max_holes)
{
    Window root_ret, parent_ret, *top = NULL;
    unsigned int ntop = 0;
    int anchor_index = -1;
    int n = 0;

    if (t->frame == None) {
        return 0;
    }
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &top, &ntop)) {
        return 0;
    }
    for (unsigned int i = 0; i < ntop; i++) {
        if (top[i] == anchor) {
            anchor_index = (int)i;
            break;
        }
    }
    if (anchor_index < 0) {
        if (top != NULL) {
            XFree(top);
        }
        return 0;
    }
    for (unsigned int i = (unsigned int)anchor_index + 1; i < ntop; i++) {
        Window win = top[i];

        if (win_is_known_overlay(win) || win == t->ring_frame) {
            continue;
        }

        XWindowAttributes attr;
        if (!XGetWindowAttributes(dpy, win, &attr)) {
            continue;
        }
        if (attr.map_state != IsViewable) {
            continue;
        }

        Rect r;
        if (!win_rect(win, &r)) {
            continue;
        }
        int x0 = r.x > rect->x ? r.x : rect->x;
        int y0 = r.y > rect->y ? r.y : rect->y;
        int x1 = (r.x + r.w) < (rect->x + rect->w)
                     ? (r.x + r.w)
                     : (rect->x + rect->w);
        int y1 = (r.y + r.h) < (rect->y + rect->h)
                     ? (r.y + r.h)
                     : (rect->y + rect->h);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        if (n >= max_holes) {
            break;
        }
        holes[n].x = (short)(x0 - rect->x);
        holes[n].y = (short)(y0 - rect->y);
        holes[n].width = (unsigned short)(x1 - x0);
        holes[n].height = (unsigned short)(y1 - y0);
        n++;
    }
    if (top != NULL) {
        XFree(top);
    }
    return n;
}

static XserverRegion overlay_full_region(const Rect *s)
{
    XRectangle full;

    full.x = 0;
    full.y = 0;
    full.width = (unsigned short)s->w;
    full.height = (unsigned short)s->h;
    return XFixesCreateRegion(dpy, &full, 1);
}

static XserverRegion overlay_base_region(const Overlay *o, const Rect *s)
{
    if (o->base_count > 0 && o->base != NULL) {
        return XFixesCreateRegion(dpy, o->base, o->base_count);
    }
    return overlay_full_region(s);
}

static int rect_lists_equal(const XRectangle *a, int na, const XRectangle *b,
                            int nb)
{
    if (na != nb) {
        return 0;
    }
    for (int i = 0; i < na; i++) {
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].width != b[i].width ||
            a[i].height != b[i].height) {
            return 0;
        }
    }
    return 1;
}

/*
 * Wine rewrites the shape of its layered windows (the title bar strip
 * included) on repaint and focus changes, and that rewrite restores the full
 * input shape: the buttons stop being drawn but start swallowing clicks again.
 * Compare what the server currently has with the region we want so that the
 * fix can be re-asserted whenever someone else has overwritten it.
 */
static int window_shape_is(Window win, const XRectangle *want, int nwant)
{
    int nbound = 0, ninput = 0, ordering = 0;
    XRectangle *bound = XShapeGetRectangles(dpy, win, ShapeBounding, &nbound,
                                            &ordering);
    XRectangle *input = XShapeGetRectangles(dpy, win, ShapeInput, &ninput,
                                            &ordering);
    int match = bound != NULL && input != NULL &&
                rect_lists_equal(want, nwant, bound, nbound) &&
                rect_lists_equal(want, nwant, input, ninput);

    if (bound != NULL) {
        XFree(bound);
    }
    if (input != NULL) {
        XFree(input);
    }
    return match;
}

static int input_shape_empty(Window win)
{
    int count = 0, ordering = 0;
    XRectangle *rects =
        XShapeGetRectangles(dpy, win, ShapeInput, &count, &ordering);
    int empty = count == 0;

    if (rects != NULL) {
        XFree(rects);
    }
    return empty;
}

static int bounding_shape_is(Window win, const XRectangle *want, int nwant)
{
    int nbound = 0, ordering = 0;
    XRectangle *bound =
        XShapeGetRectangles(dpy, win, ShapeBounding, &nbound, &ordering);
    int match =
        bound != NULL && rect_lists_equal(want, nwant, bound, nbound);

    if (bound != NULL) {
        XFree(bound);
    }
    return match;
}

static void shape_bbox(const XRectangle *rects, int count, int *out_w,
                       int *out_h)
{
    int w = 0, h = 0;

    for (int i = 0; i < count; i++) {
        int right = (int)rects[i].x + (int)rects[i].width;
        int bottom = (int)rects[i].y + (int)rects[i].height;

        if (right > w) {
            w = right;
        }
        if (bottom > h) {
            h = bottom;
        }
    }
    *out_w = w;
    *out_h = h;
}

static int shape_bounds(Window win, int *out_w, int *out_h)
{
    int count = 0, ordering = 0, w = 0, h = 0;
    XRectangle *rects;

    rects = XShapeGetRectangles(dpy, win, ShapeBounding, &count, &ordering);
    if (rects == NULL) {
        return 0;
    }
    shape_bbox(rects, count, &w, &h);
    XFree(rects);
    if (count <= 0) {
        return 0;
    }
    *out_w = w;
    *out_h = h;
    return 1;
}

static int overlay_shape_exceeds_window(Window win, const Rect *r)
{
    int bw = 0, bh = 0;

    if (!shape_bounds(win, &bw, &bh)) {
        return 0;
    }
    return bw > r->w || bh > r->h;
}

static void assign_overlays(void);

/*
 * The size WeCom refuses to go below.  Wine does not export it as a
 * WM_NORMAL_HINTS minimum, so xfwm4 happily restores a smaller geometry after
 * unmaximizing; WeCom then keeps its layered surface - and every pixel of its
 * layout, including the "-[]X" strip - at the old width while the X window is
 * narrower.  Once that happens the shape is the only remaining witness of the
 * real surface size, so mirror it into the hints and the window manager will
 * stop handing out unusable sizes.
 */
static void apply_min_size_hints(Target *t, const Rect *m)
{
    XSizeHints hints;
    long supplied = 0;
    int changed = 0;

    if (t->min_w <= 0 && t->min_h <= 0) {
        return;
    }
    if (!XGetWMNormalHints(dpy, t->main, &hints, &supplied)) {
        memset(&hints, 0, sizeof(hints));
    }
    if (t->min_w > 0 &&
        (!(hints.flags & PMinSize) || hints.min_width < t->min_w)) {
        hints.min_width = t->min_w;
        changed = 1;
    }
    if (t->min_h > 0 &&
        (!(hints.flags & PMinSize) || hints.min_height < t->min_h)) {
        hints.min_height = t->min_h;
        changed = 1;
    }
    if (!changed) {
        return;
    }
    if (!(hints.flags & PMinSize)) {
        hints.flags |= PMinSize;
        if (hints.min_width <= 0) {
            hints.min_width = 1;
        }
        if (hints.min_height <= 0) {
            hints.min_height = 1;
        }
    }
    if (!(hints.flags & (USPosition | PPosition))) {
        hints.flags |= USPosition;
        hints.x = m->x;
        hints.y = m->y;
    }
    XSetWMNormalHints(dpy, t->main, &hints);
    XFlush(dpy);
    logmsg("main %#lx minimum size %dx%d", t->main, hints.min_width,
           hints.min_height);
}

/*
 * WeCom answered a resize with a surface that is larger than the X window:
 * the layout is still bw x bh and only its left/top part is visible.  Growing
 * the window to the surface size is the only way to get the right edge - with
 * the minimize/maximize/close strip in it - back on screen.
 */
static void main_grow_to_surface(Target *t, const Rect *m, int bw, int bh)
{
    int nw = bw > m->w ? bw : m->w;
    int nh = bh > m->h ? bh : m->h;
    int screen_w = DisplayWidth(dpy, DefaultScreen(dpy));
    int screen_h = DisplayHeight(dpy, DefaultScreen(dpy));

    if (win_is_maximized(t->main)) {
        return;
    }
    /*
     * While the user drags or resizes the frame, the client shape trails the
     * new geometry for a few frames.  Growing the window then would fight the
     * drag and could record a transient size as WeCom's minimum.
     */
    if (now_ms() - t->last_config_ms < GROW_SETTLE_MS) {
        return;
    }
    /*
     * Give Wine a moment to re-assert its own size first.  A shape that is
     * briefly wider than the window during a resize is normal; only one that
     * sticks around means WeCom really kept the larger surface.
     */
    if (t->stale_bw != bw || t->stale_bh != bh) {
        t->stale_bw = bw;
        t->stale_bh = bh;
        t->stale_since_ms = now_ms();
        return;
    }
    if (now_ms() - t->stale_since_ms < GROW_SETTLE_MS) {
        return;
    }
    if (bw > m->w && bw - m->w <= MAIN_GROW_MAX && bw > t->min_w) {
        t->min_w = bw;
    }
    if (bh > m->h && bh - m->h <= MAIN_GROW_MAX && bh > t->min_h) {
        t->min_h = bh;
    }
    apply_min_size_hints(t, m);
    if (nw > screen_w - m->x) {
        nw = screen_w - m->x;
    }
    if (nh > screen_h - m->y) {
        nh = screen_h - m->y;
    }
    if (nw <= m->w && nh <= m->h) {
        return;
    }
    if (now_ms() - t->last_grow_ms < 1000) {
        return;
    }
    t->last_grow_ms = now_ms();
    XMoveResizeWindow(dpy, t->main, m->x, m->y, (unsigned)nw, (unsigned)nh);
    XFlush(dpy);
    logmsg("main %#lx grown from %dx%d to %dx%d to match its surface", t->main,
           m->w, m->h, nw, nh);
    assign_overlays();
}

/*
 * Stretch a stale mask onto the current window size.  Every rectangle keeps
 * its distance to the four old edges, so the rounded corners of WeCom's own
 * mask survive and the straight middle grows or shrinks as a whole.
 */
static void main_shape_stretch(Target *t, const Rect *m, int bw, int bh)
{
    int count = 0, ordering = 0, out = 0;
    XRectangle *rects;
    XRectangle *want;
    XserverRegion region;

    rects = XShapeGetRectangles(dpy, t->main, ShapeBounding, &count, &ordering);
    if (rects == NULL || count <= 0) {
        if (rects != NULL) {
            XFree(rects);
        }
        return;
    }
    want = malloc(sizeof(*want) * (size_t)count);
    if (want == NULL) {
        XFree(rects);
        return;
    }
    for (int i = 0; i < count; i++) {
        int left = rects[i].x;
        int top = rects[i].y;
        int right = bw - ((int)rects[i].x + (int)rects[i].width);
        int bottom = bh - ((int)rects[i].y + (int)rects[i].height);
        int w = m->w - left - right;
        int h = m->h - top - bottom;

        if (left < 0) {
            left = 0;
        }
        if (top < 0) {
            top = 0;
        }
        if (w <= 0 || h <= 0) {
            continue;
        }
        want[out].x = (short)left;
        want[out].y = (short)top;
        want[out].width = (unsigned short)w;
        want[out].height = (unsigned short)h;
        out++;
    }
    XFree(rects);
    if (out <= 0) {
        free(want);
        return;
    }
    region = XFixesCreateRegion(dpy, want, out);
    XFixesSetWindowShapeRegion(dpy, t->main, ShapeBounding, 0, 0, region);
    XFixesSetWindowShapeRegion(dpy, t->main, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(dpy, region);
    XFlush(dpy);
    free(want);
    logmsg("shape of main %#lx stretched from %dx%d to %dx%d", t->main,
           bw, bh, m->w, m->h);
}

/*
 * Wine only rewrites the bounding shape of a layered window when it flushes
 * that window's surface.  A resize can reach the X server while Wine keeps
 * its surface at the old size; xfwm4 copies that shape onto the frame, so
 * everything outside it is clipped away and the window appears to lose its
 * right edge.  Which side is stale decides the repair: a smaller shape is
 * stretched over the window, a larger shape means WeCom refused the shrink
 * and the window has to grow back to it.
 */
static void main_shape_reconcile(Target *t)
{
    Rect m;
    int bw = 0, bh = 0;

    if (!t->valid || !win_rect(t->main, &m)) {
        return;
    }
    if (!shape_bounds(t->main, &bw, &bh)) {
        return;
    }
    apply_min_size_hints(t, &m);
    if (bw > m.w + 8 || bh > m.h + 8) {
        main_grow_to_surface(t, &m, bw, bh);
        return;
    }
    t->stale_bw = 0;
    t->stale_since_ms = 0;
    if (abs(bw - m.w) <= 2 && abs(bh - m.h) <= 2) {
        return;
    }
    main_shape_stretch(t, &m, bw, bh);
}

/*
 * Cheap check used by the watchdog: only asks the server what the window's
 * shape is now and compares it with the list we last applied.  Recomputing
 * the occluders here would walk every top-level window three round trips at a
 * time, which is far too much work to do on a timer.
 */
static int overlay_shape_intact(const Overlay *o)
{
    if (o->applied == NULL) {
        return 0;
    }
    if (o->click_through) {
        return input_shape_empty(o->win) &&
               bounding_shape_is(o->win, o->applied, o->applied_count);
    }
    return window_shape_is(o->win, o->applied, o->applied_count);
}

static int overlay_capture_base(Overlay *o);

/*
 * True when the window's current bounding shape already covers exactly the
 * window's own rectangle.  Such a shape can only be the one Wine recomputed
 * for the current geometry, so it is safe to capture it as the new base.
 */
static int shape_matches_window(Window win, const Rect *s)
{
    int count = 0, ordering = 0, bw = 0, bh = 0;
    XRectangle *rects =
        XShapeGetRectangles(dpy, win, ShapeBounding, &count, &ordering);

    if (rects == NULL) {
        return 0;
    }
    shape_bbox(rects, count, &bw, &bh);
    XFree(rects);
    return count > 0 && bw == s->w && bh == s->h;
}

static void overlay_apply_shape(Target *t, Overlay *o)
{
    Rect s;
    XRectangle holes[MAX_HOLES];
    int nholes;
    XserverRegion region, input_region;
    Window anchor;

    if (!win_rect(o->win, &s)) {
        return;
    }
    if (o->base_w != s.w || o->base_h != s.h) {
        if (shape_matches_window(o->win, &s)) {
            overlay_capture_base(o);
        } else {
            /* The cached mask belongs to the old size; a plain rectangle is
               correct until Wine rewrites the shape for the new one. */
            if (o->base != NULL) {
                XFree(o->base);
            }
            if (o->base_input != NULL) {
                XFree(o->base_input);
            }
            o->base = NULL;
            o->base_count = 0;
            o->base_input = NULL;
            o->base_input_count = 0;
            o->base_w = s.w;
            o->base_h = s.h;
        }
    }
    region = overlay_base_region(o, &s);
    anchor = o->frame != None ? o->frame : win_top_level(o->win);
    nholes = collect_occluders(t, anchor, &s, holes, MAX_HOLES);
    if (nholes > 0) {
        XserverRegion covered = XFixesCreateRegion(dpy, holes, nholes);
        XFixesSubtractRegion(dpy, region, region, covered);
        XFixesDestroyRegion(dpy, covered);
    }
    int nwant = 0;
    XRectangle *want = XFixesFetchRegion(dpy, region, &nwant);

    if (o->click_through) {
        XRectangle empty = {0, 0, 0, 0};

        input_region = XFixesCreateRegion(dpy, &empty, 1);
    } else {
        input_region = XFixesCreateRegion(dpy, want, nwant);
    }
    int input_ok =
        o->click_through
            ? input_shape_empty(o->win) &&
                  bounding_shape_is(o->win, want, nwant)
            : input_region != None && window_shape_is(o->win, want, nwant);

    if (want != NULL && input_region != None && !input_ok) {
        XFixesSetWindowShapeRegion(dpy, o->win, ShapeBounding, 0, 0, region);
        XFixesSetWindowShapeRegion(dpy, o->win, ShapeInput, 0, 0, input_region);
        XFlush(dpy);
        o->applied_ms = now_ms();
    }
    if (input_region != None) {
        XFixesDestroyRegion(dpy, input_region);
    }
    if (want != NULL) {
        if (o->applied != NULL) {
            XFree(o->applied);
        }
        o->applied = want;
        o->applied_count = nwant;
    }
    XFixesDestroyRegion(dpy, region);
}

static void overlay_restore(Target *t, Overlay *o)
{
    Rect s;
    XserverRegion region;

    (void)t;
    if (!win_rect(o->win, &s)) {
        return;
    }
    region = overlay_base_region(o, &s);
    XFixesSetWindowShapeRegion(dpy, o->win, ShapeBounding, 0, 0, region);
    XFixesDestroyRegion(dpy, region);
    if (o->base_input_count > 0 && o->base_input != NULL) {
        region = XFixesCreateRegion(dpy, o->base_input, o->base_input_count);
    } else {
        region = overlay_full_region(&s);
    }
    XFixesSetWindowShapeRegion(dpy, o->win, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(dpy, region);
    XFlush(dpy);
}

static void window_full_rect(Window win, XRectangle *out)
{
    Rect r;

    memset(out, 0, sizeof(*out));
    if (win_rect(win, &r)) {
        out->width = (unsigned short)r.w;
        out->height = (unsigned short)r.h;
    }
}

/*
 * Put the shadow window's shape back to what Wine gave it.  Called when the
 * shadow disappears or is replaced, and on exit.
 */
static void ring_shape_restore(Target *t)
{
    RingShape *rs = &t->ring_shape;
    XRectangle full;

    if (!rs->captured || rs->win == None) {
        return;
    }
    if (rs->base_count > 0 && rs->base != NULL) {
        XShapeCombineRectangles(dpy, rs->win, ShapeBounding, 0, 0, rs->base,
                                rs->base_count, ShapeSet, Unsorted);
    } else {
        window_full_rect(rs->win, &full);
        XShapeCombineRectangles(dpy, rs->win, ShapeBounding, 0, 0, &full, 1,
                                ShapeSet, Unsorted);
    }
    if (rs->base_input_count > 0 && rs->base_input != NULL) {
        XShapeCombineRectangles(dpy, rs->win, ShapeInput, 0, 0, rs->base_input,
                                rs->base_input_count, ShapeSet, Unsorted);
    } else {
        window_full_rect(rs->win, &full);
        XShapeCombineRectangles(dpy, rs->win, ShapeInput, 0, 0, &full, 1,
                                ShapeSet, Unsorted);
    }
    XFlush(dpy);
}

static void ring_shape_drop(Target *t, int restore)
{
    RingShape *rs = &t->ring_shape;

    if (restore) {
        ring_shape_restore(t);
    }
    if (rs->base != NULL) {
        XFree(rs->base);
    }
    if (rs->base_input != NULL) {
        XFree(rs->base_input);
    }
    if (rs->applied != NULL) {
        XFree(rs->applied);
    }
    memset(rs, 0, sizeof(*rs));
}

static void ring_shape_capture(Target *t)
{
    RingShape *rs = &t->ring_shape;
    XRectangle *rects;
    XRectangle *input_rects;
    XRectangle full;
    int count = 0;
    int input_count = 0;
    int ordering = 0;

    ring_shape_drop(t, 0);
    if (t->ring == None) {
        return;
    }
    rs->win = t->ring;
    rects =
        XShapeGetRectangles(dpy, t->ring, ShapeBounding, &count, &ordering);
    if (rects == NULL) {
        return;
    }
    window_full_rect(t->ring, &full);
    if (count <= 0 || (count == 1 && rect_lists_equal(rects, 1, &full, 1))) {
        /* Unshaped window: an implicit full rectangle is the base. */
        XFree(rects);
        rs->captured = 1;
        return;
    }
    rs->base = rects;
    rs->base_count = count;
    input_rects = XShapeGetRectangles(dpy, t->ring, ShapeInput, &input_count,
                                      &ordering);
    if (input_rects != NULL && input_count > 0) {
        rs->base_input = input_rects;
        rs->base_input_count = input_count;
    } else if (input_rects != NULL) {
        XFree(input_rects);
    }
    rs->captured = 1;
}

/*
 * Build the owner's own shape in the shadow window's coordinate space.  The
 * shadow's opaque inner band sits under the owner; subtracting the rounded
 * shape instead of its bounding rectangle hides that band along the straight
 * edges while leaving the original soft corner transition intact.
 */
static XserverRegion owner_shape_region(Window win, int dx, int dy, int w,
                                        int h)
{
    XRectangle *rects;
    XRectangle hole;
    XserverRegion region;
    int count = 0;
    int ordering = 0;
    int i;

    rects = XShapeGetRectangles(dpy, win, ShapeBounding, &count, &ordering);
    if (rects != NULL && count > 0) {
        for (i = 0; i < count; i++) {
            rects[i].x = (short)(rects[i].x + dx);
            rects[i].y = (short)(rects[i].y + dy);
        }
        region = XFixesCreateRegion(dpy, rects, count);
        XFree(rects);
        return region;
    }
    if (rects != NULL) {
        XFree(rects);
    }

    hole.x = (short)dx;
    hole.y = (short)dy;
    hole.width = (unsigned short)w;
    hole.height = (unsigned short)h;
    return XFixesCreateRegion(dpy, &hole, 1);
}

/*
 * Keep only the part of the shadow that lies outside the owner window.  The
 * inner band of the shadow is painted opaque by Wine, so clipping it away is
 * what removes the white frame around dialogs.
 */
static void ring_apply_shape(Target *t)
{
    RingShape *rs = &t->ring_shape;
    Rect m, r;
    XRectangle full;
    XserverRegion region, cut;
    XRectangle *want;
    int nwant = 0;

    if (!t->valid || !rs->captured || rs->win == None) {
        return;
    }
    if (!win_rect(t->main, &m) || !win_rect(rs->win, &r)) {
        return;
    }
    if (rs->base_count > 0 && rs->base != NULL) {
        region = XFixesCreateRegion(dpy, rs->base, rs->base_count);
    } else {
        window_full_rect(rs->win, &full);
        region = XFixesCreateRegion(dpy, &full, 1);
    }

    if (m.x + m.w <= r.x || m.y + m.h <= r.y || m.x >= r.x + r.w ||
        m.y >= r.y + r.h) {
        XFixesDestroyRegion(dpy, region);
        return;
    }
    cut = owner_shape_region(t->main, m.x - r.x, m.y - r.y, m.w, m.h);
    XFixesSubtractRegion(dpy, region, region, cut);
    XFixesDestroyRegion(dpy, cut);

    /*
     * The mail window's shadow is override-redirect and sits above ordinary
     * windows, so its opaque band still shows through whatever covers the
     * window.  Drop the parts of the shadow that are hidden behind windows
     * stacked above the owner as well.
     */
    XRectangle holes[MAX_HOLES];
    int nholes = collect_occluders(t, t->ring_frame, &r, holes, MAX_HOLES);
    if (nholes > 0) {
        cut = XFixesCreateRegion(dpy, holes, nholes);
        XFixesSubtractRegion(dpy, region, region, cut);
        XFixesDestroyRegion(dpy, cut);
    }

    want = XFixesFetchRegion(dpy, region, &nwant);
    if (want != NULL && !window_shape_is(rs->win, want, nwant)) {
        XFixesSetWindowShapeRegion(dpy, rs->win, ShapeBounding, 0, 0, region);
        XFixesSetWindowShapeRegion(dpy, rs->win, ShapeInput, 0, 0, region);
        XFlush(dpy);
        rs->applied_ms = now_ms();
    }
    if (want != NULL) {
        if (rs->applied != NULL) {
            XFree(rs->applied);
        }
        rs->applied = want;
        rs->applied_count = nwant;
    }
    XFixesDestroyRegion(dpy, region);
}

static int ring_shape_intact(Target *t)
{
    RingShape *rs = &t->ring_shape;

    if (!rs->captured || rs->applied == NULL) {
        return 0;
    }
    return bounding_shape_is(rs->win, rs->applied, rs->applied_count);
}

static RingShape *find_ring_shape(Window win)
{
    for (int i = 0; i < MAX_TARGETS; i++) {
        RingShape *rs = &targets[i].ring_shape;

        if (targets[i].valid && rs->captured && rs->win == win) {
            return rs;
        }
    }
    return NULL;
}

/*
 * Move/resize the frame that hosts the shadow window so that the shadow
 * client stays at (main - pad, main - pad) with size (main + 2 * pad).
 */
static void glue_ring(Target *t)
{
    Rect m, client, frame;
    int dx, dy, dw, dh;

    if (t->ring == None || t->ring_frame == None) {
        return;
    }
    if (!win_rect(t->main, &m) || !win_rect(t->ring, &client) ||
        !win_rect(t->ring_frame, &frame)) {
        return;
    }
    dx = (m.x - t->ring_pad) - client.x;
    dy = (m.y - t->ring_pad) - client.y;
    dw = (m.w + 2 * t->ring_pad) - client.w;
    dh = (m.h + 2 * t->ring_pad) - client.h;
    if (dx == 0 && dy == 0 && dw == 0 && dh == 0) {
        t->glue_stuck = 0;
        return;
    }
    /*
     * When the shadow shares its frame with the main window, moving that frame
     * shifts both, so the delta never changes.  A window manager that clamps
     * the frame (against a panel, say) then turns this into an endless move
     * loop at the glue interval.  Give up while the numbers stay the same; a
     * real move of the window changes them and the glue resumes by itself.
     */
    if (dx == t->glue_dx && dy == t->glue_dy && dw == t->glue_dw &&
        dh == t->glue_dh) {
        if (t->glue_stuck >= 3) {
            return;
        }
        t->glue_stuck++;
    } else {
        t->glue_stuck = 0;
        t->glue_dx = dx;
        t->glue_dy = dy;
        t->glue_dw = dw;
        t->glue_dh = dh;
    }
    XMoveResizeWindow(dpy, t->ring_frame, frame.x + dx, frame.y + dy,
                      (unsigned)(frame.w + dw), (unsigned)(frame.h + dh));
    XFlush(dpy);
    logmsg("ring %#lx glued (dx=%d dy=%d dw=%d dh=%d)", t->ring, dx, dy, dw,
           dh);
}

/*
 * Keep the transparent title bar strip on the top edge of the main window even
 * while Wine is still one pointer event behind.  Only strip-shaped overlays
 * (as wide as the window, at its top edge) are moved; small widgets are
 * positioned by WeCom itself.
 *
 * A strip that has not caught up with a resize is still a strip: it starts at
 * the window's top left corner and spans most of its width.  Those are glued
 * to the new width as well, which is what brings the "-[]X" buttons back
 * after unmaximizing, when Wine still draws them at the old right edge.
 */
static void glue_overlays(Target *t)
{
    Rect m;

    if (!t->valid || !win_rect(t->main, &m)) {
        return;
    }
    for (int i = 0; i < t->overlay_count; i++) {
        Overlay *o = &t->overlays[i];
        Rect s;
        int dx, dy;

        if (!win_rect(o->win, &s)) {
            continue;
        }
        if (s.h > STRIP_MAX_HEIGHT) {
            continue;
        }
        if (abs(s.w - m.w) > 64 &&
            !(s.x == m.x && s.y == m.y && s.w >= m.w / 2)) {
            continue;
        }
        dx = m.x - s.x;
        dy = m.y - s.y;
        if (dx > -2 && dx < 2 && dy > -2 && dy < 2 && s.w == m.w) {
            continue;
        }
        XMoveResizeWindow(dpy, o->win, m.x, m.y, (unsigned)m.w,
                          (unsigned)s.h);
        XFlush(dpy);
        logmsg("overlay %#lx glued (dx=%d dy=%d w=%d)", o->win, dx, dy, m.w);
    }
}

/*
 * A known shadow window stays tracked even when it lags behind the main
 * window, as long as it is still the same kind of window.  The real position
 * and size are corrected later by glue_ring().
 *
 * The old version demanded that the shadow's size was already within 64 px of
 * the main window's.  After a maximize/unmaximize round trip the shadow can be
 * a whole layout strip off, and dropping it there meant it was never glued
 * back - so the white border trailed the window again and the shadow's owner
 * stayed untracked until Wine happened to redraw it.  Only identity and a
 * rough position check remain; anything else is glue's job.
 */
static int ring_still_valid(Target *t, const Rect *m)
{
    XWindowAttributes attr;
    Rect r;

    if (t->ring == None || !XGetWindowAttributes(dpy, t->ring, &attr)) {
        return 0;
    }
    if (attr.map_state != IsViewable || attr.depth != 32) {
        return 0;
    }
    if (!win_is_wecom(t->ring)) {
        return 0;
    }
    if (t->pid != 0 && win_pid(t->ring) != t->pid) {
        return 0;
    }
    if (!win_rect(t->ring, &r)) {
        return 0;
    }
    if (r.x >= m->x + m->w + 512 || r.y >= m->y + m->h + 512 ||
        m->x >= r.x + r.w + 512 || m->y >= r.y + r.h + 512) {
        return 0;
    }
    t->ring_frame = win_top_level(t->ring);
    return 1;
}

static int ring_candidate_ok(Window cand, Target *t, const Rect *m, int *pad_out)
{
    XWindowAttributes attr;
    Window trans = None;
    Rect r;
    int pad;

    if (cand == None || cand == t->main || !XGetWindowAttributes(dpy, cand, &attr)) {
        return 0;
    }
    if (attr.map_state != IsViewable || attr.depth != 32) {
        return 0;
    }
    if (!win_is_wecom(cand)) {
        return 0;
    }
    if (t->pid != 0 && win_pid(cand) != t->pid) {
        return 0;
    }
    /*
     * wxwork's shadow is a managed window with WM_TRANSIENT_FOR pointing at
     * its owner.  WeMail's shadow is override-redirect and has no transient
     * hint, so only require the hint when the candidate is managed.
     */
    if (XGetTransientForHint(dpy, cand, &trans)) {
        if (trans != t->main && trans != t->frame) {
            return 0;
        }
    } else if (!attr.override_redirect) {
        return 0;
    }
    if (!win_rect(cand, &r)) {
        return 0;
    }
    if ((r.w - m->w) % 2 != 0 || (r.h - m->h) % 2 != 0) {
        return 0;
    }
    pad = (r.w - m->w) / 2;
    if (pad < RING_PAD_MIN || pad > RING_PAD_MAX) {
        return 0;
    }
    if (r.h - m->h != 2 * pad) {
        return 0;
    }
    *pad_out = pad;
    return 1;
}

static void consider_ring_candidate(Target *t, Window cand, const Rect *m,
                                    Window *best, int *best_pad, int *best_dist)
{
    Rect r;
    int pad = 0;
    int dist;

    if (!ring_candidate_ok(cand, t, m, &pad)) {
        return;
    }
    if (!win_rect(cand, &r)) {
        return;
    }
    dist = abs(r.x - (m->x - pad)) + abs(r.y - (m->y - pad)) +
           abs(r.w - (m->w + 2 * pad)) + abs(r.h - (m->h + 2 * pad));
    if (*best == None || dist < *best_dist) {
        *best = cand;
        *best_pad = pad;
        *best_dist = dist;
    }
}

/*
 * Managed clients are covered by _NET_CLIENT_LIST, but WeMail's shadow is
 * override-redirect and therefore never appears there.  Walk the root tree as
 * well so that both kinds of shadow can be found.
 */
static void consider_ring_candidates_in_tree(Target *t, const Rect *m,
                                             Window *best, int *best_pad,
                                             int *best_dist)
{
    Window root_ret, parent_ret, *top = NULL;
    unsigned int ntop = 0;

    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &top, &ntop)) {
        return;
    }
    for (unsigned int i = 0; i < ntop; i++) {
        Window root_ret2, parent_ret2, *children = NULL;
        unsigned int nchildren = 0;

        consider_ring_candidate(t, top[i], m, best, best_pad, best_dist);
        if (!XQueryTree(dpy, top[i], &root_ret2, &parent_ret2, &children,
                        &nchildren)) {
            continue;
        }
        for (unsigned int c = 0; c < nchildren; c++) {
            consider_ring_candidate(t, children[c], m, best, best_pad,
                                    best_dist);
        }
        if (children != NULL) {
            XFree(children);
        }
    }
    if (top != NULL) {
        XFree(top);
    }
}

static void find_ring(Target *t, const Rect *m)
{
    Atom type;
    int format;
    unsigned long count = 0, after = 0;
    unsigned char *data = NULL;
    Window best = None;
    int best_pad = 0;
    int best_dist = 0;

    t->ring = None;
    t->ring_frame = None;

    if (XGetWindowProperty(dpy, root, atom_client_list, 0, 8192, False,
                           XA_WINDOW, &type, &format, &count, &after,
                           &data) == Success &&
        data != NULL) {
        Window *clients = (Window *)data;
        for (unsigned long i = 0; i < count; i++) {
            consider_ring_candidate(t, clients[i], m, &best, &best_pad,
                                    &best_dist);
        }
        XFree(data);
    }
    consider_ring_candidates_in_tree(t, m, &best, &best_pad, &best_dist);
    t->ring = best;
    if (best != None) {
        t->ring_frame = win_top_level(best);
        t->ring_pad = best_pad;
    }
}

static int overlay_is_popup(Window win)
{
    return win_has_window_type(win, atom_wm_window_type_popup_menu) ||
           win_has_window_type(win, atom_wm_window_type_dropdown_menu) ||
           win_has_window_type(win, atom_wm_window_type_tooltip);
}

/*
 * Small ARGB windows of the same process are WeCom's own chrome (title bar
 * strip, "more" button, ...).  Older Wine versions make them override-redirect;
 * newer ones wrap the title bar shadow in a managed dialog with
 * WM_TRANSIENT_FOR pointing at the main window.  Both forms are matched to the
 * window whose top right corner they sit closest to.
 */
static int overlay_still_valid(Window win, Target *t)
{
    XWindowAttributes attr;
    Window trans = None;

    if (!XGetWindowAttributes(dpy, win, &attr)) {
        return 0;
    }
    if (attr.map_state != IsViewable) {
        return 0;
    }
    if (attr.depth != 32 || win == t->main || win == t->ring) {
        return 0;
    }
    if (!win_is_wecom(win)) {
        return 0;
    }
    if (t->pid != 0 && win_pid(win) != t->pid) {
        return 0;
    }
    if (overlay_is_popup(win)) {
        return 0;
    }
    if (!attr.override_redirect) {
        if (!XGetTransientForHint(dpy, win, &trans)) {
            return 0;
        }
        if (trans != t->main && trans != t->frame) {
            return 0;
        }
    }
    return 1;
}

static int overlay_candidate_ok(Window win, Target *t, const Rect *m,
                                const Rect *r)
{
    XWindowAttributes attr;

    if (!overlay_still_valid(win, t)) {
        return 0;
    }
    if (r->w * r->h < OVERLAY_MIN_AREA || r->h > OVERLAY_MAX_HEIGHT) {
        return 0;
    }
    if (r->x < m->x - OVERLAY_SLACK || r->y < m->y - OVERLAY_SLACK) {
        return 0;
    }
    if (r->x + r->w > m->x + m->w + OVERLAY_SLACK ||
        r->y + r->h > m->y + m->h + OVERLAY_SLACK) {
        return 0;
    }
    if (!XGetWindowAttributes(dpy, win, &attr)) {
        return 0;
    }
    if (!attr.override_redirect) {
        /*
         * Managed helper windows are only interesting as the full-width title
         * bar shadow strip.  Other WeCom dialogs are transient for the main
         * window too, so keep the geometry test strict.
         */
        if (r->h > OVERLAY_MAX_HEIGHT || r->w < m->w - OVERLAY_SLACK ||
            abs(r->y - m->y) > 2) {
            return 0;
        }
    }
    return 1;
}

/*
 * An overlay that is already attached stays attached, even when its geometry
 * still describes the window's previous size.  Matching by geometry instead
 * dropped the title bar strip the moment the main window shrank - the strip
 * hangs over the new right edge by the whole size difference - after which
 * nothing glued it back and the "-[]X" buttons stayed at the old, invisible
 * position.  Identity (same process, same window class, same override-redirect
 * strip) is what decides, and glue_overlays() corrects the geometry.
 */
static int overlay_keep_known(Window win, const Rect *r)
{
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        Rect m;

        if (!t->valid || !win_rect(t->main, &m)) {
            continue;
        }
        for (int j = 0; j < t->overlay_count; j++) {
            Overlay *o = &t->overlays[j];

            if (o->win != win) {
                continue;
            }
            if (!overlay_still_valid(win, t)) {
                return 0;
            }
            if (r->x >= m.x + m.w + 512 || r->y >= m.y + m.h + 512 ||
                m.x >= r->x + r->w + 512 || m.y >= r->y + r->h + 512) {
                return 0;
            }
            o->seen = 1;
            return 1;
        }
    }
    return 0;
}

static int overlay_capture_base(Overlay *o)
{
    int count = 0;
    int input_count = 0;
    int ordering = 0;
    XRectangle *rects;
    XRectangle *input_rects;
    Rect s;

    o->base = NULL;
    o->base_count = 0;
    o->base_input = NULL;
    o->base_input_count = 0;
    o->base_w = 0;
    o->base_h = 0;
    if (!win_rect(o->win, &s)) {
        return 0;
    }
    input_rects = XShapeGetRectangles(dpy, o->win, ShapeInput, &input_count,
                                      &ordering);
    if (input_rects != NULL && input_count > 0) {
        o->base_input = input_rects;
        o->base_input_count = input_count;
    } else if (input_rects != NULL) {
        XFree(input_rects);
    }
    rects = XShapeGetRectangles(dpy, o->win, ShapeBounding, &count, &ordering);
    if (rects == NULL || count <= 0) {
        if (rects != NULL) {
            XFree(rects);
        }
        return 1;
    }
    if (count == 1 && rects[0].x == 0 && rects[0].y == 0 &&
        rects[0].width == (unsigned short)s.w &&
        rects[0].height == (unsigned short)s.h) {
        /* Unshaped window: an implicit full rectangle is enough. */
        XFree(rects);
        return 1;
    }
    o->base = rects;
    o->base_count = count;
    o->base_w = s.w;
    o->base_h = s.h;
    return 1;
}

static void overlay_free(Overlay *o)
{
    if (o->base != NULL) {
        XFree(o->base);
    }
    if (o->base_input != NULL) {
        XFree(o->base_input);
    }
    if (o->applied != NULL) {
        XFree(o->applied);
    }
    memset(o, 0, sizeof(*o));
}

static void classify(Target *t)
{
    Rect m;
    XWindowAttributes attr;
    Window keep_ring;
    int keep_pad;

    if (!win_rect(t->main, &m)) {
        return;
    }
    if (t->ring_pad == 0) {
        t->ring_pad = 18;
    }
    if (ring_still_valid(t, &m)) {
        if (t->ring_shape.win != t->ring || !t->ring_shape.captured) {
            ring_shape_drop(t, 0);
            ring_shape_capture(t);
        }
        return;
    }

    /*
     * WeCom unmaps the shadow while the window is maximized and may leave it
     * at the old size for a moment afterwards.  Forget the reference only when
     * the window is really gone; otherwise keep it and let glue_ring() pull it
     * to the current geometry once it is back.
     */
    keep_ring = t->ring;
    keep_pad = t->ring_pad;
    find_ring(t, &m);
    if (t->ring != None) {
        ring_shape_capture(t);
        return;
    }
    if (keep_ring != None &&
        XGetWindowAttributes(dpy, keep_ring, &attr)) {
        t->ring = keep_ring;
        t->ring_frame = win_top_level(keep_ring);
        t->ring_pad = keep_pad;
        return;
    }
    ring_shape_drop(t, 1);
    t->ring_frame = None;
}

static void consider_overlay_candidate(Window win, unsigned int stack_index,
                                       Rect *mains, int *frames, int *changed)
{
    Rect r;
    int best = -1;
    int best_score = 0;

    if (!win_rect(win, &r)) {
        return;
    }
    if (overlay_keep_known(win, &r)) {
        return;
    }
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        int score;

        if (!t->valid || frames[i] < 0 || (int)stack_index < frames[i]) {
            continue;
        }
        if (!overlay_candidate_ok(win, t, &mains[i], &r)) {
            continue;
        }
        score = abs((r.x + r.w) - (mains[i].x + mains[i].w)) +
                abs(r.y - mains[i].y);
        if (best < 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    if (best < 0) {
        return;
    }

    Target *t = &targets[best];
    int slot = -1;

    for (int j = 0; j < t->overlay_count; j++) {
        if (t->overlays[j].win == win) {
            t->overlays[j].seen = 1;
            slot = j;
            break;
        }
    }
    if (slot < 0 && t->overlay_count < MAX_OVERLAYS) {
        Overlay *o = &t->overlays[t->overlay_count];
        XWindowAttributes attr;

        memset(o, 0, sizeof(*o));
        o->win = win;
        o->frame = win_top_level(win);
        if (!XGetWindowAttributes(dpy, win, &attr)) {
            memset(o, 0, sizeof(*o));
            return;
        }
        o->managed = !attr.override_redirect;
        int full_width_top = r.h <= OVERLAY_MAX_HEIGHT &&
                             r.w >= mains[best].w - OVERLAY_SLACK &&
                             abs(r.y - mains[best].y) <= 2;
        o->click_through =
            full_width_top &&
            (o->managed || overlay_shape_exceeds_window(win, &r));
        if (overlay_capture_base(o)) {
            o->seen = 1;
            t->overlay_count++;
            *changed = 1;
            XShapeSelectInput(dpy, win, ShapeNotifyMask);
            logmsg("overlay %#lx of main %#lx (base rects=%d managed=%d "
                   "click_through=%d)",
                   win, t->main, o->base_count, o->managed, o->click_through);
        } else {
            memset(o, 0, sizeof(*o));
        }
    }
}

/*
 * Attach every chrome overlay window to the target it belongs to.  Called on
 * each scan; overlays that disappeared are dropped and the ones that are still
 * there keep their captured base shape.
 */
static void assign_overlays(void)
{
    Window root_ret, parent_ret, *top = NULL;
    unsigned int ntop = 0;
    Rect mains[MAX_TARGETS];
    int frames[MAX_TARGETS];
    int changed = 0;

    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &top, &ntop)) {
        return;
    }
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];

        frames[i] = -1;
        for (int j = 0; j < t->overlay_count; j++) {
            t->overlays[j].seen = 0;
        }
        if (!t->valid || !win_rect(t->main, &mains[i])) {
            continue;
        }
        for (unsigned int k = 0; k < ntop; k++) {
            if (top[k] == t->frame) {
                frames[i] = (int)k;
                break;
            }
        }
    }

    for (unsigned int k = 0; k < ntop; k++) {
        consider_overlay_candidate(top[k], k, mains, frames, &changed);
    }

    /*
     * Override-redirect helpers are root children and were covered above.
     * Managed helpers (newer Wine wraps the title bar shadow in a dialog) are
     * clients inside a WM frame, so walk _NET_CLIENT_LIST instead of querying
     * the children of every root window on the screen.
     */
    Atom type;
    int format;
    unsigned long count = 0, after = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, root, atom_client_list, 0, 8192, False,
                           XA_WINDOW, &type, &format, &count, &after,
                           &data) == Success &&
        data != NULL) {
        Window *clients = (Window *)data;

        for (unsigned long i = 0; i < count; i++) {
            Window frame = win_top_level(clients[i]);

            for (unsigned int k = 0; k < ntop; k++) {
                if (top[k] == frame) {
                    consider_overlay_candidate(clients[i], k, mains, frames,
                                               &changed);
                    break;
                }
            }
        }
        XFree(data);
    }

    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        int kept = 0;

        for (int j = 0; j < t->overlay_count; j++) {
            if (!t->overlays[j].seen) {
                logmsg("drop overlay %#lx of main %#lx", t->overlays[j].win,
                       t->main);
                overlay_restore(t, &t->overlays[j]);
                overlay_free(&t->overlays[j]);
                changed = 1;
                continue;
            }
            if (kept != j) {
                t->overlays[kept] = t->overlays[j];
            }
            kept++;
        }
        for (int j = kept; j < MAX_OVERLAYS; j++) {
            memset(&t->overlays[j], 0, sizeof(t->overlays[j]));
        }
        t->overlay_count = kept;
    }
    if (changed) {
        shape_dirty = 1;
    }
    if (top != NULL) {
        XFree(top);
    }
}

static void collect_from_tree(Window parent, int depth, Target *found,
                              int max_found, int *nfound)
{
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren = 0;

    if (*nfound >= max_found) {
        return;
    }
    if (!XQueryTree(dpy, parent, &root_ret, &parent_ret, &children,
                    &nchildren)) {
        return;
    }
    for (unsigned int i = 0; i < nchildren && *nfound < max_found; i++) {
        Window win = children[i];
        XWindowAttributes attr;

        if (!XGetWindowAttributes(dpy, win, &attr)) {
            continue;
        }
        if (attr.map_state == IsViewable && attr.depth == 24 &&
            win_is_wecom(win) &&
            win_has_window_type(win, atom_wm_window_type_normal)) {
            Target *t = &found[(*nfound)++];
            t->main = win;
            t->frame = win_top_level(win);
            t->pid = win_pid(win);
            t->valid = 1;
            continue;
        }
        if (depth < 2) {
            collect_from_tree(win, depth + 1, found, max_found, nfound);
        }
    }
    if (children != NULL) {
        XFree(children);
    }
}

static int collect_targets(Target *found, int max_found)
{
    Atom type;
    int format;
    unsigned long count = 0, after = 0;
    unsigned char *data = NULL;
    int nfound = 0;

    if (XGetWindowProperty(dpy, root, atom_client_list, 0, 8192, False,
                           XA_WINDOW, &type, &format, &count, &after,
                           &data) != Success ||
        data == NULL) {
        collect_from_tree(root, 0, found, max_found, &nfound);
        return nfound;
    }
    Window *clients = (Window *)data;
    for (unsigned long i = 0; i < count && nfound < max_found; i++) {
        Window win = clients[i];
        XWindowAttributes attr;

        if (!XGetWindowAttributes(dpy, win, &attr)) {
            continue;
        }
        if (attr.map_state != IsViewable || attr.depth != 24) {
            continue;
        }
        if (!win_is_wecom(win)) {
            continue;
        }
        if (!win_has_window_type(win, atom_wm_window_type_normal)) {
            continue;
        }
        Target *t = &found[nfound++];
        t->main = win;
        t->frame = win_top_level(win);
        t->pid = win_pid(win);
        t->valid = 1;
    }
    XFree(data);
    return nfound;
}

static void forget_target(Target *t)
{
    if (t->valid) {
        logmsg("forget main %#lx", t->main);
    }
    ring_shape_drop(t, 1);
    for (int i = 0; i < t->overlay_count; i++) {
        overlay_restore(t, &t->overlays[i]);
        overlay_free(&t->overlays[i]);
    }
    memset(t, 0, sizeof(*t));
}

static void scan(void)
{
    Target found[MAX_TARGETS];
    int nfound;

    memset(found, 0, sizeof(found));
    nfound = collect_targets(found, MAX_TARGETS);

    for (int i = 0; i < MAX_TARGETS; i++) {
        if (!targets[i].valid) {
            continue;
        }
        int seen = 0;
        for (int j = 0; j < nfound; j++) {
            if (found[j].main == targets[i].main) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            forget_target(&targets[i]);
        }
    }

    for (int j = 0; j < nfound; j++) {
        Target *slot = NULL;
        for (int i = 0; i < MAX_TARGETS; i++) {
            if (targets[i].valid && targets[i].main == found[j].main) {
                slot = &targets[i];
                break;
            }
        }
        if (slot == NULL) {
            for (int i = 0; i < MAX_TARGETS; i++) {
                if (!targets[i].valid) {
                    slot = &targets[i];
                    break;
                }
            }
        }
        if (slot == NULL) {
            break;
        }
        int was_valid = slot->valid;
        slot->main = found[j].main;
        slot->frame = found[j].frame;
        slot->pid = found[j].pid;
        slot->valid = 1;
        if (!was_valid) {
            hot_until = now_ms() + HOT_WINDOW_MS;
            slot->last_config_ms = now_ms();
        }
        classify(slot);

        XSelectInput(dpy, slot->frame,
                     StructureNotifyMask | SubstructureNotifyMask);
        XSelectInput(dpy, slot->main, StructureNotifyMask);
        logmsg("target main=%#lx frame=%#lx ring=%#lx pad=%d overlays=%d",
               slot->main, slot->frame, slot->ring, slot->ring_pad,
               slot->overlay_count);
    }
    assign_overlays();
}

static void reconcile_geometry(Target *t)
{
    if (!t->valid) {
        return;
    }
    glue_ring(t);
    glue_overlays(t);
}

static void reconcile_shape(Target *t)
{
    if (!t->valid) {
        return;
    }
    main_shape_reconcile(t);
    ring_apply_shape(t);
    for (int i = 0; i < t->overlay_count; i++) {
        overlay_apply_shape(t, &t->overlays[i]);
    }
}

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static int acquire_lock(void)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX];
    int fd;

    if (runtime != NULL && runtime[0] != '\0') {
        snprintf(path, sizeof(path), "%s/wecom-x11-fix.lock", runtime);
    } else {
        snprintf(path, sizeof(path), "/tmp/wecom-x11-fix-%lu.lock",
                 (unsigned long)getuid());
    }
    fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        return 1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return 0;
    }
    return 1;
}

static void print_scan_result(void)
{
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        Rect m = {0}, r = {0};

        if (!t->valid) {
            continue;
        }
        win_rect(t->main, &m);
        printf("main=%#lx frame=%#lx %dx%d+%d+%d\n", t->main, t->frame, m.w, m.h,
               m.x, m.y);
        if (win_rect(t->ring, &r)) {
            printf("  ring=%#lx frame=%#lx pad=%d %dx%d+%d+%d\n", t->ring,
                   t->ring_frame, t->ring_pad, r.w, r.h, r.x, r.y);
        } else {
            printf("  ring=none\n");
        }
        printf("  overlays=%d\n", t->overlay_count);
        for (int j = 0; j < t->overlay_count; j++) {
            Rect s = {0};

            if (win_rect(t->overlays[j].win, &s)) {
                printf("    overlay=%#lx %dx%d+%d+%d base=%d\n",
                       t->overlays[j].win, s.w, s.h, s.x, s.y,
                       t->overlays[j].base_count);
            }
        }
    }
}

static void print_holes(void)
{
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];

        if (!t->valid) {
            continue;
        }
        for (int k = 0; k < t->overlay_count; k++) {
            Rect s;
            XRectangle holes[MAX_HOLES];
            int n;

            if (!win_rect(t->overlays[k].win, &s)) {
                continue;
            }
            n = collect_occluders(t, t->overlays[k].frame, &s, holes,
                                  MAX_HOLES);
            printf("overlay=%#lx %dx%d+%d+%d holes=%d\n",
                   t->overlays[k].win, s.w, s.h, s.x, s.y, n);
            for (int j = 0; j < n; j++) {
                printf("  hole %d,%d %ux%u\n", holes[j].x, holes[j].y,
                       holes[j].width, holes[j].height);
            }
        }
    }
}

int main(int argc, char **argv)
{
    int scan_only = 0;
    int dry_run = 0;
    int dump_holes = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "--scan")) {
            scan_only = 1;
        } else if (!strcmp(argv[i], "--holes")) {
            dump_holes = 1;
        } else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--dry-run")) {
            dry_run = 1;
            verbose = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [-v] [--scan] [--holes] [--dry-run]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "wecom-x11-fix: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "wecom-x11-fix: cannot open display\n");
        return 1;
    }
    XSetErrorHandler(ignore_x_error);
    root = DefaultRootWindow(dpy);

    atom_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    atom_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    atom_wm_state_maximized_vert =
        XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    atom_wm_state_maximized_horz =
        XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    atom_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_wm_window_type_normal =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    atom_wm_window_type_popup_menu =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    atom_wm_window_type_dropdown_menu =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    atom_wm_window_type_tooltip =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    atom_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);

    int shape_event, shape_error;
    if (!XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
        fprintf(stderr, "wecom-x11-fix: XShape extension missing\n");
        return 1;
    }
    shape_event_base = shape_event;
    int fixes_event, fixes_error;
    if (!XFixesQueryExtension(dpy, &fixes_event, &fixes_error)) {
        fprintf(stderr, "wecom-x11-fix: XFixes extension missing\n");
        return 1;
    }

    if (scan_only || dry_run || dump_holes) {
        scan();
        if (dump_holes) {
            print_holes();
        } else {
            print_scan_result();
        }
        XCloseDisplay(dpy);
        return 0;
    }

    if (!acquire_lock()) {
        fprintf(stderr, "wecom-x11-fix: another instance is already running\n");
        XCloseDisplay(dpy);
        return 0;
    }

    XSelectInput(dpy, root,
                 SubstructureNotifyMask | PropertyChangeMask |
                     StructureNotifyMask);
    XSync(dpy, False);

    double next_scan = now_ms();
    double next_glue = now_ms();
    double next_shape = now_ms();
    double next_shape_watch = now_ms();

    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == MapNotify || ev.type == UnmapNotify ||
                ev.type == DestroyNotify || ev.type == CreateNotify) {
                double soon = now_ms() + SCAN_DEBOUNCE_MS;
                if (soon < next_scan) {
                    next_scan = soon;
                }
                shape_dirty = 1;
            } else if (ev.type == ConfigureNotify ||
                       ev.type == CirculateNotify) {
                shape_dirty = 1;
            } else if (ev.type == shape_event_base + ShapeNotify) {
                XShapeEvent *se = (XShapeEvent *)&ev;
                Overlay *o = find_overlay(se->window);
                RingShape *rs = find_ring_shape(se->window);

                /* Our own re-shape echoes back; only react to other writers. */
                if (o != NULL && now_ms() - o->applied_ms > SHAPE_ECHO_MS) {
                    logmsg("shape of overlay %#lx overwritten externally",
                           o->win);
                    shape_dirty = 1;
                }
                if (rs != NULL && now_ms() - rs->applied_ms > SHAPE_ECHO_MS) {
                    logmsg("shape of ring %#lx overwritten externally",
                           rs->win);
                    shape_dirty = 1;
                }
            }
            if (ev.type == ConfigureNotify) {
                for (int i = 0; i < MAX_TARGETS; i++) {
                    if (!targets[i].valid) {
                        continue;
                    }
                    if (ev.xconfigure.window == targets[i].frame ||
                        ev.xconfigure.window == targets[i].main) {
                        targets[i].last_config_ms = now_ms();
                        if (!dry_run) {
                            glue_ring(&targets[i]);
                            glue_overlays(&targets[i]);
                            hot_until = now_ms() + HOT_WINDOW_MS;
                        }
                    }
                }
            }
        }
        if (!running) {
            break;
        }

        double now = now_ms();
        if (now >= next_scan) {
            scan();
            next_scan = now + SCAN_INTERVAL_MS;
        }
        if (now >= next_glue) {
            for (int i = 0; i < MAX_TARGETS; i++) {
                if (!dry_run) {
                    reconcile_geometry(&targets[i]);
                }
            }
            next_glue =
                now + (now < hot_until ? GLUE_INTERVAL_MS
                                       : IDLE_GLUE_INTERVAL_MS);
        }
        if (now >= next_shape) {
            if (shape_dirty) {
                for (int i = 0; i < MAX_TARGETS; i++) {
                    if (!dry_run) {
                        reconcile_shape(&targets[i]);
                    }
                }
                shape_dirty = 0;
            }
            next_shape = now + SHAPE_INTERVAL_MS;
        }
        if (now >= next_shape_watch) {
            for (int i = 0; i < MAX_TARGETS; i++) {
                Target *t = &targets[i];

                if (!t->valid) {
                    continue;
                }
                main_shape_reconcile(t);
                for (int j = 0; j < t->overlay_count; j++) {
                    if (!overlay_shape_intact(&t->overlays[j])) {
                        shape_dirty = 1;
                        break;
                    }
                }
                if (!ring_shape_intact(t)) {
                    shape_dirty = 1;
                }
            }
            next_shape_watch = now + SHAPE_WATCHDOG_MS;
        }

        double wake = next_scan < next_glue ? next_scan : next_glue;
        if (next_shape < wake) {
            wake = next_shape;
        }
        double delta = wake - now;
        if (delta < 1.0) {
            delta = 1.0;
        }
        if (delta > 200.0) {
            delta = 200.0;
        }
        struct timeval tv;
        tv.tv_sec = (time_t)(delta / 1000.0);
        tv.tv_usec = (suseconds_t)((delta - tv.tv_sec * 1000.0) * 1000.0);

        int fd = ConnectionNumber(dpy);
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        int rc = select(fd + 1, &fds, NULL, NULL, &tv);
        if (rc < 0 && errno != EINTR) {
            break;
        }
    }

    for (int i = 0; i < MAX_TARGETS; i++) {
        if (targets[i].valid && !dry_run) {
            ring_shape_drop(&targets[i], 1);
            for (int j = 0; j < targets[i].overlay_count; j++) {
                overlay_restore(&targets[i], &targets[i].overlays[j]);
            }
        }
    }
    XSync(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
