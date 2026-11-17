/*
 * mcgregor-wm - an extremely minimal, Blackbox-flavoured X11 window manager.
 *
 *   - Each window gets a thin frame with a title bar. No toolbar, no menus.
 *   - Multiple desktops (workspaces).
 *   - New windows never take the keyboard focus away from the window you
 *     are typing in. They appear on top with a highlighted title, and one
 *     Alt+Tab takes you to them.
 *   - Alt+Tab cycles windows in most-recently-used order.
 *
 * Keys (Alt = Mod1):
 *   Alt+Tab / Alt+Shift+Tab   cycle windows, most recent first (release Alt);
 *                             the pointer jumps to the chosen window
 *   Alt+1..9                  go to desktop N
 *   Alt+Shift+1..9            send focused window to desktop N
 *   Ctrl+Alt+Left/Right       previous / next desktop
 *   Alt+Shift+Left/Right      move window to previous / next monitor
 *   Alt+F4                    close window
 *   Alt+F9                    iconify (Alt+Tab brings it back)
 *   Alt+F10                   maximize / restore
 *   Alt+F11                   fullscreen / restore
 *   Alt+Return                open a terminal (xterm)
 *   Ctrl+Alt+Shift+R          restart mcgregor-wm in place (X keeps running);
 *                             also: killall -HUP mcgregor-wm
 *   Ctrl+Alt+Shift+Q          quit mcgregor-wm
 * Mouse:
 *   click                     focus and raise
 *   click [X] on title bar    close
 *   drag title / Alt+drag     move
 *   drag an edge or corner    resize
 *   Alt+right-drag            resize from the nearest corner
 *   double-click title        maximize / restore
 *   middle-click title        lower
 *
 * SLACKWARE 3.6 BUILD. This version is written for 1998-era systems:
 * Linux 2.0, libc5, gcc 2.7.2.3 and XFree86 3.3 (X11R6.3). Compared with
 * the modern mcgregor-wm it
 *   - is plain C89 (no declarations after statements, constant aggregate
 *     initializers only, no snprintf);
 *   - draws titles with a core X font in Latin-1 (UTF-8 drawing arrived in
 *     XFree86 4.0.2); UTF-8 titles are converted to Latin-1;
 *   - treats the X screen as one monitor (Xinerama arrived in XFree86 4.0;
 *     build with -DXINERAMA to get the multi-monitor code back);
 *   - starts xterm on Alt+Return and reads ~/.mcgregor-wm/autostart.
 *
 * Needs only Xlib. Build: make. See README.
 */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <X11/keysym.h>
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>     /* select() and fd_set live here on libc5 */
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Configuration: edit and rebuild.                                    */

#define NDESKS        4         /* number of desktops (max 9) */
#define MODKEY        Mod1Mask  /* Alt */
#define BORDER        4         /* frame border width: also the grab area
                                   for resizing by the edges */
#define CORNER        20        /* how far along an edge counts as a corner */
#define SNAP          10        /* edge snapping distance in pixels, 0 = off */

/* What happens when a new window appears while you are working:
 *   FOCUS_NEW_IF_NONE  1 = focus it if nothing on this desktop has focus
 *   FOCUS_TRANSIENTS   1 = focus dialogs that belong to the focused window
 *                          (e.g. a Save dialog you just opened)
 *   RAISE_NEW          1 = show it on top (unfocused); 0 = put it below the
 *                          focused window */
#define FOCUS_NEW_IF_NONE 1
#define FOCUS_TRANSIENTS  1
#define RAISE_NEW         1

/* 1 = after Alt+Tab, move the mouse pointer to the centre of the window
 *     you switched to (handy across several monitors); 0 = leave it. */
#define WARP_ON_ALTTAB    1

/* Core X fonts to try, in order, separated by commas. XFree86 3.3's misc
 * fonts include 9x15 (15 px) and 7x13/8x13 (13 px). */
static const char *FONT          = "-*-fixed-medium-r-*-*-15-*-*-*-*-*-iso8859-1,"
                                   "-*-fixed-medium-r-*-*-13-*-*-*-*-*-iso8859-1,fixed";
#define TITLE_PAD     12        /* title bar height = font height + this */
static const char *COL_ACTIVE    = "#4f6d8f";   /* focused title bar */
static const char *COL_INACTIVE  = "#3a3a3a";   /* other title bars */
static const char *COL_ATTENTION = "#9a6a22";   /* new / urgent, not yet visited */
static const char *COL_TEXT      = "#ffffff";
static const char *COL_TEXT_DIM  = "#a8a8a8";
static const char *COL_BORDER_ACTIVE = "#4f6d8f";   /* same as the title */
static const char *COL_BORDER    = "#3a3a3a";

static const char *TERMINAL[] = { "xterm", NULL };

/* ------------------------------------------------------------------ */

#define LENGTH(a)  (sizeof(a) / sizeof((a)[0]))
#define CLEANMASK(m) ((m) & ~(numlockmask | LockMask) & \
        (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask | Mod5Mask))

typedef struct Client Client;
struct Client {
    Window win, frame, trans;
    int x, y, w, h;               /* frame position, client size */
    int sx, sy, sw, sh;           /* saved geometry for maximize/fullscreen */
    int desk;
    int mon;                      /* monitor used for maximize/fullscreen */
    int fullscreen, maximized, iconic, attention, cycunhid;
    int noinput, takefocus, candelete;
    int basew, baseh, incw, inch, minw, minh, maxw, maxh;
    char title[256];
    Client *next;                 /* all clients, creation order */
    Client *mnext;                /* most-recently-used order */
};

typedef struct {
    unsigned int mod;
    KeySym sym;
    void (*fn)(long);
    long arg;
} Key;

enum {
    WMProtocols, WMDelete, WMTakeFocus, WMState, WMChangeState,
    NetSupported, NetCheck, NetWMName, NetNumDesks, NetCurDesk, NetDeskNames,
    NetClientList, NetActive, NetWMDesk, NetWMState, NetFullscreen,
    NetMaxV, NetMaxH, NetHidden, NetAttention, NetClose, NetFrameExtents,
    NetWMType, NetTypeDock, NetTypeDesktop, NetTypeNotification, NetTypeSplash,
    Utf8, AtomLast
};
static const char *atomnames[AtomLast] = {
    "WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_TAKE_FOCUS", "WM_STATE", "WM_CHANGE_STATE",
    "_NET_SUPPORTED", "_NET_SUPPORTING_WM_CHECK", "_NET_WM_NAME",
    "_NET_NUMBER_OF_DESKTOPS", "_NET_CURRENT_DESKTOP", "_NET_DESKTOP_NAMES",
    "_NET_CLIENT_LIST", "_NET_ACTIVE_WINDOW", "_NET_WM_DESKTOP", "_NET_WM_STATE",
    "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_HIDDEN",
    "_NET_WM_STATE_DEMANDS_ATTENTION", "_NET_CLOSE_WINDOW", "_NET_FRAME_EXTENTS",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK", "_NET_WM_WINDOW_TYPE_DESKTOP",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION", "_NET_WM_WINDOW_TYPE_SPLASH",
    "UTF8_STRING",
};

static Display *dpy;
typedef struct { int x, y, w, h; } Mon;

static int scr, sw, sh, TH, fasc;
static Mon mons[16];              /* monitors (Xinerama), or the one screen */
static int nmons;
static Window root, checkwin;
static Atom A[AtomLast];
static Client *clients, *mru, *sel;
static int curdesk;
static unsigned int numlockmask;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t restarting;
static Atom mruatom;              /* _MCGREGOR_WM_MRU: Alt+Tab order kept across restarts */
static int shuttingdown;
static GC gc;
static XFontStruct *font;        /* title-bar font (Latin-1 core font) */
static unsigned long px_active, px_inactive, px_attention, px_text, px_textdim;
static unsigned long px_bactive, px_border;
static Cursor cur_normal, cur_move, cur_edge[16];

enum { EdgeL = 1, EdgeR = 2, EdgeT = 4, EdgeB = 8 };
static int (*xerrorxlib)(Display *, XErrorEvent *);
static int placecount;
static Window nowin = None;

/* Alt+Tab state */
static int cycling;
static Client *cyc[256];
static int cycn, cyci;
static Client *cycorig;

/* forward declarations */
static void focus(Client *c, int mru_update);
static void unmanage(Client *c, int destroyed);
static void view(long n);
static void handle(XEvent *e);
static void setfullscreen(Client *c, int on);
static void setmaximized(Client *c, int on);

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */

/*
 * die()
 *
 * Print an error message prefixed with "mcgregor-wm:" to stderr and exit with
 * status 1. Used only for fatal start-up problems (no display, no font,
 * another window manager already running).
 */
static void die(const char *m)
{
    fprintf(stderr, "mcgregor-wm: %s\n", m);
    exit(1);
}

/*
 * getcolor()
 *
 * Turn a colour name or "#rrggbb" string into a pixel value in the
 * default colormap. If the server can't allocate the colour, falls back to
 * black rather than failing.
 */
static unsigned long getcolor(const char *name)
{
    XColor c;
    if (!XAllocNamedColor(dpy, DefaultColormap(dpy, scr), name, &c, &c))
        return BlackPixel(dpy, scr);
    return c.pixel;
}

/*
 * wintoclient()
 *
 * Find the managed client that owns window w, where w may be either the
 * application's own window or the frame mcgregor-wm wrapped around it.
 * Returns NULL if w belongs to no managed client. Used for events that can
 * arrive on either window (clicks, exposes).
 */
static Client *wintoclient(Window w)
{
    Client *c;
    for (c = clients; c; c = c->next)
        if (c->win == w || c->frame == w)
            return c;
    return NULL;
}

/*
 * clientof()
 *
 * Find the managed client whose application window is exactly w (frames
 * don't match). Returns NULL if w isn't a managed application window. Used
 * for events that only concern the application's window: property changes,
 * map/unmap/destroy notifications, client messages.
 */
static Client *clientof(Window w)
{
    Client *c;
    for (c = clients; c; c = c->next)
        if (c->win == w)
            return c;
    return NULL;
}

/*
 * mru_remove()
 *
 * Unlink c from the most-recently-used list (the Alt+Tab order), if it is
 * in it. Walks the singly linked list via a pointer-to-pointer so the head
 * needs no special case.
 */
static void mru_remove(Client *c)
{
    Client **p;
    for (p = &mru; *p; p = &(*p)->mnext)
        if (*p == c) {
            *p = c->mnext;
            break;
        }
    c->mnext = NULL;
}

/*
 * mru_front()
 *
 * Make c the most recently used window: remove it from wherever it is in
 * the MRU list and push it on the front. Called when a window is really
 * chosen (focused by click, Alt+Tab release, activation).
 */
static void mru_front(Client *c)
{
    mru_remove(c);
    c->mnext = mru;
    mru = c;
}

/*
 * mru_second()
 *
 * Put c second in the MRU list, right behind the window currently in use.
 * New windows and windows asking for attention go here so a single
 * Alt+Tab reaches them without disturbing what you're working in. If the
 * list is empty, c becomes its only entry.
 */
static void mru_second(Client *c)
{
    mru_remove(c);
    if (!mru || mru == c) {
        c->mnext = mru;
        mru = c;
    } else {
        c->mnext = mru->mnext;
        mru->mnext = c;
    }
}

/*
 * sendevent()
 *
 * Send an ICCCM WM_PROTOCOLS client message (e.g. WM_DELETE_WINDOW or
 * WM_TAKE_FOCUS) to c, but only if the application lists that protocol in
 * its WM_PROTOCOLS property. Returns 1 if the protocol was supported and
 * the message sent, 0 otherwise, so callers can fall back (e.g. to
 * XKillClient when an app doesn't support WM_DELETE_WINDOW).
 */
static int sendevent(Client *c, Atom proto)
{
    Atom *protos;
    int n, found = 0;
    XEvent ev;

    if (XGetWMProtocols(dpy, c->win, &protos, &n)) {
        while (!found && n--)
            found = protos[n] == proto;
        XFree(protos);
    }
    if (found) {
        memset(&ev, 0, sizeof ev);
        ev.type = ClientMessage;
        ev.xclient.window = c->win;
        ev.xclient.message_type = A[WMProtocols];
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = proto;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, c->win, False, NoEventMask, &ev);
    }
    return found;
}

/*
 * getatomprop()
 *
 * Read the first atom of property prop on window w (e.g. the window's
 * _NET_WM_WINDOW_TYPE). Returns None if the property is missing or empty.
 */
static Atom getatomprop(Window w, Atom prop)
{
    Atom type, a = None;
    int fmt;
    unsigned long n, after;
    unsigned char *p = NULL;

    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_ATOM, &type, &fmt, &n,
                           &after, &p) == Success && p) {
        if (n > 0)
            a = *(Atom *)p;
        XFree(p);
    }
    return a;
}

/*
 * getcardprop()
 *
 * Read a single CARDINAL (integer) property such as _NET_WM_DESKTOP or
 * _NET_CURRENT_DESKTOP from window w. Returns def if the property is
 * missing or empty.
 */
static long getcardprop(Window w, Atom prop, long def)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *p = NULL;
    long v = def;

    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_CARDINAL, &type, &fmt, &n,
                           &after, &p) == Success && p) {
        if (n > 0)
            v = *(long *)p;
        XFree(p);
    }
    return v;
}

