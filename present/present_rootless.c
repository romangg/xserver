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

static void
present_rootless_execute(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc);

static int
present_rootless_get_ust_msc(ScreenPtr screen, WindowPtr window, uint64_t *ust, uint64_t *msc)
{
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    int                         ret;

    ret = (*screen_priv->rootless_info->get_ust_msc)(window, ust, msc);

    if (ret == Success)
        return ret;
    else
        return present_fake_get_ust_msc(screen, ust, msc);  // TODOX: fake counters per window?

}

static RRCrtcPtr
present_rootless_get_crtc(present_screen_priv_ptr screen_priv, WindowPtr window)
{
    return (*screen_priv->rootless_info->get_crtc)(window);
}

static uint32_t
present_rootless_query_capabilities(present_screen_priv_ptr screen_priv)
{
    return screen_priv->rootless_info->capabilities;
}

/*
 * When the wait fence or previous flip is completed, it's time
 * to re-try the request
 */
static void
present_rootless_re_execute(present_vblank_ptr vblank)
{
    uint64_t            ust = 0, crtc_msc = 0;

    (void) present_rootless_get_ust_msc(vblank->screen, vblank->window, &ust, &crtc_msc);  //TODOX: guard against vblank->window == NULL?

    present_rootless_execute(vblank, ust, crtc_msc);
}

static void
present_rootless_flush(WindowPtr window)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    (*screen_priv->rootless_info->flush) (window);
}

static void
present_rootless_flip_try_ready(WindowPtr window)
{
    present_window_priv_ptr window_priv = present_window_priv(window);
    present_vblank_ptr      vblank;


    xorg_list_for_each_entry(vblank, &window_priv->flip_queue, event_queue) {
        if (vblank->queued) {
            present_rootless_re_execute(vblank);
            return;
        }
    }
}

static void
present_rootless_flip_idle_vblank(present_vblank_ptr vblank)
{
    present_pixmap_idle(vblank->pixmap, vblank->window,
                        vblank->serial, vblank->idle_fence);

    /* Don't destroy these objects in the subsequent vblank destruction. */
    vblank->pixmap = NULL;
    vblank->idle_fence = NULL;

    present_vblank_destroy(vblank);
}

static void
present_rootless_flip_idle_active(WindowPtr window)
{
    present_window_priv_ptr window_priv = present_window_priv(window);

    if (window_priv->flip_active) {
        present_rootless_flip_idle_vblank(window_priv->flip_active);
        window_priv->flip_active = NULL;
    }
//    /* if we lose the active flip, the flipping window could be reparented and the DDX
//     * delete the crtc
//     */
//    window_priv->crtc = NULL;
}
/*
 * Free any left over idle vblanks
 */
void
present_rootless_free_idle_vblanks(WindowPtr window)
{
    present_window_priv_ptr         window_priv = present_window_priv(window);
    present_vblank_ptr              vblank, tmp;

    xorg_list_for_each_entry_safe(vblank, tmp, &window_priv->idle_vblank, window_list) {
        /* Deletes it from this list as well. */
        present_rootless_flip_idle_vblank(vblank);
    }
    present_rootless_flip_idle_active(window_priv->window);
}

static int
present_rootless_queue_vblank(ScreenPtr screen,
                              WindowPtr window,
                              RRCrtcPtr crtc,
                              uint64_t event_id,
                              uint64_t msc)
{
    Bool ret;
//    if (crtc == NULL)
//        ret = present_fake_queue_vblank(screen, event_id, msc);
//    else
//    {
        present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
        ret = (*screen_priv->rootless_info->queue_vblank) (window, event_id, msc);  // TODOX: also submit crtc?
//    }
    return ret;
}

