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
xwl_present_check_events(struct xwl_window *xwl_window)
{
    uint64_t                    msc = xwl_window->present_msc;
    struct xwl_present_event    *event, *tmp;

    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_event_list, list) {
        if (0 < event->target_msc && event->target_msc <= msc) {
            present_event_notify(event->event_id, 0, msc);
            xorg_list_del(&event->list);
            free(event);
        }
    }
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
    struct xwl_window *xwl_window = data;
    struct xwl_present_event    *event, *tmp;

    ErrorF("XX buffer_release 0: %i\n", buffer);

    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_event_list, list) {
        if (event->target_msc == 0 && event->buffer == buffer) {
            ErrorF("XX buffer_release: %i\n", event->event_id);
            present_event_notify(event->event_id, 0, xwl_window->present_msc);
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
xwl_present_queue_vblank(RRCrtcPtr crtc,
                        uint64_t event_id,
                        uint64_t msc)
{
    struct xwl_window *xwl_window = crtc->devPrivate;
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
    /* We make sure compositing is active. TODOX: Is this always the case in Xwayland anyway? */
#ifndef COMPOSITE
    return FALSE;
#endif
    struct xwl_window *xwl_window = crtc->devPrivate;

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
    if (RegionNumRects(&window->clipList) > 1)
        return FALSE;

    if (xwl_window->present_window != window) {
        xwl_window->present_window = window;
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
xwl_present_flip(RRCrtcPtr crtc,
                uint64_t event_id,
                uint64_t target_msc,
                PixmapPtr pixmap,
                Bool sync_flip)
{
    struct xwl_window           *xwl_window = crtc->devPrivate;
    struct xwl_screen           *xwl_screen = xwl_window->xwl_screen;
    WindowPtr                   window = xwl_window->window;
    WindowPtr                   present_window = xwl_window->present_window;
    Bool                        buffer_created;
    int32_t                     local_x, local_y;
    struct wl_buffer            *buffer;
    struct xwl_present_event    *event;

    if (xwl_window->present_need_configure) {
        xwl_window->present_need_configure = FALSE;
        xwl_present_cleanup_surfaces(xwl_window);

        if (RegionNumRects(&window->clipList) == 0 || RegionEqual(&window->clipList, &present_window->winSize)) {
            ErrorF("XX xwl_present_flip MAIN\n");

            /* We can flip directly to the main surface (full screen window) */
            xwl_window->present_surface = xwl_window->surface;
        } else {
            ErrorF("XX xwl_present_flip SUB\n");

            /* remove this part if we want to reenable sub compositing */
            xwl_window->present_need_configure = TRUE;
            return FALSE;
            /**/

            // TODOX: I fear we need to sub-composite ALL child windows in this case.
            RegionPrint(&window->clipList);
            RegionPrint(&present_window->clipList);

            xwl_window->present_surface =  wl_compositor_create_surface(xwl_window->xwl_screen->compositor);
            wl_surface_set_user_data(xwl_window->present_surface, xwl_window);

            xwl_window->present_subsurface =
                    wl_subcompositor_get_subsurface(xwl_screen->subcompositor, xwl_window->present_surface, xwl_window->surface);

            wl_subsurface_set_desync(xwl_window->present_subsurface);

            /* We calculate relative to 'firstChild', because 'xwl_window'
             * includes additionally to the pure wl_surface the window border.
             */
            local_x = present_window->clipList.extents.x1 - window->firstChild->winSize.extents.x1;
            local_y = present_window->clipList.extents.y1 - window->firstChild->winSize.extents.y1;

            wl_subsurface_set_position(xwl_window->present_subsurface, local_x, local_y);
        }
    }

    buffer = xwl_glamor_pixmap_get_wl_buffer(pixmap, &buffer_created);

    event = malloc(sizeof *event);
    if (!event) {
        // TODOX: rewind everything above (or just do flip without buffer release callback?)
        return FALSE;
    }

    event->event_id = event_id;
    event->buffer = buffer;
    /* make sure only the release callback triggers the event */
    event->target_msc = 0;

    xorg_list_add(&event->list, &xwl_window->present_event_list);

    if (buffer_created)
        wl_buffer_add_listener(buffer, &release_listener, xwl_window);

    wl_surface_attach(xwl_window->present_surface, buffer, 0, 0);

    if (!xwl_window->present_frame_callback) {
        xwl_window->present_frame_callback = wl_surface_frame(xwl_window->present_surface);
        wl_callback_add_listener(xwl_window->present_frame_callback, &present_frame_listener, xwl_window);
    }

    return TRUE;
}

static void
xwl_present_flip_executed(RRCrtcPtr crtc, uint64_t event_id, RegionPtr damage)
{
    struct xwl_window *xwl_window = crtc->devPrivate;
    BoxPtr box = RegionExtents(damage);

    wl_surface_damage(xwl_window->present_surface, box->x1, box->y1,
                      box->x2 - box->x1, box->y2 - box->y1);

    wl_surface_commit(xwl_window->present_surface);
    wl_display_flush(xwl_window->xwl_screen->display);

    present_event_notify(event_id, 0, xwl_window->present_msc);
}

static void
xwl_present_unflip(WindowPtr window, uint64_t event_id)
{
    struct xwl_window   *xwl_window = xwl_window_from_window(window);

    if(xwl_window) {
        xwl_present_cleanup_surfaces(xwl_window);
        xwl_window->present_window = NULL;
    }
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