/*
 * getwmstate()
 *
 * Read the ICCCM WM_STATE of window w: NormalState, IconicState or
 * WithdrawnState. Returns -1 if the window has no WM_STATE. Used at start
 * up to tell iconified windows from withdrawn ones.
 */
static long getwmstate(Window w)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *p = NULL;
    long v = -1;

    if (XGetWindowProperty(dpy, w, A[WMState], 0, 2, False, A[WMState], &type, &fmt,
                           &n, &after, &p) == Success && p) {
        if (n > 0)
            v = *(long *)p;
        XFree(p);
    }
    return v;
}

/*
 * setwmstate()
 *
 * Set the ICCCM WM_STATE property on c's application window to state
 * (NormalState, IconicState or WithdrawnState), with no icon window.
 * Applications and other window managers read this to know whether the
 * window is shown, iconified or unmanaged.
 */
static void setwmstate(Client *c, long state)
{
    long data[2];
    data[0] = state;
    data[1] = None;
    XChangeProperty(dpy, c->win, A[WMState], A[WMState], 32, PropModeReplace,
                    (unsigned char *)data, 2);
}

/*
 * setcard()
 *
 * Write a single CARDINAL (integer) property prop = v on window w, e.g.
 * _NET_CURRENT_DESKTOP on the root or _NET_WM_DESKTOP on a client.
 */
static void setcard(Window w, Atom prop, long v)
{
    XChangeProperty(dpy, w, prop, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&v, 1);
}

/*
 * setnetstate()
 *
 * Rewrite c's _NET_WM_STATE property from its flags: FULLSCREEN,
 * MAXIMIZED_VERT + MAXIMIZED_HORZ, HIDDEN (iconified) and DEMANDS_ATTENTION.
 * Pagers, panels and the applications themselves read this; it is also
 * what lets fullscreen survive a restart.
 */
static void setnetstate(Client *c)
{
    Atom s[5];
    int n = 0;
    if (c->fullscreen) s[n++] = A[NetFullscreen];
    if (c->maximized) { s[n++] = A[NetMaxV]; s[n++] = A[NetMaxH]; }
    if (c->iconic) s[n++] = A[NetHidden];
    if (c->attention) s[n++] = A[NetAttention];
    XChangeProperty(dpy, c->win, A[NetWMState], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)s, n);
}

/*
 * updateclientlist()
 *
 * Rebuild the root window's _NET_CLIENT_LIST property, the list of all
 * managed application windows in creation order, which taskbars, pagers
 * and tools like wmctrl use to enumerate windows.
 */
static void updateclientlist(void)
{
    Client *c;
    XDeleteProperty(dpy, root, A[NetClientList]);
    for (c = clients; c; c = c->next)
        XChangeProperty(dpy, root, A[NetClientList], XA_WINDOW, 32, PropModeAppend,
                        (unsigned char *)&c->win, 1);
}

/*
 * spawn()
 *
 * Start a program in the background. Forks; the child closes its copy of
 * the X connection, starts a new session (so it survives mcgregor-wm and
 * gets no terminal signals), and execs argv[0] from PATH. The parent
 * doesn't wait: finished children are reaped by the SIGCHLD handler.
 */
