/*
 * Copyright © 2013 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifdef HAVE_XORG_CONFIG_H
#include <xorg-config.h>
#endif

#include "present_priv.h"
#include <gcstruct.h>
#include <misync.h>
#include <misyncstr.h>
#ifdef MONOTONIC_CLOCK
#include <time.h>
#endif

/*
 * Returns:
 * TRUE if the first MSC value is after the second one
 * FALSE if the first MSC value is equal to or before the second one
 */
static Bool
msc_is_after(uint64_t test, uint64_t reference)
{
    return (int64_t)(test - reference) > 0;
}

/*
 * Returns:
 * TRUE if the first MSC value is equal to or after the second one
 * FALSE if the first MSC value is before the second one
 */
static Bool
msc_is_equal_or_after(uint64_t test, uint64_t reference)
{
    return (int64_t)(test - reference) >= 0;
}

/*
 * Copies the update region from a pixmap to the target drawable
 */
void
present_copy_region(DrawablePtr drawable,
                    PixmapPtr pixmap,
                    RegionPtr update,
                    int16_t x_off,
                    int16_t y_off)
{
    ScreenPtr   screen = drawable->pScreen;
    GCPtr       gc;

    gc = GetScratchGC(drawable->depth, screen);
    if (update) {
        ChangeGCVal     changes[2];

        changes[0].val = x_off;
        changes[1].val = y_off;
        ChangeGC(serverClient, gc,
                 GCClipXOrigin|GCClipYOrigin,
                 changes);
        (*gc->funcs->ChangeClip)(gc, CT_REGION, update, 0);
    }
    ValidateGC(drawable, gc);
    (*gc->ops->CopyArea)(&pixmap->drawable,
                         drawable,
                         gc,
                         0, 0,
                         pixmap->drawable.width, pixmap->drawable.height,
                         x_off, y_off);
    if (update)
        (*gc->funcs->ChangeClip)(gc, CT_NONE, NULL, 0);
    FreeScratchGC(gc);
}

void
present_vblank_notify(present_vblank_ptr vblank, CARD8 kind, CARD8 mode, uint64_t ust, uint64_t crtc_msc)
{
    int         n;

    if (vblank->window)
        present_send_complete_notify(vblank->window, kind, mode, vblank->serial, ust, crtc_msc - vblank->msc_offset);
    for (n = 0; n < vblank->num_notifies; n++) {
        WindowPtr   window = vblank->notifies[n].window;
        CARD32      serial = vblank->notifies[n].serial;

        if (window)
            present_send_complete_notify(window, kind, mode, serial, ust, crtc_msc - vblank->msc_offset);
    }
}

void
present_pixmap_idle(PixmapPtr pixmap, WindowPtr window, CARD32 serial, struct present_fence *present_fence)
{
    if (present_fence)
        present_fence_set_triggered(present_fence);
    if (window) {
        DebugPresent(("\ti %08lx\n", pixmap ? pixmap->drawable.id : 0));
        present_send_idle_notify(window, serial, pixmap, present_fence);
    }
}

RRCrtcPtr
present_get_crtc(WindowPtr window)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    if (!screen_priv)
        return NULL;

    return screen_priv->get_crtc(screen_priv, window);
}

uint32_t
present_query_capabilities(RRCrtcPtr crtc)
{
    present_screen_priv_ptr     screen_priv;

    if (!crtc)
        return 0;

    screen_priv = present_screen_priv(crtc->pScreen);

    if (!screen_priv)
        return 0;

    return screen_priv->query_capabilities(screen_priv);
}

struct pixmap_visit {
    PixmapPtr   old;
    PixmapPtr   new;
};

static int
present_set_tree_pixmap_visit(WindowPtr window, void *data)
{
    struct pixmap_visit *visit = data;
    ScreenPtr           screen = window->drawable.pScreen;

    if ((*screen->GetWindowPixmap)(window) != visit->old)
        return WT_DONTWALKCHILDREN;
    (*screen->SetWindowPixmap)(window, visit->new);
    return WT_WALKCHILDREN;
}

void
present_set_tree_pixmap(WindowPtr window,
                        PixmapPtr expected,
                        PixmapPtr pixmap)
{
    struct pixmap_visit visit;
    ScreenPtr           screen = window->drawable.pScreen;

    visit.old = (*screen->GetWindowPixmap)(window);
    if (expected && visit.old != expected)
        return;

    visit.new = pixmap;
    if (visit.old == visit.new)
        return;
    TraverseTree(window, present_set_tree_pixmap_visit, &visit);
}

/*
 * Called when the wait fence is triggered; just gets the current msc/ust and
 * calls the proper execute again. That will re-check the fence and pend the
 * request again if it's still not actually ready
 */
