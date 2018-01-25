/*
 * Copyright © 2018 Roman Gilg
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

#define FRAME_TIMER_IVAL 67 // ~15fps

static struct xorg_list xwl_present_windows;

void
xwl_present_cleanup(WindowPtr window)
{
    struct xwl_window           *xwl_window = xwl_window_of_top(window);
    struct xwl_present_event    *event, *tmp;

    ErrorF("CC xwl_present_cleanup XWL_WINDOW %d\n", xwl_window);

    if (!xwl_window)
        return;

    ErrorF("CC xwl_present_cleanup PRESENT_WINDOW %d\n", xwl_window->present_window);

    if (!xwl_window->present_window)
        return;

    if (xwl_window->present_window != window && xwl_window->window != window)
        /* Unrealizing a non-presenting sibling */
        return;

    /*
     * At this point we're either:
     * - Unflipping.
     * - Unrealizing the presenting window 'xwl_window->present_window'
     *   or its ancestor 'xwl_window->window'.
     * And therefore need to cleanup.
     */

    if (xwl_window->present_frame_callback) {
        wl_callback_destroy(xwl_window->present_frame_callback);
        xwl_window->present_frame_callback = NULL;
    }

    /* Reset base data */
    xorg_list_del(&xwl_window->present_link);

    xwl_window->present_surface = NULL;
    xwl_window->present_window = NULL;

    TimerFree(xwl_window->present_frame_timer);
    xwl_window->present_frame_timer = NULL;

    /* Clear remaining events */
    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_event_list, list) {
        xorg_list_del(&event->list);
        free(event);
    }

    /* Clear remaining buffer releases and inform Present about free ressources */
    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_release_queue, list) {
        present_wnmd_event_notify(xwl_window->window, event->event_id, 0, xwl_window->present_msc);
        xorg_list_del(&event->list);
        if (event->pending)
            event->abort = TRUE;
        else
            free(event);
    }
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
    WindowPtr                   present_window = wl_buffer_get_user_data(buffer);
    struct xwl_window           *xwl_window;
    struct xwl_present_event    *event, *tmp;
    Bool                        found_window = FALSE;


    ErrorF("YYY buffer_release BUFFER %d\n", buffer);

    /* Find window */
    xorg_list_for_each_entry(xwl_window, &xwl_present_windows, present_link) {
        if (xwl_window->present_window == present_window) {
            found_window = TRUE;
            break;
        }
    }

    ErrorF("YYY buffer_release FOUND_WINDOW %d\n", found_window);

    if (!found_window)
        return;

    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_release_queue, list) {
        ErrorF("Y buffer_release EVENT %d | %d\n", event->event_id, event->buffer);
        if (event->buffer == buffer) {
            if (!event->abort)
                present_wnmd_event_notify(present_window, event->event_id, 0, xwl_window->present_msc);
            event->abort = TRUE;

            if (!event->pending) {
                xorg_list_del(&event->list);
                free(event);
            }
            break;
        }
    }
}

static const struct wl_buffer_listener release_listener = {
    buffer_release
};