static void spawn(const char **argv)
{
    if (fork() == 0) {
        if (dpy)
            close(ConnectionNumber(dpy));
        setsid();
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "mcgregor-wm: cannot run %s\n", argv[0]);
        _exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* Properties                                                          */

/*
 * utf8tolatin1()
 *
 * Copy the UTF-8 string s into dst (size bytes, always NUL-terminated) as
 * Latin-1, which is what the core title font can show. Characters U+0000
 * to U+00FF are kept; anything beyond Latin-1, and any malformed byte, is
 * shown as '?'. Needed because X11R6.3 has no UTF-8 text drawing.
 */
static void utf8tolatin1(char *dst, size_t size, const unsigned char *s)
{
    size_t o = 0;
    unsigned int cp;
    int extra;

    while (*s && o + 1 < size) {
        if (*s < 0x80) {
            cp = *s++;
            extra = 0;
        } else if ((*s & 0xe0) == 0xc0) {
            cp = *s++ & 0x1f;
            extra = 1;
        } else if ((*s & 0xf0) == 0xe0) {
            cp = *s++ & 0x0f;
            extra = 2;
        } else if ((*s & 0xf8) == 0xf0) {
            cp = *s++ & 0x07;
            extra = 3;
        } else {
            s++;                       /* stray continuation byte */
            dst[o++] = '?';
            continue;
        }
        while (extra > 0 && (*s & 0xc0) == 0x80) {
            cp = (cp << 6) | (*s++ & 0x3f);
            extra--;
        }
        dst[o++] = (extra == 0 && cp <= 0xff) ? (char)cp : '?';
    }
    dst[o] = 0;
}

/*
 * updatetitle()
 *
 * Refresh c->title from the window's title properties, as Latin-1 for the
 * core title font. Prefers the EWMH _NET_WM_NAME (UTF-8, converted with
 * utf8tolatin1()) and falls back to the ICCCM WM_NAME: STRING is already
 * Latin-1 and is copied, and any other encoding (COMPOUND_TEXT) goes
 * through Xlib's XmbTextPropertyToTextList into the locale's encoding.
 * The result is truncated to fit c->title and always NUL-terminated.
 */
static void updatetitle(Client *c)
{
    XTextProperty tp;
    char **list = NULL;
    int n;

    c->title[0] = 0;
    if (!XGetTextProperty(dpy, c->win, &tp, A[NetWMName]) || !tp.nitems) {
        if (!XGetTextProperty(dpy, c->win, &tp, XA_WM_NAME) || !tp.nitems)
            return;
    }
    if (tp.encoding == A[Utf8]) {
        utf8tolatin1(c->title, sizeof c->title, tp.value);
    } else if (tp.encoding == XA_STRING) {
        strncpy(c->title, (char *)tp.value, sizeof c->title - 1);
    } else if (XmbTextPropertyToTextList(dpy, &tp, &list, &n) >= Success && n > 0 &&
               list && *list) {
        strncpy(c->title, *list, sizeof c->title - 1);
        XFreeStringList(list);
    }
    c->title[sizeof c->title - 1] = 0;
    XFree(tp.value);
}

/*
 * updatesizehints()
 *
 * Read c's WM_NORMAL_HINTS (ICCCM size hints) into the client: base size,
 * resize increments (e.g. a terminal's character cell), and minimum and
 * maximum size. Missing values fall back the way ICCCM says (base from min
 * and vice versa); minimums are never below 1. applyhints() then uses
 * these when the window is resized.
 */
static void updatesizehints(Client *c)
{
    XSizeHints h;
    long supplied;

    c->basew = c->baseh = c->incw = c->inch = c->maxw = c->maxh = 0;
    c->minw = c->minh = 1;
    if (!XGetWMNormalHints(dpy, c->win, &h, &supplied))
        return;
    if (h.flags & PBaseSize) { c->basew = h.base_width; c->baseh = h.base_height; }
    else if (h.flags & PMinSize) { c->basew = h.min_width; c->baseh = h.min_height; }
    if (h.flags & PResizeInc) { c->incw = h.width_inc; c->inch = h.height_inc; }
    if (h.flags & PMaxSize) { c->maxw = h.max_width; c->maxh = h.max_height; }
    if (h.flags & PMinSize) { c->minw = h.min_width; c->minh = h.min_height; }
    else if (h.flags & PBaseSize) { c->minw = h.base_width; c->minh = h.base_height; }
    if (c->minw < 1) c->minw = 1;
    if (c->minh < 1) c->minh = 1;
}

/*
 * updatewmhints()
 *
 * Read c's WM_HINTS. Records whether the window accepts keyboard focus
 * (the input hint; some apps say no and use WM_TAKE_FOCUS instead), and
 * turns the urgency hint into mcgregor-wm's attention flag (orange title) if
 * the window isn't the focused one.
 */
static void updatewmhints(Client *c)
{
    XWMHints *wh = XGetWMHints(dpy, c->win);
    if (!wh)
        return;
    c->noinput = (wh->flags & InputHint) && !wh->input;
    if ((wh->flags & XUrgencyHint) && c != sel)
        c->attention = 1;
    XFree(wh);
}

/*
 * updateprotocols()
 *
 * Read c's WM_PROTOCOLS and remember whether it supports WM_TAKE_FOCUS
 * (it wants to be told when it gets focus) and WM_DELETE_WINDOW (it can
 * be asked politely to close).
 */
static void updateprotocols(Client *c)
{
    Atom *protos;
    int n;
    c->takefocus = c->candelete = 0;
    if (XGetWMProtocols(dpy, c->win, &protos, &n)) {
        while (n--) {
            if (protos[n] == A[WMTakeFocus]) c->takefocus = 1;
            if (protos[n] == A[WMDelete]) c->candelete = 1;
        }
        XFree(protos);
    }
}

/*
 * applyhints()
 *
 * Adjust a requested client size *w x *h to obey c's size hints: clamp to
 * the minimum and maximum size, then round down to a whole number of
 * resize increments above the base size (so a terminal is always a whole
 * number of characters). Never returns a size smaller than 1x1.
 */
static void applyhints(Client *c, int *w, int *h)
{
    if (*w < c->minw) *w = c->minw;
    if (*h < c->minh) *h = c->minh;
    if (c->maxw > 0 && *w > c->maxw) *w = c->maxw;
    if (c->maxh > 0 && *h > c->maxh) *h = c->maxh;
    if (c->incw > 1) *w -= (*w - c->basew) % c->incw;
    if (c->inch > 1) *h -= (*h - c->baseh) % c->inch;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

/* ------------------------------------------------------------------ */
/* Monitors                                                            */

/*
 * updatemons()
 *
 * Rebuild the monitor list mons[]. In this Slackware build (no -DXINERAMA)
 * it is always the one X screen; with -DXINERAMA the list comes from
 * Xinerama. Mirrored outputs reporting the same rectangle are counted
 * once. With no Xinerama, or no monitors reported, the whole X screen is
 * one monitor. The list is then sorted left to right (then top to bottom)
 * so "next monitor" for Alt+Shift+Right is predictable, and any client
 * whose remembered monitor index no longer exists is moved to the last
 * one. Cheap enough to call whenever a monitor-dependent action happens,
 * so layout changes are picked up without RandR events.
 */
static void updatemons(void)
{
    int i, j;
#ifdef XINERAMA
    int n = 0;
    XineramaScreenInfo *info = NULL;

    nmons = 0;
    if (XineramaIsActive(dpy))
        info = XineramaQueryScreens(dpy, &n);
    for (i = 0; i < n && nmons < (int)LENGTH(mons); i++) {
        for (j = 0; j < nmons; j++)              /* skip mirrored outputs */
            if (mons[j].x == info[i].x_org && mons[j].y == info[i].y_org &&
                mons[j].w == info[i].width && mons[j].h == info[i].height)
                break;
        if (j < nmons)
            continue;
        mons[nmons].x = info[i].x_org;
        mons[nmons].y = info[i].y_org;
        mons[nmons].w = info[i].width;
        mons[nmons].h = info[i].height;
        nmons++;
    }
    if (info)
        XFree(info);
#else
    nmons = 0;           /* X11R6.3 has no Xinerama: the screen is one monitor */
#endif
    if (nmons == 0) {
        mons[0].x = mons[0].y = 0;
        mons[0].w = sw;
        mons[0].h = sh;
        nmons = 1;
    }
    {
        Client *c;                 /* a monitor may have gone away */
        for (c = clients; c; c = c->next)
            if (c->mon >= nmons)
                c->mon = nmons - 1;
    }
    /* left-to-right, then top-to-bottom, so "next monitor" is predictable */
    for (i = 1; i < nmons; i++)
        for (j = i; j > 0 && (mons[j].x < mons[j - 1].x ||
                              (mons[j].x == mons[j - 1].x && mons[j].y < mons[j - 1].y)); j--) {
            Mon t = mons[j];
            mons[j] = mons[j - 1];
            mons[j - 1] = t;
        }
}

/*
 * monat()
 *
 * Return the index of the monitor that contains the root-window point
 * (x, y). If the point is in a gap between monitors or off all of them,
 * return the nearest monitor by squared distance to its rectangle.
 */
static int monat(int x, int y)
{
    int i, best = 0;
    long bestd = -1;
    for (i = 0; i < nmons; i++) {
        long dx = 0, dy = 0, d;
        if (x < mons[i].x) dx = mons[i].x - x;
        else if (x >= mons[i].x + mons[i].w) dx = x - (mons[i].x + mons[i].w - 1);
        if (y < mons[i].y) dy = mons[i].y - y;
        else if (y >= mons[i].y + mons[i].h) dy = y - (mons[i].y + mons[i].h - 1);
        d = dx * dx + dy * dy;
        if (d == 0)
            return i;
        if (bestd < 0 || d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

/*
 * clientmon()
 *
 * Return the monitor a client is on, judged by the centre of its frame
 * (so a window straddling two monitors belongs to the one holding most of
 * it).
 */
static int clientmon(Client *c)
{
    return monat(c->x + BORDER + c->w / 2, c->y + BORDER + (c->h + TH) / 2);
}

/*
 * pointermon()
 *
 * Return the monitor the mouse pointer is currently on. New windows
 * without a position of their own are placed on this monitor.
 */
static int pointermon(void)
{
    Window r, ch;
    int rx = 0, ry = 0, wx, wy;
    unsigned int mask;
    XQueryPointer(dpy, root, &r, &ch, &rx, &ry, &wx, &wy, &mask);
    return monat(rx, ry);
}

/*
 * clamptomon()
 *
 * Make client c fit inside monitor m: shrink the client if it is wider or
 * taller than the monitor (allowing for border and title bar), then slide
 * the frame so its right/bottom edges and then its left/top edges are on
 * the monitor. Left/top win, which keeps the title bar reachable.
 */
static void clamptomon(Client *c, int m)
{
    Mon *M = &mons[m];
    int maxw = M->w - 2 * BORDER, maxh = M->h - 2 * BORDER - TH;
    if (c->w > maxw) c->w = maxw;
    if (c->h > maxh) c->h = maxh;
    if (c->x + c->w + 2 * BORDER > M->x + M->w) c->x = M->x + M->w - c->w - 2 * BORDER;
    if (c->y + c->h + TH + 2 * BORDER > M->y + M->h) c->y = M->y + M->h - c->h - TH - 2 * BORDER;
    if (c->x < M->x) c->x = M->x;
    if (c->y < M->y) c->y = M->y;
}

/*
 * maxgeom()
 *
 * Set c's geometry to fill its monitor (c->mon) exactly: the frame at the
 * monitor's top-left corner and the client sized so frame border plus
 * title bar plus client equals the monitor size. Used for maximize.
 */
static void maxgeom(Client *c)
{
    Mon *M = &mons[c->mon];
    c->x = M->x;
    c->y = M->y;
    c->w = M->w - 2 * BORDER;
    c->h = M->h - 2 * BORDER - TH;
}

/* ------------------------------------------------------------------ */
/* Frames                                                              */

/*
 * closebox()
 *
 * Compute where the [X] close box sits in c's title bar: a square TH-10
 * pixels high (at least 8), vertically centred, with the same gap to the
 * right edge as to the top. Returns its top-left corner and size in frame
 * coordinates through x, y and size.
 */
static void closebox(Client *c, int *x, int *y, int *size)
{
    *size = TH - 10;
    if (*size < 8)
        *size = 8;
    *y = (TH - *size) / 2;
    *x = c->w - *size - *y;
}

/*
 * inclosebox()
 *
 * Return 1 if the frame-relative point (px, py) is on c's close box,
 * counting a 2-pixel margin around it so it's easy to hit.
 */
static int inclosebox(Client *c, int px, int py)
{
    int x, y, sz;
    closebox(c, &x, &y, &sz);
    return px >= x - 2 && px < x + sz + 2 && py >= y - 2 && py < y + sz + 2;
}

/*
 * drawclosebox()
 *
 * Draw c's close box: a filled square with a 1-pixel outline and a
 * 2-pixel-wide X inside. Normally the square is the title colour (bg) and
 * the outline and X are the text colour (fg). With pressed set the colours
 * swap, showing the button as held down.
 */
static void drawclosebox(Client *c, unsigned long bg, unsigned long fg, int pressed)
{
    int x, y, sz, m;
    closebox(c, &x, &y, &sz);
    m = sz / 4 + 1;                       /* inset of the X inside the box */
    XSetForeground(dpy, gc, pressed ? fg : bg);
    XFillRectangle(dpy, c->frame, gc, x, y, sz, sz);
    XSetForeground(dpy, gc, pressed ? bg : fg);
    XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);
    XDrawRectangle(dpy, c->frame, gc, x, y, sz - 1, sz - 1);
    XSetLineAttributes(dpy, gc, 2, LineSolid, CapRound, JoinRound);
    XDrawLine(dpy, c->frame, gc, x + m, y + m, x + sz - 1 - m, y + sz - 1 - m);
    XDrawLine(dpy, c->frame, gc, x + sz - 1 - m, y + m, x + m, y + sz - 1 - m);
    XSetLineAttributes(dpy, gc, 1, LineSolid, CapButt, JoinMiter);
}

/*
 * titlecolors()
 *
 * Choose c's title bar colours: blue background and white text when it
 * is the focused window, orange with white text when it wants attention
 * (new or urgent, not yet visited), grey with dim text otherwise.
 */
static void titlecolors(Client *c, unsigned long *bg, unsigned long *fg)
{
    *bg = c == sel ? px_active : c->attention ? px_attention : px_inactive;
    *fg = (c == sel || c->attention) ? px_text : px_textdim;
}

/*
 * draw()
 *
 * Repaint c's frame: set the border and title-bar colour for its current
 * state, clear the title bar, draw the title text clipped so it stops
 * short of the close box, and draw the close box. Does nothing for a
 * fullscreen window, whose frame is entirely covered by the application.
 */
static void draw(Client *c)
{
    unsigned long bg, fg;
    int len, ty, bx, by, bs;
    XRectangle clip;

    if (c->fullscreen)
        return;
    titlecolors(c, &bg, &fg);
    XSetWindowBackground(dpy, c->frame, bg);
    XSetWindowBorder(dpy, c->frame, c == sel ? px_bactive :
                                    c->attention ? px_attention : px_border);
    XClearArea(dpy, c->frame, 0, 0, c->w, TH, False);

    /* title text, clipped so it never runs under the close box */
    closebox(c, &bx, &by, &bs);
    clip.x = 0;
    clip.y = 0;
    clip.width = bx > 8 ? bx - 6 : 1;
    clip.height = TH;
    XSetClipRectangles(dpy, gc, 0, 0, &clip, 1, Unsorted);
    XSetForeground(dpy, gc, fg);
    len = (int)strlen(c->title);
    ty = (TH - 1 + fasc) / 2;   /* baseline that centres the text */
    XDrawString(dpy, c->frame, gc, 8, ty, c->title, len);
    XSetClipMask(dpy, gc, None);

    drawclosebox(c, bg, fg, 0);
}

/*
 * sendconfigure()
 *
 * Send c a synthetic ConfigureNotify telling it its real position and
 * size on the screen. ICCCM requires this after the window manager moves
 * or resizes a reparented window, because the app's own window only moves
 * relative to its frame and would otherwise think it's still at (0, TH).
 * For fullscreen it reports the monitor's rectangle; otherwise the client
 * area inside the border and title bar.
 */
static void sendconfigure(Client *c)
{
    XConfigureEvent ce;
    memset(&ce, 0, sizeof ce);
    ce.type = ConfigureNotify;
    ce.display = dpy;
    ce.event = c->win;
    ce.window = c->win;
    if (c->fullscreen) {
        ce.x = mons[c->mon].x;
        ce.y = mons[c->mon].y;
        ce.width = mons[c->mon].w;
        ce.height = mons[c->mon].h;
    } else {
        ce.x = c->x + BORDER;
        ce.y = c->y + BORDER + TH;
        ce.width = c->w;
        ce.height = c->h;
    }
    ce.border_width = 0;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

/*
 * applygeom()
 *
 * Push c's geometry to the X server. Fullscreen: the frame becomes the
 * whole monitor with no border, and the client covers it, title bar
 * included. Normal: the frame gets its border and is placed at (c->x,
 * c->y) with room for the title bar, and the client sits under the title
 * bar. Either way the app is then sent a synthetic ConfigureNotify.
 */
static void applygeom(Client *c)
{
    if (c->fullscreen) {
        Mon *M = &mons[c->mon];
        XSetWindowBorderWidth(dpy, c->frame, 0);
        XMoveResizeWindow(dpy, c->frame, M->x, M->y, M->w, M->h);
        XMoveResizeWindow(dpy, c->win, 0, 0, M->w, M->h);
    } else {
        XSetWindowBorderWidth(dpy, c->frame, BORDER);
        XMoveResizeWindow(dpy, c->frame, c->x, c->y, c->w, c->h + TH);
        XMoveResizeWindow(dpy, c->win, 0, TH, c->w, c->h);
    }
    sendconfigure(c);
}

/*
 * setframeextents()
 *
 * Publish _NET_FRAME_EXTENTS on c: how many pixels mcgregor-wm's frame adds
 * on the left, right, top (border + title bar) and bottom. All zero when
 * fullscreen. Applications use it to position themselves accurately.
 */
static void setframeextents(Client *c)
{
    long e[4];
    e[0] = e[1] = e[3] = BORDER;          /* left, right, bottom */
    e[2] = BORDER + TH;                   /* top: border + title bar */
    if (c->fullscreen)
        e[0] = e[1] = e[2] = e[3] = 0;
    XChangeProperty(dpy, c->win, A[NetFrameExtents], XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)e, 4);
}

/*
 * raiseclient()
 *
 * Raise c's frame to the top of the stacking order, then raise any of its
 * dialogs (windows transient for it on the same desktop and not
 * iconified) above it, so a dialog is never hidden behind its parent.
 */
static void raiseclient(Client *c)
{
    Client *t;
    XRaiseWindow(dpy, c->frame);
    for (t = clients; t; t = t->next)       /* keep its dialogs above it */
        if (t->trans == c->win && t->desk == c->desk && !t->iconic)
            XRaiseWindow(dpy, t->frame);
}

/*
 * showclient()
 *
 * Show or hide c by mapping or unmapping its frame (the application
 * window stays mapped inside it, so it gets no unmap events). Showing sets
 * WM_STATE to Normal; hiding sets it to Iconic only if the window is
 * iconified (a window hidden because it is on another desktop keeps
 * Normal).
 */
static void showclient(Client *c, int show)
{
    if (show) {
        XMapWindow(dpy, c->frame);
        setwmstate(c, NormalState);
    } else {
        XUnmapWindow(dpy, c->frame);
        if (c->iconic)
            setwmstate(c, IconicState);
    }
}

/*
 * visible()
 *
 * Return 1 if c should be on screen right now: on the current desktop and
 * not iconified.
 */
static int visible(Client *c)
{
    return c->desk == curdesk && !c->iconic;
}

/*
 * grabbuttons()
 *
 * Set up passive grabs on c's application window for mouse buttons 1-3
 * with any modifier. The grabs are synchronous: the X server freezes the
 * pointer on a click so mcgregor-wm can focus and raise the window (or start
 * an Alt+drag move or resize) and then replay the click to the
 * application with XAllowEvents. This is how click-to-focus works without
 * swallowing the click.
 */
static void grabbuttons(Client *c)
{
    /* One synchronous grab for every button-1..3 press on the client:
     * we focus/raise, then replay the click to the application (or start
     * a move/resize when Alt is held). */
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
    XGrabButton(dpy, Button1, AnyModifier, c->win, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button2, AnyModifier, c->win, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, AnyModifier, c->win, False, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);
}

/* ------------------------------------------------------------------ */
/* Focus                                                               */

/*
 * setfocus()
 *
 * Give the X keyboard focus to c, or to the root window when c is NULL.
 * Windows whose input hint says they don't take focus are skipped, and
 * windows that support WM_TAKE_FOCUS are also sent that message (Java and
 * some toolkits set focus themselves when they get it). Only touches the
 * X focus; focus() also updates mcgregor-wm's own state.
 */
static void setfocus(Client *c)
{
    if (c && !c->noinput)
        XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    else if (!c)
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    if (c && c->takefocus)
        sendevent(c, A[WMTakeFocus]);
}

/*
 * focus()
 *
 * Make c the focused window (or nothing, if c is NULL or not visible).
 * Redraws the previously focused frame in inactive colours, clears c's
 * attention flag, moves c to the front of the Alt+Tab list when
 * mru_update is set (not during an Alt+Tab preview), redraws it in active
 * colours, sets the X input focus and updates _NET_ACTIVE_WINDOW.
 */
static void focus(Client *c, int mru_update)
{
    Client *old = sel;

    if (c && !visible(c))
        c = NULL;
    sel = c;
    if (old && old != c)
        draw(old);
    if (c) {
        if (c->attention) {
            c->attention = 0;
            setnetstate(c);
        }
        if (mru_update)
            mru_front(c);
        draw(c);
    }
    setfocus(c);
    XChangeProperty(dpy, root, A[NetActive], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)(c ? &c->win : &nowin), 1);
}

/*
 * focusrecent()
 *
 * Focus the most recently used window that is visible on the current
 * desktop, or nothing if there is none. Used after the focused window
 * closes, is iconified, or moves away, and after switching desktops.
 */
static void focusrecent(void)
{
    Client *c;
    for (c = mru; c; c = c->mnext)
        if (visible(c))
            break;
    focus(c, 1);
}

/*
 * related()
 *
 * Return 1 if clients a and b belong together as a window and its dialog:
 * one is transient for the other, or both are dialogs of the same parent.
 * Focus may move freely between related windows without counting as focus
 * stealing.
 */
static int related(Client *a, Client *b)
{
    return a && b && (a->trans == b->win || b->trans == a->win ||
                      (a->trans && a->trans == b->trans));
}

/* ------------------------------------------------------------------ */
/* Manage / unmanage                                                   */

/*
 * skiptype()
 *
 * Return 1 if window w is a type mcgregor-wm leaves unframed and unmanaged:
 * docks/panels, desktop windows (wallpaper/icon managers), notifications
 * and splash screens, as given by _NET_WM_WINDOW_TYPE.
 */
static int skiptype(Window w)
{
    Atom t = getatomprop(w, A[NetWMType]);
    return t == A[NetTypeDock] || t == A[NetTypeDesktop] ||
           t == A[NetTypeNotification] || t == A[NetTypeSplash];
}

/*
 * hasstate()
 *
 * Return 1 if window w's _NET_WM_STATE property contains atom a. Used when
 * a window is first managed to honour states the application set before
 * mapping, such as starting fullscreen or maximized.
 */
static int hasstate(Window w, Atom a)
{
    Atom type, *s;
    int fmt, found = 0;
    unsigned long n, after, i;
    unsigned char *p = NULL;

    if (XGetWindowProperty(dpy, w, A[NetWMState], 0, 32, False, XA_ATOM, &type, &fmt,
                           &n, &after, &p) == Success && p) {
        s = (Atom *)p;
        for (i = 0; i < n; i++)
            if (s[i] == a)
                found = 1;
        XFree(p);
    }
    return found;
}

/*
 * place()
 *
 * Decide where a newly managed window goes and remember its monitor.
 * - At start-up, or when the app gave a position (USPosition/PPosition, or
 *   any non-zero x/y), the position is honoured: the frame is put around
 *   it so the application's content ends up exactly where it asked.
 * - A dialog is centred over its parent.
 * - Anything else is cascaded (40 px in, then stepping diagonally by a
 *   title-bar height for each new window, ten steps before repeating) on
 *   the monitor the mouse is on, or centred if the cascade would run off.
 * Finally the window is clamped to its monitor so the title bar is
 * reachable. The monitor list is refreshed first.
 */
static void place(Client *c, Client *parent, XWindowAttributes *wa, int startup)
{
    XSizeHints h;
    long supplied;
    int userpos = 0, m;

    if (XGetWMNormalHints(dpy, c->win, &h, &supplied))
        userpos = (h.flags & (USPosition | PPosition)) != 0;

    updatemons();      /* cheap, and picks up layout changes */
    if (startup || userpos || wa->x != 0 || wa->y != 0) {
        /* Treat the requested position as the client's; the frame goes
         * around it so the window content ends up where it asked. */
        c->x = wa->x - BORDER;
        c->y = wa->y - BORDER - TH;
        m = clientmon(c);
    } else if (parent) {
        c->x = parent->x + (parent->w - c->w) / 2;
        c->y = parent->y + (parent->h - c->h) / 2;
        m = clientmon(parent);
    } else {
        /* cascade on the monitor the mouse is on */
        int step = TH + 8;
        Mon *M;
        m = pointermon();
        M = &mons[m];
        c->x = M->x + 40 + (placecount % 10) * step;
        c->y = M->y + 40 + (placecount % 10) * step;
        placecount++;
        if (c->x + c->w + 2 * BORDER > M->x + M->w)
            c->x = M->x + (M->w - c->w - 2 * BORDER) / 2;
        if (c->y + c->h + TH + 2 * BORDER > M->y + M->h)
            c->y = M->y + (M->h - c->h - TH - 2 * BORDER) / 2;
    }
    clamptomon(c, m);     /* fit it and keep the title bar reachable */
    c->mon = m;
}

/*
 * manage()
 *
 * Start managing a newly mapped (or, at start-up, existing) window w.
 * Docks, desktop windows, notifications and splash screens are just
 * mapped and left alone. For everything else:
 *  1. Read the transient parent, title, size hints, WM_HINTS and protocols.
 *  2. Pick the desktop: the parent's for a dialog, otherwise the current
 *     one; at start-up (including after a restart) the window's own
 *     _NET_WM_DESKTOP and iconic WM_STATE are honoured.
 *  3. Place it (place()), create the frame window and reparent the app
 *     window into it below the title bar, add it to the save-set (so it
 *     survives if mcgregor-wm dies), grab buttons and publish properties.
 *  4. Apply initial maximize/fullscreen states the app requested.
 *  5. The no-focus-stealing rule: the window is focused only if nothing on
 *     this desktop has focus (FOCUS_NEW_IF_NONE) or it is a dialog of the
 *     focused window (FOCUS_TRANSIENTS). Otherwise it is shown on top
 *     (RAISE_NEW; a fullscreen one goes just below the focused window
 *     instead), marked for attention (orange title) and put second in the
 *     Alt+Tab list so one Alt+Tab reaches it.
 * Windows on another desktop or iconified are managed but left hidden.
 */
static void manage(Window w, XWindowAttributes *wa, int startup)
{
    Client *c, *parent = NULL;
    Window tr = None;
    XSetWindowAttributes fa;
    long d;
    int wantfocus;

    if (skiptype(w)) {
        /* Panels, desktop windows, notifications: map as-is, unframed. */
        XMapWindow(dpy, w);
        if (getatomprop(w, A[NetWMType]) == A[NetTypeDesktop])
            XLowerWindow(dpy, w);
        return;
    }

    c = calloc(1, sizeof *c);
    if (!c)
        return;
    c->win = w;
    c->w = wa->width;
    c->h = wa->height;
    if (XGetTransientForHint(dpy, w, &tr) && tr != None && tr != w) {
        c->trans = tr;
        parent = clientof(tr);
    }
    updatetitle(c);
    updatesizehints(c);
    updatewmhints(c);
    updateprotocols(c);

    c->desk = parent ? parent->desk : curdesk;
    if (startup) {
        d = getcardprop(w, A[NetWMDesk], -1);
        if (d >= 0 && d < NDESKS)
            c->desk = (int)d;
        c->iconic = getwmstate(w) == IconicState;
    }
    place(c, parent, wa, startup);

    fa.background_pixel = px_inactive;
    fa.border_pixel = px_border;
    fa.event_mask = SubstructureRedirectMask | SubstructureNotifyMask |
                    ButtonPressMask | ExposureMask | PointerMotionMask |
                    LeaveWindowMask;
    fa.cursor = cur_normal;
    c->frame = XCreateWindow(dpy, root, c->x, c->y, c->w, c->h + TH, BORDER,
                             CopyFromParent, InputOutput, CopyFromParent,
                             CWBackPixel | CWBorderPixel |
                             CWEventMask | CWCursor, &fa);

    XSelectInput(dpy, w, PropertyChangeMask | FocusChangeMask);
    XSetWindowBorderWidth(dpy, w, 0);
    XAddToSaveSet(dpy, w);
    XReparentWindow(dpy, w, c->frame, 0, TH);
    grabbuttons(c);

    c->next = clients;
    clients = c;
    setcard(w, A[NetWMDesk], c->desk);
    setframeextents(c);
    updateclientlist();

    if (hasstate(w, A[NetMaxV]) && hasstate(w, A[NetMaxH]) && !startup)
        setmaximized(c, 1);
    if (hasstate(w, A[NetFullscreen]))
        setfullscreen(c, 1);
    applygeom(c);
    XMapWindow(dpy, w);

    /* --- the no-focus-stealing rule --- */
    wantfocus = 0;
    if (!startup && visible(c)) {
        if (!sel)
            wantfocus = FOCUS_NEW_IF_NONE;
        else if (FOCUS_TRANSIENTS && parent && parent == sel)
            wantfocus = 1;
    }

    if (visible(c)) {
        if (wantfocus || startup || (RAISE_NEW && !(c->fullscreen && sel))) {
            XRaiseWindow(dpy, c->frame);
        } else if (sel) {
            /* just below the focused window */
            XWindowChanges wc;
            wc.sibling = sel->frame;
            wc.stack_mode = Below;
            XConfigureWindow(dpy, c->frame, CWSibling | CWStackMode, &wc);
        }
        XMapWindow(dpy, c->frame);
        setwmstate(c, NormalState);
    } else {
        setwmstate(c, c->iconic ? IconicState : NormalState);
    }

    if (wantfocus) {
        focus(c, 1);
    } else {
        if (!startup && sel)
            c->attention = 1;            /* highlight it until visited */
        if (sel)
            mru_second(c);               /* one Alt+Tab away */
        else {
            c->mnext = mru;
            mru = c;
        }
        draw(c);
    }
    setnetstate(c);
}

/*
 * unmanage()
 *
 * Stop managing c and free it. Removes it from the client and Alt+Tab
 * lists and from any Alt+Tab cycle in progress. If the application window
 * still exists (destroyed == 0) it is put back on the root window at the
 * same on-screen position, removed from the save-set and, unless mcgregor-wm
 * is shutting down or restarting, marked Withdrawn with its mcgregor-wm
 * properties deleted. The frame is destroyed (inside a server grab so no
 * events slip in between), and if c had focus, focus passes to the most
 * recently used visible window.
 */
static void unmanage(Client *c, int destroyed)
{
    Client **p;
    int i;

    for (p = &clients; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    mru_remove(c);
    for (i = 0; i < cycn; i++)
        if (cyc[i] == c)
            cyc[i] = NULL;
    if (cycorig == c)
        cycorig = NULL;

    XGrabServer(dpy);
    if (!destroyed) {
        XSelectInput(dpy, c->win, NoEventMask);
        XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
        XReparentWindow(dpy, c->win, root, c->x + BORDER, c->y + BORDER + TH);
        XRemoveFromSaveSet(dpy, c->win);
        if (!shuttingdown) {
            setwmstate(c, WithdrawnState);
            XDeleteProperty(dpy, c->win, A[NetWMDesk]);
            XDeleteProperty(dpy, c->win, A[NetWMState]);
        }
    }
    XDestroyWindow(dpy, c->frame);
    XSync(dpy, False);
    XUngrabServer(dpy);

    if (sel == c) {
        sel = NULL;
        focusrecent();
    }
    free(c);
    updateclientlist();
}

/* ------------------------------------------------------------------ */
/* Actions (bound to keys)                                             */

/*
 * setdeskprop()
 *
 * Publish the current desktop number in the root's _NET_CURRENT_DESKTOP,
 * for pagers, panels and for restoring the desktop after a restart.
 */
static void setdeskprop(void)
{
    setcard(root, A[NetCurDesk], curdesk);
}

/*
 * view()
 *
 * Switch to desktop n (0-based). Shows the frames of the new desktop's
 * non-iconified windows first, then hides the old desktop's, which keeps
 * flicker down, updates _NET_CURRENT_DESKTOP and focuses the most recently
 * used window there. Out-of-range or same-desktop requests are ignored.
 * Bound to Alt+1..9; also used for pager requests.
 */
static void view(long n)
{
    Client *c;
    if (n < 0 || n >= NDESKS || n == curdesk)
        return;
    for (c = clients; c; c = c->next)          /* map new desk first */
        if (c->desk == n && !c->iconic)
            showclient(c, 1);
    for (c = clients; c; c = c->next)
        if (c->desk == curdesk)
            showclient(c, 0);
    curdesk = (int)n;
    setdeskprop();
    sel = NULL;
    focusrecent();
}

/*
 * viewrel()
 *
 * Switch to the desktop d steps away, wrapping around at the ends.
 * Bound to Ctrl+Alt+Left (d = -1) and Ctrl+Alt+Right (d = +1).
 */
static void viewrel(long d)
{
    view((curdesk + d + NDESKS) % NDESKS);
}

/*
 * sendto()
 *
 * Move the focused window, and any dialogs belonging to it, to desktop n:
 * update their _NET_WM_DESKTOP, hide them, and focus the most recently
 * used window left on this desktop. Bound to Alt+Shift+1..9.
 */
static void sendto(long n)
{
    Client *c = sel, *t;
    if (!c || n < 0 || n >= NDESKS || n == c->desk)
        return;
    for (t = clients; t; t = t->next)          /* dialogs travel too */
        if (t == c || t->trans == c->win) {
            t->desk = (int)n;
            setcard(t->win, A[NetWMDesk], n);
            showclient(t, 0);
        }
    sel = NULL;
    focusrecent();
}

/*
 * closeclient()
 *
 * Close c. Asks politely with WM_DELETE_WINDOW if the application
 * supports it (it may show a "save changes?" dialog); otherwise
 * disconnects the application from the X server with XKillClient, which
 * closes all its windows.
 */
static void closeclient(Client *c)
{
    if (!sendevent(c, A[WMDelete])) {
        XGrabServer(dpy);
        XKillClient(dpy, c->win);
        XSync(dpy, False);
        XUngrabServer(dpy);
    }
}

/*
 * killclient()
 *
 * Close the focused window, if any. Bound to Alt+F4.
 */
static void killclient(long unused)
{
    (void)unused;
    if (sel)
        closeclient(sel);
}

/*
 * closeboxpress()
 *
 * Handle a click on c's close box like a real button. Draws the box
 * pressed and grabs the pointer, then follows the mouse: the box shows
 * pressed only while the pointer is over it. Other events that arrive
 * meanwhile are handled normally; if the window goes away on its own the
 * loop stops. On release the box is redrawn and the window is closed only
 * if the pointer is still over the box, so sliding off cancels.
 */
static void closeboxpress(Client *c)
{
    XEvent ev;
    unsigned long bg, fg;
    int inside = 1;
    Window win = c->win;

    titlecolors(c, &bg, &fg);
    drawclosebox(c, bg, fg, 1);
    if (XGrabPointer(dpy, c->frame, False, ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime) != GrabSuccess)
        return;
    for (;;) {
        XMaskEvent(dpy, ButtonReleaseMask | PointerMotionMask | ExposureMask |
                   SubstructureRedirectMask, &ev);
        if (ev.type == ButtonRelease)
            break;
        if (ev.type == MotionNotify) {
            int now = inclosebox(c, ev.xmotion.x, ev.xmotion.y);
            if (now != inside) {
                inside = now;
                titlecolors(c, &bg, &fg);
                drawclosebox(c, bg, fg, inside);
            }
        } else {
            handle(&ev);
            if (!clientof(win)) {           /* it went away meanwhile */
                XUngrabPointer(dpy, CurrentTime);
                return;
            }
        }
    }
    XUngrabPointer(dpy, CurrentTime);
    inside = inclosebox(c, ev.xbutton.x, ev.xbutton.y);
    titlecolors(c, &bg, &fg);
    drawclosebox(c, bg, fg, 0);
    if (inside)
        closeclient(c);
}

/*
 * setfullscreen()
 *
 * Turn fullscreen on or off for c. Turning it on saves the current
 * geometry (unless the window is maximized, whose saved geometry is kept)
 * and the window's monitor, and raises it; the frame then covers that
 * monitor with the title bar and border hidden. Turning it off goes back
 * to the maximized geometry if the window was maximized, otherwise to the
 * saved geometry. Updates the frame, _NET_FRAME_EXTENTS, _NET_WM_STATE and
 * the title bar.
 */
static void setfullscreen(Client *c, int on)
{
    if (on == c->fullscreen)
        return;
    if (on) {
        if (!c->maximized) {
            c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
            updatemons();
            c->mon = clientmon(c);
        }
        c->fullscreen = 1;
        raiseclient(c);
    } else {
        c->fullscreen = 0;
        if (c->maximized) {
            maxgeom(c);
        } else {
            c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        }
    }
    applygeom(c);
    setframeextents(c);
    setnetstate(c);
    draw(c);
}

/*
 * togglefullscreen()
 *
 * Toggle fullscreen on the focused window. Bound to Alt+F11.
 */
static void togglefullscreen(long unused)
{
    (void)unused;
    if (sel)
        setfullscreen(sel, !sel->fullscreen);
}

/*
 * setmaximized()
 *
 * Turn maximize on or off for c (ignored while fullscreen). On: save the
 * current geometry, refresh the monitor list, and fill the window's monitor
 * with the frame. Off: restore the saved geometry. Updates the frame and
 * _NET_WM_STATE.
 */
static void setmaximized(Client *c, int on)
{
    if (c->fullscreen || on == c->maximized)
        return;
    if (on) {
        c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
        updatemons();
        c->mon = clientmon(c);
        maxgeom(c);
        c->maximized = 1;
    } else {
        c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        c->maximized = 0;
    }
    applygeom(c);
    setnetstate(c);
}

/*
 * togglemax()
 *
 * Toggle maximize on the focused window. Bound to Alt+F10; double-clicking
 * the title bar does the same.
 */
static void togglemax(long unused)
{
    (void)unused;
    if (sel)
        setmaximized(sel, !sel->maximized);
}

/*
 * movemon()
 *
 * Send the focused window to the next (dir = +1) or previous (dir = -1)
 * monitor in left-to-right order, wrapping around. Its position is scaled
 * so it keeps the same relative place on the new monitor. A maximized or
 * fullscreen window gets its saved (restore) geometry moved and is refitted
 * to the new monitor, so it stays maximized or fullscreen there. Bound to
 * Alt+Shift+Left/Right.
 */
static void movemon(long dir)
{
    Client *c = sel;
    Mon *from, *to;
    int m, nm;
    if (!c)
        return;
    updatemons();
    if (nmons < 2)
        return;
    if (c->mon >= nmons)
        c->mon = nmons - 1;
    m = (c->fullscreen || c->maximized) ? c->mon : clientmon(c);
    nm = (m + (int)dir + nmons) % nmons;
    from = &mons[m];
    to = &mons[nm];
    if (c->fullscreen || c->maximized) {
        /* move the restore geometry, then refit */
        c->sx = to->x + (c->sx - from->x) * to->w / from->w;
        c->sy = to->y + (c->sy - from->y) * to->h / from->h;
        c->mon = nm;
        if (c->maximized && !c->fullscreen)
            maxgeom(c);
    } else {
        c->x = to->x + (c->x - from->x) * to->w / from->w;
        c->y = to->y + (c->y - from->y) * to->h / from->h;
        clamptomon(c, nm);
        c->mon = nm;
    }
    applygeom(c);
    raiseclient(c);
}

/*
 * iconify()
 *
 * Iconify (minimize) c: hide its frame, mark it iconic (WM_STATE Iconic,
 * _NET_WM_STATE_HIDDEN) and, if it had focus, focus the most recently
 * used visible window. There is no icon; Alt+Tab brings it back.
 */
static void iconify(Client *c)
{
    c->iconic = 1;
    showclient(c, 0);
    setnetstate(c);
    if (sel == c) {
        sel = NULL;
        focusrecent();
    }
}

/*
 * deiconify()
 *
 * Un-iconify c: clear the iconic flag, show its frame if it is on the
 * current desktop, and update _NET_WM_STATE.
 */
static void deiconify(Client *c)
{
    c->iconic = 0;
    if (c->desk == curdesk)
        showclient(c, 1);
    setnetstate(c);
}

/*
 * iconifykey()
 *
 * Iconify the focused window. Bound to Alt+F9.
 */
static void iconifykey(long unused)
{
    (void)unused;
    if (sel)
        iconify(sel);
}

/*
 * spawnterm()
 *
 * Open a terminal (the TERMINAL command, x-terminal-emulator by default).
 * Bound to Alt+Return.
 */
static void spawnterm(long unused)
{
    (void)unused;
    spawn(TERMINAL);
}

/*
 * restart()
 *
 * Ask the main loop to restart mcgregor-wm in place: set the restarting flag
 * and stop the loop. main() then hands all windows back to X and re-execs
 * the binary in the same process. Bound to Ctrl+Alt+Shift+R.
 */
static void restart(long unused)
{
    (void)unused;
    restarting = 1;
    running = 0;
}

/*
 * quit()
 *
 * Ask the main loop to stop; main() then hands all windows back to X and
 * exits. Bound to Ctrl+Alt+Shift+Q.
 */
static void quit(long unused)
{
    (void)unused;
    running = 0;
}

/*
 * activate()
 *
 * Bring c to the user: switch to its desktop if needed, un-iconify it,
 * raise it and focus it. Used for activation requests from pagers and
 * tools (not from the application itself).
 */
static void activate(Client *c)
{
    if (c->desk != curdesk)
        view(c->desk);
    if (c->iconic)
        deiconify(c);
    raiseclient(c);
    focus(c, 1);
}

/* --- Alt+Tab, most recently used first --- */

/*
 * modheld()
 *
 * Return 1 if the Alt key (MODKEY) is held down right now, by asking the
 * server for the current modifier state. Used to catch an Alt+Tab where
 * Alt was released before mcgregor-wm grabbed the keyboard.
 */
static int modheld(void)
{
    Window r, ch;
    int rx, ry, wx, wy;
    unsigned int mask = 0;
    XQueryPointer(dpy, root, &r, &ch, &rx, &ry, &wx, &wy, &mask);
    return (mask & MODKEY) != 0;
}

/*
 * cyclepreview()
 *
 * Show the window currently selected in an Alt+Tab cycle: un-iconify it
 * temporarily if needed (remembered in cycunhid so it can be hidden again
 * if not chosen), raise it and focus it without changing the Alt+Tab order
 * yet.
 */
static void cyclepreview(void)
{
    Client *c = cyc[cyci];
    if (!c)
        return;
    if (c->iconic) {
        deiconify(c);
        c->cycunhid = 1;
    }
    raiseclient(c);
    focus(c, 0);     /* don't touch MRU until Alt is released */
}

/*
 * cycleend()
 *
 * Finish an Alt+Tab cycle. With commit set the selected window is chosen;
 * otherwise (Escape) the window focused before the cycle is restored.
 * Releases the keyboard grab, re-iconifies windows that were only shown as
 * previews, then raises and focuses the chosen window and moves it to the
 * front of the Alt+Tab list. If WARP_ON_ALTTAB is on and the cycle was
 * committed, the mouse pointer is moved to the centre of the chosen window
 * (or of its monitor, if fullscreen). If the chosen window has gone, the
 * most recent visible window is focused instead.
 */
static void cycleend(int commit)
{
    Client *chosen = commit ? cyc[cyci] : cycorig;
    int i;

    XUngrabKeyboard(dpy, CurrentTime);
    cycling = 0;
    for (i = 0; i < cycn; i++)
        if (cyc[i] && cyc[i]->cycunhid) {
            cyc[i]->cycunhid = 0;
            if (cyc[i] != chosen)
                iconify(cyc[i]);
        }
    if (chosen && visible(chosen)) {
        raiseclient(chosen);
        focus(chosen, 1);
        if (WARP_ON_ALTTAB && commit) {
            int fw = chosen->fullscreen ? mons[chosen->mon].w : chosen->w;
            int fh = chosen->fullscreen ? mons[chosen->mon].h : chosen->h;
            XWarpPointer(dpy, None, chosen->win, 0, 0, 0, 0, fw / 2, fh / 2);
        }
    } else {
        focusrecent();
    }
    cycn = 0;
}

/*
 * cyclestep()
 *
 * Move the Alt+Tab selection one step in direction dir (+1 forward, -1
 * back), wrapping around and skipping entries whose window has closed
 * during the cycle, then preview the new selection.
 */
static void cyclestep(long dir)
{
    int tries;
    if (!cycn)
        return;
    for (tries = 0; tries < cycn; tries++) {
        cyci = (cyci + (int)dir + cycn) % cycn;
        if (cyc[cyci])
            break;
    }
    cyclepreview();
}

/*
 * cycle()
 *
 * Alt+Tab / Alt+Shift+Tab. If a cycle is already running, just step.
 * Otherwise start one: collect this desktop's windows (iconified ones
 * included) in most-recently-used order, remember the current focus for
 * Escape, and grab the keyboard so the Alt release can be seen. The first
 * step goes from the focused window to the next most recent one (or to the
 * first/last when nothing is focused). If the keyboard can't be grabbed,
 * or Alt was already released (a very quick tap), the switch is done at
 * once.
 */
static void cycle(long dir)
{
    Client *c;

    if (cycling) {
        cyclestep(dir);
        return;
    }
    cycn = 0;
    for (c = mru; c && cycn < (int)LENGTH(cyc); c = c->mnext)
        if (c->desk == curdesk)
            cyc[cycn++] = c;
    if (cycn == 0)
        return;
    cycorig = sel;
    /* start from the focused window (first in MRU when there is one) */
    cyci = (sel && cyc[0] == sel) ? 0 : (dir > 0 ? -1 : 0);
    if (cyci < 0)
        cyci = cycn - 1;   /* so that the step below lands on index 0 */
    if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime)
        != GrabSuccess) {
        /* can't watch for the Alt release: do a single switch */
        cyclestep(dir);
        cycleend(1);
        return;
    }
    cycling = 1;
    cyclestep(dir);
    if (!modheld())          /* Alt already released (very quick tap) */
        cycleend(1);
}

/* ------------------------------------------------------------------ */
/* Key bindings                                                        */

#define DESKKEYS(K, N) \
    { MODKEY, K, view, N }, { MODKEY | ShiftMask, K, sendto, N },

static const Key keys[] = {
    { MODKEY,                         XK_Tab,       cycle,            +1 },
    { MODKEY | ShiftMask,             XK_Tab,       cycle,            -1 },
    { MODKEY | ControlMask,           XK_Left,      viewrel,          -1 },
    { MODKEY | ControlMask,           XK_Right,     viewrel,          +1 },
    { MODKEY | ShiftMask,             XK_Left,      movemon,          -1 },
    { MODKEY | ShiftMask,             XK_Right,     movemon,          +1 },
    { MODKEY,                         XK_F4,        killclient,        0 },
    { MODKEY,                         XK_F9,        iconifykey,        0 },
    { MODKEY,                         XK_F10,       togglemax,         0 },
    { MODKEY,                         XK_F11,       togglefullscreen,  0 },
    { MODKEY,                         XK_Return,    spawnterm,         0 },
    { MODKEY | ControlMask | ShiftMask, XK_q,       quit,              0 },
    { MODKEY | ControlMask | ShiftMask, XK_r,       restart,           0 },
    DESKKEYS(XK_1, 0) DESKKEYS(XK_2, 1) DESKKEYS(XK_3, 2)
    DESKKEYS(XK_4, 3) DESKKEYS(XK_5, 4) DESKKEYS(XK_6, 5)
    DESKKEYS(XK_7, 6) DESKKEYS(XK_8, 7) DESKKEYS(XK_9, 8)
};

/*
 * updatenumlockmask()
 *
 * Find which modifier bit (Mod1-Mod5) Num Lock is mapped to, so key and
 * button handling can ignore it: a shortcut must work whether Num Lock is
 * on or off.
 */
static void updatenumlockmask(void)
{
    int i, j;
    XModifierKeymap *mm = XGetModifierMapping(dpy);
    KeyCode nl = XKeysymToKeycode(dpy, XK_Num_Lock);
    numlockmask = 0;
    for (i = 0; i < 8; i++)
        for (j = 0; j < mm->max_keypermod; j++)
            if (nl && mm->modifiermap[i * mm->max_keypermod + j] == nl)
                numlockmask = 1u << i;
    XFreeModifiermap(mm);
}

/*
 * grabkeys()
 *
 * Register mcgregor-wm's keyboard shortcuts (the keys[] table) as passive
 * grabs on the root window, so the server delivers them to mcgregor-wm
 * whatever window has focus. Each key is grabbed four times: with and
 * without Caps Lock and Num Lock. Desktop keys beyond NDESKS and keys with
 * no keycode on this keyboard are skipped. Called at start-up and when the
 * keyboard mapping changes.
 */
static void grabkeys(void)
{
    unsigned int i, j, mods[4];
    KeyCode kc;

    updatenumlockmask();
    mods[0] = 0;
    mods[1] = LockMask;
    mods[2] = numlockmask;
    mods[3] = numlockmask | LockMask;
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    for (i = 0; i < LENGTH(keys); i++) {
        if ((keys[i].fn == view || keys[i].fn == sendto) && keys[i].arg >= NDESKS)
            continue;
        if (!(kc = XKeysymToKeycode(dpy, keys[i].sym)))
            continue;
        for (j = 0; j < 4; j++)
            XGrabKey(dpy, kc, keys[i].mod | mods[j], root, True,
                     GrabModeAsync, GrabModeAsync);
    }
}

/* ------------------------------------------------------------------ */
/* Mouse move / resize                                                 */

/*
 * snap()
 *
 * Snap c to the edges of its monitor while it is being moved: any frame
 * edge within SNAP pixels of the matching monitor edge is moved onto it.
 * Disabled when SNAP is 0.
 */
static void snap(Client *c)
{
    int fw = c->w + 2 * BORDER, fh = c->h + TH + 2 * BORDER;
    Mon *M;
    if (!SNAP)
        return;
    M = &mons[clientmon(c)];
    if (abs(c->x - M->x) < SNAP) c->x = M->x;
    if (abs(c->y - M->y) < SNAP) c->y = M->y;
    if (abs(M->x + M->w - (c->x + fw)) < SNAP) c->x = M->x + M->w - fw;
    if (abs(M->y + M->h - (c->y + fh)) < SNAP) c->y = M->y + M->h - fh;
}

/*
 * edgeat()
 *
 * Return which edges of c's frame the point (x, y) is on, as a bit mask
 * of EdgeL/EdgeR/EdgeT/EdgeB. (x, y) is relative to the inside of the
 * frame, so the border area has x < 0, x >= width, y < 0 or y >= height.
 * Within CORNER pixels of the end of an edge both directions are set, so
 * corners resize diagonally. Returns 0 inside the frame (the title bar)
 * and always 0 for fullscreen windows.
 */
static int edgeat(Client *c, int x, int y)
{
    int fw = c->w, fh = c->h + TH, e = 0;
    if (c->fullscreen)
        return 0;
    if (x < 0) e |= EdgeL;
    else if (x >= fw) e |= EdgeR;
    if (y < 0) e |= EdgeT;
    else if (y >= fh) e |= EdgeB;
    if (!e)
        return 0;
    /* near a corner along an edge: resize both ways */
    if (e & (EdgeL | EdgeR)) {
        if (y < CORNER) e |= EdgeT;
        else if (y >= fh - CORNER) e |= EdgeB;
    }
    if (e & (EdgeT | EdgeB)) {
        if (x < CORNER) e |= EdgeL;
        else if (x >= fw - CORNER) e |= EdgeR;
    }
    return e;
}

/*
 * dragloop()
 *
 * Move or resize c with the mouse until the button is released. edges = 0
 * moves; otherwise it is the EdgeL/R/T/B mask of the edges being dragged.
 * The pointer is grabbed with a matching cursor, and a maximized window
 * stops being maximized. Each motion event (limited to 60 per second)
 * computes the offset from the starting point (px, py):
 *  - move: the frame follows the pointer and snaps to monitor edges;
 *  - resize: dragged edges grow or shrink, size hints are applied, and
 *    for left/top edges the frame moves so the opposite side stays put.
 * Configure requests, map requests and exposes that arrive meanwhile are
 * handled normally. The final geometry is applied on release.
 */
static void dragloop(Client *c, int edges, int px, int py)
{
    XEvent ev;
    int ox = c->x, oy = c->y, ow = c->w, oh = c->h;
    Time last = 0;

    if (c->fullscreen)
        return;
    if (XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask, GrabModeAsync, GrabModeAsync, None,
                     edges ? cur_edge[edges] : cur_move, CurrentTime) != GrabSuccess)
        return;
    if (c->maximized) {
        c->maximized = 0;
        setnetstate(c);
    }
    for (;;) {
        XMaskEvent(dpy, ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                   ExposureMask | SubstructureRedirectMask, &ev);
        if (ev.type == ButtonRelease)
            break;
        if (ev.type != MotionNotify) {
            if (ev.type != ButtonPress)
                handle(&ev);
            continue;
        }
        if (ev.xmotion.time - last < 1000 / 60)
            continue;
        last = ev.xmotion.time;
        if (edges) {
            int dx = ev.xmotion.x_root - px, dy = ev.xmotion.y_root - py;
            int w = ow, h = oh;
            if (edges & EdgeR) w = ow + dx;
            if (edges & EdgeL) w = ow - dx;
            if (edges & EdgeB) h = oh + dy;
            if (edges & EdgeT) h = oh - dy;
            applyhints(c, &w, &h);
            if (w == c->w && h == c->h)
                continue;
            /* left/top edges move the frame so the opposite side stays put */
            c->x = (edges & EdgeL) ? ox + ow - w : ox;
            c->y = (edges & EdgeT) ? oy + oh - h : oy;
            c->w = w;
            c->h = h;
        } else {
            c->x = ox + ev.xmotion.x_root - px;
            c->y = oy + ev.xmotion.y_root - py;
            snap(c);
        }
        applygeom(c);
        if (edges)
            draw(c);
    }
    XUngrabPointer(dpy, CurrentTime);
    applygeom(c);
}

/* ------------------------------------------------------------------ */
/* Event handlers                                                      */

/*
 * buttonpress()
 *
 * Handle a mouse button press. Three cases:
 *  - Click on an application window (from the synchronous passive grab):
 *    with Alt, button 1 moves and button 3 resizes from the nearest corner;
 *    without Alt, the window is focused and raised and the click is
 *    replayed to the application. Any open Alt+Tab cycle is accepted first.
 *  - Click on the frame border: buttons 1 or 3 resize from that edge or
 *    corner.
 *  - Click on the title bar: button 1 on the close box closes; button 2
 *    lowers; button 1 elsewhere focuses, raises and moves (a double click
 *    within 300 ms toggles maximize); button 3 resizes from the
 *    bottom-right.
 * Unknown windows get their click replayed so the pointer never stays
 * frozen.
 */
static void buttonpress(XEvent *e)
{
    XButtonEvent *ev = &e->xbutton;
    Client *c = wintoclient(ev->window);
    static Time lastclick;
    static Window lastwin;

    if (!c) {
        /* never leave the pointer frozen by our synchronous grab */
        XAllowEvents(dpy, ReplayPointer, CurrentTime);
        return;
    }
    if (ev->window == c->win) {
        /* our passive grab on the client */
        if ((CLEANMASK(ev->state) & MODKEY) && (ev->button == Button1 ||
                                                ev->button == Button3)) {
            XAllowEvents(dpy, AsyncPointer, CurrentTime);
            if (cycling)
                cycleend(1);
            raiseclient(c);
            focus(c, 1);
            if (ev->button == Button3) {
                /* resize from the corner nearest the pointer */
                int e = (ev->x < c->w / 2 ? EdgeL : EdgeR) |
                        (ev->y < c->h / 2 ? EdgeT : EdgeB);
                dragloop(c, e, ev->x_root, ev->y_root);
            } else {
                dragloop(c, 0, ev->x_root, ev->y_root);
            }
            return;
        }
        if (c != sel) {
            if (cycling)
                cycleend(1);
            raiseclient(c);
            focus(c, 1);
        } else {
            raiseclient(c);
        }
        XAllowEvents(dpy, ReplayPointer, CurrentTime);
        return;
    }
    /* the frame: its border (edges) or the title bar */
    if (cycling)
        cycleend(1);
    if ((ev->button == Button1 || ev->button == Button3) && edgeat(c, ev->x, ev->y)) {
        raiseclient(c);
        focus(c, 1);
        dragloop(c, edgeat(c, ev->x, ev->y), ev->x_root, ev->y_root);
        return;
    }
    if (ev->button == Button2) {
        XLowerWindow(dpy, c->frame);
        return;
    }
    if (ev->button == Button1 && inclosebox(c, ev->x, ev->y)) {
        closeboxpress(c);
        return;
    }
    raiseclient(c);
    focus(c, 1);
    if (ev->button == Button1) {
        if (lastwin == c->frame && ev->time - lastclick < 300) {
            lastclick = 0;
            setmaximized(c, !c->maximized);
            return;
        }
        lastclick = ev->time;
        lastwin = c->frame;
        dragloop(c, 0, ev->x_root, ev->y_root);
    } else if (ev->button == Button3) {
        dragloop(c, EdgeR | EdgeB, ev->x_root, ev->y_root);
    }
}

/* Cursor cache for motionnotify(): the frame and edge the cursor was last
 * set for, so the cursor is only changed when it needs to be. */
static Window lastwin;
static int lastedge = -1;

/*
 * motionnotify()
 *
 * Set the mouse cursor while the pointer moves over a frame: the matching
 * resize arrow over an edge or corner, the normal arrow over the title
 * bar. Only calls the server when the edge under the pointer changes.
 */
static void motionnotify(XEvent *e)
{
    XMotionEvent *ev = &e->xmotion;
    Client *c;
    int edge;

    if (!(c = wintoclient(ev->window)) || ev->window != c->frame)
        return;
    edge = edgeat(c, ev->x, ev->y);
    if (edge == lastedge && lastwin == c->frame)
        return;
    lastedge = edge;
    lastwin = c->frame;
    XDefineCursor(dpy, c->frame, edge ? cur_edge[edge] : cur_normal);
}

/*
 * leavenotify()
 *
 * When the pointer leaves a frame, reset its cursor to the normal arrow
 * and forget the cached edge, so the next entry sets the cursor again.
 */
static void leavenotify(XEvent *e)
{
    Client *c = wintoclient(e->xcrossing.window);
    if (c && e->xcrossing.window == c->frame) {
        XDefineCursor(dpy, c->frame, cur_normal);
        lastedge = -1;
    }
}

/*
 * keypress()
 *
 * Handle a key press from the grabbed shortcuts. During an Alt+Tab cycle:
 * Tab or Shift+Tab steps, Escape cancels, modifier keys are ignored, and
 * any other key accepts the current choice and is then handled as a
 * normal shortcut. Otherwise the key and its modifiers (ignoring Caps Lock
 * and Num Lock) are looked up in keys[] and the matching action is run.
 */
static void keypress(XEvent *e)
{
    XKeyEvent *ev = &e->xkey;
    KeySym sym = XLookupKeysym(ev, 0);
    unsigned int i;

    if (cycling) {
        if (sym == XK_Tab
#ifdef XK_ISO_Left_Tab
            || sym == XK_ISO_Left_Tab
#endif
            ) {
            cyclestep((ev->state & ShiftMask) ? -1 : +1);
            return;
        }
        if (sym == XK_Escape) {
            cycleend(0);
            return;
        }
        if (sym == XK_Alt_L || sym == XK_Alt_R || sym == XK_Meta_L ||
            sym == XK_Meta_R || sym == XK_Shift_L || sym == XK_Shift_R)
            return;
        cycleend(1);   /* any other key: accept, then handle it as usual */
    }
    for (i = 0; i < LENGTH(keys); i++)
        if (sym == keys[i].sym && CLEANMASK(keys[i].mod) == CLEANMASK(ev->state)) {
            keys[i].fn(keys[i].arg);
            return;
        }
}

/*
 * keyrelease()
 *
 * End an Alt+Tab cycle when Alt (or Meta) is released, choosing the
 * selected window.
 */
static void keyrelease(XEvent *e)
{
    KeySym sym = XLookupKeysym(&e->xkey, 0);
    if (cycling && (sym == XK_Alt_L || sym == XK_Alt_R || sym == XK_Meta_L ||
                    sym == XK_Meta_R))
        cycleend(1);
}

/*
 * maprequest()
 *
 * An application asked to show a window. If it is already managed (an
 * iconified window being restored by the app), it is un-iconified.
 * Otherwise, unless it is override-redirect (menus, tooltips) or already
 * gone, it is managed.
 */
static void maprequest(XEvent *e)
{
    XMapRequestEvent *ev = &e->xmaprequest;
    XWindowAttributes wa;
    Client *c = clientof(ev->window);

    if (c) {
        /* an iconified or hidden window asked to be shown again */
        if (c->iconic)
            deiconify(c);
        return;
    }
    if (!XGetWindowAttributes(dpy, ev->window, &wa) || wa.override_redirect)
        return;
    manage(ev->window, &wa, 0);
}

/*
 * configurerequest()
 *
 * An application asked to move, resize or restack a window.
 *  - Unmanaged windows get exactly what they asked for.
 *  - Fullscreen windows are only told their current geometry.
 *  - Managed windows: the requested position is taken as the client's,
 *    so the frame goes around it; the size is clamped to the size hints
 *    and clears maximize. Restacking is honoured only for the focused
 *    window, so a background window can't jump in front of the one you're
 *    using. The result is applied and the frame redrawn.
 */
static void configurerequest(XEvent *e)
{
    XConfigureRequestEvent *ev = &e->xconfigurerequest;
    Client *c = clientof(ev->window);
    XWindowChanges wc;

    if (!c) {
        wc.x = ev->x;
        wc.y = ev->y;
        wc.width = ev->width;
        wc.height = ev->height;
        wc.border_width = ev->border_width;
        wc.sibling = ev->above;
        wc.stack_mode = ev->detail;
        XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
        return;
    }
    if (c->fullscreen) {
        sendconfigure(c);
        return;
    }
    if (ev->value_mask & CWX) c->x = ev->x - BORDER;
    if (ev->value_mask & CWY) c->y = ev->y - BORDER - TH;
    if (ev->value_mask & (CWWidth | CWHeight)) {
        int w = (ev->value_mask & CWWidth) ? ev->width : c->w;
        int h = (ev->value_mask & CWHeight) ? ev->height : c->h;
        applyhints(c, &w, &h);
        c->w = w;
        c->h = h;
        c->maximized = 0;
    }
    /* Stacking requests are honoured only for the focused window, so a
     * background window can't push itself in front of you. */
    if ((ev->value_mask & CWStackMode) && c == sel) {
        if (ev->detail == Above)
            raiseclient(c);
        else if (ev->detail == Below)
            XLowerWindow(dpy, c->frame);
    }
    applygeom(c);
    draw(c);
}

/*
 * unmapnotify()
 *
 * A window was unmapped. If it is a managed application window and the
 * unmap is real (seen on its frame, or the synthetic unmap ICCCM asks
 * clients to send when withdrawing), the window is unmanaged. Unmaps that
 * mcgregor-wm causes itself (hiding frames, reparenting at start-up) arrive
 * differently and are ignored.
 */
static void unmapnotify(XEvent *e)
{
    XUnmapEvent *ev = &e->xunmap;
    Client *c = clientof(ev->window);

    if (!c)
        return;
    /* Real withdrawals show up as UnmapNotify on the frame (the client's
     * parent), or as the synthetic event ICCCM asks clients to send. */
    if (ev->event == c->frame || (ev->send_event && ev->event == root))
        unmanage(c, 0);
}

/*
 * destroynotify()
 *
 * A managed application window was destroyed: unmanage it, without
 * trying to touch the window that no longer exists.
 */
static void destroynotify(XEvent *e)
{
    Client *c = clientof(e->xdestroywindow.window);
    if (c)
        unmanage(c, 1);
}

/*
 * propertynotify()
 *
 * A property changed on an application window. Title changes redraw the
 * title bar; WM_HINTS changes may set or clear urgency (an urgent window
 * turns orange and moves second in the Alt+Tab list); size-hint,
 * protocol and transient-for changes are re-read. Deleted properties are
 * ignored.
 */
static void propertynotify(XEvent *e)
{
    XPropertyEvent *ev = &e->xproperty;
    Client *c;

    if (ev->state == PropertyDelete || !(c = clientof(ev->window)))
        return;
    if (ev->atom == XA_WM_NAME || ev->atom == A[NetWMName]) {
        updatetitle(c);
        draw(c);
    } else if (ev->atom == XA_WM_HINTS) {
        int was = c->attention;
        updatewmhints(c);
        if (c->attention != was) {
            if (c->attention)
                mru_second(c);
            setnetstate(c);
            draw(c);
        }
    } else if (ev->atom == XA_WM_NORMAL_HINTS) {
        updatesizehints(c);
    } else if (ev->atom == A[WMProtocols]) {
        updateprotocols(c);
    } else if (ev->atom == XA_WM_TRANSIENT_FOR) {
        Window tr = None;
        XGetTransientForHint(dpy, c->win, &tr);
        c->trans = tr == c->win ? None : tr;
    }
}

/*
 * netstate()
 *
 * Apply one _NET_WM_STATE change to c. action is 0 (remove), 1 (add) or
 * 2 (toggle) and a is the state: fullscreen, maximized (vertical or
 * horizontal both mean full maximize) or demands-attention. The focused
 * window never gets the attention flag.
 */
static void netstate(Client *c, long action, Atom a)
{
    /* action: 0 remove, 1 add, 2 toggle */
    if (a == A[NetFullscreen])
        setfullscreen(c, action == 2 ? !c->fullscreen : (int)action);
    else if (a == A[NetMaxV] || a == A[NetMaxH])
        setmaximized(c, action == 2 ? !c->maximized : (int)action);
    else if (a == A[NetAttention]) {
        int on = action == 2 ? !c->attention : (int)action;
        if (c == sel)
            on = 0;
        if (on && !c->attention)
            mru_second(c);
        c->attention = on;
        setnetstate(c);
        draw(c);
    }
}

/*
 * clientmessage()
 *
 * Handle EWMH/ICCCM requests sent to the root window by applications,
 * pagers and tools such as wmctrl or xdotool:
 *  - _NET_CURRENT_DESKTOP: switch desktop.
 *  - _NET_ACTIVE_WINDOW: honoured when it comes from a pager or tool
 *    (source 2); from the application itself (source 0 or 1) it counts as
 *    focus stealing, so the window is only marked for attention.
 *  - _NET_CLOSE_WINDOW: close the window.
 *  - _NET_WM_STATE: add/remove/toggle up to two states.
 *  - _NET_WM_DESKTOP: move the window to another desktop.
 *  - WM_CHANGE_STATE to IconicState: iconify (an app's own minimize).
 */
static void clientmessage(XEvent *e)
{
    XClientMessageEvent *ev = &e->xclient;
    Client *c = clientof(ev->window);

    if (ev->message_type == A[NetCurDesk]) {
        view(ev->data.l[0]);
        return;
    }
    if (!c)
        return;
    if (ev->message_type == A[NetActive]) {
        /* Source indication 2 = a pager or tool acting for the user:
         * do it. 0/1 = the application itself asking for focus: that's
         * focus stealing, so only flag the window. */
        if (ev->data.l[0] == 2) {
            activate(c);
        } else if (c != sel) {
            c->attention = 1;
            mru_second(c);
            setnetstate(c);
            draw(c);
        }
    } else if (ev->message_type == A[NetClose]) {
        if (!sendevent(c, A[WMDelete]))
            XKillClient(dpy, c->win);
    } else if (ev->message_type == A[NetWMState]) {
        netstate(c, ev->data.l[0], (Atom)ev->data.l[1]);
        if (ev->data.l[2] && (Atom)ev->data.l[2] != (Atom)ev->data.l[1])
            netstate(c, ev->data.l[0], (Atom)ev->data.l[2]);
    } else if (ev->message_type == A[NetWMDesk]) {
        long n = ev->data.l[0];
        if (n >= 0 && n < NDESKS && n != c->desk) {
            c->desk = (int)n;
            setcard(c->win, A[NetWMDesk], n);
            showclient(c, visible(c));
            if (sel == c) {
                sel = NULL;
                focusrecent();
            }
        }
    } else if (ev->message_type == A[WMChangeState] && ev->data.l[0] == IconicState) {
        iconify(c);
    }
}

/*
 * focusin()
 *
 * Catch applications moving the keyboard focus by themselves (calling
 * XSetInputFocus). Focus changes caused by grabs or pointer-root focus are
 * ignored, as are FocusIn events for the window mcgregor-wm focused. If
 * another managed window took focus and it isn't a dialog of (or parent
 * of) the focused window, the focus is put back and the thief is marked
 * for attention and moved second in the Alt+Tab list. If nothing was
 * focused, or the windows are related, the new window is accepted.
 */
static void focusin(XEvent *e)
{
    XFocusChangeEvent *ev = &e->xfocus;
    Client *c;

    if (ev->mode == NotifyGrab || ev->mode == NotifyUngrab ||
        ev->detail == NotifyPointer || ev->detail == NotifyPointerRoot ||
        ev->detail == NotifyDetailNone)
        return;
    c = clientof(ev->window);
    if (!c || c == sel)
        return;
    /* A program moved the focus by itself. Allow it within the same
     * application window group (window <-> its dialogs); otherwise put the
     * focus back where the user left it. */
    if (sel && !related(c, sel)) {
        setfocus(sel);
        if (!c->attention) {
            c->attention = 1;
            mru_second(c);
            setnetstate(c);
            draw(c);
        }
    } else if (visible(c)) {
        focus(c, 1);
    }
}

/*
 * expose()
 *
 * Redraw a frame's title bar when part of it becomes visible. Only the
 * last expose of a batch (count == 0) triggers a redraw.
 */
static void expose(XEvent *e)
{
    Client *c;
    if (e->xexpose.count == 0 && (c = wintoclient(e->xexpose.window)) &&
        e->xexpose.window == c->frame)
        draw(c);
}

/*
 * rootconfigure()
 *
 * The root window changed size (a monitor was added, removed or
 * rearranged). Refresh the screen size and monitor list, then refit every
 * window: maximized and fullscreen windows to their monitor (their saved
 * restore geometry is pulled onto a monitor too), and normal windows are
 * clamped onto a monitor, so nothing is left on a monitor that's gone.
 */
static void rootconfigure(XEvent *e)
{
    Client *c;
    if (e->xconfigure.window != root)
        return;
    sw = e->xconfigure.width;
    sh = e->xconfigure.height;
    updatemons();
    for (c = clients; c; c = c->next) {
        if (c->mon >= nmons)
            c->mon = nmons - 1;
        if (c->fullscreen || c->maximized) {
            /* the saved size must fit too, for when it's restored */
            int x = c->x, y = c->y, w = c->w, h = c->h, m = c->mon;
            c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
            clamptomon(c, monat(c->x + c->w / 2, c->y + c->h / 2));
            c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
            c->x = x; c->y = y; c->w = w; c->h = h; c->mon = m;
            if (c->maximized && !c->fullscreen)
                maxgeom(c);
        } else {
            /* a window left on a monitor that went away comes back */
            clamptomon(c, clientmon(c));
        }
        applygeom(c);
    }
}

/*
 * mappingnotify()
 *
 * The keyboard mapping changed (e.g. setxkbmap): refresh Xlib's cached
 * mapping and re-grab the shortcut keys, whose keycodes may have moved.
 */
static void mappingnotify(XEvent *e)
{
    XRefreshKeyboardMapping(&e->xmapping);
    if (e->xmapping.request == MappingKeyboard ||
        e->xmapping.request == MappingModifier)
        grabkeys();
}

/*
 * handle()
 *
 * Dispatch one X event to its handler. Everything mcgregor-wm reacts to goes
 * through here, including events that arrive while a drag or close-box
 * press is in progress.
 */
static void handle(XEvent *e)
{
    switch (e->type) {
    case ButtonPress:      buttonpress(e); break;
    case MotionNotify:     motionnotify(e); break;
    case LeaveNotify:      leavenotify(e); break;
    case KeyPress:         keypress(e); break;
    case KeyRelease:       keyrelease(e); break;
    case MapRequest:       maprequest(e); break;
    case ConfigureRequest: configurerequest(e); break;
    case UnmapNotify:      unmapnotify(e); break;
    case DestroyNotify:    destroynotify(e); break;
    case PropertyNotify:   propertynotify(e); break;
    case ClientMessage:    clientmessage(e); break;
    case FocusIn:          focusin(e); break;
    case Expose:           expose(e); break;
    case ConfigureNotify:  rootconfigure(e); break;
    case MappingNotify:    mappingnotify(e); break;
    }
}

/* ------------------------------------------------------------------ */
/* Startup / shutdown                                                  */

/*
 * xerror()
 *
 * X error handler. Windows can vanish at any moment, so errors that only
 * mean "that window is already gone" (BadWindow, BadDrawable, and the
 * BadMatch/BadAccess races on focus, configure and grabs) are ignored.
 * Anything else is printed and passed to Xlib's default handler.
 */
static int xerror(Display *d, XErrorEvent *ee)
{
    /* Windows vanish all the time; these races are harmless. */
    if (ee->error_code == BadWindow ||
        (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch) ||
        (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch) ||
        (ee->request_code == X_GrabButton && ee->error_code == BadAccess) ||
        (ee->request_code == X_GrabKey && ee->error_code == BadAccess) ||
        (ee->request_code == X_ChangeWindowAttributes && ee->error_code == BadMatch) ||
        ee->error_code == BadDrawable)
        return 0;
    fprintf(stderr, "mcgregor-wm: X error request %d code %d\n",
            ee->request_code, ee->error_code);
    return xerrorxlib(d, ee);
}

/*
 * xerrorstart()
 *
 * Temporary X error handler used while selecting SubstructureRedirect on
 * the root window at start-up. Only one client may do that, so any error
 * means another window manager is running: exit with a message.
 */
static int xerrorstart(Display *d, XErrorEvent *ee)
{
    (void)d;
    (void)ee;
    die("another window manager is already running");
    return -1;
}

/*
 * sigchld()
 *
 * SIGCHLD handler: reap every finished child process (programs started
 * with Alt+Return or from autostart) so none are left as zombies.
 * Preserves errno, since it can interrupt any system call.
 */
static void sigchld(int s)
{
    int saved = errno;
    (void)s;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = saved;
}

/*
 * setupfont()
 *
 * Load the title-bar font: try each comma-separated name in FONT in turn
 * with XLoadQueryFont and keep the first that exists, falling back to
 * "fixed". Sets TH, the title bar height, to the font height plus
 * TITLE_PAD, and fasc, the ascent used to centre the text.
 */
static void setupfont(void)
{
    char name[256];
    const char *p = FONT, *comma;
    size_t len;

    while (!font && *p) {
        comma = strchr(p, ',');
        len = comma ? (size_t)(comma - p) : strlen(p);
        if (len < sizeof name) {
            memcpy(name, p, len);
            name[len] = 0;
            font = XLoadQueryFont(dpy, name);
        }
        p += len;
        if (*p == ',')
            p++;
    }
    if (!font)
        font = XLoadQueryFont(dpy, "fixed");
    if (!font)
        die("cannot load any font (is the X font path set up?)");
    fasc = font->ascent;
    TH = font->ascent + font->descent + TITLE_PAD;
}

/*
 * setup()
 *
 * One-time start-up: install the SIGCHLD handler, take over the root
 * window (SubstructureRedirect; fails if another window manager is
 * running), install the X error handler, intern atoms, read the monitor
 * layout, load the font, allocate colours, create the GC and cursors, and
 * publish EWMH support: the supporting-WM check window named "mcgregor-wm",
 * _NET_SUPPORTED, the desktop count and names. The current desktop is read
 * back from the root (so a restart keeps it). Finally the shortcut keys
 * are grabbed and focus is set to the root.
 */
static void setup(void)
{
    XSetWindowAttributes wa;
    XGCValues gv;
    long ndesks = NDESKS;
    struct sigaction sa;
    unsigned int i;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    sw = DisplayWidth(dpy, scr);
    sh = DisplayHeight(dpy, scr);
    updatemons();

    xerrorxlib = XSetErrorHandler(xerrorstart);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
                 StructureNotifyMask | PropertyChangeMask);
    XSync(dpy, False);
    XSetErrorHandler(xerror);
    XSync(dpy, False);

    for (i = 0; i < AtomLast; i++)
        A[i] = XInternAtom(dpy, atomnames[i], False);

    setupfont();
    px_active = getcolor(COL_ACTIVE);
    px_inactive = getcolor(COL_INACTIVE);
    px_attention = getcolor(COL_ATTENTION);
    px_text = getcolor(COL_TEXT);
    px_textdim = getcolor(COL_TEXT_DIM);
    px_bactive = getcolor(COL_BORDER_ACTIVE);
    px_border = getcolor(COL_BORDER);
    gv.foreground = px_text;
    gv.graphics_exposures = False;
    gv.font = font->fid;
    gc = XCreateGC(dpy, root, GCForeground | GCGraphicsExposures | GCFont, &gv);

    cur_normal = XCreateFontCursor(dpy, XC_left_ptr);
    cur_move = XCreateFontCursor(dpy, XC_fleur);
    cur_edge[EdgeL] = XCreateFontCursor(dpy, XC_left_side);
    cur_edge[EdgeR] = XCreateFontCursor(dpy, XC_right_side);
    cur_edge[EdgeT] = XCreateFontCursor(dpy, XC_top_side);
    cur_edge[EdgeB] = XCreateFontCursor(dpy, XC_bottom_side);
    cur_edge[EdgeT | EdgeL] = XCreateFontCursor(dpy, XC_top_left_corner);
    cur_edge[EdgeT | EdgeR] = XCreateFontCursor(dpy, XC_top_right_corner);
    cur_edge[EdgeB | EdgeL] = XCreateFontCursor(dpy, XC_bottom_left_corner);
    cur_edge[EdgeB | EdgeR] = XCreateFontCursor(dpy, XC_bottom_right_corner);
    wa.cursor = cur_normal;
    XChangeWindowAttributes(dpy, root, CWCursor, &wa);

    /* EWMH: identify ourselves and what we support */
    checkwin = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, checkwin, A[NetCheck], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, checkwin, A[NetWMName], A[Utf8], 8, PropModeReplace,
                    (unsigned char *)"mcgregor-wm", 11);
    XChangeProperty(dpy, root, A[NetCheck], XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, root, A[NetSupported], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&A[NetSupported], Utf8 - NetSupported);
    XChangeProperty(dpy, root, A[NetNumDesks], XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&ndesks, 1);
    {
        char names[NDESKS * 4];
        int n = 0;
        for (i = 0; i < NDESKS; i++)
            n += sprintf(names + n, "%u", i + 1) + 1;
        XChangeProperty(dpy, root, A[NetDeskNames], A[Utf8], 8, PropModeReplace,
                        (unsigned char *)names, n);
    }
    curdesk = (int)getcardprop(root, A[NetCurDesk], 0);   /* survive restarts */
    if (curdesk < 0 || curdesk >= NDESKS)
        curdesk = 0;
    setdeskprop();
    XDeleteProperty(dpy, root, A[NetClientList]);

    grabkeys();
    XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
}

