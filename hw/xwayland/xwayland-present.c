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
            present_event_notify(event->event_id, 0, msc);
            xorg_list_del(&event->list);
            free(event);
        }
    }
}

void
xwl_present_unrealize(WindowPtr window)
{
    struct xwl_window           *xwl_window = xwl_window_from_window(window);
    struct xwl_output           *xwl_output;
    struct xwl_present_event    *event, *tmp;
    uint64_t                    msc;

    if (xwl_window == NULL)
        return;
    if (!xwl_window->present_window )
        return;
    if (xwl_window->present_window != window)
        return;

    xwl_output = xwl_window->present_xwl_output;
    msc = xwl_output ? xwl_output->msc : present_last_msc;


    /* Clear remaining buffer releases */
    xorg_list_for_each_entry_safe(event, tmp, &xwl_present_release, list) {
        if (event->xwl_window == xwl_window) {
            present_event_notify(event->event_id, 0, msc);
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

    xorg_list_for_each_entry_safe(event, tmp, &xwl_present_release, list) {
        if (event->xwl_window == xwl_window && event->buffer == buffer) {
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
    struct xwl_window *xwl_window = data;

    wl_callback_destroy(xwl_window->present_frame_callback);
    xwl_window->present_frame_callback = NULL;

    xwl_window->present_msc++;

    xwl_present_check_events(xwl_window);
}

static const struct wl_callback_listener present_frame_listener = {
    present_frame_callback
};

static int
xwl_present_box_coverage(BoxPtr boxA, BoxPtr boxB)
{
    BoxRec boxC;

    boxC->x1 = boxA->x1 > boxB->x1 ? boxA->x1 : boxB->x1;
    boxC->x2 = boxA->x2 < boxB->x2 ? boxA->x2 : boxB->x2;

    if (boxC->x1 >= boxC->x2) {
        boxC->x1 = boxC->x2 = boxC->y1 = boxC->y2 = 0;
    } else {
        boxC->y1 = boxA->y1 > boxB->y1 ? boxA->y1 : boxB->y1;
        boxC->y2 = boxA->y2 < boxB->y2 ? boxA->y2 : boxB->y2;
        if (boxC->y1 >= boxC->y2)
            boxC->x1 = boxC->x2 = boxC->y1 = boxC->y2 = 0;
    }
    return (int)(boxC->x2 - boxC->x1) * (int)(boxC->y2 - boxC->y1);
}

static int
xwl_present_output_coverage(struct xwl_output *xwl_output, WindowPtr window)
{
    BoxRec output_box, win_box, inters_box;

    output_box.x1 = xwl_output->x;
    output_box.y1 = xwl_output->y;
    output_box.x2 = output_box.x1 + xwl_output->width;
    output_box.y2 = output_box.y1 + xwl_output->height;

    win_box.x1 = window->drawable.x;
    win_box.y1 = window->drawable.y;
    win_box.x2 = win_box.x1 + window->drawable.width;
    win_box.y2 = win_box.y1 + window->drawable.height;

    inters_box.x1 = output_box->x1 > win_box->x1 ? output_box->x1 : win_box->x1;
    inters_box.x2 = output_box->x2 < win_box->x2 ? output_box->x2 : win_box->x2;

    if (inters_box.x1 >= inters_box.x2) {
        inters_box.x1 = inters_box.x2 = inters_box.y1 = inters_box.y2 = 0;
    } else {
        inters_box.y1 = output_box->y1 > win_box->y1 ? output_box->y1 : win_box->y1;
        inters_box.y2 = output_box->y2 < win_box->y2 ? output_box->y2 : win_box->y2;
        if (inters_box.y1 >= inters_box.y2)
            inters_box.x1 = inters_box.x2 = inters_box.y1 = inters_box.y2 = 0;
    }
    return (int)(inters_box.x2 - inters_box.x1) * (int)(inters_box.y2 - inters_box.y1);
}

static RRCrtcPtr
xwl_present_get_crtc(WindowPtr window)
{
    struct xwl_screen   *xwl_screen;
    struct xwl_window   *xwl_window = xwl_window_from_window(window);
    struct xwl_output   *xwl_output;
    RRCrtcPtr           crtc;
    int                 best_coverage, coverage;

    if (!xwl_window)
        return NULL;

    crtc = NULL;
    best_coverage = 0;
    xwl_screen = xwl_window->xwl_screen;

    /*
     * On a single head system, just take the one output
     */
    if (xwl_screen->output_count == 1) {
        xwl_output = xorg_list_first_entry(&xwl_screen->output_list, struct *xwl_output, link);
        return xwl_output->randr_crtc;
    }

    /*
     * Test if the window is completely on the
     * old output, optimizing the selection process
     *
     */
    xorg_list_for_each_entry(xwl_output, &xwl_screen->output_list, link) {
        if (xwl_window->present_xwl_output == xwl_output) {
            if (window->drawable.x <= xwl_output->x &&
                    window->drawable.y <= xwl_output->y &&
                    window->drawable.width <= xwl_output->width
                    window->drawable.height <= xwl_output->height)
                return xwl_output->randr_crtc;
        }
    }

    xwl_window->present_xwl_output = NULL;

    /*
     * At this point we'll update the crtc in any case,
     * but NULL is still possible, if the window is not covered
     * on any crtc at the moment
     *
     */
    xorg_list_for_each_entry(xwl_output, &xwl_screen->output_list, link) {
        coverage = xwl_present_crtc_coverage(xwl_output, window);
        if (best_coverage < coverage) {
            xwl_window->present_xwl_output = xwl_output;
            crtc = xwl_output->randr_crtc;
        }
    }

    return crtc;
}

static int
xwl_present_get_ust_msc(RRCrtcPtr crtc, CARD64 *ust, CARD64 *msc)
{
    struct xwl_output   *xwl_output;
    Bool                output_found = FALSE;

    xorg_list_for_each_entry(xwl_output, &xwl_screen->output_list, link) {
        if (crtc == xwl_output->randr_crtc) {
            output_found = TRUE;
            break;
        }
    }

    if (!output_found)
        return BadMatch;

    *ust = 0;
    *msc = xwl_output->msc;

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
    struct xwl_window   *xwl_window = xwl_window_from_window(window);
    RRCrtcPtr           crtc_internal;
    struct xwl_output   *xwl_output;

    if (!xwl_window)
        return FALSE;

    crtc_internal = xwl_present_get_crtc(window);
    if (!crtc_internal)
        return FALSE;

    if (crtc && crtc_internal != crtc)
        return FALSE;

    xwl_output = crtc_internal->devPrivate;
    xwl_window->present_last_msc = xwl_output->msc;

//    /* Make sure the client doesn't try to flip to another crtc
//     * than the one created for 'xwl_window'
//     */
//    if (xwl_window->present_crtc_fake != crtc)
//        return FALSE;

//    /* In order to reduce complexity, we currently allow only one subsurface, i.e. one completely visible region */
//    if (RegionNumRects(&window->clipList) > 1)  //TODOX: we need to test if xwl_window has already another flipping window instead and unflip the old one in this case - is done below already
//        return FALSE;

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
xwl_present_flip(WindowPtr window,
                 RRCrtcPtr crtc,
                 uint64_t event_id,
                 uint64_t target_msc,
                 PixmapPtr pixmap,
                 Bool sync_flip)
{
    struct xwl_window           *xwl_window = xwl_window_from_window(window);
    struct xwl_screen           *xwl_screen = xwl_window->xwl_screen;
    WindowPtr                   window = xwl_window->window;
    WindowPtr                   present_window = xwl_window->present_window;
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
xwl_present_flip_executed(WindowPtr window, RRCrtcPtr crtc, uint64_t event_id, RegionPtr damage)
{
    struct xwl_window   *xwl_window = xwl_window_from_window(window);
    struct xwl_output   *xwl_output = crtc->devPrivate;
    BoxPtr              box = RegionExtents(damage);    // TODOX: this needs to be converted to surface local coordinates!

    wl_surface_damage(xwl_window->present_surface, box->x1, box->y1,
                      box->x2 - box->x1, box->y2 - box->y1);

    wl_surface_commit(xwl_window->present_surface);
    wl_display_flush(xwl_window->xwl_screen->display);

    present_event_notify(event_id, 0, xwl_output->msc);
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
    .flip_rootless = xwl_present_flip,
    .unflip_rootless = xwl_present_unflip,
    .flip_executed = xwl_present_flip_executed,
};

Bool
xwl_present_init(ScreenPtr screen)
{
    xorg_list_init(&xwl_present_release);
    return present_screen_init(screen, &xwl_present_screen_info);
}
