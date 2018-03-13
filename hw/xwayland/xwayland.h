/*
 * Copyright © 2014 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of the
 * copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifndef XWAYLAND_H
#define XWAYLAND_H

#include <dix-config.h>

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <wayland-client.h>

#include <X11/X.h>

#include <fb.h>
#include <input.h>
#include <dix.h>
#include <randrstr.h>
#include <exevents.h>

#include "relative-pointer-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "xwayland-keyboard-grab-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

struct xwl_format {
    uint32_t format;
    int num_modifiers;
    uint64_t *modifiers;
};

struct xwl_screen {
    int width;
    int height;
    int depth;
    ScreenPtr screen;
    int expecting_event;
    enum RootClipMode root_clip_mode;

    int wm_fd;
    int listen_fds[5];
    int listen_fd_count;
    int rootless;
    int glamor;
    int present;

    CreateScreenResourcesProcPtr CreateScreenResources;
    CloseScreenProcPtr CloseScreen;
    RealizeWindowProcPtr RealizeWindow;
    UnrealizeWindowProcPtr UnrealizeWindow;
    XYToWindowProcPtr XYToWindow;

    struct xorg_list output_list;
    struct xorg_list seat_list;
    struct xorg_list damage_window_list;

    int wayland_fd;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_registry *input_registry;
    struct wl_compositor *compositor;
    struct zwp_tablet_manager_v2 *tablet_manager;
    struct wl_shm *shm;
    struct wl_shell *shell;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_xwayland_keyboard_grab_manager_v1 *wp_grab;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    uint32_t serial;

#define XWL_FORMAT_ARGB8888 (1 << 0)
#define XWL_FORMAT_XRGB8888 (1 << 1)
#define XWL_FORMAT_RGB565   (1 << 2)

    int prepare_read;
    int wait_flush;

    char *device_name;
    int drm_fd;
    int fd_render_node;
    int drm_authenticated;
    struct wl_drm *drm;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    uint32_t num_formats;
    struct xwl_format *formats;
    uint32_t capabilities;
    void *egl_display, *egl_context;
    struct gbm_device *gbm;
    struct glamor_context *glamor_ctx;
    int dmabuf_capable;

    Atom allow_commits_prop;
};

struct xwl_window {
    struct xwl_screen *xwl_screen;
    struct wl_surface *surface;
    struct wl_shell_surface *shell_surface;
    WindowPtr window;
    DamagePtr damage;
    struct xorg_list link_damage;
    struct wl_callback *frame_callback;
    Bool allow_commits;

    /* present */
    RRCrtcPtr present_crtc_fake;
    struct xorg_list present_link;
    WindowPtr present_window;
    uint64_t present_msc;
    uint64_t present_ust;

    OsTimerPtr present_timer;
    Bool present_timer_firing;

    struct wl_callback *present_frame_callback;
    struct wl_callback *present_sync_callback;

    struct xorg_list present_event_list;
    struct xorg_list present_release_queue;
};

struct xwl_present_event {
    uint64_t event_id;
    uint64_t target_msc;

    Bool abort;
    Bool pending;
    Bool buffer_released;

    WindowPtr present_window;
    struct xwl_window *xwl_window;
    struct wl_buffer *buffer;

    struct xorg_list list;
};

#define MODIFIER_META 0x01

struct xwl_touch {
    struct xwl_window *window;
    int32_t id;
    int x, y;
    struct xorg_list link_touch;
};

struct xwl_pointer_warp_emulator {
    struct xwl_seat *xwl_seat;
    struct xwl_window *locked_window;
    struct zwp_locked_pointer_v1 *locked_pointer;
};

struct xwl_cursor {
    void (* update_proc) (struct xwl_cursor *);
    struct wl_surface *surface;
    struct wl_callback *frame_cb;
    Bool needs_update;
};

struct xwl_seat {
    DeviceIntPtr pointer;
    DeviceIntPtr relative_pointer;
    DeviceIntPtr keyboard;
    DeviceIntPtr touch;
    DeviceIntPtr stylus;
    DeviceIntPtr eraser;
    DeviceIntPtr puck;
    struct xwl_screen *xwl_screen;
    struct wl_seat *seat;
    struct wl_pointer *wl_pointer;
    struct zwp_relative_pointer_v1 *wp_relative_pointer;
    struct wl_keyboard *wl_keyboard;
    struct wl_touch *wl_touch;
    struct zwp_tablet_seat_v2 *tablet_seat;
    struct wl_array keys;
    struct xwl_window *focus_window;
    struct xwl_window *tablet_focus_window;
    uint32_t id;
    uint32_t pointer_enter_serial;
    struct xorg_list link;
    CursorPtr x_cursor;
    struct xwl_cursor cursor;
    WindowPtr last_xwindow;

    struct xorg_list touches;

    size_t keymap_size;
    char *keymap;
    struct wl_surface *keyboard_focus;

    struct xorg_list sync_pending;

    struct xwl_pointer_warp_emulator *pointer_warp_emulator;

    struct xwl_window *cursor_confinement_window;
    struct zwp_confined_pointer_v1 *confined_pointer;

    struct {
        Bool has_absolute;
        wl_fixed_t x;
        wl_fixed_t y;

        Bool has_relative;
        double dx;
        double dy;
        double dx_unaccel;
        double dy_unaccel;
    } pending_pointer_event;

    struct xorg_list tablets;
    struct xorg_list tablet_tools;
    struct xorg_list tablet_pads;
    struct zwp_xwayland_keyboard_grab_v1 *keyboard_grab;
};