/*
 * scan()
 *
 * Manage the windows that already exist when mcgregor-wm starts (after a
 * restart, or when replacing another window manager). Normal windows are
 * managed first and dialogs second, so their parents already exist; only
 * windows that are visible or iconified are taken. After a restart the
 * saved Alt+Tab order (_MCGREGOR_WM_MRU) is put back and the previously
 * focused window (_NET_ACTIVE_WINDOW) is focused again; otherwise the most
 * recent visible window is focused.
 */
static void scan(void)
{
    unsigned int i, n, pass;
    Window d1, d2, *wins = NULL;
    XWindowAttributes wa;

    if (!XQueryTree(dpy, root, &d1, &d2, &wins, &n))
        return;
    for (pass = 0; pass < 2; pass++) {       /* normal windows, then dialogs */
        for (i = 0; i < n; i++) {
            Window tr;
            int istrans;
            if (!XGetWindowAttributes(dpy, wins[i], &wa) || wa.override_redirect ||
                wins[i] == checkwin)
                continue;
            istrans = XGetTransientForHint(dpy, wins[i], &tr);
            if (istrans != (int)pass)
                continue;
            if (wa.map_state == IsViewable || getwmstate(wins[i]) == IconicState)
                manage(wins[i], &wa, 1);
        }
    }
    if (wins)
        XFree(wins);

    /* After a restart: put Alt+Tab history and the focus back. */
    {
        Atom type;
        int fmt;
        unsigned long nw = 0, after;
        unsigned char *p = NULL;
        Window active = None;
        Client *c;

        if (XGetWindowProperty(dpy, root, mruatom, 0, 4096, True, XA_WINDOW, &type, &fmt,
                               &nw, &after, &p) == Success && p) {
            Window *w = (Window *)p;
            for (i = nw; i-- > 0;)              /* oldest first, so newest ends up first */
                if ((c = clientof(w[i])))
                    mru_front(c);
            XFree(p);
        }
        p = NULL;
        if (XGetWindowProperty(dpy, root, A[NetActive], 0, 1, False, XA_WINDOW, &type, &fmt,
                               &nw, &after, &p) == Success && p) {
            if (nw)
                active = *(Window *)p;
            XFree(p);
        }
        if (active && (c = clientof(active)) && visible(c)) {
            focus(c, 1);
            return;
        }
    }
    focusrecent();
}

