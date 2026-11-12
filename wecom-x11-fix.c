/*
 * wecom-x11-fix - X11 workarounds for Deepin-Wine WeCom
 *
 * Two layout bugs are handled:
 *
 * 1. WeCom maps a transparent override-redirect helper window over the top
 *    edge of its main window (a client-side title bar strip) and raises it
 *    above every other window on the screen.  Because the strip has a full
 *    input shape, clicks aimed at whatever is really on top at that place are
 *    swallowed by WeCom.  The strip's input and bounding regions are trimmed
 *    by the areas that are covered by windows stacked above WeCom, so those
 *    clicks reach the window the user actually sees and the caption buttons
 *    the strip draws there stop bleeding through it, while the uncovered part
 *    of the strip keeps working as WeCom's title bar.
 *
 * 2. WeCom draws its drop shadow into a separate 32 bit window that is larger
 *    than the main window on every side.  During an interactive move or
 *    resize the shadow is reconfigured one pointer event late, which shows up
 *    as a white border trailing the window.  The shadow is glued to the main
 *    window as soon as the window manager reports a new geometry.
 *
 * Nothing is written to the Wine prefix, the application data or any
 * configuration file.  The program only adjusts X11 window geometry and
 * input shapes; it restores the strip input shape when it exits.
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
#include <sys/file.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define WECOM_CLASS "com.qq.weixin.work.deepin"
#define MAX_TARGETS 8
#define RING_PAD_MIN 2
#define RING_PAD_MAX 40
#define STRIP_MAX_HEIGHT 64
#define MAX_HOLES 64
#define SCAN_INTERVAL_MS 10000.0
#define SCAN_DEBOUNCE_MS 250.0
#define GLUE_INTERVAL_MS 50.0
#define IDLE_GLUE_INTERVAL_MS 250.0
#define HOT_WINDOW_MS 600.0
#define SHAPE_INTERVAL_MS 30.0

typedef struct {
    int x, y, w, h;
} Rect;

typedef struct {
    Window main;
    Window frame;
    Window ring;
    Window ring_frame;
    Window strip;
    int ring_pad;
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
static Atom atom_wm_window_type;
static Atom atom_wm_window_type_normal;
static Atom atom_client_list;

static Target targets[MAX_TARGETS];

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
        match = strcmp(hint.res_class, WECOM_CLASS) == 0;
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

/*
 * Fill a hole list with the parts of the strip rectangle that are covered by
 * windows stacked above the WeCom frame.  Coordinates are strip-local.
 */
static int collect_occluders(Target *t, const Rect *strip, XRectangle *holes,
                             int max_holes)
{
    Window root_ret, parent_ret, *top = NULL;
    unsigned int ntop = 0;
    int above_frame = 0;
    int n = 0;

    if (t->frame == None) {
        return 0;
    }
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &top, &ntop)) {
        return 0;
    }
    for (unsigned int i = 0; i < ntop; i++) {
        Window win = top[i];

        if (win == t->frame) {
            above_frame = 1;
            continue;
        }
        if (!above_frame) {
            continue;
        }
        if (win == t->strip || win == t->ring_frame) {
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
        int x0 = r.x > strip->x ? r.x : strip->x;
        int y0 = r.y > strip->y ? r.y : strip->y;
        int x1 = (r.x + r.w) < (strip->x + strip->w)
                     ? (r.x + r.w)
                     : (strip->x + strip->w);
        int y1 = (r.y + r.h) < (strip->y + strip->h)
                     ? (r.y + r.h)
                     : (strip->y + strip->h);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        if (n >= max_holes) {
            break;
        }
        holes[n].x = (short)(x0 - strip->x);
        holes[n].y = (short)(y0 - strip->y);
        holes[n].width = (unsigned short)(x1 - x0);
        holes[n].height = (unsigned short)(y1 - y0);
        n++;
    }
    if (top != NULL) {
        XFree(top);
    }
    return n;
}

