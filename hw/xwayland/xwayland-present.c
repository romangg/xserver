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

static struct xorg_list xwl_present_release;    //TODOX: integrate into xwl_window struct?

static void
xwl_present_check_events(struct xwl_window *xwl_window)
{
    uint64_t                    msc = xwl_window->present_msc;
    struct xwl_present_event    *event, *tmp;

    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_event_list, list) {
        if (event->target_msc <= msc) {
            present_winmode_event_notify(xwl_window->present_window, event->event_id, 0, msc);
            xorg_list_del(&event->list);
            free(event);
        }
    }
}

void
xwl_present_unrealize(WindowPtr window)
{
    struct xwl_window           *xwl_window = xwl_window_get(window);
    struct xwl_present_event    *event, *tmp;

    if (xwl_window == NULL)
        return;
    if (!xwl_window->present_window )
        return;
    if (xwl_window->present_window != window)
        return;

    /* Clear remaining buffer releases */
    xorg_list_for_each_entry_safe(event, tmp, &xwl_present_release, list) {
        if (event->xwl_window == xwl_window) {
            present_winmode_event_notify(window, event->event_id, 0, xwl_window->present_msc);
            xorg_list_del(&event->list);
            free(event);
        }
    }

    xwl_window->present_window = NULL;
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
    struct xwl_window           *xwl_window = wl_buffer_get_user_data(buffer);
    struct xwl_present_event    *event, *tmp;

    // TODOX: instead data as the present_window and then get xwl_window?
    if (!xwl_window->present_window)
        return;

    xorg_list_for_each_entry_safe(event, tmp, &xwl_present_release, list) {
        if (event->xwl_window == xwl_window && event->buffer == buffer) {
            present_winmode_event_notify(xwl_window->present_window, event->event_id, 0, xwl_window->present_msc);
            xorg_list_del(&event->list);
            free(event);
            break;
        }
    }
}

static const struct wl_buffer_listener release_listener = {
    buffer_release
};

static void
present_frame_callback(void *data,
               struct wl_callback *callback,
               uint32_t time)
{
    struct xwl_window *xwl_window = data;

    wl_callback_destroy(xwl_window->present_frame_callback);
    xwl_window->present_frame_callback = NULL;

    xwl_window->present_msc++;

    xwl_present_check_events(xwl_window);
}

static const struct wl_callback_listener present_frame_listener = {
    present_frame_callback
};

static RRCrtcPtr
xwl_present_get_crtc(WindowPtr present_window)
{
    struct xwl_window *xwl_window = xwl_window_get(present_window);
    if (xwl_window == NULL)
        return NULL;

    return xwl_window->present_crtc_fake;
}

static int
xwl_present_get_ust_msc(WindowPtr present_window, uint64_t *ust, uint64_t *msc)
{
    struct xwl_window *xwl_window = xwl_window_get(present_window);
    if (!xwl_window)
        return BadAlloc;
    *ust = 0;
    *msc = xwl_window->present_msc;

    return Success;
}

/*
 * Queue an event to report back to the Present extension when the specified
 * MSC has past
 */