/*
 * autostart()
 *
 * Run ~/.mcgregor-wm/autostart in the background if it exists and is
 * executable. Called once at login, not on restarts. The path is built
 * with explicit length checks because libc5 may lack snprintf().
 */
static void autostart(void)
{
    static const char tail[] = "/.mcgregor-wm/autostart";
    char path[1024];
    const char *home = getenv("HOME");
    const char *argv[2];
    struct stat st;

    if (!home || strlen(home) + sizeof tail > sizeof path)
        return;
    strcpy(path, home);
    strcat(path, tail);
    if (stat(path, &st) == 0 && (st.st_mode & S_IXUSR)) {
        argv[0] = path;
        argv[1] = NULL;
        spawn(argv);
    }
}

/*
 * cleanup()
 *
 * Shut down cleanly so no window is lost. When restarting, the Alt+Tab
 * order is first saved in the root property _MCGREGOR_WM_MRU. Every window is
 * then unmanaged, bottom to top so the stacking order survives:
 * fullscreen windows get their normal geometry back, and each application
 * window is put back on the root where it was on screen. Finally the key
 * grabs, the check window and (unless restarting) _NET_ACTIVE_WINDOW are
 * removed and focus goes back to the pointer.
 */
static void cleanup(void)
{
    Client *c;
    Window d1, d2, *wins = NULL;
    unsigned int i, n = 0;

    shuttingdown = 1;
    if (restarting) {
        /* remember the Alt+Tab order for the next instance */
        XDeleteProperty(dpy, root, mruatom);
        for (c = mru; c; c = c->mnext)
            XChangeProperty(dpy, root, mruatom, XA_WINDOW, 32, PropModeAppend,
                            (unsigned char *)&c->win, 1);
    }
    /* Put every window back on the root so a new WM (or none) can take
     * over without losing anything. Bottom to top, so stacking survives. */
    if (XQueryTree(dpy, root, &d1, &d2, &wins, &n)) {
        for (i = 0; i < n; i++) {
            for (c = clients; c && c->frame != wins[i]; c = c->next)
                ;
            if (!c)
                continue;
            if (c->fullscreen) {
                c->fullscreen = 0;
                c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
            }
            unmanage(c, 0);
        }
        if (wins)
            XFree(wins);
    }
    while ((c = clients)) {
        if (c->fullscreen) {
            c->fullscreen = 0;
            c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        }
        unmanage(c, 0);
    }
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    XDestroyWindow(dpy, checkwin);
    if (!restarting)
        XDeleteProperty(dpy, root, A[NetActive]);
    XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
    XSync(dpy, False);
}

