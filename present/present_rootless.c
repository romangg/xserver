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

#ifdef HAVE_XORG_CONFIG_H
#include <xorg-config.h>
#endif

#include "present_priv.h"

static ScreenPtr present_rootless_screen;

Bool
present_rootless_flip_(WindowPtr window,
                      RRCrtcPtr crtc,
                      uint64_t event_id,
                      uint64_t target_msc,
                      PixmapPtr pixmap,
                      Bool sync_flip)
{
    ScreenPtr                   screen = window->pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    return (*screen_priv->info->flip_rootless) (window, crtc, event_id, target_msc, pixmap, sync_flip);
}

void
present_rootless_flip_try_ready(WindowPtr window)
{
    present_vblank_ptr  vblank;

    xorg_list_for_each_entry(vblank, &present_flip_queue, event_queue) {
        if (vblank->queued && vblank->window == window) {
            present_re_execute(vblank);
            return;
        }
    }
}

void
present_rootless_flip_idle_vblank(present_vblank_ptr vblank)
{
    present_pixmap_idle(vblank->pixmap, vblank->window,
                        vblank->serial, vblank->idle_fence);

    /* Don't destroy these objects in the subsequent vblank destruction. */
    vblank->pixmap = NULL;
    vblank->idle_fence = NULL;

    present_vblank_destroy(vblank);
}

void
present_rootless_flip_idle_active(WindowPtr window)
{
    present_window_priv_ptr window_priv = present_window_priv(window);

    if (window_priv->flip_active) {
        present_flip_idle_rootless_vblank(window_priv->flip_active);
        window_priv->flip_active = NULL;
    }
    /* if we lose the active flip, the flipping window could be reparented and the DDX
     * delete the crtc
     */
    window_priv->crtc = NULL;
}

void
present_rootless_set_abort_flip(WindowPtr window)
{
    present_window_priv_ptr window_priv = present_window_priv(window);

    if (!window_priv->flip_pending->abort_flip) {
        present_restore_window_pixmap_only(window);
        window_priv->flip_pending->abort_flip = TRUE;
    }
}

void
present_rootless_unflip(WindowPtr window)
{
    present_window_priv_ptr window_priv = present_window_priv(window);
    present_screen_priv_ptr screen_priv = present_screen_priv(window->drawable.pScreen);

    assert (!window_priv->unflip_event_id);
    assert (!window_priv->flip_pending);

    present_restore_window_pixmap_only(window);
    present_free_window_vblank_idle(window);

    window_priv->unflip_event_id = ++present_event_id;
    DebugPresent(("u %lld\n", window_priv->unflip_event_id));
    (*screen_priv->info->unflip_rootless) (window, window_priv->unflip_event_id);
}

void
present_rootless_flip_notify(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    present_window_priv_ptr     window_priv = present_window_priv(window);
    present_vblank_ptr          prev_vblank;

    DebugPresent(("\tn %lld %p %8lld: %08lx -> %08lx\n",
                  vblank->event_id, vblank, vblank->target_msc,
                  vblank->pixmap ? vblank->pixmap->drawable.id : 0,
                  vblank->window ? vblank->window->drawable.id : 0));

    assert (vblank == window_priv->flip_pending);

    xorg_list_del(&vblank->event_queue);

    if (window_priv->flip_active) {
        /* Put the flip back in the window_list and wait for further notice from DDX */
        prev_vblank = window_priv->flip_active;
        xorg_list_append(&prev_vblank->window_list, &window_priv->idle_vblank);
        xorg_list_append(&prev_vblank->event_queue, &present_idle_queue);
    }
    window_priv->flip_active = vblank;
    window_priv->flip_pending = NULL;

    if (vblank->abort_flip)
        present_unflip_rootless(window);

    present_vblank_notify(vblank, PresentCompleteKindPixmap, PresentCompleteModeFlip, ust, crtc_msc);
    present_rootless_flip_try_ready(window);
}

void
present_rootless_event_unflip(uint64_t event_id)
{
    present_screen_priv_ptr screen_priv = present_screen_priv(present_rootless_screen);
    present_window_priv_ptr window_priv;

    xorg_list_for_each_entry(window_priv, &screen_priv->windows, screen_list) {
        if (event_id == window_priv->unflip_event_id) {
            DebugPresent(("\tun %lld\n", event_id));
            window_priv->unflip_event_id = 0;
            present_flip_idle_rootless_active(window_priv->window);
            present_flip_try_ready_rootless(window_priv->window);
            return;
        }
    }
}

/*
 * 'window' is being reconfigured. Check to see if it is involved
 * in flipping and clean up as necessary
 */
void
present_rootless_check_flip_window (WindowPtr window)
{
    present_window_priv_ptr     window_priv = present_window_priv(window);
    present_vblank_ptr          flip_pending;
    present_vblank_ptr          flip_active;
    present_vblank_ptr          vblank;

    /* If this window hasn't ever been used with Present, it can't be
     * flipping
     */
    if (!window_priv)
        return;

    if (window_priv->unflip_event_id)
        return;

    flip_pending = window_priv->flip_pending;
    flip_active = window_priv->flip_active;

    if (flip_pending) {
        if (!present_check_flip(flip_pending->crtc, flip_pending->window, flip_pending->pixmap,
                                flip_pending->sync_flip, NULL, 0, 0))
            present_set_abort_flip_rootless(window);
    } else if (flip_active) {
        if (!present_check_flip(flip_active->crtc, flip_active->window, flip_active->pixmap, flip_active->sync_flip, NULL, 0, 0))
            present_unflip_rootless(window);
    }

    /* Now check any queued vblanks */
    xorg_list_for_each_entry(vblank, &window_priv->vblank, window_list) {
        if (vblank->queued && vblank->flip && !present_check_flip(vblank->crtc, window, vblank->pixmap, vblank->sync_flip, NULL, 0, 0)) {
            vblank->flip = FALSE;
            if (vblank->sync_flip)
                vblank->requeue = TRUE;
        }
    }
}