static int
xwl_present_queue_vblank(WindowPtr present_window,
                         RRCrtcPtr crtc,
                         uint64_t event_id,
                         uint64_t msc)
{
    struct xwl_window *xwl_window = xwl_window_get(present_window);
    struct xwl_present_event *event;

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
xwl_present_abort_vblank(WindowPtr present_window, RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    struct xwl_window *xwl_window = xwl_window_get(present_window);
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
                       WindowPtr present_window,
                       PixmapPtr pixmap,
                       Bool sync_flip)
{
    /* We make sure compositing is active. TODOX: Is this always the case in Xwayland anyway? */
#ifndef COMPOSITE
    return FALSE;
#endif
    /* we can't take crtc->devPrivate because window might have been reparented and
     * the former parent xwl_window destroyed */
    struct xwl_window *xwl_window = xwl_window_get(present_window);

    if (!xwl_window)
        return FALSE;

    if (!xwl_window->present_crtc_fake)
        return FALSE;
    /* Make sure the client doesn't try to flip to another crtc
     * than the one created for 'xwl_window'
     */
    if (xwl_window->present_crtc_fake != crtc)
        return FALSE;

    /* In order to reduce complexity, we currently allow only one subsurface, i.e. one completely visible region */
    if (RegionNumRects(&present_window->clipList) > 1)
        return FALSE;

    if (xwl_window->present_window != present_window) {
        xwl_window->present_window = present_window;
        xwl_window->present_need_configure = TRUE;
    }
    return TRUE;
}

static void
xwl_present_cleanup_surfaces(struct xwl_window *xwl_window)
{
    if (xwl_window->present_subsurface) {
        wl_subsurface_destroy(xwl_window->present_subsurface);
        wl_surface_destroy(xwl_window->present_surface);
        xwl_window->present_subsurface = NULL;
    }
    xwl_window->present_surface = NULL;
}

static Bool
xwl_present_flip(WindowPtr present_window,
                 RRCrtcPtr crtc,
                 uint64_t event_id,
                 uint64_t target_msc,
                 PixmapPtr pixmap,
                 Bool sync_flip)
{
    struct xwl_window           *xwl_window = xwl_window_get(present_window);
    struct xwl_screen           *xwl_screen = xwl_window->xwl_screen;
    WindowPtr                   window = xwl_window->window;
    BoxPtr                      win_box, present_box;
    Bool                        buffer_created;
    struct wl_buffer            *buffer;
    struct xwl_present_event    *event;
    struct wl_region            *input_region;

    win_box = RegionExtents(&window->winSize);
    present_box = RegionExtents(&present_window->winSize);

    if (xwl_window->present_need_configure) {
        xwl_window->present_need_configure = FALSE;
        xwl_present_cleanup_surfaces(xwl_window);

        if (RegionEqual(&window->winSize, &present_window->winSize)) {
            ErrorF("XX xwl_present_flip MAIN\n");

            /* We can flip directly to the main surface (full screen window without clips) */
            xwl_window->present_surface = xwl_window->surface;
        } else {
            ErrorF("XX xwl_present_flip SUB\n");

            xwl_window->present_surface =  wl_compositor_create_surface(xwl_window->xwl_screen->compositor);
            wl_surface_set_user_data(xwl_window->present_surface, xwl_window);

            xwl_window->present_subsurface =
                    wl_subcompositor_get_subsurface(xwl_screen->subcompositor, xwl_window->present_surface, xwl_window->surface);
            wl_subsurface_set_desync(xwl_window->present_subsurface);

            input_region = wl_compositor_create_region(xwl_screen->compositor);
            wl_surface_set_input_region(xwl_window->present_surface, input_region);
            wl_region_destroy(input_region);

            wl_subsurface_set_position(xwl_window->present_subsurface,
                                       present_box->x1 - win_box->x1,
                                       present_box->y1 - win_box->y1);
        }
    }

    event = malloc(sizeof *event);
    if (!event) {
        // TODOX: rewind everything above (or just do flip without buffer release callback?)
        return FALSE;
    }

    buffer = xwl_glamor_pixmap_get_wl_buffer(pixmap,
                                             present_box->x2 - present_box->x1,
                                             present_box->y2 - present_box->y1,
                                             &buffer_created);

    event->event_id = event_id;
    event->xwl_window = xwl_window;
    event->buffer = buffer;

    xorg_list_add(&event->list, &xwl_present_release);

    if (buffer_created)
        wl_buffer_add_listener(buffer, &release_listener, NULL);

    wl_buffer_set_user_data(buffer, xwl_window);
    wl_surface_attach(xwl_window->present_surface, buffer, 0, 0);

    if (!xwl_window->present_frame_callback) {
        xwl_window->present_frame_callback = wl_surface_frame(xwl_window->present_surface);
        wl_callback_add_listener(xwl_window->present_frame_callback, &present_frame_listener, xwl_window);
    }

    return TRUE;
}

static void
xwl_present_flip_executed(WindowPtr present_window, RRCrtcPtr crtc, uint64_t event_id, RegionPtr damage)
{
    struct xwl_window *xwl_window = xwl_window_get(present_window);
    BoxPtr box = RegionExtents(damage);

    wl_surface_damage(xwl_window->present_surface, box->x1, box->y1,
                      box->x2 - box->x1, box->y2 - box->y1);

    wl_surface_commit(xwl_window->present_surface);
    wl_display_flush(xwl_window->xwl_screen->display);

    present_winmode_event_notify(present_window, event_id, 0, xwl_window->present_msc);
}

static void
xwl_present_unflip(WindowPtr window, uint64_t event_id)
{
    struct xwl_window   *xwl_window = xwl_window_get(window);

    if(xwl_window) {
        xwl_present_cleanup_surfaces(xwl_window);
        xwl_window->present_window = NULL;
    }
    present_winmode_event_notify(window, event_id, 0, 0);
}

static present_winmode_screen_info_rec xwl_present_info = {
    .version = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc = xwl_present_get_crtc,

    .get_ust_msc = xwl_present_get_ust_msc,
    .queue_vblank = xwl_present_queue_vblank,
    .abort_vblank = xwl_present_abort_vblank,

    .flush = xwl_present_flush,

    .capabilities = PresentCapabilityAsync,
    .check_flip = xwl_present_check_flip,
    .flip = xwl_present_flip,
    .unflip = xwl_present_unflip,
    .flip_executed = xwl_present_flip_executed,
};

Bool
xwl_present_init(ScreenPtr screen)
{
    xorg_list_init(&xwl_present_release);
    return present_winmode_screen_init(screen, &xwl_present_info);
}