static void
present_wait_fence_triggered(void *param)
{
    present_vblank_ptr      vblank = param;
    ScreenPtr               screen = vblank->screen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    screen_priv->re_execute(vblank);
}

Bool
present_execute_wait(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    if (vblank->requeue) {
        vblank->requeue = FALSE;
        if (msc_is_after(vblank->target_msc, crtc_msc) &&
            Success == screen_priv->queue_vblank(screen,
                                                 vblank->window,
                                                 vblank->crtc,
                                                 vblank->event_id,
                                                 vblank->target_msc))
            return FALSE;
    }

    if (vblank->wait_fence) {
        if (!present_fence_check_triggered(vblank->wait_fence)) {
            present_fence_set_callback(vblank->wait_fence, present_wait_fence_triggered, vblank);
            return FALSE;
        }
    }
    return TRUE;
}

void
present_execute_copy(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    /* If present_flip failed, we may have to requeue for the target MSC */
    if (vblank->target_msc == crtc_msc + 1 &&
        Success == screen_priv->queue_vblank(screen,
                                             vblank->window,
                                             vblank->crtc,
                                             vblank->event_id,
                                             vblank->target_msc)) {
        vblank->queued = TRUE;
        return;
    }

    present_copy_region(&window->drawable, vblank->pixmap, vblank->update, vblank->x_off, vblank->y_off);

    /* present_copy_region sticks the region into a scratch GC,
     * which is then freed, freeing the region
     */
    vblank->update = NULL;
    screen_priv->flush(window);

    present_pixmap_idle(vblank->pixmap, vblank->window, vblank->serial, vblank->idle_fence);
}

void
present_execute_post(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    uint8_t mode;

    /* Compute correct CompleteMode
     */
    if (vblank->kind == PresentCompleteKindPixmap) {
        if (vblank->pixmap && vblank->window)
            mode = PresentCompleteModeCopy;
        else
            mode = PresentCompleteModeSkip;
    }
    else
        mode = PresentCompleteModeCopy;

    present_vblank_notify(vblank, vblank->kind, mode, ust, crtc_msc);
    present_vblank_destroy(vblank);
}

void
present_adjust_timings(uint32_t options,
                uint64_t *crtc_msc,
                uint64_t *target_msc,
                uint64_t divisor,
                uint64_t remainder)
{
    /* Adjust target_msc to match modulus
     */
    if (msc_is_equal_or_after(*crtc_msc, *target_msc)) {
        if (divisor != 0) {
            *target_msc = *crtc_msc - (*crtc_msc % divisor) + remainder;
            if (options & PresentOptionAsync) {
                if (msc_is_after(*crtc_msc, *target_msc))
                    *target_msc += divisor;
            } else {
                if (msc_is_equal_or_after(*crtc_msc, *target_msc))
                    *target_msc += divisor;
            }
        } else {
            *target_msc = *crtc_msc;
            if (!(options & PresentOptionAsync))
                (*target_msc)++;
        }
    }
}

void
present_scrap_obsolete_vblank(present_vblank_ptr vblank)
{
    DebugPresent(("\tx %lld %p %8lld: %08lx -> %08lx (crtc %p)\n",
                  vblank->event_id, vblank, vblank->target_msc,
                  vblank->pixmap->drawable.id, vblank->window->drawable.id,
                  vblank->crtc));

    present_pixmap_idle(vblank->pixmap, vblank->window, vblank->serial, vblank->idle_fence);
    present_fence_destroy(vblank->idle_fence);
    dixDestroyPixmap(vblank->pixmap, vblank->pixmap->drawable.id);

    vblank->pixmap = NULL;
    vblank->idle_fence = NULL;
    vblank->flip = FALSE;
}