static void strip_apply_shape(Target *t)
{
    Rect s;
    XRectangle full;
    XRectangle holes[MAX_HOLES];
    int nholes;
    XserverRegion region;

    if (t->strip == None || !win_rect(t->strip, &s)) {
        return;
    }
    full.x = 0;
    full.y = 0;
    full.width = (unsigned short)s.w;
    full.height = (unsigned short)s.h;

    nholes = collect_occluders(t, &s, holes, MAX_HOLES);
    region = XFixesCreateRegion(dpy, &full, 1);
    if (nholes > 0) {
        XserverRegion covered = XFixesCreateRegion(dpy, holes, nholes);
        XFixesSubtractRegion(dpy, region, region, covered);
        XFixesDestroyRegion(dpy, covered);
    }
    XFixesSetWindowShapeRegion(dpy, t->strip, ShapeBounding, 0, 0, region);
    XFixesSetWindowShapeRegion(dpy, t->strip, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(dpy, region);
    XFlush(dpy);
}

static void strip_restore_full(Target *t)
{
    Rect s;
    XRectangle full;
    XserverRegion region;

    if (t->strip == None || !win_rect(t->strip, &s)) {
        return;
    }
    full.x = 0;
    full.y = 0;
    full.width = (unsigned short)s.w;
    full.height = (unsigned short)s.h;
    region = XFixesCreateRegion(dpy, &full, 1);
    XFixesSetWindowShapeRegion(dpy, t->strip, ShapeBounding, 0, 0, region);
    XFixesSetWindowShapeRegion(dpy, t->strip, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(dpy, region);
    XFlush(dpy);
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
        return;
    }
    XMoveResizeWindow(dpy, t->ring_frame, frame.x + dx, frame.y + dy,
                      (unsigned)(frame.w + dw), (unsigned)(frame.h + dh));
    XFlush(dpy);
    logmsg("ring %#lx glued (dx=%d dy=%d dw=%d dh=%d)", t->ring, dx, dy, dw,
           dh);
}

/*
 * Keep the transparent strip on the top edge of the main window even while
 * Wine is still one pointer event behind.
 */
static void glue_strip(Target *t)
{
    Rect m, s;
    int dx, dy;

    if (t->strip == None) {
        return;
    }
    if (!win_rect(t->main, &m) || !win_rect(t->strip, &s)) {
        return;
    }
    dx = m.x - s.x;
    dy = m.y - s.y;
    if (dx > -2 && dx < 2 && dy > -2 && dy < 2 && s.w == m.w) {
        return;
    }
    XMoveResizeWindow(dpy, t->strip, m.x, m.y, (unsigned)m.w,
                      (unsigned)s.h);
    XFlush(dpy);
    logmsg("strip %#lx glued (dx=%d dy=%d w=%d)", t->strip, dx, dy, m.w);
}

/*
 * A known shadow window stays tracked even when it lags behind the main
 * window, as long as it is still the same kind of window.  The real position
 * is corrected later by glue_ring().
 */
static int ring_still_valid(Target *t, const Rect *m)
{
    XWindowAttributes attr;
    Rect r;

    if (t->ring == None || !XGetWindowAttributes(dpy, t->ring, &attr)) {
        return 0;
    }
    if (attr.map_state != IsViewable || attr.depth != 32 ||
        attr.override_redirect) {
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
    if (abs(r.w - (m->w + 2 * t->ring_pad)) > 64 ||
        abs(r.h - (m->h + 2 * t->ring_pad)) > 64) {
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
    if (attr.map_state != IsViewable || attr.depth != 32 ||
        attr.override_redirect) {
        return 0;
    }
    if (!win_is_wecom(cand)) {
        return 0;
    }
    if (t->pid != 0 && win_pid(cand) != t->pid) {
        return 0;
    }
    if (!XGetTransientForHint(dpy, cand, &trans)) {
        return 0;
    }
    if (trans != t->main && trans != t->frame) {
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
    } else {
        Window root_ret, parent_ret, *top = NULL;
        unsigned int ntop = 0;
        if (XQueryTree(dpy, root, &root_ret, &parent_ret, &top, &ntop)) {
            for (unsigned int i = 0; i < ntop; i++) {
                Window root_ret2, parent_ret2, *children = NULL;
                unsigned int nchildren = 0;
                consider_ring_candidate(t, top[i], m, &best, &best_pad,
                                        &best_dist);
                if (!XQueryTree(dpy, top[i], &root_ret2, &parent_ret2,
                                &children, &nchildren)) {
                    continue;
                }
                for (unsigned int c = 0; c < nchildren; c++) {
                    consider_ring_candidate(t, children[c], m, &best,
                                            &best_pad, &best_dist);
                }
                if (children != NULL) {
                    XFree(children);
                }
            }
            if (top != NULL) {
                XFree(top);
            }
        }
    }
    t->ring = best;
    if (best != None) {
        t->ring_frame = win_top_level(best);
        t->ring_pad = best_pad;
    }
}

static int strip_still_valid(Target *t, const Rect *m)
{
    XWindowAttributes attr;
    Rect r;

    if (t->strip == None || !XGetWindowAttributes(dpy, t->strip, &attr)) {
        return 0;
    }
    if (!attr.override_redirect || attr.map_state != IsViewable) {
        return 0;
    }
    if (!win_is_wecom(t->strip)) {
        return 0;
    }
    if (t->pid != 0 && win_pid(t->strip) != t->pid) {
        return 0;
    }
    if (!win_rect(t->strip, &r)) {
        return 0;
    }
    if (r.h > STRIP_MAX_HEIGHT) {
        return 0;
    }
    if (abs(r.x - m->x) > 200 || abs(r.y - m->y) > 200) {
        return 0;
    }
    if (abs(r.w - m->w) > 64) {
        return 0;
    }
    return 1;
}

static void find_strip(Target *t, const Rect *m)
{
    Window root_ret, parent_ret, *top = NULL;
    unsigned int ntop = 0;

    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &top, &ntop)) {
        return;
    }
    for (unsigned int i = 0; i < ntop; i++) {
        Window win = top[i];
        XWindowAttributes attr;
        Rect r;

        if (!XGetWindowAttributes(dpy, win, &attr)) {
            continue;
        }
        if (!attr.override_redirect || attr.map_state != IsViewable) {
            continue;
        }
        if (!win_is_wecom(win)) {
            continue;
        }
        if (t->pid != 0 && win_pid(win) != t->pid) {
            continue;
        }
        if (!win_rect(win, &r)) {
            continue;
        }
        if (r.h > STRIP_MAX_HEIGHT) {
            continue;
        }
        if (abs(r.x - m->x) > 200 || abs(r.y - m->y) > 200) {
            continue;
        }
        if (abs(r.w - m->w) > 64) {
            continue;
        }
        t->strip = win;
        if (top != NULL) {
            XFree(top);
        }
        return;
    }
    if (top != NULL) {
        XFree(top);
    }
}

static void classify(Target *t)
{
    Rect m;

    if (!win_rect(t->main, &m)) {
        return;
    }
    if (t->ring_pad == 0) {
        t->ring_pad = 18;
    }
    if (!ring_still_valid(t, &m)) {
        find_ring(t, &m);
    }
    if (!strip_still_valid(t, &m)) {
        t->strip = None;
        find_strip(t, &m);
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
            strip_restore_full(&targets[i]);
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
        }
        classify(slot);

        XSelectInput(dpy, slot->frame,
                     StructureNotifyMask | SubstructureNotifyMask);
        XSelectInput(dpy, slot->main, StructureNotifyMask);
        logmsg("target main=%#lx frame=%#lx ring=%#lx pad=%d strip=%#lx",
               slot->main, slot->frame, slot->ring, slot->ring_pad,
               slot->strip);
    }
}