struct xwl_tablet {
    struct xorg_list link;
    struct zwp_tablet_v2 *tablet;
    struct xwl_seat *seat;
};

struct xwl_tablet_tool {
    struct xorg_list link;
    struct zwp_tablet_tool_v2 *tool;
    struct xwl_seat *seat;

    DeviceIntPtr xdevice;
    uint32_t proximity_in_serial;
    uint32_t x;
    uint32_t y;
    uint32_t pressure;
    float tilt_x;
    float tilt_y;
    float rotation;
    float slider;

    uint32_t buttons_now,
             buttons_prev;

    int32_t wheel_clicks;

    struct xwl_cursor cursor;
};

struct xwl_tablet_pad_ring {
    unsigned int index;
    struct xorg_list link;
    struct xwl_tablet_pad_group *group;
    struct zwp_tablet_pad_ring_v2 *ring;
};

struct xwl_tablet_pad_strip {
    unsigned int index;
    struct xorg_list link;
    struct xwl_tablet_pad_group *group;
    struct zwp_tablet_pad_strip_v2 *strip;
};

struct xwl_tablet_pad_group {
    struct xorg_list link;
    struct xwl_tablet_pad *pad;
    struct zwp_tablet_pad_group_v2 *group;

    struct xorg_list pad_group_ring_list;
    struct xorg_list pad_group_strip_list;
};

struct xwl_tablet_pad {
    struct xorg_list link;
    struct zwp_tablet_pad_v2 *pad;
    struct xwl_seat *seat;

    DeviceIntPtr xdevice;

    unsigned int nbuttons;
    struct xorg_list pad_group_list;
};

struct xwl_output {
    struct xorg_list link;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg_output;
    uint32_t server_output_id;
    struct xwl_screen *xwl_screen;
    RROutputPtr randr_output;
    RRCrtcPtr randr_crtc;
    int32_t x, y, width, height, refresh;
    Rotation rotation;
    Bool wl_output_done;
    Bool xdg_output_done;
};

struct xwl_pixmap;

void xwl_sync_events (struct xwl_screen *xwl_screen);

Bool xwl_screen_init_cursor(struct xwl_screen *xwl_screen);

struct xwl_screen *xwl_screen_get(ScreenPtr screen);

void xwl_tablet_tool_set_cursor(struct xwl_tablet_tool *tool);
void xwl_seat_set_cursor(struct xwl_seat *xwl_seat);

void xwl_seat_destroy(struct xwl_seat *xwl_seat);

void xwl_seat_clear_touch(struct xwl_seat *xwl_seat, WindowPtr window);

void xwl_seat_emulate_pointer_warp(struct xwl_seat *xwl_seat,
                                   struct xwl_window *xwl_window,
                                   SpritePtr sprite,
                                   int x, int y);

void xwl_seat_destroy_pointer_warp_emulator(struct xwl_seat *xwl_seat);

void xwl_seat_cursor_visibility_changed(struct xwl_seat *xwl_seat);

void xwl_seat_confine_pointer(struct xwl_seat *xwl_seat,
                              struct xwl_window *xwl_window);
void xwl_seat_unconfine_pointer(struct xwl_seat *xwl_seat);

Bool xwl_screen_init_output(struct xwl_screen *xwl_screen);

struct xwl_output *xwl_output_create(struct xwl_screen *xwl_screen,
                                     uint32_t id);

void xwl_output_destroy(struct xwl_output *xwl_output);

void xwl_output_remove(struct xwl_output *xwl_output);

RRModePtr xwayland_cvt(int HDisplay, int VDisplay,
                       float VRefresh, Bool Reduced, Bool Interlaced);

void xwl_pixmap_set_private(PixmapPtr pixmap, struct xwl_pixmap *xwl_pixmap);
struct xwl_pixmap *xwl_pixmap_get(PixmapPtr pixmap);

struct xwl_window *xwl_window_of_top(WindowPtr window);

Bool xwl_shm_create_screen_resources(ScreenPtr screen);
PixmapPtr xwl_shm_create_pixmap(ScreenPtr screen, int width, int height,
                                int depth, unsigned int hint);
Bool xwl_shm_destroy_pixmap(PixmapPtr pixmap);
struct wl_buffer *xwl_shm_pixmap_get_wl_buffer(PixmapPtr pixmap);


Bool xwl_glamor_init(struct xwl_screen *xwl_screen);

Bool xwl_screen_set_drm_interface(struct xwl_screen *xwl_screen,
                                  uint32_t id, uint32_t version);
Bool xwl_screen_set_dmabuf_interface(struct xwl_screen *xwl_screen,
                                     uint32_t id, uint32_t version);
struct wl_buffer *xwl_glamor_pixmap_get_wl_buffer(PixmapPtr pixmap,
                                                  unsigned short width,
                                                  unsigned short height,
                                                  Bool *created);

Bool xwl_present_init(ScreenPtr screen);
void xwl_present_cleanup(WindowPtr window);

void xwl_screen_release_tablet_manager(struct xwl_screen *xwl_screen);

void xwl_output_get_xdg_output(struct xwl_output *xwl_output);
void xwl_screen_init_xdg_output(struct xwl_screen *xwl_screen);

#ifdef XV
/* glamor Xv Adaptor */
Bool xwl_glamor_xv_init(ScreenPtr pScreen);
#endif

#ifdef XF86VIDMODE
void xwlVidModeExtensionInit(void);
#endif

#endif