present_vblank_ptr
present_create_vblank(present_window_priv_ptr window_priv,
                      PixmapPtr pixmap,
                      CARD32 serial,
                      RegionPtr valid,
                      RegionPtr update,
                      int16_t x_off,
                      int16_t y_off,
                      RRCrtcPtr target_crtc,
                      SyncFence *wait_fence,
                      SyncFence *idle_fence,
                      uint32_t options,
                      const uint32_t *capabilities,
                      present_notify_ptr notifies,
                      int num_notifies,
                      uint64_t *target_msc,
                      uint64_t crtc_msc)
{
    WindowPtr                   window = window_priv->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    present_vblank_ptr          vblank;

    vblank = calloc (1, sizeof (present_vblank_rec));
    if (!vblank)
        return NULL;

    xorg_list_append(&vblank->window_list, &window_priv->vblank);
    xorg_list_init(&vblank->event_queue);

    vblank->screen = screen;
    vblank->window = window;
    vblank->pixmap = pixmap;

    screen_priv->create_event_id(window_priv, vblank);

    if (pixmap) {
        vblank->kind = PresentCompleteKindPixmap;
        pixmap->refcnt++;
    } else
        vblank->kind = PresentCompleteKindNotifyMSC;

    vblank->serial = serial;

    if (valid) {
        vblank->valid = RegionDuplicate(valid);
        if (!vblank->valid)
            goto no_mem;
    }
    if (update) {
        vblank->update = RegionDuplicate(update);
        if (!vblank->update)
            goto no_mem;
    }

    vblank->x_off = x_off;
    vblank->y_off = y_off;
    vblank->target_msc = *target_msc;
    vblank->crtc = target_crtc;
    vblank->msc_offset = window_priv->msc_offset;
    vblank->notifies = notifies;
    vblank->num_notifies = num_notifies;

    if (pixmap != NULL &&
        !(options & PresentOptionCopy) &&
        capabilities) {
        if (msc_is_after(*target_msc, crtc_msc) &&
            screen_priv->check_flip (target_crtc, window, pixmap, TRUE, valid, x_off, y_off))
        {
            vblank->flip = TRUE;
            vblank->sync_flip = TRUE;
            *target_msc = *target_msc - 1;
        } else if ((*capabilities & PresentCapabilityAsync) &&
            screen_priv->check_flip (target_crtc, window, pixmap, FALSE, valid, x_off, y_off))
        {
            vblank->flip = TRUE;
        }
    }

    if (wait_fence) {
        vblank->wait_fence = present_fence_create(wait_fence);
        if (!vblank->wait_fence)
            goto no_mem;
    }

    if (idle_fence) {
        vblank->idle_fence = present_fence_create(idle_fence);
        if (!vblank->idle_fence)
            goto no_mem;
    }

    if (pixmap)
        DebugPresent(("q %lld %p %8lld: %08lx -> %08lx (crtc %p) flip %d vsync %d serial %d\n",
                      vblank->event_id, vblank, *target_msc,
                      vblank->pixmap->drawable.id, vblank->window->drawable.id,
                      target_crtc, vblank->flip, vblank->sync_flip, vblank->serial));

    return vblank;

no_mem:
    vblank->notifies = NULL;
    present_vblank_destroy(vblank);
    return NULL;
}

int
present_pixmap(WindowPtr window,
               PixmapPtr pixmap,
               CARD32 serial,
               RegionPtr valid,
               RegionPtr update,
               int16_t x_off,
               int16_t y_off,
               RRCrtcPtr target_crtc,
               SyncFence *wait_fence,
               SyncFence *idle_fence,
               uint32_t options,
               uint64_t window_msc,
               uint64_t divisor,
               uint64_t remainder,
               present_notify_ptr notifies,
               int num_notifies)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_window_priv_ptr     window_priv = present_get_window_priv(window, TRUE);
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    if (!window_priv)
        return BadAlloc;

    return screen_priv->present_pixmap(window_priv,
                                       pixmap,
                                       serial,
                                       valid,
                                       update,
                                       x_off,
                                       y_off,
                                       target_crtc,
                                       wait_fence,
                                       idle_fence,
                                       options,
                                       window_msc,
                                       divisor,
                                       remainder,
                                       notifies,
                                       num_notifies);
}

int
present_notify_msc(WindowPtr window,
                   CARD32 serial,
                   uint64_t target_msc,
                   uint64_t divisor,
                   uint64_t remainder)
{
    return present_pixmap(window,
                          NULL,
                          serial,
                          NULL, NULL,
                          0, 0,
                          NULL,
                          NULL, NULL,
                          divisor == 0 ? PresentOptionAsync : 0,
                          target_msc, divisor, remainder, NULL, 0);
}

void
present_vblank_destroy(present_vblank_ptr vblank)
{
    /* Remove vblank from window and screen lists */
    xorg_list_del(&vblank->window_list);

    xorg_list_del(&vblank->event_queue);

    DebugPresent(("\td %lld %p %8lld: %08lx -> %08lx\n",
                  vblank->event_id, vblank, vblank->target_msc,
                  vblank->pixmap ? vblank->pixmap->drawable.id : 0,
                  vblank->window ? vblank->window->drawable.id : 0));

    /* Drop pixmap reference */
    if (vblank->pixmap && vblank->pixmap->refcnt > 1)
        dixDestroyPixmap(vblank->pixmap, vblank->pixmap->drawable.id);

    /* Free regions */
    if (vblank->valid)
        RegionDestroy(vblank->valid);
    if (vblank->update)
        RegionDestroy(vblank->update);

    if (vblank->wait_fence)
        present_fence_destroy(vblank->wait_fence);

    if (vblank->idle_fence)
        present_fence_destroy(vblank->idle_fence);

    if (vblank->notifies)
        present_destroy_notifies(vblank->notifies, vblank->num_notifies);

    free(vblank);
}
