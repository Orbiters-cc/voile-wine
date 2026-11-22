/*
 * wineobelisk.drv unix side: user driver backed by the Obelisk window server wsd
 *
 * One AF_UNIX connection per process. Top-level visible windows get a wsd window: a macOS titled window
 * (title bar and buttons drawn by macOS, like winemac.drv) when the Win32 window has a caption, a borderless
 * one otherwise (Wine draws any frame itself). Their window surface is copied into a shared memory buffer
 * (memfd sent with SCM_RIGHTS) and presented. A Win32 thread created by the PE side reads the wsd events and
 * injects keyboard and mouse input with NtUserSendHardwareInput; moves, resizes and minimizes made by the user
 * on the macOS window are applied to the Win32 window by its own thread (a posted driver message).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#define OEMRESOURCE

#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "ntuser.h"
#include "wine/gdi_driver.h"
#include "wine/debug.h"
#include "kbd.h"
#include "unixlib.h"
#include "wsd_protocol.h"
#include "keymap.h"

WINE_DEFAULT_DEBUG_CHANNEL(obelisk);

#define SHM_ALIGN      0x4000  /* host page; wsd maps the whole fd */
#define STRIDE_ALIGN   256     /* multiple of every Metal linear texture alignment */
#define NB_BUFFERS     2

struct obelisk_buffer
{
    UINT  id;        /* wsd buffer id, 0 when unused */
    void *bits;
    size_t map_size;
    UINT  width, height, stride;
    BOOL  busy;      /* presented, no EV_RELEASE yet */
};

struct obelisk_win
{
    HWND  hwnd;      /* 0 when the slot is free */
    BOOL  created;   /* wsd window exists */
    BOOL  shown;
    BOOL  fullscreen;
    BOOL  minimized; /* in the Dock */
    BOOL  external;  /* a D3D swapchain (nptunix) presents into this window, GDI flushes are not shown */
    UINT  style;     /* WSD_STYLE_* frame of the wsd window */
    UINT  width, height;
    int   x, y;      /* last position sent to wsd */
    RECT  window;    /* window rect in screen coordinates */
    RECT  visible;   /* visible rect in screen coordinates */
    RECT  client;    /* client rect relative to the visible rect (window pixels) */
    BOOL  frame_pending;
    RECT  pending;   /* visible rect the user gave the macOS window, applied by the window's thread */
    struct obelisk_buffer buffers[NB_BUFFERS];
};

/* driver messages, handled by OBELISK_WindowMessage in the window's thread */
enum { WM_OBELISK_APPLY_FRAME = WM_WINE_FIRST_DRIVER_MSG, WM_OBELISK_TAKE_FOCUS };

static int wsd_fd = -1;
static pthread_mutex_t write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static struct obelisk_win windows[WSD_MAX_WINDOWS];
static UINT next_buffer_id = 1;
static UINT64 present_serial;
static pthread_mutex_t display_lock = PTHREAD_MUTEX_INITIALIZER;
static struct wsd_hello_reply hello;  /* display_lock held: the screen, from EV_HELLO then EV_DISPLAY */
static pthread_cond_t display_cond = PTHREAD_COND_INITIALIZER; /* display_lock held: display_serial moved */
static UINT64 display_serial;  /* display_lock held: counts the EV_DISPLAY, and the rejections of SET_DISPLAY_MODE */
static BOOL cursor_visible = TRUE;
static const WCHAR obelisk_window_propW[] = {'_','_','w','i','n','e','_','o','b','e','l','i','s','k','_','w','i','n','d','o','w',0};
static void (*release_listener)( UINT buffer_id, UINT shown, UINT64 serial );
static void (*presented_listener)( UINT buffer_id, UINT64 serial, UINT64 presented_ns );
static BOOL debug_injected;  /* WINE_OBELISK_DEBUG_INJECT already sent */
static BOOL wsd_send( UINT type, const void *payload, UINT len, const void *extra, UINT extra_len, int fd );
static UINT relative_window;  /* wsd window in relative mouse mode, 0 when none */
static BOOL clip_active;
static float relative_rest_x, relative_rest_y;  /* fractions of a pixel not sent yet */
static UINT64 last_warp_ns;  /* last warp (SetCursorPos, end of relative mode): older wsd moves carry the old place */

static BOOL latency_trace;
static BOOL keep_foreground;  /* WINE_OBELISK_KEEP_FOREGROUND, see WSD_EV_APP_DEACTIVATED */

static UINT64 now_ns(void)
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (UINT64)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/***********************************************************************
 *           protocol helpers
 */
static BOOL write_all( const void *data, size_t size, int fd_to_send )
{
    const char *ptr = data;
    BOOL first = TRUE;

    while (size)
    {
        struct iovec iov = { (void *)ptr, size };
        char control[CMSG_SPACE(sizeof(int))];
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
        ssize_t ret;

        if (first && fd_to_send != -1)
        {
            struct cmsghdr *cmsg;
            memset( control, 0, sizeof(control) );
            msg.msg_control = control;
            msg.msg_controllen = sizeof(control);
            cmsg = CMSG_FIRSTHDR( &msg );
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy( CMSG_DATA(cmsg), &fd_to_send, sizeof(int) );
        }
        ret = sendmsg( wsd_fd, &msg, MSG_NOSIGNAL );
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            WARN( "sendmsg failed: %s\n", strerror(errno) );
            return FALSE;
        }
        first = FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

static BOOL wsd_send( UINT type, const void *payload, UINT len, const void *extra, UINT extra_len, int fd )
{
    char buf[sizeof(struct wsd_hdr) + WSD_MAX_PAYLOAD];
    struct wsd_hdr *hdr = (struct wsd_hdr *)buf;
    BOOL ret;

    if (wsd_fd == -1 || len + extra_len > WSD_MAX_PAYLOAD) return FALSE;
    hdr->type = type;
    hdr->len = len + extra_len;
    if (len) memcpy( hdr + 1, payload, len );
    if (extra_len) memcpy( (char *)(hdr + 1) + len, extra, extra_len );
    pthread_mutex_lock( &write_lock );
    ret = write_all( buf, sizeof(*hdr) + len + extra_len, fd );
    pthread_mutex_unlock( &write_lock );
    TRACE( "request %u len %u fd %d -> %d\n", type, len + extra_len, fd, ret );
    return ret;
}

static BOOL read_all( void *data, size_t size )
{
    char *ptr = data;
    while (size)
    {
        ssize_t ret = read( wsd_fd, ptr, size );
        if (ret < 0 && errno == EINTR) continue;
        if (ret <= 0) return FALSE;
        ptr += ret;
        size -= ret;
    }
    return TRUE;
}

/* reads one message; payload must hold WSD_MAX_PAYLOAD bytes */
static BOOL wsd_read( struct wsd_hdr *hdr, void *payload )
{
    if (!read_all( hdr, sizeof(*hdr) )) return FALSE;
    if (hdr->len > WSD_MAX_PAYLOAD) return FALSE;
    return read_all( payload, hdr->len );
}

/***********************************************************************
 *           window table (win_lock held)
 */
static struct obelisk_win *get_win( HWND hwnd )
{
    UINT i;
    for (i = 0; i < WSD_MAX_WINDOWS; i++) if (windows[i].hwnd == hwnd) return &windows[i];
    return NULL;
}

static struct obelisk_win *get_win_by_id( UINT id )
{
    if (!id || id > WSD_MAX_WINDOWS || !windows[id - 1].hwnd) return NULL;
    return &windows[id - 1];
}

static UINT win_id( struct obelisk_win *win ) { return win - windows + 1; }

static struct obelisk_win *alloc_win( HWND hwnd )
{
    UINT i;
    for (i = 0; i < WSD_MAX_WINDOWS; i++)
    {
        if (windows[i].hwnd) continue;
        memset( &windows[i], 0, sizeof(windows[i]) );
        windows[i].hwnd = hwnd;
        return &windows[i];
    }
    WARN( "more than %u top-level windows, %p not shown\n", WSD_MAX_WINDOWS, hwnd );
    return NULL;
}

static void free_buffer( struct obelisk_buffer *buf )
{
    if (!buf->id) return;
    wsd_send( WSD_BUFFER_DETACH, &(struct wsd_buffer){ buf->id }, sizeof(struct wsd_buffer), NULL, 0, -1 );
    munmap( buf->bits, buf->map_size );
    memset( buf, 0, sizeof(*buf) );
}

static void destroy_win( struct obelisk_win *win )
{
    UINT i;
    for (i = 0; i < NB_BUFFERS; i++) free_buffer( &win->buffers[i] );
    if (win->created)
        wsd_send( WSD_WINDOW_DESTROY, &(struct wsd_window){ win_id( win ) }, sizeof(struct wsd_window), NULL, 0, -1 );
    /* wsd leaves relative mode with its window, whose id the next window created may get */
    if (win_id( win ) == relative_window) relative_window = 0;
    memset( win, 0, sizeof(*win) );
}

static BOOL is_toplevel( HWND hwnd )
{
    return hwnd != NtUserGetDesktopWindow() && NtUserGetAncestor( hwnd, GA_PARENT ) == NtUserGetDesktopWindow();
}

/* The wsd frame of a Win32 window: a window with a caption gets the macOS title bar and the buttons its system
 * menu, minimize box and sizing border ask for (the same rule as winemac.drv); any other one is borderless. A topmost
 * window stays above the others, a WS_EX_NOACTIVATE one never takes the keyboard. */
static UINT frame_style( UINT style, UINT ex_style )
{
    UINT ret = WSD_STYLE_TITLED;

    if ((style & WS_CAPTION) != WS_CAPTION || (ex_style & WS_EX_LAYERED)) ret = WSD_STYLE_BORDERLESS;
    else
    {
        if (style & WS_SYSMENU) ret |= WSD_STYLE_CLOSABLE;
        if (style & WS_MINIMIZEBOX) ret |= WSD_STYLE_MINIATURIZABLE;
        if (style & WS_THICKFRAME) ret |= WSD_STYLE_RESIZABLE;
    }
    if (ex_style & WS_EX_TOPMOST) ret |= WSD_STYLE_TOPMOST;
    if (ex_style & WS_EX_NOACTIVATE) ret |= WSD_STYLE_NOACTIVATE;
    return ret;
}

/* creates a shared memory buffer and attaches it in wsd */
static BOOL alloc_buffer( struct obelisk_buffer *buf, UINT width, UINT height )
{
    UINT stride = (width * 4 + STRIDE_ALIGN - 1) & ~(STRIDE_ALIGN - 1);
    size_t size = ((size_t)stride * height + SHM_ALIGN - 1) & ~(size_t)(SHM_ALIGN - 1);
    struct wsd_buffer_attach attach;
    void *bits;
    int fd;

    if ((fd = syscall( __NR_memfd_create, "wsd-buffer", 1 /* MFD_CLOEXEC */ )) < 0)
    {
        ERR( "memfd_create failed: %s\n", strerror(errno) );
        return FALSE;
    }
    if (ftruncate( fd, size ) < 0 ||
        (bits = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 )) == MAP_FAILED)
    {
        ERR( "shared buffer of %zu bytes failed: %s\n", size, strerror(errno) );
        close( fd );
        return FALSE;
    }
    memset( &attach, 0, sizeof(attach) );
    attach.buffer_id = next_buffer_id++;
    attach.width = width;
    attach.height = height;
    attach.stride = stride;
    attach.format = WSD_FMT_BGRA8;
    attach.offset = 0;
    attach.size = (UINT64)stride * height;
    if (!wsd_send( WSD_BUFFER_ATTACH, &attach, sizeof(attach), NULL, 0, fd ))
    {
        close( fd );
        munmap( bits, size );
        return FALSE;
    }
    close( fd );
    buf->id = attach.buffer_id;
    buf->bits = bits;
    buf->map_size = size;
    buf->width = width;
    buf->height = height;
    buf->stride = stride;
    buf->busy = FALSE;
    TRACE( "buffer %u %ux%u stride %u\n", buf->id, width, height, stride );
    return TRUE;
}