void
present_rootless_restore_window_pixmap(WindowPtr window)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_window_priv_ptr     window_priv = present_window_priv(window);
    PixmapPtr                   flip_pixmap = window_priv->flip_pending ? window_priv->flip_pending->pixmap : window_priv->flip_active->pixmap;

    assert (flip_pixmap);

    if (!window_priv->restore_pixmap)
        return;

    /* Update the screen pixmap with the current flip pixmap contents
     * Only do this the first time for a particular unflip operation
     *
     */
    if (screen->GetWindowPixmap(window) == flip_pixmap)
        present_copy_region(&window_priv->restore_pixmap->drawable, flip_pixmap, NULL, 0, 0);

    /* Switch back to using the original window pixmap now to avoid
     * 2D applications drawing to the wrong pixmap.
     */
    present_set_tree_pixmap(window, flip_pixmap, window_priv->restore_pixmap);
    window_priv->restore_pixmap->refcnt--;
    window_priv->restore_pixmap = NULL;
}

void
present_rootless_set_abort_flip(WindowPtr window)
{
    present_window_priv_ptr window_priv = present_window_priv(window);

    if (!window_priv->flip_pending->abort_flip) {
        present_rootless_restore_window_pixmap(window);
        window_priv->flip_pending->abort_flip = TRUE;
    }
}

static void
present_rootless_unflip(WindowPtr window)
{
    present_window_priv_ptr window_priv = present_window_priv(window);
    present_screen_priv_ptr screen_priv = present_screen_priv(window->drawable.pScreen);

    assert (!window_priv->unflip_event_id);
    assert (!window_priv->flip_pending);

    present_rootless_restore_window_pixmap(window);
    present_rootless_free_idle_vblanks(window);

    window_priv->unflip_event_id = ++window_priv->event_id;
    DebugPresent(("u %lld\n", window_priv->unflip_event_id));
    (*screen_priv->rootless_info->unflip) (window, window_priv->unflip_event_id);
}

static void
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
        xorg_list_append(&prev_vblank->event_queue, &window_priv->idle_queue);
    }
    window_priv->flip_active = vblank;
    window_priv->flip_pending = NULL;

    if (vblank->abort_flip)
        present_rootless_unflip(window);

    present_vblank_notify(vblank, PresentCompleteKindPixmap, PresentCompleteModeFlip, ust, crtc_msc);
    present_rootless_flip_try_ready(window);
}

void
present_rootless_event_notify(WindowPtr window, uint64_t event_id, uint64_t ust, uint64_t msc)
{
    present_window_priv_ptr     window_priv = present_window_priv(window);
    present_vblank_ptr          vblank;

    // TODOX: go through a static window list to make sure window_priv is no dangling ptr?
    if (!window_priv)
        return;
    if (!event_id)
        return;

    DebugPresent(("\te %lld ust %lld msc %lld\n", event_id, ust, msc));
    xorg_list_for_each_entry(vblank, &window_priv->exec_queue, event_queue) {
        if (event_id == vblank->event_id) {
            present_rootless_execute(vblank, ust, msc);
            return;
        }
    }
    xorg_list_for_each_entry(vblank, &window_priv->flip_queue, event_queue) {
        if (vblank->event_id == event_id) {
            if (vblank->queued) {
                present_rootless_execute(vblank, ust, msc);
            } else {
                assert(vblank->window);
                present_rootless_flip_notify(vblank, ust, msc);
            }
            return;
        }
    }

    xorg_list_for_each_entry(vblank, &window_priv->idle_queue, event_queue) {
        if (vblank->event_id == event_id) {
            present_rootless_flip_idle_vblank(vblank);
            return;
        }
    }

    if (event_id == window_priv->unflip_event_id) {
        DebugPresent(("\tun %lld\n", event_id));
        window_priv->unflip_event_id = 0;
        present_rootless_flip_idle_active(window_priv->window);
        present_rootless_flip_try_ready(window_priv->window);
    }
}

static Bool
present_rootless_check_flip(RRCrtcPtr    crtc,
                   WindowPtr    window,
                   PixmapPtr    pixmap,
                   Bool         sync_flip,
                   RegionPtr    valid,
                   int16_t      x_off,
                   int16_t      y_off)
{
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    if (!screen_priv)
        return FALSE;

    if (!screen_priv->rootless_info)
        return FALSE;

//    if (!crtc)
//        return FALSE;

    /* Check to see if the driver supports flips at all */
    if (!screen_priv->rootless_info->flip)
        return FALSE;

    /* Source pixmap must align with window exactly */
    if (x_off || y_off) {
        return FALSE;
    }

    if (window->drawable.width != pixmap->drawable.width ||
            window->drawable.height != pixmap->drawable.height)
        return FALSE;

    /* Ask the driver for permission */
    if (screen_priv->rootless_info->check_flip) {
        if (!(*screen_priv->rootless_info->check_flip) (crtc, window, pixmap, sync_flip)) {
            DebugPresent(("\td %08lx -> %08lx\n", window->drawable.id, pixmap ? pixmap->drawable.id : 0));
            return FALSE;
        }
    }

    return TRUE;
}