static void
xwl_present_events_notify(struct xwl_window *xwl_window)
{
    uint64_t                    msc = xwl_window->present_msc;
    struct xwl_present_event    *event, *tmp;

    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_event_list, list) {
        ErrorF("NNN xwl_present_events_notify MSCs: %d | %d\n", event->target_msc, msc);
        if (event->target_msc <= msc) {
            present_wnmd_event_notify(xwl_window->present_window, event->event_id, 0, msc);
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
    struct xwl_window *xwl_window = data;

    ErrorF("XX present_frame_callback MSC: %d\n", xwl_window->present_msc);
    ErrorF("XX present_frame_callback FIRING1: %d\n", xwl_window->present_frame_timer_firing);

    /* we do not need the timer anymore for this frame */
    TimerCancel(xwl_window->present_frame_timer);

    wl_callback_destroy(xwl_window->present_frame_callback);
    xwl_window->present_frame_callback = NULL;

    if (xwl_window->present_frame_timer_firing) {
        /* If the timer was firing, this frame callback is too late */
        xwl_window->present_frame_timer_firing = FALSE;
        return;
    }

    xwl_window->present_msc++;
    xwl_present_events_notify(xwl_window);
}

static const struct wl_callback_listener present_frame_listener = {
    present_frame_callback
};

static CARD32
present_frame_timer_callback(OsTimerPtr timer,
                             CARD32 time,
                             void *arg)
{
    struct xwl_window *xwl_window = arg;

    ErrorF("XX present_frame_timer_callback MSC: %d\n", xwl_window->present_msc);
    ErrorF("XX present_frame_timer_callback PRESENT_FRAME_CALLBACK: %d\n", xwl_window->present_frame_callback);

    xwl_window->present_frame_timer_firing = TRUE;

    xwl_window->present_msc++;
    xwl_present_events_notify(xwl_window);

    return FRAME_TIMER_IVAL;
}

static void
xwl_present_sync_callback(void *data,
               struct wl_callback *callback,
               uint32_t time)
{
    struct xwl_present_event *event = data;
    struct xwl_window *xwl_window = event->xwl_window;

    ErrorF("XX xwl_present_sync_callback MSC %d\n", xwl_window->present_msc);
    ErrorF("XX xwl_present_sync_callback 1. EVENT_ID %d\n", event->event_id);
    ErrorF("XX xwl_present_sync_callback ABORT %d\n", event->abort);

    event->pending = FALSE;

    // event might have been aborted
    if (event->abort) {
        ErrorF("XX xwl_present_sync_callback ABORT!\n");
        xorg_list_del(&event->list);
        free(event);
    } else {
        ErrorF("XX xwl_present_sync_callback 2. EVENT_ID %d\n", event->event_id);

        present_wnmd_event_notify(xwl_window->present_window,
                                  event->event_id,
                                  0,
                                  xwl_window->present_msc);
    }

    ErrorF("XX xwl_present_sync_callback END!\n");
}

static const struct wl_callback_listener xwl_present_sync_listener = {
    xwl_present_sync_callback
};

static RRCrtcPtr
xwl_present_get_crtc(WindowPtr present_window)
{
    struct xwl_window *xwl_window = xwl_window_of_top(present_window);
    if (xwl_window == NULL)
        return NULL;

    return xwl_window->present_crtc_fake;
}

static int
xwl_present_get_ust_msc(WindowPtr present_window, uint64_t *ust, uint64_t *msc)
{
    struct xwl_window *xwl_window = xwl_window_of_top(present_window);
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
}

/*
 * Remove a pending vblank event so that it is not reported
 * to the extension
 */
static void
xwl_present_abort_vblank(WindowPtr present_window, RRCrtcPtr crtc, uint64_t event_id, uint64_t msc)
{
    struct xwl_window *xwl_window = xwl_window_of_top(present_window);
    struct xwl_present_event *event, *tmp;

    ErrorF("XX xwl_present_abort_vblank EVENT %d\n", event_id);

    xorg_list_for_each_entry_safe(event, tmp, &xwl_window->present_event_list, list) {
        if (event->event_id == event_id) {
            xorg_list_del(&event->list);
            free(event);
            return;
        }
    }

    xorg_list_for_each_entry(event, &xwl_window->present_release_queue, list) {
        if (event->event_id == event_id) {
            event->abort = TRUE;
            break;
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
    struct xwl_window *xwl_window = xwl_window_of_top(present_window);

    ErrorF("XX xwl_present_check_flip XWL_WINDOW %d\n", xwl_window);

    if (!xwl_window)
        return FALSE;

    ErrorF("XX xwl_present_check_flip PRESENT_WINDOW %d\n", xwl_window->present_window);

    if (!xwl_window->present_crtc_fake)
        return FALSE;
    /*
     * Make sure the client doesn't try to flip to another crtc
     * than the one created for 'xwl_window'
     */
    if (xwl_window->present_crtc_fake != crtc)
        return FALSE;
    if (!RegionEqual(&xwl_window->window->winSize, &present_window->winSize))
        return FALSE;

    ErrorF("XX xwl_present_check_flip TRUE\n");

    return TRUE;
}

static Bool
xwl_present_flip(WindowPtr present_window,
                 RRCrtcPtr crtc,
                 uint64_t event_id,
                 uint64_t target_msc,
                 PixmapPtr pixmap,
                 Bool sync_flip)
{
    struct xwl_window           *xwl_window = xwl_window_of_top(present_window);
    BoxPtr                      present_box;
    Bool                        buffer_created;
    struct wl_buffer            *buffer;
    struct xwl_present_event    *event;

    present_box = RegionExtents(&present_window->winSize);

    ErrorF("ZZ xwl_present_flip START %d\n");

    /* We always switch to another child window, if it wants to present. */
    if (xwl_window->present_window != present_window) {
        if (xwl_window->present_window)
            xwl_present_cleanup(xwl_window->present_window);
        xwl_window->present_window = present_window;
        xorg_list_add(&xwl_window->present_link, &xwl_present_windows);

        /* We can flip directly to the main surface (full screen window without clips) */
        xwl_window->present_surface = xwl_window->surface;
    }

    event = malloc(sizeof *event);
    if (!event) {
        xwl_present_cleanup(present_window);
        return FALSE;
    }

    buffer = xwl_glamor_pixmap_get_wl_buffer(pixmap,
                                             present_box->x2 - present_box->x1,
                                             present_box->y2 - present_box->y1,
                                             &buffer_created);

    ErrorF("ZZ xwl_present_flip BUFFER %d\n", buffer);

    event->event_id = event_id;
    event->xwl_window = xwl_window;
    event->buffer = buffer;
    event->target_msc = xwl_window->present_msc;
    event->pending = TRUE;
    event->abort = FALSE;

    xorg_list_add(&event->list, &xwl_window->present_release_queue);

    if (buffer_created)
        wl_buffer_add_listener(buffer, &release_listener, NULL);

    wl_buffer_set_user_data(buffer, present_window);
    wl_surface_attach(xwl_window->present_surface, buffer, 0, 0);

    ErrorF("ZZ xwl_present_flip PRESENT_FRAME_CALLBACK %d\n", xwl_window->present_frame_callback);

    if (!xwl_window->present_frame_callback) {
        xwl_window->present_frame_timer = TimerSet(xwl_window->present_frame_timer, 0, FRAME_TIMER_IVAL, &present_frame_timer_callback, xwl_window);

        xwl_window->present_frame_callback = wl_surface_frame(xwl_window->present_surface);
        wl_callback_add_listener(xwl_window->present_frame_callback, &present_frame_listener, xwl_window);
    }

    ErrorF("ZZZ2 X %d, %d | %d\n", present_box->x1, present_box->x2, present_box->x2 - present_box->x1);
    ErrorF("ZZZ2 Y %d, %d | %d\n", present_box->y1, present_box->y2, present_box->y2 - present_box->y1);

    wl_surface_damage(xwl_window->present_surface, 0, 0,
                      present_box->x2 - present_box->x1, present_box->y2 - present_box->y1);

    wl_surface_commit(xwl_window->present_surface);

    xwl_window->present_sync_callback = wl_display_sync(xwl_window->xwl_screen->display);
    wl_callback_add_listener(xwl_window->present_sync_callback, &xwl_present_sync_listener, event);

    ErrorF("ZZ xwl_present_flip MSC %d\n", xwl_window->present_msc);

    wl_display_flush(xwl_window->xwl_screen->display);


//    present_wnmd_event_notify(xwl_window->present_window,
//                              event->event_id,
//                              0,
//                              xwl_window->present_msc);
//    event->pending = FALSE;

    ErrorF("ZZ xwl_present_flip END\n");

    return TRUE;
}

static void
xwl_present_unflip(WindowPtr window, uint64_t event_id)
{
    ErrorF("XX xwl_present_unflip EVENT %d\n", event_id);
    xwl_present_cleanup(window);
    present_wnmd_event_notify(window, event_id, 0, 0);
}

static present_wnmd_info_rec xwl_present_info = {
    .version = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc = xwl_present_get_crtc,

    .get_ust_msc = xwl_present_get_ust_msc,
    .queue_vblank = xwl_present_queue_vblank,
    .abort_vblank = xwl_present_abort_vblank,

    .flush = xwl_present_flush,

    .capabilities = PresentCapabilityAsync,
    .check_flip = xwl_present_check_flip,
    .flip = xwl_present_flip,
    .unflip = xwl_present_unflip
};

Bool
xwl_present_init(ScreenPtr screen)
{
    xorg_list_init(&xwl_present_windows);
    return present_wnmd_screen_init(screen, &xwl_present_info);
}
