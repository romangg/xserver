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
        present_send_complete_notify(vblank->window, kind, mode, vblank->serial, ust, crtc_msc - vblank->msc_offset, vblank->client);
    for (n = 0; n < vblank->num_notifies; n++) {
        WindowPtr   window = vblank->notifies[n].window;
        CARD32      serial = vblank->notifies[n].serial;

        if (window)
            present_send_complete_notify(window, kind, mode, serial, ust, crtc_msc - vblank->msc_offset, vblank->client);
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

    if (!screen_priv->info)
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

    if (!screen_priv->info)
        return 0;

    return screen_priv->info->capabilities;
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

static void
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
 * calls present_execute again. That will re-check the fence and pend the
 * request again if it's still not actually ready
 */
static void
present_wait_fence_triggered(void *param)
{
    present_vblank_ptr  vblank = param;
    present_re_execute(vblank);
}

int
present_pixmap(WindowPtr window,
               PixmapPtr pixmap,
               ClientPtr client,
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
                                       client,
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

void
present_vblank_destroy(present_vblank_ptr vblank)
{
    /* Remove vblank from window and screen lists */
    xorg_list_del(&vblank->window_list);

    DebugPresent(("\td %lld %p %8lld: %08lx -> %08lx\n",
                  vblank->event_id, vblank, vblank->target_msc,
                  vblank->pixmap ? vblank->pixmap->drawable.id : 0,
                  vblank->window ? vblank->window->drawable.id : 0));

    /* Drop pixmap reference */
    if (vblank->pixmap)
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