/*
 * 'window' is being reconfigured. Check to see if it is involved
 * in flipping and clean up as necessary
 */
static void
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
        if (!present_rootless_check_flip(flip_pending->crtc, flip_pending->window, flip_pending->pixmap,
                                flip_pending->sync_flip, NULL, 0, 0))
            present_rootless_set_abort_flip(window);
    } else if (flip_active) {
        if (!present_rootless_check_flip(flip_active->crtc, flip_active->window, flip_active->pixmap, flip_active->sync_flip, NULL, 0, 0))
            present_rootless_unflip(window);
    }

    /* Now check any queued vblanks */
    xorg_list_for_each_entry(vblank, &window_priv->vblank, window_list) {
        if (vblank->queued && vblank->flip && !present_rootless_check_flip(vblank->crtc, window, vblank->pixmap, vblank->sync_flip, NULL, 0, 0)) {
            vblank->flip = FALSE;
            if (vblank->sync_flip)
                vblank->requeue = TRUE;
        }
    }
}

static Bool
present_rootless_flip(WindowPtr window,
                      RRCrtcPtr crtc,
                      uint64_t event_id,
                      uint64_t target_msc,
                      PixmapPtr pixmap,
                      Bool sync_flip)
{
    ScreenPtr                   screen = crtc->pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    return (*screen_priv->rootless_info->flip) (window, crtc, event_id, target_msc, pixmap, sync_flip);
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

static void
present_rootless_execute(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_window_priv_ptr     window_priv = present_window_priv(window);
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

    if (!present_execute_wait(vblank, crtc_msc))
        return;


    if (vblank->flip && vblank->pixmap && vblank->window) {
        if (window_priv->flip_pending || window_priv->unflip_event_id) {
            DebugPresent(("\tr %lld %p (pending %p unflip %lld)\n",
                          vblank->event_id, vblank,
                          window_priv->flip_pending, window_priv->unflip_event_id));
            xorg_list_del(&vblank->event_queue);
            xorg_list_append(&vblank->event_queue, &window_priv->flip_queue);
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
            xorg_list_add(&vblank->event_queue, &window_priv->flip_queue);

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
                if (*screen_priv->rootless_info->flip_executed)
                    (*screen_priv->rootless_info->flip_executed) (vblank->window, vblank->crtc, vblank->event_id, damage);

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
            present_rootless_set_abort_flip(window);
        else if (!window_priv->unflip_event_id && window_priv->flip_active)
            present_rootless_unflip(window);

        present_execute_flip_recover(vblank, crtc_msc);
        if (vblank->queued) {
            xorg_list_add(&vblank->event_queue, &window_priv->exec_queue);
            xorg_list_append(&vblank->window_list, &window_priv->vblank);

            return;
        }
    }

    present_execute_complete(vblank, ust, crtc_msc);
}

static void
present_rootless_create_event_id(present_window_priv_ptr window_priv, present_vblank_ptr vblank)
{
    vblank->event_id = ++window_priv->event_id;
}

static uint64_t
present_rootless_window_to_crtc_msc(WindowPtr window, RRCrtcPtr crtc, uint64_t window_msc, uint64_t new_msc)
{
    present_window_priv_ptr     window_priv = present_get_window_priv(window, TRUE);

    if (crtc != window_priv->crtc) {
        uint64_t        old_ust, old_msc;

        if (window_priv->crtc == PresentCrtcNeverSet) {
            window_priv->msc_offset = 0;
        } else {
            /* The old CRTC may have been turned off, in which case
             * we'll just use whatever previous MSC we'd seen from this CRTC
             */

            if (present_rootless_get_ust_msc(window->drawable.pScreen, window, &old_ust, &old_msc) != Success)   // TODOX: why here again?
                old_msc = window_priv->msc;

            window_priv->msc_offset += new_msc - old_msc;
        }
        window_priv->crtc = crtc;
    }

    return window_msc + window_priv->msc_offset;
}

static int
present_rootless_present_pixmap(present_window_priv_ptr window_priv,
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
    WindowPtr                   window = window_priv->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr     screen_priv = present_screen_priv(screen);
    uint64_t                    ust = 0;
    uint64_t                    target_msc;
    uint64_t                    crtc_msc = 0;
    present_vblank_ptr          vblank, tmp;

    target_crtc = present_rootless_get_crtc(screen_priv, window);

    if (present_rootless_get_ust_msc(screen, window, &ust, &crtc_msc) == Success)
        window_priv->msc = crtc_msc;

    target_msc = present_rootless_window_to_crtc_msc(window, target_crtc, window_msc, crtc_msc);

    present_adjust_timings(options,
                           &crtc_msc,
                           &target_msc,
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
            if (vblank->flip_ready)
                present_rootless_re_execute(vblank);
        }
    }

    vblank = present_create_vblank(window_priv,
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
                                notifies,
                                num_notifies,
                                &target_msc,
                                crtc_msc);
    if (!vblank)
        return BadAlloc;

    xorg_list_append(&vblank->event_queue, &window_priv->exec_queue);
    vblank->queued = TRUE;
    if (crtc_msc < target_msc) {
        if (present_rootless_queue_vblank(screen, window, target_crtc, vblank->event_id, target_msc) == Success) {
            return Success;
        }
        DebugPresent(("present_queue_vblank failed\n"));
    }

    present_rootless_execute(vblank, ust, crtc_msc);
    return Success;
}

static void
present_rootless_flips_destroy(ScreenPtr screen)
{
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);
    present_window_priv_ptr window_priv;

    xorg_list_for_each_entry(window_priv, &screen_priv->windows, screen_list) {
        /* Reset window pixmaps back to the original window pixmap */
        if (window_priv->flip_pending)
            present_rootless_set_abort_flip(window_priv->window);

        /* Drop reference to any pending flip or unflip pixmaps. */
        present_rootless_free_idle_vblanks(window_priv->window);
    }
}

