/*
 * Copyright © 2017 Roman Gilg
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

#include "xwayland.h"

#include <present.h>

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
    struct xwl_present_event *event = data;

    //TODOX: how to translate this in Present extension?
}

static const struct wl_buffer_listener release_listener = {
    buffer_release
};

static void
xwl_present_check_events(struct xwl_window *xwl_window)
{
    uint64_t                    msc = xwl_window->present_msc;
    struct xwl_present_event    *event, *tmp;

    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_event_list, list) {
        if (event->target_msc <= msc) {
            present_event_notify(event->event_id, 0, msc);
            xorg_list_del(&event->list);
            free(event);
        }
    }
}

static void
present_frame_callback(void *data,
               struct wl_callback *callback,
               uint32_t time)
{
//    struct xwl_present_event *event, *tmp;
    struct xwl_window *xwl_window = data;

    wl_callback_destroy(xwl_window->present_frame_callback);
    xwl_window->present_frame_callback = NULL;

    xwl_window->present_msc++;

    xwl_present_check_events(xwl_window);
}

static const struct wl_callback_listener present_frame_listener = {
    present_frame_callback
};

static void
xwl_present_commit_buffer_frame(struct xwl_window *xwl_window, PixmapPtr pixmap)
{
    ErrorF("XX xwl_present_commit_buffer_frame: %i, %i\n", pixmap, pixmap->drawable.pScreen);

    struct wl_buffer *buffer = xwl_glamor_pixmap_get_wl_buffer(pixmap);
    wl_surface_attach(xwl_window->surface, buffer, 0, 0);

    wl_surface_damage(xwl_window->surface, 0, 0, pixmap->drawable.width, pixmap->drawable.height);

    if (!xwl_window->present_frame_callback) {
        xwl_window->present_frame_callback = wl_surface_frame(xwl_window->surface);
        wl_callback_add_listener(xwl_window->present_frame_callback, &present_frame_listener, xwl_window);
    }

//    wl_buffer_add_listener(buffer, &release_listener, event); //TODOX: if we can use it, we need make sure this only happens once per wl_buffer/Pixmap
    wl_surface_commit(xwl_window->surface);

    wl_display_flush(xwl_window->xwl_screen->display);
}

static RRCrtcPtr
xwl_present_get_crtc(WindowPtr window)
{
    struct xwl_window *xwl_window = xwl_window_from_window(window);
    if (xwl_window == NULL)
        return NULL;

    return xwl_window->present_crtc_fake;
}

static int
xwl_present_get_ust_msc(RRCrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    struct xwl_window *xwl_window = crtc->devPrivate;
    *ust = 0;
    *msc = xwl_window->present_msc;

    return Success;
}

/*
 * Queue an event to report back to the Present extension when the specified
 * MSC has past
 */
static int
xwl_present_queue_vblank(RRCrtcPtr crtc,
                        uint64_t event_id,
                        uint64_t msc)
{
    /*
     * Queuing events doesn't work yet: There needs to be a Wayland protocol
     * extension infroming clients about timings.
     *
     * See for a proposal for that:
     * https://cgit.freedesktop.org/wayland/wayland-protocols/tree/stable/presentation-time
     *
     */
    return BadRequest;
    /* */

    struct xwl_window *xwl_window = crtc->devPrivate;
    struct xwl_present_event *event;

    event = malloc(sizeof *event);
    if (!event)
        return BadAlloc;

    event->event_id = event_id;
    event->target_msc = msc;
    xorg_list_add(&event->list, &xwl_window->present_event_list);

    return Success;
}

/*
 * Remove a pending vblank event so that it is not reported
 * to the extension
 */
static void
xwl_present_abort_vblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    struct xwl_window *xwl_window = crtc->devPrivate;
    struct xwl_present_event *event, *tmp;

    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_event_list, list) {
        if (event->event_id == event_id) {
            xorg_list_del(&event->list);
            free(event);
            return;
        }
    }
}

static void
xwl_present_flush(WindowPtr window)
{
    /* Only called when a Pixmap is copied instead of flipped,
     * but in this case we wait on the next block_handler. */
}

static Bool
xwl_present_check_flip(RRCrtcPtr crtc,
                      WindowPtr window,
                      PixmapPtr pixmap,
                      Bool sync_flip)
{
    struct xwl_window *xwl_window;

    xwl_window = xwl_window_from_window(window);
    if (!xwl_window)
        return FALSE;

    return xwl_window->present_crtc_fake && xwl_window->present_crtc_fake == crtc;
}

static Bool
xwl_present_flip(RRCrtcPtr crtc,
                uint64_t event_id,
                uint64_t target_msc,
                PixmapPtr pixmap,
                Bool sync_flip)
{
    struct xwl_window   *xwl_window = crtc->devPrivate;
    ScreenPtr           screen = xwl_window->xwl_screen->screen;

    if (!xwl_window->present_restore_pixmap)
        xwl_window->present_restore_pixmap = (*screen->GetWindowPixmap)(xwl_window->window);

    xwl_present_commit_buffer_frame(xwl_window, pixmap);

    return TRUE;
}

static void
xwl_present_flip_executed(RRCrtcPtr crtc, uint64_t event_id)
{
    struct xwl_window *xwl_window = crtc->devPrivate;
    present_event_notify(event_id, 0, xwl_window->present_msc);
}

static void
xwl_present_unflip(WindowPtr window, uint64_t event_id)
{
    struct xwl_window   *xwl_window = xwl_window_from_window(window);

    xwl_window->present_restore_pixmap = NULL;
    present_event_notify(event_id, 0, 0);
}

static present_screen_info_rec xwl_present_screen_info = {
    .version = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc = xwl_present_get_crtc,
    .get_ust_msc = xwl_present_get_ust_msc,
    .queue_vblank = xwl_present_queue_vblank,
    .abort_vblank = xwl_present_abort_vblank,
    .flush = xwl_present_flush,
    .rootless = TRUE,
    .capabilities = PresentCapabilityAsync,
    .check_flip = xwl_present_check_flip,
    .flip = xwl_present_flip,
    .unflip_rootless = xwl_present_unflip,
    .flip_executed = xwl_present_flip_executed,
};

Bool
xwl_present_init(ScreenPtr screen)
{
    return present_screen_init(screen, &xwl_present_screen_info);
}