/*
 * sigterm()
 *
 * Handler for SIGTERM, SIGINT and SIGHUP: stop the main loop. SIGHUP also
 * sets the restarting flag, so "killall -HUP mcgregor-wm" restarts mcgregor-wm
 * in place instead of quitting.
 */
static void sigterm(int s)
{
    if (s == SIGHUP)
        restarting = 1;
    running = 0;
}

/*
 * main()
 *
 * Program entry. Accepts -v (print version) and the internal -r flag,
 * which a restarted copy is given so autostart isn't run twice. Sets the
 * locale (for UTF-8 titles), opens the display, installs the signal
 * handlers, runs setup() and scan(), then the event loop: wait in select()
 * so a signal can interrupt it, then handle every pending X event. When
 * the loop stops, cleanup() hands the windows back. On restart the
 * process re-execs the mcgregor-wm binary on disk with -r (falling back to
 * /proc/self/exe), keeping the same process id so the X session carries on
 * and a newly installed build takes effect.
 */
int main(int argc, char **argv)
{
    XEvent ev;
    struct sigaction sa;
    int restarted = 0;

    if (argc == 2 && !strcmp(argv[1], "-v")) {
        puts("mcgregor-wm 1.0");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "-r"))   /* internal: we were restarted */
        restarted = 1;
    else if (argc != 1) {
        fputs("usage: mcgregor-wm [-v]\n", stderr);
        return 1;
    }
    setlocale(LC_CTYPE, "");
    if (!XSupportsLocale())
        setlocale(LC_CTYPE, "C");
    if (!(dpy = XOpenDisplay(NULL)))
        die("cannot open display");

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    setup();
    mruatom = XInternAtom(dpy, "_MCGREGOR_WM_MRU", False);
    scan();
    if (!restarted)
        autostart();
    XSync(dpy, False);

    while (running) {
        /* Wait in select() so a SIGTERM can break us out cleanly. */
        if (!XPending(dpy)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ConnectionNumber(dpy), &fds);
            if (select(ConnectionNumber(dpy) + 1, &fds, NULL, NULL, NULL) < 0)
                continue;
            if (!XPending(dpy))
                continue;
        }
        XNextEvent(dpy, &ev);
        handle(&ev);
    }
    cleanup();
    XCloseDisplay(dpy);
    if (restarting) {
        /* Replace ourselves with a fresh copy, keeping our process id, so
         * the X session (which waits on this process) carries on. Use the
         * binary on disk, so a newly installed build takes effect. */
        char *args[3];
        args[0] = argv[0];
        args[1] = "-r";
        args[2] = NULL;
        execvp(argv[0], args);
        args[0] = "mcgregor-wm";
        execv("/proc/self/exe", args);
        perror("mcgregor-wm: restart failed");
        return 1;
    }
    return 0;
}