static void
present_rootless_abort_vblank(ScreenPtr screen, void* target, uint64_t event_id, uint64_t msc)
{
    WindowPtr               window = target;
    present_window_priv_ptr window_priv = present_window_priv(window);
    present_vblank_ptr      vblank;

//    if (window == NULL)
//        present_fake_abort_vblank(screen, event_id, msc);
//    else
//    {
        present_screen_priv_ptr     screen_priv = present_screen_priv(screen);

        (*screen_priv->rootless_info->abort_vblank) (window, event_id, msc);
//    }

    xorg_list_for_each_entry(vblank, &window_priv->exec_queue, event_queue) {
        int64_t match = event_id - vblank->event_id;
        if (match == 0) {
            xorg_list_del(&vblank->event_queue);
            vblank->queued = FALSE;
            return;
        }
        if (match < 0)
            break;
    }
    xorg_list_for_each_entry(vblank, &window_priv->flip_queue, event_queue) {
        if (vblank->event_id == event_id) {
            xorg_list_del(&vblank->event_queue);
            vblank->queued = FALSE;
            return;
        }
    }
}

void
present_rootless_init_rootless(present_screen_priv_ptr screen_priv)
{
    screen_priv->check_flip_window = &present_rootless_check_flip_window;
    screen_priv->create_event_id = &present_rootless_create_event_id;
    screen_priv->present_pixmap = &present_rootless_present_pixmap;
    screen_priv->flips_destroy = &present_rootless_flips_destroy;
    screen_priv->re_execute = &present_rootless_re_execute;
    screen_priv->queue_vblank = &present_rootless_queue_vblank;
    screen_priv->get_crtc = &present_rootless_get_crtc;
    screen_priv->query_capabilities = &present_rootless_query_capabilities;
    screen_priv->check_flip = &present_rootless_check_flip;
    screen_priv->abort_vblank = &present_rootless_abort_vblank;
    screen_priv->flush = &present_rootless_flush;
}