/*
 * Once the required MSC has been reached, execute the pending request.
 *
 * For requests to actually present something, either blt contents to
 * the screen or queue a frame buffer swap.
 *
 * For requests to just get the current MSC/UST combo, skip that part and
 * go straight to event delivery
 */

void
present_rootless_execute(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_window_priv_ptr     window_priv = present_window_priv(window);
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    if (!present_execute_wait(vblank, msc))
        return;


    if (vblank->flip && vblank->pixmap && vblank->window) {
        if (window_priv->flip_pending || window_priv->unflip_event_id) {
            DebugPresent(("\tr %lld %p (pending %p unflip %lld)\n",
                          vblank->event_id, vblank,
                          window_priv->flip_pending, window_priv->unflip_event_id));
            xorg_list_del(&vblank->event_queue);
            xorg_list_append(&vblank->event_queue, &present_flip_queue);
            vblank->flip_ready = TRUE;
            return;
        }
    }

    xorg_list_del(&vblank->event_queue);
    xorg_list_del(&vblank->window_list);
    vblank->queued = FALSE;

    if (vblank->pixmap && vblank->window) {

        if (vblank->flip) {
            RegionPtr damage;

            DebugPresent(("\tf %lld %p %8lld: %08lx -> %08lx\n",
                          vblank->event_id, vblank, crtc_msc,
                          vblank->pixmap->drawable.id, vblank->window->drawable.id));

            /* Prepare to flip by placing it in the flip queue
             */
            xorg_list_add(&vblank->event_queue, &present_flip_queue);

            /* Try to flip
             */
            window_priv->flip_pending = vblank;

            if (present_rootless_flip(vblank->window, vblank->crtc, vblank->event_id, vblank->target_msc, vblank->pixmap, vblank->sync_flip)) {
                /* Fix window pixmaps:
                 *  1) Remember original window pixmap
                 *  2) Set current flip window pixmap to the new pixmap
                 */
                if (!window_priv->restore_pixmap) {
                    window_priv->restore_pixmap = (*screen->GetWindowPixmap)(window);
                    window_priv->restore_pixmap->refcnt++;
                }
                present_set_tree_pixmap(vblank->window, NULL, vblank->pixmap);

                /* Report update region as damaged
                 */
                if (vblank->update) {
                    damage = vblank->update;
                    RegionIntersect(damage, damage, &window->clipList);
                } else
                    damage = &window->clipList;

                DamageDamageRegion(&vblank->window->drawable, damage);
                if (*screen_priv->info->flip_executed)
                    (*screen_priv->info->flip_executed) (vblank->window, vblank->crtc, vblank->event_id, damage);

                return;
            }

            xorg_list_del(&vblank->event_queue);
            /* Oops, flip failed. Clear the flip_pending field
              */
            window_priv->flip_pending = NULL;

            vblank->flip = FALSE;
        }
        DebugPresent(("\tc %p %8lld: %08lx -> %08lx\n", vblank, crtc_msc, vblank->pixmap->drawable.id, vblank->window->drawable.id));

        if (window_priv->flip_pending)
            present_set_abort_flip_rootless(window);
        else if (!window_priv->unflip_event_id && window_priv->flip_active)
            present_unflip_rootless(window);

        if (!present_execute_flip_recover(vblank, crtc_msc))
            return;
    }

    present_execute_complete(vblank, ust, crtc_msc);
}

int
present_rootless_pixmap(WindowPtr window,
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
    uint64_t                    ust = 0;
    uint64_t                    target_msc;
    uint64_t                    crtc_msc = 0;
    present_vblank_ptr          vblank, tmp;
    present_window_priv_ptr     window_priv = present_get_window_priv(window, TRUE);

    target_crtc = present_get_crtc(window);

    present_timings(window_priv,
                    target_crtc,
                    &ust,
                    &crtc_msc,
                    &target_msc,
                    window_msc,
                    divisor,
                    remainder);

    if (!update && pixmap) {
        xorg_list_for_each_entry_safe(vblank, tmp, &window_priv->vblank, window_list) {

            if (!vblank->pixmap)
                continue;

            if (!vblank->queued)
                continue;

            if (vblank->target_msc != target_msc)
                continue;

            if (vblank->window != window)
                continue;

            present_scrap_obsolete_vblank(vblank);
        }
    }

    return present_create_vblank(window_priv,
                                 pixmap,
                                 serial,
                                 valid,
                                 update,
                                 x_off,
                                 y_off,
                                 target_crtc,
                                 *wait_fence,
                                 *idle_fence,
                                 options,
                                 notifies,
                                 num_notifies);
}

void
present_rootless_flips_destroy(ScreenPtr screen)
{
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);
    present_window_priv_ptr window_priv;

    xorg_list_for_each_entry(window_priv, &screen_priv->windows, screen_list) {
        /* Reset window pixmaps back to the original window pixmap */
        if (window_priv->flip_pending)
            present_set_abort_flip_rootless(window_priv->window);

        /* Drop reference to any pending flip or unflip pixmaps. */
        present_free_window_vblank_idle(window_priv->window);
    }
}