/* win_lock released: a shown, unminimized fullscreen window, where a hidden cursor means a captured mouse */
static BOOL fullscreen_shown( UINT window_id )
{
    struct obelisk_win *win;
    BOOL ret;
    pthread_mutex_lock( &win_lock );
    ret = (win = get_win_by_id( window_id )) && win->created && win->shown && win->fullscreen && !win->minimized;
    pthread_mutex_unlock( &win_lock );
    return ret;
}

/* win_lock released. Relative (raw delta) mouse while the game hides the cursor and confines it to its window, or
 * hides it over a fullscreen window, like a captured mouse on Windows, as winewayland.drv locks the pointer
 * (wayland_pointer_update_constraint); absolute otherwise. A clip of the whole screen reaches the driver as no clip
 * (win32u input.c): without the fullscreen case, a game that hides its cursor there (HELLDIVERS 2) was relative only
 * during win32u's own clip after a button press, and each return to absolute mode gave it the deltas of that time
 * back as one jump. Leaving relative mode warps the Mac pointer to Wine's cursor, which followed the deltas while the
 * pointer stood still, as winewayland.drv gives Wine's cursor to the locked pointer as its position hint: the first
 * absolute move then carries no offset. */
static void update_relative_mouse( UINT window_id )
{
    UINT want = (!cursor_visible && (clip_active || fullscreen_shown( window_id ))) ? window_id : 0;
    POINT pos;
    if (want == relative_window) return;
    if (relative_window)
        wsd_send( WSD_RELATIVE_MOUSE, &(struct wsd_window_flag){ relative_window, 0 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    if (want)
        wsd_send( WSD_RELATIVE_MOUSE, &(struct wsd_window_flag){ want, 1 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    TRACE( "relative mouse window %u -> %u\n", relative_window, want );
    relative_window = want;
    if (!want && NtUserGetCursorPos( &pos ))
    {
        __atomic_store_n( &last_warp_ns, now_ns(), __ATOMIC_RELAXED );
        TRACE( "warp to %d,%d, the cursor after relative mode\n", (int)pos.x, (int)pos.y );
        wsd_send( WSD_CURSOR_WARP, &(struct wsd_cursor_pos){ pos.x, pos.y }, sizeof(struct wsd_cursor_pos), NULL, 0, -1 );
    }
}

/***********************************************************************
 *           window surface
 */
static void obelisk_surface_set_clip( struct window_surface *surface, const RECT *rects, UINT count )
{
}

static BOOL obelisk_surface_flush( struct window_surface *surface, const RECT *rect, const RECT *dirty,
                                const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                const BITMAPINFO *shape_info, const void *shape_bits )
{
    UINT surf_width = color_info->bmiHeader.biWidth, surf_height = abs( color_info->bmiHeader.biHeight );
    UINT src_stride = surf_width * 4, width, height, y, i;
    int x0, y0;
    struct obelisk_buffer *buf = NULL;
    struct obelisk_win *win;
    struct wsd_present present;
    BOOL ret = FALSE;

    pthread_mutex_lock( &win_lock );
    if (!(win = get_win( surface->hwnd )) || !win->created) goto done;  /* stays dirty until shown */
    if (win->external || win->minimized) { ret = TRUE; goto done; }  /* nothing to show */

    /* The surface is usually larger than the window (Wine rounds it up); present exactly the visible window:
     * rect is the surface position relative to the visible rect, so the window starts at (-left, -top). */
    x0 = max( 0, -rect->left );
    y0 = max( 0, -rect->top );
    width = min( win->width, surf_width - min( (UINT)x0, surf_width ));
    height = min( win->height, surf_height - min( (UINT)y0, surf_height ));
    if (!width || !height) { ret = TRUE; goto done; }

    for (i = 0; i < NB_BUFFERS; i++)
    {
        struct obelisk_buffer *b = &win->buffers[i];
        if (b->id && (b->width != width || b->height != height) && !b->busy) free_buffer( b );
        if (!b->id && !alloc_buffer( b, width, height )) goto done;
        if (!b->busy && b->width == width && b->height == height) { buf = b; break; }
    }
    if (!buf) goto done;  /* both buffers in flight, retried on the next flush */

    for (y = 0; y < height; y++)
        memcpy( (char *)buf->bits + y * buf->stride,
                (const char *)color_bits + (y + y0) * src_stride + x0 * 4, width * 4 );
    if (shape_bits)  /* opaque everywhere for now, wsd ignores alpha */
        TRACE( "window shape ignored\n" );

    present.window_id = win_id( win );
    present.buffer_id = buf->id;
    present.serial = __atomic_add_fetch( &present_serial, 1, __ATOMIC_SEQ_CST );
    present.client_time_ns = now_ns();
    buf->busy = TRUE;
    if (!(ret = wsd_send( WSD_PRESENT, &present, sizeof(present), NULL, 0, -1 ))) buf->busy = FALSE;
    if (ret && !debug_injected && getenv( "WINE_OBELISK_DEBUG_INJECT" ))
    {
        /* test only, once per process: wsd feeds synthetic key, button and motion events to this window */
        debug_injected = TRUE;
        wsd_send( WSD_DEBUG_INJECT, &(struct wsd_window_flag){ win_id( win ), 1 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    }
done:
    pthread_mutex_unlock( &win_lock );
    return ret;
}

static void obelisk_surface_destroy( struct window_surface *surface )
{
}

static const struct window_surface_funcs obelisk_surface_funcs =
{
    obelisk_surface_set_clip,
    obelisk_surface_flush,
    obelisk_surface_destroy
};

static BOOL OBELISK_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface )
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    int width = surface_rect->right - surface_rect->left, height = surface_rect->bottom - surface_rect->top;
    struct window_surface *previous;

    if ((previous = *surface) && previous->funcs == &obelisk_surface_funcs) return TRUE;
    if (!is_toplevel( hwnd )) return TRUE;  /* default surface */
    if (previous) window_surface_release( previous );

    memset( info, 0, sizeof(*info) );
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height;  /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = width * height * 4;
    info->bmiHeader.biCompression = BI_RGB;
    *surface = window_surface_create( sizeof(struct window_surface), &obelisk_surface_funcs, hwnd, surface_rect, info, 0 );
    TRACE( "hwnd %p rect %s surface %p\n", hwnd, wine_dbgstr_rect( surface_rect ), *surface );
    return TRUE;
}

/***********************************************************************
 *           window management
 */
static BOOL OBELISK_WindowPosChanging( HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects )
{
    return is_toplevel( hwnd );  /* top-level windows get a driver surface */
}

/* The caption and borders macOS draws for a titled window: win32u leaves them out of the visible rect. */
static BOOL OBELISK_GetWindowStyleMasks( HWND hwnd, UINT style, UINT ex_style, UINT *style_mask, UINT *ex_style_mask )
{
    if (!(frame_style( style, ex_style ) & WSD_STYLE_TITLED)) return FALSE;
    *style_mask = WS_CAPTION | WS_DLGFRAME | WS_THICKFRAME;
    *ex_style_mask = WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME;
    return TRUE;
}

static void OBELISK_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                                   const struct window_rects *rects, struct window_surface *surface )
{
    DWORD style = NtUserGetWindowLongW( hwnd, GWL_STYLE ), ex_style = NtUserGetWindowLongW( hwnd, GWL_EXSTYLE );
    BOOL visible = (style & WS_VISIBLE) && !(style & WS_MINIMIZE) && !IsRectEmpty( &rects->visible );
    BOOL minimized = (style & WS_VISIBLE) && (style & WS_MINIMIZE);
    BOOL fullscreen = !!(swp_flags & WINE_SWP_FULLSCREEN);
    UINT width = rects->visible.right - rects->visible.left, height = rects->visible.bottom - rects->visible.top;
    UINT frame = frame_style( style, ex_style ), id;
    struct obelisk_win *win = NULL;

    if (!is_toplevel( hwnd )) return;
    TRACE( "hwnd %p rects %s flags %08x visible %u fullscreen %u\n", hwnd, debugstr_window_rects( rects ),
           swp_flags, visible, fullscreen );

    pthread_mutex_lock( &win_lock );
    if (!(win = get_win( hwnd )))
    {
        if (!visible || !(win = alloc_win( hwnd ))) goto done;
    }
    if (minimized && win->created && win->shown)
    {
        /* into the Dock, or out of sight for a window without a minimize button, which wsd brings back when the user
         * comes back to the game; Wine's minimized position (off screen) is not given to wsd */
        if (!win->minimized)
            wsd_send( WSD_WINDOW_MINIMIZE, &(struct wsd_window_flag){ win_id( win ), 1 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->minimized = TRUE;
        goto done;
    }
    if (win->minimized && !minimized)
    {
        wsd_send( WSD_WINDOW_MINIMIZE, &(struct wsd_window_flag){ win_id( win ), 0 }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->minimized = FALSE;
    }
    if (win->created && frame != win->style)
    {
        wsd_send( WSD_WINDOW_SET_STYLE, &(struct wsd_window_flag){ win_id( win ), frame }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->style = frame;
    }
    win->window = rects->window;
    win->visible = rects->visible;
    win->client = rects->client;
    OffsetRect( &win->client, -rects->visible.left, -rects->visible.top );

    if (visible && win->created && !fullscreen && !win->fullscreen)
    {
        struct wsd_window_frame frame = { win_id( win ), 0, rects->visible.left, rects->visible.top, width, height };
        if (win->width != width || win->height != height) frame.flags |= WSD_FRAME_SIZE;
        if (win->x != rects->visible.left || win->y != rects->visible.top) frame.flags |= WSD_FRAME_MOVE;
        if (frame.flags) wsd_send( WSD_WINDOW_SET_FRAME, &frame, sizeof(frame), NULL, 0, -1 );
        win->width = width;
        win->height = height;
        win->x = rects->visible.left;
        win->y = rects->visible.top;
    }
    if (visible && (width > 16384 || height > 16384))
    {
        /* off-screen helper windows (Wine sizes some popups far beyond the screen): wsd limit is 16384 px */
        TRACE( "hwnd %p too large for wsd (%ux%u), not shown\n", hwnd, width, height );
        visible = FALSE;
    }
    if (visible && !win->created)
    {
        struct wsd_window_create create = { win_id( win ), width, height, frame };
        WCHAR textW[256];
        char title[256 * 3];
        DWORD len = 0;
        INT n = NtUserInternalGetWindowText( hwnd, textW, ARRAY_SIZE(textW) );

        if (n > 0) RtlUnicodeToUTF8N( title, sizeof(title), &len, textW, n * sizeof(WCHAR) );
        if (!wsd_send( WSD_WINDOW_CREATE, &create, sizeof(create), title, len, -1 )) goto done;
        win->created = TRUE;
        win->style = frame;
        /* nptunix (Neptune D3D presentation) finds the wsd window of a HWND through this property */
        NtUserSetProp( hwnd, obelisk_window_propW, ULongToHandle( win_id( win ) ));
        win->width = width;
        win->height = height;
        win->x = rects->visible.left;
        win->y = rects->visible.top;
        {
            struct wsd_window_frame frame = { win_id( win ), WSD_FRAME_MOVE, win->x, win->y, width, height };
            wsd_send( WSD_WINDOW_SET_FRAME, &frame, sizeof(frame), NULL, 0, -1 );
        }
    }
    if (win->created && visible != win->shown)
    {
        /* a window shown without activation (SW_SHOWNA, a menu, a tooltip) leaves the Mac's key window where it is */
        UINT show = !visible ? 0 : (swp_flags & SWP_NOACTIVATE) || (ex_style & WS_EX_NOACTIVATE) ? 2 : 1;
        wsd_send( WSD_WINDOW_SHOW, &(struct wsd_window_flag){ win_id( win ), show }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->shown = visible;
        /* but the foreground window becomes the key window all the same: Wine activated it, or kept it active, while
         * wsd did not show it (hidden, empty or too large), and OBELISK_ActivateWindow only gives the key window to a
         * shown window; winex11.drv likewise activates the foreground window it maps (window_set_wm_state) */
        if (show == 2 && hwnd == NtUserGetForegroundWindow())
            wsd_send( WSD_WINDOW_FOCUS, &(struct wsd_window){ win_id( win ) }, sizeof(struct wsd_window), NULL, 0, -1 );
    }
    if (win->created && visible && fullscreen != win->fullscreen)
    {
        wsd_send( WSD_WINDOW_FULLSCREEN, &(struct wsd_window_flag){ win_id( win ), fullscreen }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
        win->fullscreen = fullscreen;
    }
done:
    id = win ? win_id( win ) : 0;
    pthread_mutex_unlock( &win_lock );
    /* shown, fullscreen or minimized changed, also by the user (WSD_EV_MINIMIZE posts WM_SYSCOMMAND): relative mouse
     * mode follows (update_relative_mouse) */
    if (id && hwnd == NtUserGetForegroundWindow()) update_relative_mouse( id );
}

/* Applies in the window's thread the frame the user gave the macOS window (WSD_EV_FRAME). */
static void apply_frame( HWND hwnd )
{
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    struct obelisk_win *win;
    RECT rect;

    pthread_mutex_lock( &win_lock );
    if (!(win = get_win( hwnd )) || !win->frame_pending || win->minimized || win->fullscreen)
    {
        if (win) win->frame_pending = FALSE;
        pthread_mutex_unlock( &win_lock );
        return;
    }
    win->frame_pending = FALSE;
    /* the window rect around the new visible rect, with the same non-client margins */
    rect.left   = win->pending.left - (win->visible.left - win->window.left);
    rect.top    = win->pending.top - (win->visible.top - win->window.top);
    rect.right  = win->pending.right + (win->window.right - win->visible.right);
    rect.bottom = win->pending.bottom + (win->window.bottom - win->visible.bottom);
    if (rect.right - rect.left == win->window.right - win->window.left &&
        rect.bottom - rect.top == win->window.bottom - win->window.top) flags |= SWP_NOSIZE;
    /* wsd already shows this frame: WindowPosChanged must not send it back */
    win->x = win->pending.left;
    win->y = win->pending.top;
    win->width = win->pending.right - win->pending.left;
    win->height = win->pending.bottom - win->pending.top;
    pthread_mutex_unlock( &win_lock );

    TRACE( "hwnd %p moved by the user to %s\n", hwnd, wine_dbgstr_rect( &rect ));
    NtUserSetWindowPos( hwnd, 0, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags );
}

static LRESULT OBELISK_WindowMessage( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    switch (msg)
    {
    case WM_OBELISK_APPLY_FRAME:
        apply_frame( hwnd );
        return 0;
    case WM_OBELISK_TAKE_FOCUS:
    {
        /* The user made it the Mac's key window: it takes the foreground whatever process has it (Wine's foreground
         * lock refused the event thread once the desktop had it), as winemac.drv's macdrv_window_got_focus gives it,
         * in the window's thread. A window still minimized waits for the restore posted before this message. */
        DWORD style = NtUserGetWindowLongW( hwnd, GWL_STYLE );
        if ((style & WS_VISIBLE) && !(style & (WS_DISABLED | WS_MINIMIZE))) NtUserSetForegroundWindowInternal( hwnd );
        return 0;
    }
    }
    FIXME( "unknown driver message %#x hwnd %p\n", msg, hwnd );
    return 0;
}

static void OBELISK_DestroyWindow( HWND hwnd )
{
    struct obelisk_win *win;

    NtUserRemoveProp( hwnd, obelisk_window_propW );

    pthread_mutex_lock( &win_lock );
    if ((win = get_win( hwnd ))) destroy_win( win );
    pthread_mutex_unlock( &win_lock );
}

static void OBELISK_SetWindowText( HWND hwnd, LPCWSTR text )
{
    struct obelisk_win *win;
    char title[1024];
    DWORD len = 0;

    if (text) RtlUnicodeToUTF8N( title, sizeof(title), &len, text, wcslen( text ) * sizeof(WCHAR) );
    pthread_mutex_lock( &win_lock );
    if ((win = get_win( hwnd )) && win->created)
        wsd_send( WSD_SET_TITLE, &(struct wsd_window){ win_id( win ) }, sizeof(struct wsd_window), title, len, -1 );
    pthread_mutex_unlock( &win_lock );
}

/* Wine activated the window: the Mac's key window follows, as winemac.drv gives it the focus in ActivateWindow. */
static void OBELISK_ActivateWindow( HWND hwnd, HWND previous )
{
    struct obelisk_win *win;

    pthread_mutex_lock( &win_lock );
    if ((win = get_win( hwnd )) && win->created && win->shown)
        wsd_send( WSD_WINDOW_FOCUS, &(struct wsd_window){ win_id( win ) }, sizeof(struct wsd_window), NULL, 0, -1 );
    pthread_mutex_unlock( &win_lock );
}

/***********************************************************************
 *           cursor
 */

/* The Mac cursors that stand for Windows' own (winemac.drv's table in mouse.c): the module and resource of a cursor
 * Wine loaded from one of these modules. */
static const struct { WCHAR module[16]; WORD id; UINT cursor; } system_cursors[] =
{
    { u"user32.dll",   OCR_NORMAL, WSD_CURSOR_ARROW },
    { u"user32.dll",   OCR_IBEAM,  WSD_CURSOR_IBEAM },
    { u"user32.dll",   OCR_CROSS,  WSD_CURSOR_CROSSHAIR },
    { u"user32.dll",   OCR_SIZEWE, WSD_CURSOR_RESIZE_LEFT_RIGHT },
    { u"user32.dll",   OCR_SIZENS, WSD_CURSOR_RESIZE_UP_DOWN },
    { u"user32.dll",   OCR_NO,     WSD_CURSOR_NOT_ALLOWED },
    { u"user32.dll",   OCR_HAND,   WSD_CURSOR_POINTING_HAND },
    { u"comctl32.dll", 102,        WSD_CURSOR_CLOSED_HAND },
    { u"comctl32.dll", 104,        WSD_CURSOR_DRAG_COPY },
    { u"comctl32.dll", 105,        WSD_CURSOR_ARROW },
    { u"comctl32.dll", 106,        WSD_CURSOR_RESIZE_LEFT_RIGHT },
    { u"comctl32.dll", 107,        WSD_CURSOR_RESIZE_LEFT_RIGHT },
    { u"comctl32.dll", 108,        WSD_CURSOR_POINTING_HAND },
    { u"comctl32.dll", 135,        WSD_CURSOR_RESIZE_UP_DOWN },
    { u"ole32.dll",    1,          WSD_CURSOR_NOT_ALLOWED },
    { u"ole32.dll",    2,          WSD_CURSOR_CLOSED_HAND },
    { u"ole32.dll",    3,          WSD_CURSOR_DRAG_COPY },
    { u"ole32.dll",    4,          WSD_CURSOR_DRAG_LINK },
    { u"riched20.dll", 105,        WSD_CURSOR_POINTING_HAND },
    { u"riched20.dll", 109,        WSD_CURSOR_DRAG_COPY },
    { u"riched20.dll", 110,        WSD_CURSOR_CLOSED_HAND },
    { u"riched20.dll", 111,        WSD_CURSOR_NOT_ALLOWED },
};

#define CURSOR_CACHE 64  /* wsd keeps at most 64 cursor images per client */

static pthread_mutex_t cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { HCURSOR handle; UINT cursor; } cursor_cache[CURSOR_CACHE];  /* cursor_lock held */
static UINT cursor_cache_next;
static HCURSOR current_cursor;

/* The Mac cursor of a Windows system cursor, or -1. */
static int system_cursor( const WCHAR *module, WORD id )
{
    const WCHAR *name = wcsrchr( module, '\\' ) ? wcsrchr( module, '\\' ) + 1 : module;
    UINT i;

    if (!id) return -1;  /* a named resource */
    for (i = 0; i < ARRAY_SIZE(system_cursors); i++)
        if (system_cursors[i].id == id && !wcsicmp( name, system_cursors[i].module )) return system_cursors[i].cursor;
    return -1;
}

/* The cursor's first frame as BGRA pixels with straight alpha, drawn as winemac.drv draws it (image.c,
 * create_cgimage_from_icon_bitmaps; mouse.c, create_monochrome_cursor): the color through DrawIconEx, with its alpha
 * or else the AND mask's; a monochrome cursor from its AND and XOR masks, an inverting pixel shown black. */
static UINT *cursor_pixels( HCURSOR cursor, const ICONINFO *info, UINT width, UINT height )
{
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *bmi = (BITMAPINFO *)buffer;
    UINT mask_stride = (width + 31) / 32 * 4, rows = info->hbmColor ? height : height * 2, x, y, i;
    BYTE *mask = malloc( mask_stride * rows );
    UINT *pixels = calloc( width * height, sizeof(UINT) ), *bits;
    BOOL has_alpha = FALSE, ok = FALSE;
    HBITMAP dib = 0;
    HDC hdc = NtGdiCreateCompatibleDC( 0 );

    if (!mask || !pixels || !hdc) goto done;
    memset( bmi, 0, sizeof(buffer) );
    bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
    bmi->bmiHeader.biWidth = width;
    bmi->bmiHeader.biHeight = -(int)rows;
    bmi->bmiHeader.biPlanes = 1;
    bmi->bmiHeader.biBitCount = 1;
    bmi->bmiHeader.biCompression = BI_RGB;
    if (!NtGdiGetDIBitsInternal( hdc, info->hbmMask, 0, rows, mask, bmi, DIB_RGB_COLORS, 0, 0 )) goto done;

    if (info->hbmColor)
    {
        bmi->bmiHeader.biHeight = -(int)height;
        bmi->bmiHeader.biBitCount = 32;
        if (!(dib = NtGdiCreateDIBSection( hdc, NULL, 0, bmi, DIB_RGB_COLORS, 0, 0, 0, (void **)&bits ))) goto done;
        memset( bits, 0, width * height * 4 );
        NtGdiSelectBitmap( hdc, dib );
        if (!NtUserDrawIconEx( hdc, 0, 0, cursor, width, height, 0, NULL, DI_NORMAL )) goto done;
        for (i = 0; i < width * height && !has_alpha; i++) has_alpha = (bits[i] & 0xff000000) != 0;
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
            {
                UINT p = bits[y * width + x], a = p >> 24;
                if (!has_alpha)  /* AND mask: 1 leaves the screen as it is */
                    pixels[y * width + x] = (mask[y * mask_stride + x / 8] & (0x80 >> (x % 8))) ? 0 : p | 0xff000000;
                else if (a && a < 255)  /* DrawIconEx blends onto the zeroed bits: the colors come premultiplied */
                    pixels[y * width + x] = (a << 24) | (min( 255, ((p >> 16) & 0xff) * 255 / a ) << 16) |
                                            (min( 255, ((p >> 8) & 0xff) * 255 / a ) << 8) | min( 255, (p & 0xff) * 255 / a );
                else pixels[y * width + x] = p;
            }
    }
    else
    {
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
            {
                BYTE bit = 0x80 >> (x % 8);
                BOOL and_bit = !!(mask[y * mask_stride + x / 8] & bit), xor_bit = !!(mask[(y + height) * mask_stride + x / 8] & bit);
                pixels[y * width + x] = (and_bit && !xor_bit) ? 0 : (and_bit ^ xor_bit) ? 0xffffffff : 0xff000000;
            }
    }
    ok = TRUE;

done:
    if (dib) NtGdiDeleteObjectApp( dib );
    if (hdc) NtGdiDeleteObjectApp( hdc );
    free( mask );
    if (!ok)
    {
        free( pixels );
        pixels = NULL;
    }
    return pixels;
}

/* Gives wsd the cursor's image under the given id; FALSE when it cannot be shown (wsd shows the arrow). */
static BOOL define_cursor( HCURSOR cursor, UINT id )
{
    WCHAR module_name[MAX_PATH], res_name[MAX_PATH];
    UNICODE_STRING module = { 0, sizeof(module_name) - sizeof(WCHAR), module_name };
    UNICODE_STRING res = { 0, sizeof(res_name) - sizeof(WCHAR), res_name };
    struct wsd_cursor_define define = { id };
    ICONINFO info;
    BITMAP bm;
    UINT *pixels = NULL;
    BOOL ret = FALSE;
    int fd = -1;

    if (!NtUserGetIconInfo( cursor, &info, &module, &res, NULL, 0 )) return FALSE;
    NtGdiExtGetObjectW( info.hbmMask, sizeof(bm), &bm );
    define.width = bm.bmWidth;
    define.height = info.hbmColor ? bm.bmHeight : max( 1, bm.bmHeight / 2 );
    /* winemac.drv's rule: a hot spot outside the cursor is its center */
    if (info.xHotspot >= define.width || info.yHotspot >= define.height)
    {
        info.xHotspot = define.width / 2;
        info.yHotspot = define.height / 2;
    }
    define.hot_x = info.xHotspot;
    define.hot_y = info.yHotspot;
    define.stride = define.width * 4;
    define.size = (UINT64)define.stride * define.height;
    if (define.width && define.width <= 256 && define.height <= 256 &&
        (pixels = cursor_pixels( cursor, &info, define.width, define.height )) &&
        (fd = syscall( __NR_memfd_create, "wsd-cursor", 1 /* MFD_CLOEXEC */ )) >= 0 &&
        pwrite( fd, pixels, define.size, 0 ) == (ssize_t)define.size)
        ret = wsd_send( WSD_CURSOR_DEFINE, &define, sizeof(define), NULL, 0, fd );
    if (fd >= 0) close( fd );
    free( pixels );
    if (info.hbmColor) NtGdiDeleteObjectApp( info.hbmColor );
    NtGdiDeleteObjectApp( info.hbmMask );
    TRACE( "cursor %p %ux%u hot spot %d,%d -> id %#x %d\n", cursor, define.width, define.height, define.hot_x,
           define.hot_y, id, ret );
    return ret;
}

/* The Mac cursor for a Windows one: the Mac's own for a system cursor, else its image, defined once (cursor_lock held). */
static UINT mac_cursor( HCURSOR cursor )
{
    WCHAR module_name[MAX_PATH], res_name[MAX_PATH];
    UNICODE_STRING module = { 0, sizeof(module_name) - sizeof(WCHAR), module_name };
    UNICODE_STRING res = { 0, sizeof(res_name) - sizeof(WCHAR), res_name };
    ICONINFO info;
    UINT i, slot;
    int sys = -1;

    for (i = 0; i < CURSOR_CACHE; i++) if (cursor_cache[i].handle == cursor) return cursor_cache[i].cursor;
    if (NtUserGetIconInfo( cursor, &info, &module, &res, NULL, 0 ))
    {
        module_name[module.Length] = 0;  /* win32u gives the lengths in characters */
        sys = system_cursor( module_name, res.Length ? 0 : LOWORD( res.Buffer ));
        TRACE( "cursor %p from %s %u -> Mac cursor %d\n", cursor, debugstr_w(module_name), LOWORD( res.Buffer ), sys );
        if (info.hbmColor) NtGdiDeleteObjectApp( info.hbmColor );
        NtGdiDeleteObjectApp( info.hbmMask );
    }
    /* never the slot of the cursor shown: wsd would show the new image in its place */
    do slot = cursor_cache_next++ % CURSOR_CACHE; while (current_cursor && cursor_cache[slot].handle == current_cursor);
    cursor_cache[slot].handle = cursor;
    if (sys >= 0) cursor_cache[slot].cursor = sys;
    else cursor_cache[slot].cursor = define_cursor( cursor, WSD_CURSOR_FIRST_DEFINED + slot )
                                     ? WSD_CURSOR_FIRST_DEFINED + slot : WSD_CURSOR_ARROW;
    return cursor_cache[slot].cursor;
}

static void OBELISK_DestroyCursorIcon( HCURSOR cursor )
{
    UINT i;

    pthread_mutex_lock( &cursor_lock );
    for (i = 0; i < CURSOR_CACHE; i++) if (cursor_cache[i].handle == cursor) cursor_cache[i].handle = 0;
    if (current_cursor == cursor) current_cursor = 0;
    pthread_mutex_unlock( &cursor_lock );
}

static void OBELISK_SetCursor( HWND hwnd, HCURSOR cursor )
{
    BOOL visible = cursor != 0;

    if (cursor)
    {
        pthread_mutex_lock( &cursor_lock );
        if (cursor != current_cursor)
        {
            struct wsd_cursor_set set = { mac_cursor( cursor ) };
            current_cursor = cursor;
            wsd_send( WSD_CURSOR_SET, &set, sizeof(set), NULL, 0, -1 );
        }
        pthread_mutex_unlock( &cursor_lock );
    }
    if (visible == cursor_visible) return;
    cursor_visible = visible;
    wsd_send( WSD_CURSOR_VISIBLE, &(struct wsd_window_flag){ 0, visible }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    {
        struct obelisk_win *win;
        UINT id = 0;
        pthread_mutex_lock( &win_lock );
        if ((win = get_win( NtUserGetForegroundWindow() )) && win->created) id = win_id( win );
        pthread_mutex_unlock( &win_lock );
        update_relative_mouse( id );
    }
}

/* SetCursorPos and the moves of SendInput: the Mac pointer follows (winemac.drv's SetCursorPos). In relative mode
 * the pointer stands still and the game reads deltas. */
static BOOL OBELISK_SetCursorPos( INT x, INT y )
{
    if (relative_window) return TRUE;
    __atomic_store_n( &last_warp_ns, now_ns(), __ATOMIC_RELAXED );
    TRACE( "warp to %d,%d\n", x, y );
    return wsd_send( WSD_CURSOR_WARP, &(struct wsd_cursor_pos){ x, y }, sizeof(struct wsd_cursor_pos), NULL, 0, -1 );
}

static BOOL OBELISK_ClipCursor( const RECT *clip, BOOL reset )
{
    HWND foreground = NtUserGetForegroundWindow();
    struct wsd_cursor_clip req = { 0 };
    struct obelisk_win *win;

    pthread_mutex_lock( &win_lock );
    if (!(win = get_win( foreground )) || !win->created)
    {
        pthread_mutex_unlock( &win_lock );
        return TRUE;
    }
    req.window_id = win_id( win );
    if (clip && !reset)
    {
        RECT rc;
        rc.left = max( clip->left, win->visible.left );
        rc.top = max( clip->top, win->visible.top );
        rc.right = min( clip->right, win->visible.right );
        rc.bottom = min( clip->bottom, win->visible.bottom );
        /* an empty rectangle holds the cursor on one point, as Unity's locked cursor at the window's center */
        if (rc.left <= rc.right && rc.top <= rc.bottom && rc.left < win->visible.right && rc.top < win->visible.bottom)
        {
            req.enable = 1;
            req.x = rc.left - win->visible.left;
            req.y = rc.top - win->visible.top;
            req.width = max( rc.right - rc.left, 1 );
            req.height = max( rc.bottom - rc.top, 1 );
        }
    }
    pthread_mutex_unlock( &win_lock );
    wsd_send( WSD_CURSOR_CLIP, &req, sizeof(req), NULL, 0, -1 );
    clip_active = req.enable;
    update_relative_mouse( req.window_id );
    return TRUE;
}

/* SPI_SETSCREENSAVEACTIVE: while a game turns the screen saver off, the display stays on (winemac.drv's
 * SystemParametersInfo); win32u keeps the value itself. */
static BOOL OBELISK_SystemParametersInfo( UINT action, UINT int_param, void *ptr_param, UINT flags )
{
    if (action == SPI_SETSCREENSAVEACTIVE)
        wsd_send( WSD_SCREEN_SAVER, &(struct wsd_window_flag){ 0, !!int_param }, sizeof(struct wsd_window_flag), NULL, 0, -1 );
    return FALSE;
}

/***********************************************************************
 *           display devices
 */
static UINT OBELISK_UpdateDisplayDevices( const struct gdi_device_manager *manager, void *param )
{
    /* Smaller modes in the shape of each usual screen, 4:3, 16:9, 16:10 as a MacBook's and 21:9 as an ultrawide
     * monitor's, so a game finds one that fills the screen it is on without stretching. */
    static const struct { UINT w, h; } extra[] = { {640,480}, {800,600}, {1024,768}, {1280,720}, {1280,800},
                                                     {1440,900}, {1600,900}, {1680,1050}, {1920,1080}, {1920,1200},
                                                     {2560,1080}, {2560,1440}, {2560,1600}, {3440,1440} };
    struct pci_id pci_id = { 0 };
    struct gdi_monitor monitor = { 0 };
    DEVMODEW current = { .dmSize = sizeof(current) }, modes[ARRAY_SIZE(extra) + 2];
    UINT i, count = 0, hz, width, height, native_width, native_height;

    pthread_mutex_lock( &display_lock );
    hz = hello.max_refresh_hz ? hello.max_refresh_hz : 60;
    width = hello.display_width_px;
    height = hello.display_height_px;
    /* The modes are those of the screen as it is without the mode a game chose (OBELISK_ChangeDisplaySettings); a wsd
     * from before the display mode tells no native size, its screen is always that one. */
    native_width = hello.native_width_px ? hello.native_width_px : width;
    native_height = hello.native_height_px ? hello.native_height_px : height;
    pthread_mutex_unlock( &display_lock );

    if (!width || !height) return STATUS_NOT_IMPLEMENTED;

    manager->add_gpu( NULL, &pci_id, NULL, param );
    manager->add_source( "wsd", DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE,
                         NtUserGetSystemDpiForProcess( NULL ), param );
    SetRect( &monitor.rc_monitor, 0, 0, width, height );
    /* The whole screen, not wsd's work area: Rocket League places its borderless window from the work area (at y 60
     * under a 30 px menu bar), which then no longer covers the screen and is not fullscreen. AppKit keeps a title
     * bar below the menu bar by itself (WSD_EV_FRAME). */
    monitor.rc_work = monitor.rc_monitor;
    manager->add_monitor( &monitor, param );

    current.dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                       DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY | DM_POSITION;
    current.dmBitsPerPel = 32;
    current.dmPelsWidth = width;
    current.dmPelsHeight = height;
    current.dmDisplayFrequency = hz;
    modes[count] = current;
    modes[count].dmFields &= ~DM_POSITION;
    modes[count].dmPelsWidth = native_width;
    modes[count++].dmPelsHeight = native_height;
    for (i = 0; i < ARRAY_SIZE(extra); i++)
    {
        /* Only modes the screen can hold: wsd's display is smaller than the screen under a lower render percent, and a
         * mode larger than it in one dimension (1600x900 on a 1512x982 desktop) would let a game pick a size it cannot
         * show. The screen's own size is already the first mode. */
        if (extra[i].w > native_width || extra[i].h > native_height ||
            (extra[i].w == native_width && extra[i].h == native_height)) continue;
        modes[count] = modes[0];
        modes[count].dmPelsWidth = extra[i].w;
        modes[count++].dmPelsHeight = extra[i].h;
    }
    /* The mode a game chose is the current one, even when it is none of those (wsd keeps a desktop above 640x480). */
    for (i = 0; i < count; i++) if (modes[i].dmPelsWidth == width && modes[i].dmPelsHeight == height) break;
    if (i == count)
    {
        modes[count] = modes[0];
        modes[count].dmPelsWidth = width;
        modes[count++].dmPelsHeight = height;
    }
    manager->add_modes( &current, count, modes, param );
    return STATUS_SUCCESS;
}

/***********************************************************************
 *           OBELISK_ChangeDisplaySettings
 *
 * A game that switches the display to a smaller mode (Direct3D exclusive fullscreen, SetFullscreenState with a mode
 * switch) draws its picture in the pixels of that mode and reads the pointer in them, as on Windows. wsd has no display
 * mode to switch: it lowers the scale it converts pixels and points with, so the desktop, the windows and the pointer
 * are in the pixels of the mode and the picture is stretched to the screen (WSD_SET_DISPLAY_MODE, like a render
 * percent). A mode of the screen's size or more, or the restorer asking for the registry's, gives the screen back.
 * wsd answers with the screen as it is (EV_DISPLAY), which Wine then reads again (UpdateDisplayDevices) to give the
 * game its mode.
 */
static LONG OBELISK_ChangeDisplaySettings( LPDEVMODEW displays, LPCWSTR primary_name, HWND hwnd, DWORD flags, LPVOID lparam )
{
    struct wsd_display_mode mode = { 0 };
    struct timespec until;
    UINT64 serial;

    if (displays && displays->dmPelsWidth && displays->dmPelsHeight)
    {
        mode.width_px = displays->dmPelsWidth;
        mode.height_px = displays->dmPelsHeight;
    }
    TRACE( "display mode %ux%u flags %#x\n", mode.width_px, mode.height_px, (int)flags );

    pthread_mutex_lock( &display_lock );
    serial = display_serial;
    pthread_mutex_unlock( &display_lock );
    if (!wsd_send( WSD_SET_DISPLAY_MODE, &mode, sizeof(mode), NULL, 0, -1 )) return DISP_CHANGE_SUCCESSFUL;

    clock_gettime( CLOCK_REALTIME, &until );
    until.tv_sec += 1;
    pthread_mutex_lock( &display_lock );
    while (display_serial == serial)
        if (pthread_cond_timedwait( &display_cond, &display_lock, &until )) break;
    pthread_mutex_unlock( &display_lock );
    return DISP_CHANGE_SUCCESSFUL;
}

/***********************************************************************
 *           keyboard layout
 *
 * wsd tells what each key types in the keyboard layout selected on the Mac (WSD_EV_KEYMAP), and keymap.c makes the
 * Windows layout of it: a key is sent with the virtual key the layout gives it, win32u reads the layout's tables
 * for MapVirtualKey, VkKeyScan and GetKeyNameText, and ToUnicodeEx types the layout's characters, dead keys
 * included. Until wsd tells (an older wsd never does), the keys are those of a US keyboard, as before.
 */
struct obelisk_layout
{
    LONG                   ref;
    struct obelisk_keymap *keymap;
    KBDTABLES              tables;
    ULONG_PTR              modifiers[4];  /* a MODIFIERS and its 8 numbers */
    VK_TO_BIT              vk_to_bit[4];
    VK_TO_WCHAR_TABLE      vk_to_wchar_table[2];
    VK_TO_WCHARS8          vk_to_wchars[OBELISK_KEYS + 32];
    USHORT                 vsc_to_vk[0x80];
    VSC_VK                 vsc_to_vk_e0[64];
    VSC_VK                 vsc_to_vk_e1[2];
    VSC_LPWSTR             key_names[64];
    VSC_LPWSTR             key_names_ext[32];
    WCHAR                  names[1024];
};

static pthread_mutex_t layout_lock = PTHREAD_MUTEX_INITIALIZER;
static struct obelisk_layout *current_layout;  /* layout_lock held */
static BOOL altgr_control;  /* event thread: the left Control of AltGr is down */

/* win32u's US tables (input.c), which the layout's own keys replace */
static const USHORT us_vsc_to_vk[] =
{
    T00, T01, T02, T03, T04, T05, T06, T07,
    T08, T09, T0A, T0B, T0C, T0D, T0E, T0F,
    T10, T11, T12, T13, T14, T15, T16, T17,
    T18, T19, T1A, T1B, T1C, T1D, T1E, T1F,
    T20, T21, T22, T23, T24, T25, T26, T27,
    T28, T29, T2A, T2B, T2C, T2D, T2E, T2F,
    T30, T31, T32, T33, T34, T35, T36 | KBDEXT, T37 | KBDMULTIVK,
    T38, T39, T3A, T3B, T3C, T3D, T3E, T3F,
    T40, T41, T42, T43, T44, T45 | KBDEXT | KBDMULTIVK, T46 | KBDMULTIVK, T47 | KBDNUMPAD | KBDSPECIAL,
    T48 | KBDNUMPAD | KBDSPECIAL, T49 | KBDNUMPAD | KBDSPECIAL, T4A, T4B | KBDNUMPAD | KBDSPECIAL,
    T4C | KBDNUMPAD | KBDSPECIAL, T4D | KBDNUMPAD | KBDSPECIAL, T4E, T4F | KBDNUMPAD | KBDSPECIAL,
    T50 | KBDNUMPAD | KBDSPECIAL, T51 | KBDNUMPAD | KBDSPECIAL, T52 | KBDNUMPAD | KBDSPECIAL,
    T53 | KBDNUMPAD | KBDSPECIAL, T54, T55, T56, T57,
    T58, T59, T5A, T5B, T5C, T5D, T5E, T5F,
    T60, T61, T62, T63, T64, T65, T66, T67,
    T68, T69, T6A, T6B, T6C, T6D, T6E, T6F,
    T70, T71, T72, T73, T74, T75, T76, T77,
    T78, T79, T7A, T7B, T7C, T7D, T7E
};

static const VSC_VK us_vsc_to_vk_e0[] =
{
    {0x10, X10 | KBDEXT},
    {0x19, X19 | KBDEXT},
    {0x1d, X1D | KBDEXT},
    {0x20, X20 | KBDEXT},
    {0x21, X21 | KBDEXT},
    {0x22, X22 | KBDEXT},
    {0x24, X24 | KBDEXT},
    {0x2e, X2E | KBDEXT},
    {0x30, X30 | KBDEXT},
    {0x32, X32 | KBDEXT},
    {0x35, X35 | KBDEXT},
    {0x37, X37 | KBDEXT},
    {0x38, X38 | KBDEXT},
    {0x47, X47 | KBDEXT},
    {0x48, X48 | KBDEXT},
    {0x49, X49 | KBDEXT},
    {0x4b, X4B | KBDEXT},
    {0x4d, X4D | KBDEXT},
    {0x4f, X4F | KBDEXT},
    {0x50, X50 | KBDEXT},
    {0x51, X51 | KBDEXT},
    {0x52, X52 | KBDEXT},
    {0x53, X53 | KBDEXT},
    {0x5b, X5B | KBDEXT},
    {0x5c, X5C | KBDEXT},
    {0x5d, X5D | KBDEXT},
    {0x5f, X5F | KBDEXT},
    {0x65, X65 | KBDEXT},
    {0x66, X66 | KBDEXT},
    {0x67, X67 | KBDEXT},
    {0x68, X68 | KBDEXT},
    {0x69, X69 | KBDEXT},
    {0x6a, X6A | KBDEXT},
    {0x6b, X6B | KBDEXT},
    {0x6c, X6C | KBDEXT},
    {0x6d, X6D | KBDEXT},
    {0x1c, X1C | KBDEXT},
    {0x46, X46 | KBDEXT},
    {0},
};

static const VSC_VK us_vsc_to_vk_e1[] =
{
    {0x1d, Y1D},
    {0},
};

static const struct { BYTE vsc; const char *name; } us_key_names[] =
{
    { 0x01, "Esc" },
    { 0x0e, "Backspace" },
    { 0x0f, "Tab" },
    { 0x1c, "Enter" },
    { 0x1d, "Ctrl" },
    { 0x2a, "Shift" },
    { 0x36, "Right Shift" },
    { 0x37, "Num *" },
    { 0x38, "Alt" },
    { 0x39, "Space" },
    { 0x3a, "Caps Lock" },
    { 0x3b, "F1" },
    { 0x3c, "F2" },
    { 0x3d, "F3" },
    { 0x3e, "F4" },
    { 0x3f, "F5" },
    { 0x40, "F6" },
    { 0x41, "F7" },
    { 0x42, "F8" },
    { 0x43, "F9" },
    { 0x44, "F10" },
    { 0x45, "Pause" },
    { 0x46, "Scroll Lock" },
    { 0x47, "Num 7" },
    { 0x48, "Num 8" },
    { 0x49, "Num 9" },
    { 0x4a, "Num -" },
    { 0x4b, "Num 4" },
    { 0x4c, "Num 5" },
    { 0x4d, "Num 6" },
    { 0x4e, "Num +" },
    { 0x4f, "Num 1" },
    { 0x50, "Num 2" },
    { 0x51, "Num 3" },
    { 0x52, "Num 0" },
    { 0x53, "Num Del" },
    { 0x54, "Sys Req" },
    { 0x57, "F11" },
    { 0x58, "F12" },
    { 0x7c, "F13" },
    { 0x7d, "F14" },
    { 0x7e, "F15" },
    { 0x7f, "F16" },
    { 0x80, "F17" },
    { 0x81, "F18" },
    { 0x82, "F19" },
    { 0x83, "F20" },
    { 0x84, "F21" },
    { 0x85, "F22" },
    { 0x86, "F23" },
    { 0x87, "F24" },
};

static const struct { BYTE vsc; const char *name; } us_key_names_ext[] =
{
    { 0x1c, "Num Enter" },
    { 0x1d, "Right Ctrl" },
    { 0x35, "Num /" },
    { 0x37, "Prnt Scrn" },
    { 0x38, "Right Alt" },
    { 0x45, "Num Lock" },
    { 0x46, "Break" },
    { 0x47, "Home" },
    { 0x48, "Up" },
    { 0x49, "Page Up" },
    { 0x4b, "Left" },
    { 0x4d, "Right" },
    { 0x4f, "End" },
    { 0x50, "Down" },
    { 0x51, "Page Down" },
    { 0x52, "Insert" },
    { 0x53, "Delete" },
    { 0x54, "<00>" },
    { 0x56, "Help" },
    { 0x5b, "Left Windows" },
    { 0x5c, "Right Windows" },
    { 0x5d, "Application" },
};

static struct obelisk_layout *grab_layout(void)
{
    struct obelisk_layout *layout;

    pthread_mutex_lock( &layout_lock );
    if ((layout = current_layout)) layout->ref++;
    pthread_mutex_unlock( &layout_lock );
    return layout;
}

static void release_layout( struct obelisk_layout *layout )
{
    LONG ref;

    pthread_mutex_lock( &layout_lock );
    ref = --layout->ref;
    pthread_mutex_unlock( &layout_lock );
    if (ref) return;
    free( layout->keymap );
    free( layout );
}

static WCHAR table_char( WCHAR c )
{
    return c ? c : WCH_NONE;
}

/* One line of the layout's table: the characters of a virtual key by modifier bits (Shift 1, Ctrl 2, Alt 4). Alt
 * alone types what the key types without it (WM_SYSCHAR), Ctrl+Alt is AltGr, and Ctrl alone is win32u's business. */
static void add_wchars( VK_TO_WCHARS8 **entry, BYTE vkey, WCHAR plain, WCHAR shift, WCHAR altgr, WCHAR shift_altgr,
                        BYTE attributes )
{
    VK_TO_WCHARS8 line = { .VirtualKey = vkey, .Attributes = attributes };

    line.wch[0] = line.wch[KBDALT] = table_char( plain );
    line.wch[KBDSHIFT] = line.wch[KBDALT | KBDSHIFT] = table_char( shift );
    line.wch[KBDCTRL] = line.wch[KBDCTRL | KBDSHIFT] = WCH_NONE;
    line.wch[KBDCTRL | KBDALT] = table_char( altgr );
    line.wch[KBDCTRL | KBDALT | KBDSHIFT] = table_char( shift_altgr );
    *(*entry)++ = line;
}

/* The KBDTABLES of a layout: win32u's US tables, where the character keys have the layout's virtual keys and
 * characters. */
static struct obelisk_layout *create_layout( struct obelisk_keymap *keymap )
{
    static const struct { BYTE vkey; WCHAR c; } same_everywhere[] =
    {
        { VK_TAB, '\t' }, { VK_BACK, '\b' }, { VK_ESCAPE, 0x1b }, { VK_RETURN, '\r' }, { VK_CANCEL, 3 },
        { VK_ADD, '+' }, { VK_DIVIDE, '/' }, { VK_MULTIPLY, '*' }, { VK_SUBTRACT, '-' },
    };
    struct obelisk_layout *layout;
    VK_TO_WCHARS8 *wchars;
    VSC_LPWSTR *name;
    VSC_VK *e0;
    WCHAR *text;
    MODIFIERS *modifiers;
    UINT key, i, mod;

    if (!(layout = calloc( 1, sizeof(*layout) ))) return NULL;
    C_ASSERT( sizeof(layout->modifiers) >= offsetof( MODIFIERS, ModNumber[8] ) );
    modifiers = (MODIFIERS *)layout->modifiers;
    layout->ref = 1;
    layout->keymap = keymap;

    layout->vk_to_bit[0].Vk = VK_SHIFT;
    layout->vk_to_bit[0].ModBits = KBDSHIFT;
    layout->vk_to_bit[1].Vk = VK_CONTROL;
    layout->vk_to_bit[1].ModBits = KBDCTRL;
    layout->vk_to_bit[2].Vk = VK_MENU;
    layout->vk_to_bit[2].ModBits = KBDALT;
    modifiers->pVkToBit = layout->vk_to_bit;
    modifiers->wMaxModBits = 7;
    for (mod = 0; mod < 8; mod++) modifiers->ModNumber[mod] = mod;

    /* scan code -> virtual key */
    C_ASSERT( sizeof(layout->vsc_to_vk) >= sizeof(us_vsc_to_vk) );
    C_ASSERT( sizeof(layout->vsc_to_vk_e0) >= sizeof(us_vsc_to_vk_e0) + 8 * sizeof(VSC_VK) );
    memcpy( layout->vsc_to_vk, us_vsc_to_vk, sizeof(us_vsc_to_vk) );
    memcpy( layout->vsc_to_vk_e0, us_vsc_to_vk_e0, sizeof(us_vsc_to_vk_e0) );
    memcpy( layout->vsc_to_vk_e1, us_vsc_to_vk_e1, sizeof(us_vsc_to_vk_e1) );
    e0 = layout->vsc_to_vk_e0 + ARRAY_SIZE(us_vsc_to_vk_e0) - 1;  /* its end mark */
    for (key = 0; key < OBELISK_KEYS; key++)
    {
        WORD scan = keymap->scan[key], vkey = keymap->vkey[key];
        if (keymap->key_of_vkey[vkey & 0xff] != key || vkey == VK_SPACE) continue;
        if (!(scan & 0xff)) continue;
        if (!(scan & 0x100)) layout->vsc_to_vk[scan & 0x7f] = vkey;
        else if (e0 < layout->vsc_to_vk_e0 + ARRAY_SIZE(layout->vsc_to_vk_e0) - 1)
        {
            e0->Vsc = scan & 0xff;
            e0->Vk = vkey | KBDEXT;
            e0++;
        }
    }

    /* virtual key -> characters */
    wchars = layout->vk_to_wchars;
    for (key = 0; key < OBELISK_KEYS; key++)
    {
        const WCHAR *c = keymap->chars[key];
        WORD vkey = keymap->vkey[key];
        BYTE attributes = 0;
        if (keymap->key_of_vkey[vkey & 0xff] != key) continue;
        if (!c[0] && !c[1] && !c[2] && !c[3]) continue;
        /* Caps Lock types what Shift types */
        if (c[OBELISK_LEVEL_CAPS] && c[OBELISK_LEVEL_CAPS] == c[OBELISK_LEVEL_SHIFT] && c[OBELISK_LEVEL_SHIFT] != c[0])
            attributes = CAPLOK;
        add_wchars( &wchars, vkey, c[0], c[OBELISK_LEVEL_SHIFT], keymap->altgr ? c[OBELISK_LEVEL_ALTGR] : 0,
                    keymap->altgr ? c[OBELISK_LEVEL_ALTGR | OBELISK_LEVEL_SHIFT] : 0, attributes );
    }
    if (keymap->key_of_vkey[VK_SPACE] == 0xff) add_wchars( &wchars, VK_SPACE, ' ', ' ', 0, 0, 0 );
    for (i = 0; i < ARRAY_SIZE(same_everywhere); i++)
        add_wchars( &wchars, same_everywhere[i].vkey, same_everywhere[i].c, same_everywhere[i].c, 0, 0, 0 );
    /* the keypad's decimal key: a comma in the layouts that count with one */
    key = keymap->chars[0x41][0] ? keymap->chars[0x41][0] : '.';
    add_wchars( &wchars, VK_DECIMAL, key, key, 0, 0, 0 );
    for (i = 0; i < 10; i++) add_wchars( &wchars, VK_NUMPAD0 + i, '0' + i, 0, 0, 0, 0 );
    layout->vk_to_wchar_table[0].pVkToWchars = (VK_TO_WCHARS1 *)layout->vk_to_wchars;
    layout->vk_to_wchar_table[0].nModifications = 8;
    layout->vk_to_wchar_table[0].cbSize = sizeof(layout->vk_to_wchars[0]);

    /* key names: the US ones; a character key is named by win32u after what it types */
    text = layout->names;
    name = layout->key_names;
    for (i = 0; i < ARRAY_SIZE(us_key_names); i++, name++)
    {
        const char *p = us_key_names[i].name;
        name->vsc = us_key_names[i].vsc;
        name->pwsz = text;
        while ((*text++ = (unsigned char)*p++)) {}
    }
    name = layout->key_names_ext;
    for (i = 0; i < ARRAY_SIZE(us_key_names_ext); i++, name++)
    {
        const char *p = us_key_names_ext[i].name;
        name->vsc = us_key_names_ext[i].vsc;
        name->pwsz = text;
        while ((*text++ = (unsigned char)*p++)) {}
    }

    layout->tables.pCharModifiers = modifiers;
    layout->tables.pVkToWcharTable = layout->vk_to_wchar_table;
    layout->tables.pKeyNames = layout->key_names;
    layout->tables.pKeyNamesExt = layout->key_names_ext;
    layout->tables.pusVSCtoVK = layout->vsc_to_vk;
    layout->tables.bMaxVSCtoVK = ARRAY_SIZE(us_vsc_to_vk);
    layout->tables.pVSCtoVK_E0 = layout->vsc_to_vk_e0;
    layout->tables.pVSCtoVK_E1 = layout->vsc_to_vk_e1;
    layout->tables.fLocaleFlags = MAKELONG( keymap->altgr ? KLLF_ALTGR : 0, KBD_VERSION );
    return layout;
}

/* A piece of the Mac's keyboard layout; the last one makes it the layout of the process. */
static void handle_keymap( const struct wsd_ev_keymap *ev, const char *bytes, UINT len )
{
    static char *blob;        /* event thread */
    static UINT serial, received, total;
    struct obelisk_layout *layout, *previous;
    struct obelisk_keymap *keymap;

    if (!ev->offset)
    {
        free( blob );
        blob = NULL;
        received = 0;
        serial = ev->serial;
        total = ev->total;
        if (!total || total > (1u << 20) || !(blob = malloc( total ))) return;
    }
    if (!blob || ev->serial != serial || ev->total != total || ev->offset != received || len > total - received)
    {
        WARN( "keyboard layout %u: piece at %u out of order\n", ev->serial, ev->offset );
        return;
    }
    memcpy( blob + received, bytes, len );
    if ((received += len) < total) return;

    keymap = obelisk_keymap_create( blob, total, serial );
    free( blob );
    blob = NULL;
    if (!keymap || !(layout = create_layout( keymap )))
    {
        WARN( "keyboard layout %u refused, the keys stay as they are\n", serial );
        free( keymap );
        return;
    }
    TRACE( "keyboard layout %u: %s%s%s, %u compositions\n", serial, keymap->source, keymap->iso ? ", ISO" : "",
           keymap->altgr ? ", right Option is AltGr" : "", keymap->composition_count );
    pthread_mutex_lock( &layout_lock );
    previous = current_layout;
    current_layout = layout;
    pthread_mutex_unlock( &layout_lock );
    if (previous)
    {
        HWND foreground = NtUserGetForegroundWindow();
        DWORD pid = 0;
        release_layout( previous );
        /* the player picked another input source: the program with the keyboard hears of it as it does on Windows,
         * and reads the keys' names and characters again if it keeps them */
        if (foreground && NtUserGetWindowThread( foreground, &pid ) && pid == GetCurrentProcessId())
            NtUserPostMessage( foreground, WM_INPUTLANGCHANGE, DEFAULT_CHARSET, (LPARAM)NtUserGetKeyboardLayout( 0 ) );
    }
}

static const KBDTABLES *OBELISK_KbdLayerDescriptor( HKL hkl )
{
    struct obelisk_layout *layout = grab_layout();
    return layout ? &layout->tables : NULL;
}

static void OBELISK_ReleaseKbdTables( const KBDTABLES *tables )
{
    release_layout( CONTAINING_RECORD( tables, struct obelisk_layout, tables ) );
}

/* The characters of the layout's own keys, dead keys included; win32u types the others from the tables (Return,
 * the keypad, Ctrl with a key). The accent of a dead key waits in its thread for the next character. */
static INT OBELISK_ToUnicodeEx( UINT virt, UINT scan, const BYTE *state, WCHAR *str, int size, UINT flags, HKL hkl )
{
    static __thread struct { UINT serial; WCHAR accent; } dead;
    struct obelisk_layout *layout;
    BOOL ctrl = state[VK_CONTROL] & 0x80, alt = state[VK_MENU] & 0x80;
    BOOL menu = flags & 1, keep = flags & 4;
    WCHAR none = 0, *pending = &dead.accent;
    UINT level = 0;
    INT ret;

    if (scan & 0x8000) return -2;  /* a key going up types nothing */
    if (ctrl && !alt) return -2;
    if (!(layout = grab_layout())) return -2;
    if (ctrl && alt && !layout->keymap->altgr)
    {
        release_layout( layout );
        return -2;
    }
    if (dead.serial != layout->keymap->serial)
    {
        dead.serial = layout->keymap->serial;
        dead.accent = 0;
    }
    if (state[VK_SHIFT] & 0x80) level |= OBELISK_LEVEL_SHIFT;
    if (ctrl && alt) level |= OBELISK_LEVEL_ALTGR;
    if (state[VK_CAPITAL] & 1) level |= OBELISK_LEVEL_CAPS;
    if (menu) pending = &none;  /* a menu's accelerator: the key's character, whatever was typed before */
    ret = obelisk_keymap_to_unicode( layout->keymap, virt, level, menu || keep, pending, str, size );
    if (menu && ret == -1) ret = 1;
    if (ret >= -1)
    {
        INT count = ret < 0 ? 1 : ret;
        if (count < size) str[count] = 0;
        TRACE( "vkey %#x level %u -> %d %s\n", virt, level, ret, debugstr_wn( str, min( count, size ) ) );
    }
    release_layout( layout );
    return ret;
}

/***********************************************************************
 *           input
 */
static void send_key_input( HWND hwnd, WORD vkey, WORD scan, BOOL down )
{
    INPUT input = { .type = INPUT_KEYBOARD };

    input.ki.wVk = vkey;
    input.ki.wScan = scan & 0xff;
    if (scan & 0x100) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    NtUserSendHardwareInput( hwnd, 0, &input, 0 );
}

static void send_key( HWND hwnd, const struct wsd_ev_key *ev )
{
    struct obelisk_layout *layout;
    UINT code = ev->mac_keycode;
    BOOL down = ev->flags & WSD_KEY_DOWN, altgr = FALSE;
    WORD vkey, scan;

    if (code >= OBELISK_KEYS)
    {
        TRACE( "unmapped mac keycode %#x\n", ev->mac_keycode );
        return;
    }
    /* The virtual key is the one the Mac's layout gives the key (A on the key that types "a"), and the scan code
     * the one of the key's place, both as the layout's tables tell them to the program (MapVirtualKey): wsd reads
     * the layout again for the keyboard a key comes from before it sends the key. */
    if ((layout = grab_layout()))
    {
        vkey = layout->keymap->vkey[code];
        scan = layout->keymap->scan[code];
        altgr = layout->keymap->altgr;
        release_layout( layout );
    }
    else
    {
        /* Without a layout (a wsd that sends none), the key of its place on a US keyboard. On an ISO keyboard
         * kVK_ISO_Section is the key left of 1 and kVK_ANSI_Grave the one right of the left Shift: each takes the
         * other's Windows key, so that a key keeps its scan code wherever it is (winemac.drv swaps their scan codes
         * the same way). */
        if ((ev->flags & WSD_KEY_ISO) && (code == 0x0a || code == 0x32)) code ^= 0x0a ^ 0x32;
        vkey = obelisk_default_keys[code].vkey;
        scan = obelisk_default_keys[code].scan;
    }
    if (!vkey)
    {
        TRACE( "unmapped mac keycode %#x\n", ev->mac_keycode );
        return;
    }
    TRACE( "hwnd %p kvk %#x vk %#x scan %#x %s\n", hwnd, ev->mac_keycode, vkey, scan, down ? "down" : "up" );
    /* AltGr: where the layout types ASCII with Option, the right Option key is the AltGr of a PC keyboard, which
     * Windows sends as the left Control then the right Alt. */
    if (ev->mac_keycode == 0x3d /* kVK_RightOption */)
    {
        if (down && altgr && !altgr_control)
        {
            altgr_control = TRUE;
            send_key_input( hwnd, VK_LCONTROL, 0x1d, TRUE );
        }
        send_key_input( hwnd, vkey, scan, down );
        if (!down && altgr_control)
        {
            altgr_control = FALSE;
            send_key_input( hwnd, VK_LCONTROL, 0x1d, FALSE );
        }
        return;
    }
    send_key_input( hwnd, vkey, scan, down );
}

static void send_mouse( HWND hwnd, DWORD flags, int x, int y, DWORD data )
{
    INPUT input = { .type = INPUT_MOUSE };

    input.mi.dx = x;
    input.mi.dy = y;
    input.mi.mouseData = data;
    input.mi.dwFlags = flags;
    TRACE( "hwnd %p flags %#x %d,%d data %#x\n", hwnd, (UINT)flags, x, y, (UINT)data );
    NtUserSendHardwareInput( hwnd, 0, &input, 0 );
}

static void handle_event( const struct wsd_hdr *hdr, const char *payload )
{
    const struct wsd_window *w = (const struct wsd_window *)payload;
    struct obelisk_win *win = NULL;
    HWND hwnd = 0;
    RECT visible = { 0 };

    if (hdr->type != WSD_EV_RELEASE && hdr->type != WSD_EV_PRESENTED && hdr->type != WSD_EV_ERROR &&
        hdr->type != WSD_EV_DISPLAY && hdr->type != WSD_EV_KEYMAP && hdr->len >= 4)
    {
        pthread_mutex_lock( &win_lock );
        if ((win = get_win_by_id( w->window_id )))
        {
            hwnd = win->hwnd;
            visible = win->visible;
        }
        pthread_mutex_unlock( &win_lock );
    }

    switch (hdr->type)
    {
    case WSD_EV_RELEASE:
    {
        const struct wsd_ev_release *ev = (const void *)payload;
        void (*listener)( UINT, UINT, UINT64 ) = release_listener;
        UINT i, j;
        if (listener && (ev->buffer_id & 0x80000000)) listener( ev->buffer_id, ev->shown, ev->serial );
        pthread_mutex_lock( &win_lock );
        for (i = 0; i < WSD_MAX_WINDOWS; i++)
            for (j = 0; j < NB_BUFFERS; j++)
                if (windows[i].buffers[j].id == ev->buffer_id) windows[i].buffers[j].busy = FALSE;
        pthread_mutex_unlock( &win_lock );
        break;
    }
    case WSD_EV_PRESENTED:
    {
        const struct wsd_ev_presented *ev = (const void *)payload;
        void (*listener)( UINT, UINT64, UINT64 ) = presented_listener;
        if (listener && (ev->buffer_id & 0x80000000)) listener( ev->buffer_id, ev->serial, ev->presented_ns );
        break;
    }
    case WSD_EV_KEY:
    {
        const struct wsd_ev_key *ev = (const void *)payload;
        UINT64 rx = latency_trace ? now_ns() : 0;
        if (hwnd) send_key( hwnd, ev );
        /* guest CLOCK_MONOTONIC is the host CLOCK_UPTIME_RAW under LKD, so these are comparable to wsd times */
        if (latency_trace)
            fprintf( stderr, "wineobelisk latency: key %#x %s event %llu read +%.3f ms sent +%.3f ms\n",
                     ev->mac_keycode, (ev->flags & WSD_KEY_DOWN) ? "down" : "up", (unsigned long long)ev->time_ns,
                     (rx - ev->time_ns) / 1e6, (now_ns() - ev->time_ns) / 1e6 );
        break;
    }
    case WSD_EV_MOTION:
    {
        const struct wsd_ev_motion *ev = (const void *)payload;
        /* sent before the last SetCursorPos: Wine's cursor would go back where the game moved it from */
        if (ev->time_ns <= __atomic_load_n( &last_warp_ns, __ATOMIC_RELAXED )) break;
        if (hwnd) send_mouse( hwnd, MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                              visible.left + (int)ev->x, visible.top + (int)ev->y, 0 );
        break;
    }
    case WSD_EV_RELATIVE:
    {
        const struct wsd_ev_relative *ev = (const void *)payload;
        int dx, dy;
        if (!hwnd) break;
        relative_rest_x += ev->dx;
        relative_rest_y += ev->dy;
        dx = (int)relative_rest_x;
        dy = (int)relative_rest_y;
        relative_rest_x -= dx;
        relative_rest_y -= dy;
        if (dx || dy) send_mouse( hwnd, MOUSEEVENTF_MOVE, dx, dy, 0 );
        break;
    }
    case WSD_EV_BUTTON:
    {
        const struct wsd_ev_button *ev = (const void *)payload;
        DWORD flags, data = 0;
        if (!hwnd) break;
        switch (ev->button)
        {
        case 0: flags = MOUSEEVENTF_LEFTDOWN; break;
        case 1: flags = MOUSEEVENTF_RIGHTDOWN; break;
        case 2: flags = MOUSEEVENTF_MIDDLEDOWN; break;
        case 3: flags = MOUSEEVENTF_XDOWN; data = XBUTTON1; break;
        case 4: flags = MOUSEEVENTF_XDOWN; data = XBUTTON2; break;
        default: flags = 0; break;
        }
        if (!flags) break;
        if (!ev->down) flags <<= 1;
        /* in relative mode the Mac pointer stands still where the game's cursor is not */
        if (relative_window != ev->window_id)
            send_mouse( hwnd, MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE,
                        visible.left + (int)ev->x, visible.top + (int)ev->y, 0 );
        send_mouse( hwnd, flags, 0, 0, data );
        break;
    }
    case WSD_EV_WHEEL:
    {
        const struct wsd_ev_wheel *ev = (const void *)payload;
        if (!hwnd) break;
        if (ev->dy) send_mouse( hwnd, MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(int)(ev->dy * (ev->precise ? 4 : WHEEL_DELTA)) );
        if (ev->dx) send_mouse( hwnd, MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)(int)(-ev->dx * (ev->precise ? 4 : WHEEL_DELTA)) );
        break;
    }
    case WSD_EV_FOCUS:
    {
        const struct wsd_window_flag *ev = (const void *)payload;
        TRACE( "focus %u hwnd %p\n", ev->value, hwnd );
        if (hwnd && ev->value) NtUserPostMessage( hwnd, WM_OBELISK_TAKE_FOCUS, 0, 0 );
        break;
    }
    case WSD_EV_CLOSE:
        if (hwnd) NtUserPostMessage( hwnd, WM_SYSCOMMAND, SC_CLOSE, 0 );
        break;
    case WSD_EV_FRAME:
    {
        const struct wsd_window_frame *ev = (const void *)payload;
        BOOL post = FALSE;
        pthread_mutex_lock( &win_lock );
        if ((win = get_win_by_id( ev->window_id )) && win->hwnd == hwnd)
        {
            SetRect( &win->pending, ev->x, ev->y, ev->x + (int)ev->width, ev->y + (int)ev->height );
            post = !win->frame_pending;  /* one message for a burst of moves */
            win->frame_pending = TRUE;
        }
        pthread_mutex_unlock( &win_lock );
        if (hwnd && post) NtUserPostMessage( hwnd, WM_OBELISK_APPLY_FRAME, 0, 0 );
        break;
    }
    case WSD_EV_MINIMIZE:
    {
        const struct wsd_window_flag *ev = (const void *)payload;
        BOOL changed = FALSE;
        pthread_mutex_lock( &win_lock );
        if ((win = get_win_by_id( ev->window_id )) && win->hwnd == hwnd && win->minimized != !!ev->value)
        {
            win->minimized = !!ev->value;
            changed = TRUE;
        }
        pthread_mutex_unlock( &win_lock );
        TRACE( "hwnd %p minimized %u by the user (changed %u)\n", hwnd, ev->value, changed );
        if (hwnd && changed) NtUserPostMessage( hwnd, WM_SYSCOMMAND, ev->value ? SC_MINIMIZE : SC_RESTORE, 0 );
        /* restored by the user, as from the taskbar on Windows: the window takes the focus once restored */
        if (hwnd && changed && !ev->value) NtUserPostMessage( hwnd, WM_OBELISK_TAKE_FOCUS, 0, 0 );
        break;
    }
    case WSD_EV_ZOOM:
    {
        DWORD style = hwnd ? NtUserGetWindowLongW( hwnd, GWL_STYLE ) : 0;
        if (style & WS_MAXIMIZEBOX)
            NtUserPostMessage( hwnd, WM_SYSCOMMAND, (style & WS_MAXIMIZE) ? SC_RESTORE : SC_MAXIMIZE, 0 );
        break;
    }
    case WSD_EV_RESIZE:
    {
        const struct wsd_ev_resize *ev = (const void *)payload;
        TRACE( "hwnd %p resized to %ux%u\n", hwnd, ev->width_px, ev->height_px );
        break;
    }
    case WSD_EV_APP_DEACTIVATED:
    {
        HWND foreground = NtUserGetForegroundWindow();
        DWORD pid = 0;
        BOOL keep = FALSE;
        /* Another Mac app is in front: the game goes to the background as winemac.drv sends it there
         * (macdrv_app_deactivated), the desktop taking the foreground, and gets WM_ACTIVATEAPP. The process that has
         * the foreground does it. A game that minimizes itself and resizes its buffers on WM_ACTIVATEAPP(0) can be
         * given WINE_OBELISK_KEEP_FOREGROUND by its Obelisk profile: its fullscreen window then keeps the foreground,
         * and the game is not told it lost the foreground (no WM_ACTIVATEAPP/WM_ACTIVATE). wsd still suspends the
         * relative mouse and releases the held keys as the window resigns key. */
        if (!foreground || !NtUserGetWindowThread( foreground, &pid ) || pid != GetCurrentProcessId()) break;
        if (keep_foreground)
        {
            pthread_mutex_lock( &win_lock );
            if ((win = get_win( foreground )) && win->fullscreen) keep = TRUE;
            pthread_mutex_unlock( &win_lock );
        }
        if (keep)
        {
            TRACE( "app deactivated, fullscreen %p kept in the foreground\n", foreground );
            break;
        }
        TRACE( "app deactivated, foreground %p to the desktop\n", foreground );
        NtUserClipCursor( NULL );
        NtUserSetForegroundWindowInternal( NtUserGetDesktopWindow() );
        break;
    }
    case WSD_EV_DISPLAY:
    {
        struct wsd_hello_reply display = { 0 };
        DWORD pid = 0;
        memcpy( &display, payload, min( hdr->len, sizeof(display) ));
        pthread_mutex_lock( &display_lock );
        hello = display;
        display_serial++;
        pthread_cond_broadcast( &display_cond );
        pthread_mutex_unlock( &display_lock );
        TRACE( "display now %ux%u @%u Hz\n", display.display_width_px, display.display_height_px, display.max_refresh_hz );
        /* Each process keeps the new size; the desktop's tells Wine, which asks UpdateDisplayDevices again
         * (winemac.drv's macdrv_displays_changed). */
        NtUserGetWindowThread( NtUserGetDesktopWindow(), &pid );
        if (pid == GetCurrentProcessId()) NtUserCallNoParam( NtUserCallNoParam_DisplayModeChanged );
        break;
    }
    case WSD_EV_KEYMAP:
    {
        const struct wsd_ev_keymap *ev = (const void *)payload;
        if (hdr->len >= sizeof(*ev)) handle_keymap( ev, payload + sizeof(*ev), hdr->len - sizeof(*ev) );
        break;
    }
    case WSD_EV_ERROR:
    {
        const struct wsd_ev_error *ev = (const void *)payload;
        ERR( "wsd rejected request %u: %d\n", ev->request_type, ev->code );
        if (ev->request_type == WSD_SET_DISPLAY_MODE) /* a wsd from before the display mode: nothing to wait for */
        {
            pthread_mutex_lock( &display_lock );
            display_serial++;
            pthread_cond_broadcast( &display_cond );
            pthread_mutex_unlock( &display_lock );
        }
        break;
    }
    default:
        break;
    }
}

/***********************************************************************
 *           API for other unix libraries of the process (nptunix.so, Neptune D3D presentation)
 *
 * Found with dlopen( "wineobelisk.so", RTLD_NOLOAD ) + dlsym. Buffer ids with bit 31 set belong to the caller;
 * their EV_RELEASE events go to the listener (called on the event thread).
 */
#define OBELISK_EXPORT __attribute__((visibility("default")))

OBELISK_EXPORT BOOL obelisk_wsd_send( UINT type, const void *payload, UINT len, int fd )
{
    return wsd_send( type, payload, len, NULL, 0, fd );
}

OBELISK_EXPORT void obelisk_wsd_set_release_listener( void (*listener)( UINT buffer_id, UINT shown, UINT64 serial ) )
{
    release_listener = listener;
}

OBELISK_EXPORT void obelisk_wsd_set_presented_listener( void (*listener)( UINT buffer_id, UINT64 serial, UINT64 presented_ns ) )
{
    presented_listener = listener;
}

/* client area of a wsd window, in window pixels (top-left of the window's visible rect) */
OBELISK_EXPORT BOOL obelisk_wsd_client_rect( UINT window_id, int *x, int *y, UINT *width, UINT *height )
{
    struct obelisk_win *win;
    BOOL ret = FALSE;
    pthread_mutex_lock( &win_lock );
    if ((win = get_win_by_id( window_id )) && win->client.right > win->client.left && win->client.bottom > win->client.top)
    {
        *x = win->client.left;
        *y = win->client.top;
        *width = win->client.right - win->client.left;
        *height = win->client.bottom - win->client.top;
        ret = TRUE;
    }
    pthread_mutex_unlock( &win_lock );
    return ret;
}

OBELISK_EXPORT void obelisk_wsd_set_external( UINT window_id, BOOL external )
{
    struct obelisk_win *win;
    pthread_mutex_lock( &win_lock );
    if ((win = get_win_by_id( window_id ))) win->external = external;
    pthread_mutex_unlock( &win_lock );
}

OBELISK_EXPORT BOOL obelisk_wsd_get_display( UINT *width, UINT *height, UINT *refresh_hz )
{
    BOOL ret;

    pthread_mutex_lock( &display_lock );
    if ((ret = hello.display_width_px != 0))
    {
        *width = hello.display_width_px;
        *height = hello.display_height_px;
        *refresh_hz = hello.max_refresh_hz ? hello.max_refresh_hz : 60;
    }
    pthread_mutex_unlock( &display_lock );
    return ret;
}

/* The screen as it is without the display mode a game chose, for the modes DXGI lists. */
OBELISK_EXPORT BOOL obelisk_wsd_get_native_display( UINT *width, UINT *height )
{
    BOOL ret;

    pthread_mutex_lock( &display_lock );
    if ((ret = hello.display_width_px != 0))
    {
        *width = hello.native_width_px ? hello.native_width_px : hello.display_width_px;
        *height = hello.native_height_px ? hello.native_height_px : hello.display_height_px;
    }
    pthread_mutex_unlock( &display_lock );
    return ret;
}

OBELISK_EXPORT UINT64 obelisk_wsd_next_serial(void)
{
    return __atomic_add_fetch( &present_serial, 1, __ATOMIC_SEQ_CST );
}

/***********************************************************************
 *           unix calls
 */
static const struct user_driver_funcs obeliskdrv_funcs =
{
    .pActivateWindow = OBELISK_ActivateWindow,
    .pChangeDisplaySettings = OBELISK_ChangeDisplaySettings,
    .pClipCursor = OBELISK_ClipCursor,
    .pCreateWindowSurface = OBELISK_CreateWindowSurface,
    .pDestroyCursorIcon = OBELISK_DestroyCursorIcon,
    .pDestroyWindow = OBELISK_DestroyWindow,
    .pGetWindowStyleMasks = OBELISK_GetWindowStyleMasks,
    .pKbdLayerDescriptor = OBELISK_KbdLayerDescriptor,
    .pReleaseKbdTables = OBELISK_ReleaseKbdTables,
    .pSetCursor = OBELISK_SetCursor,
    .pSetCursorPos = OBELISK_SetCursorPos,
    .pSetWindowText = OBELISK_SetWindowText,
    .pSystemParametersInfo = OBELISK_SystemParametersInfo,
    .pToUnicodeEx = OBELISK_ToUnicodeEx,
    .pUpdateDisplayDevices = OBELISK_UpdateDisplayDevices,
    .pWindowMessage = OBELISK_WindowMessage,
    .pWindowPosChanged = OBELISK_WindowPosChanged,
    .pWindowPosChanging = OBELISK_WindowPosChanging,
};

static NTSTATUS obeliskdrv_init( void *arg )
{
    const char *path = getenv( "WINE_OBELISK_SOCKET" );
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    struct wsd_hello req = { WSD_PROTOCOL_VERSION, getpid() };
    char payload[WSD_MAX_PAYLOAD];
    struct wsd_hdr hdr;

    if (!path) path = "/run/obelisk/wsd.sock";
    if (strlen( path ) >= sizeof(addr.sun_path)) return STATUS_UNSUCCESSFUL;
    strcpy( addr.sun_path, path );
    if ((wsd_fd = socket( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0 )) < 0) return STATUS_UNSUCCESSFUL;
    if (connect( wsd_fd, (struct sockaddr *)&addr, sizeof(addr) ) < 0)
    {
        WARN( "cannot connect to wsd at %s: %s\n", path, strerror(errno) );
        goto fail;
    }
    if (!wsd_send( WSD_HELLO, &req, sizeof(req), NULL, 0, -1 )) goto fail;
    do
    {
        if (!wsd_read( &hdr, payload )) goto fail;
    } while (hdr.type != WSD_EV_HELLO);
    memcpy( &hello, payload, min( hdr.len, sizeof(hello) ));
    TRACE( "wsd pid %u display %ux%u @%u Hz scale %u/1000\n", hello.server_pid, hello.display_width_px,
           hello.display_height_px, hello.max_refresh_hz, hello.backing_scale_x1000 );
    __wine_set_user_driver( &obeliskdrv_funcs, WINE_GDI_DRIVER_VERSION );
    return STATUS_SUCCESS;

fail:
    close( wsd_fd );
    wsd_fd = -1;
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS obeliskdrv_read_events( void *arg )
{
    char payload[WSD_MAX_PAYLOAD];
    struct wsd_hdr hdr;
    const char *env = getenv( "WINE_OBELISK_KEEP_FOREGROUND" );

    latency_trace = getenv( "WINE_OBELISK_LATENCY" ) != NULL;
    keep_foreground = env && *env && *env != '0';

    while (wsd_read( &hdr, payload )) handle_event( &hdr, payload );
    return STATUS_UNSUCCESSFUL;
}

/* the program's icon becomes the Dock and Cmd-Tab icon */
static NTSTATUS obeliskdrv_set_app_icon( void *arg )
{
    const struct obeliskdrv_app_icon_params *params = arg;
    struct wsd_app_icon icon = { params->width, params->height, params->width * 4 };
    size_t size = (size_t)icon.stride * icon.height;
    size_t map_size = (size + SHM_ALIGN - 1) & ~(size_t)(SHM_ALIGN - 1);
    void *bits;
    BOOL ret;
    int fd;

    if (!params->width || !params->height || params->width > 1024 || params->height > 1024)
        return STATUS_INVALID_PARAMETER;
    if ((fd = syscall( __NR_memfd_create, "wsd-icon", 1 /* MFD_CLOEXEC */ )) < 0) return STATUS_UNSUCCESSFUL;
    if (ftruncate( fd, map_size ) < 0 ||
        (bits = mmap( NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 )) == MAP_FAILED)
    {
        close( fd );
        return STATUS_UNSUCCESSFUL;
    }
    memcpy( bits, params->bits, size );
    munmap( bits, map_size );
    icon.size = size;
    ret = wsd_send( WSD_APP_ICON, &icon, sizeof(icon), NULL, 0, fd );
    close( fd );
    TRACE( "icon %ux%u -> %d\n", params->width, params->height, ret );
    return ret ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    obeliskdrv_init,
    obeliskdrv_read_events,
    obeliskdrv_set_app_icon,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == obeliskdrv_unix_func_count );

#ifdef _WIN64

/* The calls of a 32-bit process (new WoW64): only the icon's parameters hold a pointer. */
static NTSTATUS wow64_obeliskdrv_set_app_icon( void *arg )
{
    struct
    {
        UINT  width, height;
        ULONG bits;
    } const *params32 = arg;
    struct obeliskdrv_app_icon_params params =
    {
        .width = params32->width,
        .height = params32->height,
        .bits = ULongToPtr( params32->bits ),
    };

    return obeliskdrv_set_app_icon( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    obeliskdrv_init,
    obeliskdrv_read_events,
    wow64_obeliskdrv_set_app_icon,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == obeliskdrv_unix_func_count );

#endif /* _WIN64 */