static void reconcile_geometry(Target *t)
{
    if (!t->valid) {
        return;
    }
    glue_ring(t);
    glue_strip(t);
}

static void reconcile_shape(Target *t)
{
    if (!t->valid) {
        return;
    }
    strip_apply_shape(t);
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
        Rect m = {0}, r = {0}, s = {0};

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
        if (win_rect(t->strip, &s)) {
            printf("  strip=%#lx %dx%d+%d+%d\n", t->strip, s.w, s.h, s.x, s.y);
        } else {
            printf("  strip=none\n");
        }
    }
}

static void print_holes(void)
{
    for (int i = 0; i < MAX_TARGETS; i++) {
        Target *t = &targets[i];
        Rect s;
        XRectangle holes[MAX_HOLES];
        int n;

        if (!t->valid || !win_rect(t->strip, &s)) {
            continue;
        }
        n = collect_occluders(t, &s, holes, MAX_HOLES);
        printf("strip=%#lx %dx%d+%d+%d holes=%d\n", t->strip, s.w, s.h, s.x,
               s.y, n);
        for (int j = 0; j < n; j++) {
            printf("  hole %d,%d %ux%u\n", holes[j].x, holes[j].y,
                   holes[j].width, holes[j].height);
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
    atom_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    atom_wm_window_type_normal =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    atom_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);

    int shape_event, shape_error;
    if (!XShapeQueryExtension(dpy, &shape_event, &shape_error)) {
        fprintf(stderr, "wecom-x11-fix: XShape extension missing\n");
        return 1;
    }
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
            }
            if (ev.type == ConfigureNotify) {
                for (int i = 0; i < MAX_TARGETS; i++) {
                    if (!targets[i].valid) {
                        continue;
                    }
                    if (ev.xconfigure.window == targets[i].frame ||
                        ev.xconfigure.window == targets[i].main) {
                        if (!dry_run) {
                            glue_ring(&targets[i]);
                            glue_strip(&targets[i]);
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
            strip_restore_full(&targets[i]);
        }
    }
    XSync(dpy, False);
    XCloseDisplay(dpy);
    return 0;
}
