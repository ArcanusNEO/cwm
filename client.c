/*
 * calmwm - the calm window manager
 *
 * Copyright (c) 2004 Marius Aamodt Eriksen <marius@monkey.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $OpenBSD$
 */

#include <sys/types.h>
#include "queue.h"

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calmwm.h"

static void			 client_apply_geom(struct client_ctx *);
static void			 client_class_hint(struct client_ctx *);
static int			 client_geom_overlap(struct geom *, struct geom *);
static int			 client_has_above(struct screen_ctx *);
static struct client_ctx	*client_highest_normal(struct screen_ctx *);
static struct client_ctx	*client_lowest_above(struct screen_ctx *);
static int			 client_recent_popup_geom(struct screen_ctx *,
				     Window, struct geom *);
static void			 client_ignore_transients(struct client_ctx *);
static void			 client_read_transient(struct client_ctx *);
static void			 client_placement(struct client_ctx *);
static void			 client_mwm_hints(struct client_ctx *);

struct client_ctx *
client_init(Window win, struct screen_ctx *sc)
{
	struct client_ctx	*cc;
	XWindowAttributes	 wattr;
	int			 mapped, scanning = (sc != NULL);
	long			 state;

	if (win == None)
		return NULL;
	if (!XGetWindowAttributes(X_Dpy, win, &wattr))
		return NULL;

	if (sc == NULL) {
		if ((sc = screen_find(wattr.root)) == NULL)
			return NULL;
		mapped = 1;
	} else {
		if (wattr.override_redirect || wattr.map_state != IsViewable)
			return NULL;
		mapped = wattr.map_state != IsUnmapped;
	}

	XGrabServer(X_Dpy);

	cc = xmalloc(sizeof(*cc));
	cc->sc = sc;
	cc->win = win;
	cc->name = NULL;
	cc->label = NULL;
	cc->gc = NULL;
	cc->res_class = NULL;
	cc->res_name = NULL;
	cc->flags = 0;
	cc->stackingorder = 0;
	cc->initial_state = 0;
	memset(&cc->hint, 0, sizeof(cc->hint));
	TAILQ_INIT(&cc->nameq);

	cc->geom.x = wattr.x;
	cc->geom.y = wattr.y;
	cc->geom.w = wattr.width;
	cc->geom.h = wattr.height;
	cc->colormap = wattr.colormap;
	cc->obwidth = wattr.border_width;
	cc->bwidth = Conf.bwidth;

	client_set_name(cc);
	conf_client(cc);

	client_wm_hints(cc);
	client_class_hint(cc);
	client_wm_protocols(cc);
	client_get_sizehints(cc);
	client_read_transient(cc);
	client_mwm_hints(cc);

	if (cc->flags & CLIENT_IGNORE)
		cc->bwidth = 0;
	cc->sbwidth = cc->bwidth;
	cc->dim.w = (cc->geom.w - cc->hint.basew) / cc->hint.incw;
	cc->dim.h = (cc->geom.h - cc->hint.baseh) / cc->hint.inch;
	cc->ptr.x = cc->geom.w / 2;
	cc->ptr.y = cc->geom.h / 2;

	if (wattr.map_state != IsViewable) {
		client_placement(cc);
		client_apply_geom(cc);
		if (cc->initial_state)
			xu_set_wm_state(cc->win, cc->initial_state);
	}

	XSelectInput(X_Dpy, cc->win,
	    EnterWindowMask | PropertyChangeMask | KeyReleaseMask);

	XAddToSaveSet(X_Dpy, cc->win);

	/* Notify client of its configuration. */
	client_config(cc);

	TAILQ_INSERT_TAIL(&sc->clientq, cc, entry);

	xu_ewmh_net_client_list(sc);
	xu_ewmh_net_client_list_stacking(sc);
	xu_ewmh_restore_net_wm_state(cc);
	/* Publish inherited above state and discard states rejected by ignore. */
	if (cc->flags & (CLIENT_ABOVE | CLIENT_IGNORE))
		xu_ewmh_set_net_wm_state(cc);

	xu_get_wm_state(cc->win, &state);
	if (state == IconicState && !(cc->flags & CLIENT_IGNORE))
		client_hide(cc);
	else if (scanning)
		client_map(cc);
	else
		client_show(cc);

	if (mapped) {
		if (cc->gc) {
			group_movetogroup(cc, cc->gc->num);
			goto out;
		}
		if (group_restore(cc))
			goto out;
		if (group_autogroup(cc))
			goto out;
		if (Conf.stickygroups)
			group_assign(sc->group_active, cc);
		else
			group_assign(NULL, cc);
	}
out:
	if (cc->flags & CLIENT_IGNORE)
		client_ignore_transients(cc);
	XSync(X_Dpy, False);
	XUngrabServer(X_Dpy);

	return cc;
}

struct client_ctx *
client_current(struct screen_ctx *sc)
{
	struct screen_ctx	*_sc;
	struct client_ctx	*cc;

	if (sc) {
		TAILQ_FOREACH(cc, &sc->clientq, entry) {
			if (cc->flags & CLIENT_ACTIVE)
				return cc;
		}
	} else {
		TAILQ_FOREACH(_sc, &Screenq, entry) {
			TAILQ_FOREACH(cc, &_sc->clientq, entry) {
				if (cc->flags & CLIENT_ACTIVE)
					return cc;
			}
		}
	}
	return NULL;
}

struct client_ctx *
client_find(Window win)
{
	struct screen_ctx	*sc;
	struct client_ctx	*cc;

	TAILQ_FOREACH(sc, &Screenq, entry) {
		TAILQ_FOREACH(cc, &sc->clientq, entry) {
			if (cc->win == win)
				return cc;
		}
	}
	return NULL;
}

struct client_ctx *
client_next(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct client_ctx	*newcc;

	return(((newcc = TAILQ_NEXT(cc, entry)) != NULL) ?
	    newcc : TAILQ_FIRST(&sc->clientq));
}

struct client_ctx *
client_prev(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct client_ctx	*newcc;

	return(((newcc = TAILQ_PREV(cc, client_q, entry)) != NULL) ?
	    newcc : TAILQ_LAST(&sc->clientq, client_q));
}

void
client_remove(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct winname		*wn;

	TAILQ_REMOVE(&sc->clientq, cc, entry);

	xu_ewmh_net_client_list(sc);
	xu_ewmh_net_client_list_stacking(sc);

	if (cc->flags & CLIENT_ACTIVE)
		xu_ewmh_net_active_window(sc, None);

	while ((wn = TAILQ_FIRST(&cc->nameq)) != NULL) {
		TAILQ_REMOVE(&cc->nameq, wn, entry);
		free(wn->name);
		free(wn);
	}

	free(cc->name);
	free(cc->label);
	free(cc->res_class);
	free(cc->res_name);
	free(cc);
}

static Bool
client_active_notify(Display *dpy, XEvent *ev, XPointer arg)
{
	return ev->type == PropertyNotify &&
	    ev->xproperty.atom == ewmh[_NET_ACTIVE_WINDOW] &&
	    ev->xproperty.serial == *(unsigned long *)arg;
}

int
client_set_active(struct client_ctx *cc, Time time)
{
	struct screen_ctx	*sc = cc->sc;
	struct client_ctx	*oldcc;
	unsigned long		 serial;
	XEvent			 ev;

	if (cc->flags & CLIENT_HIDDEN)
		return 0;

	/* Ignored windows that take no input never get focus. */
	if ((cc->flags & CLIENT_IGNORE) && (cc->flags & CLIENT_NOINPUT) &&
	    !(cc->flags & CLIENT_WM_TAKE_FOCUS))
		return 0;

	XInstallColormap(X_Dpy, cc->colormap);

	serial = NextRequest(X_Dpy);
	xu_ewmh_net_active_window(sc, cc->win);
	if (time == CurrentTime && (cc->flags & CLIENT_WM_TAKE_FOCUS)) {
		/* ICCCM requires a server timestamp, even for an untimed request. */
		XIfEvent(X_Dpy, &ev, client_active_notify, (XPointer)&serial);
		time = ev.xproperty.time;
	}
	if ((cc->flags & CLIENT_INPUT) ||
	    (!(cc->flags & CLIENT_WM_TAKE_FOCUS))) {
		XSetInputFocus(X_Dpy, cc->win,
		    RevertToPointerRoot, time);
	}
	if (cc->flags & CLIENT_WM_TAKE_FOCUS)
		xu_send_clientmsg(cc->win, cwmh[WM_TAKE_FOCUS], time);

	if ((oldcc = client_current(sc)) != NULL) {
		oldcc->flags &= ~CLIENT_ACTIVE;
		client_draw_border(oldcc);
	}

	/* If we're in the middle of cycling, don't change the order. */
	if (!sc->cycling)
		client_mtf(cc);

	cc->flags |= CLIENT_ACTIVE;
	cc->flags &= ~CLIENT_URGENCY;
	client_draw_border(cc);
	conf_grab_mouse(cc->win);
	return 1;
}

void
client_focus(struct client_ctx *cc, Time time)
{
	unsigned long	serial;

	if (!(cc->flags & CLIENT_IGNORE))
		client_ptr_warp(cc);

	/* A crossing queued by the raise or warp must not undo the selection. */
	serial = NextRequest(X_Dpy);
	if (client_set_active(cc, time))
		cc->sc->focus_serial = serial;
}

void
client_toggle_freeze(struct client_ctx *cc)
{
	if (cc->flags & (CLIENT_FULLSCREEN | CLIENT_IGNORE))
		return;

	cc->flags ^= CLIENT_FREEZE;
	xu_ewmh_set_net_wm_state(cc);
}

void
client_toggle_hidden(struct client_ctx *cc)
{
	if (cc->flags & CLIENT_IGNORE)
		return;

	cc->flags ^= CLIENT_HIDDEN;
	xu_ewmh_set_net_wm_state(cc);
}

void
client_toggle_skip_pager(struct client_ctx *cc)
{
	cc->flags ^= CLIENT_SKIP_PAGER;
	xu_ewmh_set_net_wm_state(cc);
}

void
client_toggle_skip_taskbar(struct client_ctx *cc)
{
	cc->flags ^= CLIENT_SKIP_TASKBAR;
	xu_ewmh_set_net_wm_state(cc);
}

void
client_toggle_sticky(struct client_ctx *cc)
{
	if (cc->flags & CLIENT_IGNORE)
		return;

	cc->flags ^= CLIENT_STICKY;
	xu_ewmh_set_net_wm_state(cc);
}

void
client_toggle_above(struct client_ctx *cc)
{
	/* Change only this window's layer, even if it covers an open dialog. */
	cc->flags ^= CLIENT_ABOVE;

	if (!(cc->flags & CLIENT_HIDDEN))
		client_raise(cc);

	xu_ewmh_set_net_wm_state(cc);
}

void
client_toggle_fullscreen(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct geom		 area;

	if (cc->flags & CLIENT_IGNORE)
		return;

	if ((cc->flags & CLIENT_FREEZE) &&
	    !(cc->flags & CLIENT_FULLSCREEN))
		return;

	if (cc->flags & CLIENT_FULLSCREEN) {
		if ((cc->flags & CLIENT_MAXFLAGS) != CLIENT_MAXIMIZED)
			cc->bwidth = cc->sbwidth;
		cc->geom = cc->fullgeom;
		cc->flags &= ~(CLIENT_FULLSCREEN | CLIENT_FREEZE);
		goto resize;
	}

	cc->fullgeom = cc->geom;

	area = screen_area(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2, 0);

	cc->bwidth = 0;
	cc->geom = area;
	cc->flags |= (CLIENT_FULLSCREEN | CLIENT_FREEZE);

resize:
	client_resize(cc, 0);
	xu_ewmh_set_net_wm_state(cc);
	client_ptr_inbound(cc, 1);
}

void
client_toggle_maximize(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct geom		 area;

	if (cc->flags & CLIENT_IGNORE)
		return;

	if (cc->flags & CLIENT_FREEZE)
		return;

	if ((cc->flags & CLIENT_MAXFLAGS) == CLIENT_MAXIMIZED) {
		cc->bwidth = cc->sbwidth;
		cc->geom = cc->savegeom;
		cc->flags &= ~CLIENT_MAXIMIZED;
		goto resize;
	}

	if (!(cc->flags & CLIENT_VMAXIMIZED)) {
		cc->savegeom.h = cc->geom.h;
		cc->savegeom.y = cc->geom.y;
	}

	if (!(cc->flags & CLIENT_HMAXIMIZED)) {
		cc->savegeom.w = cc->geom.w;
		cc->savegeom.x = cc->geom.x;
	}

	area = screen_area(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2, 1);

	cc->bwidth = 0;
	cc->geom.x = area.x;
	cc->geom.y = area.y;
	cc->geom.w = area.w - (cc->bwidth * 2);
	cc->geom.h = area.h - (cc->bwidth * 2);
	cc->flags |= CLIENT_MAXIMIZED;

resize:
	client_resize(cc, 0);
	xu_ewmh_set_net_wm_state(cc);
	client_ptr_inbound(cc, 1);
}

void
client_toggle_vmaximize(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct geom		 area;

	if (cc->flags & CLIENT_IGNORE)
		return;

	if (cc->flags & CLIENT_FREEZE)
		return;

	area = screen_area(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2, 1);

	if (cc->flags & CLIENT_VMAXIMIZED) {
		cc->geom.y = cc->savegeom.y;
		cc->geom.h = cc->savegeom.h;
		cc->flags &= ~CLIENT_VMAXIMIZED;
		/* Refit the width as the border returns. */
		if (cc->flags & CLIENT_HMAXIMIZED) {
			cc->bwidth = cc->sbwidth;
			cc->geom.w = area.w - (cc->bwidth * 2);
		}
		goto resize;
	}

	cc->savegeom.y = cc->geom.y;
	cc->savegeom.h = cc->geom.h;

	/* Drop the border and refit the width. */
	if (cc->flags & CLIENT_HMAXIMIZED) {
		cc->bwidth = 0;
		cc->geom.w = area.w;
	}
	cc->geom.y = area.y;
	cc->geom.h = area.h - (cc->bwidth * 2);
	cc->flags |= CLIENT_VMAXIMIZED;

resize:
	client_resize(cc, 0);
	xu_ewmh_set_net_wm_state(cc);
	client_ptr_inbound(cc, 1);
}

void
client_toggle_hmaximize(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct geom		 area;

	if (cc->flags & CLIENT_IGNORE)
		return;

	if (cc->flags & CLIENT_FREEZE)
		return;

	area = screen_area(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2, 1);

	if (cc->flags & CLIENT_HMAXIMIZED) {
		cc->geom.x = cc->savegeom.x;
		cc->geom.w = cc->savegeom.w;
		cc->flags &= ~CLIENT_HMAXIMIZED;
		/* Refit the height as the border returns. */
		if (cc->flags & CLIENT_VMAXIMIZED) {
			cc->bwidth = cc->sbwidth;
			cc->geom.h = area.h - (cc->bwidth * 2);
		}
		goto resize;
	}

	cc->savegeom.x = cc->geom.x;
	cc->savegeom.w = cc->geom.w;

	/* Drop the border and refit the height. */
	if (cc->flags & CLIENT_VMAXIMIZED) {
		cc->bwidth = 0;
		cc->geom.h = area.h;
	}
	cc->geom.x = area.x;
	cc->geom.w = area.w - (cc->bwidth * 2);
	cc->flags |= CLIENT_HMAXIMIZED;

resize:
	client_resize(cc, 0);
	xu_ewmh_set_net_wm_state(cc);
	client_ptr_inbound(cc, 1);
}

void
client_resize(struct client_ctx *cc, int reset)
{
	if (cc->flags & CLIENT_IGNORE)
		return;
	if (reset) {
		cc->bwidth = cc->sbwidth;
		if (cc->flags & CLIENT_FULLSCREEN)
			cc->flags &= ~(CLIENT_FULLSCREEN | CLIENT_FREEZE);
		cc->flags &= ~CLIENT_MAXIMIZED;
		xu_ewmh_set_net_wm_state(cc);
	}

	client_apply_geom(cc);
}

static void
client_apply_geom(struct client_ctx *cc)
{
	client_draw_border(cc);

	XMoveResizeWindow(X_Dpy, cc->win, cc->geom.x,
	    cc->geom.y, cc->geom.w, cc->geom.h);
	cc->dim.w = (cc->geom.w - cc->hint.basew) / cc->hint.incw;
	cc->dim.h = (cc->geom.h - cc->hint.baseh) / cc->hint.inch;
	client_config(cc);
}

void
client_move(struct client_ctx *cc)
{
	if (cc->flags & CLIENT_IGNORE)
		return;
	XMoveWindow(X_Dpy, cc->win, cc->geom.x, cc->geom.y);
	client_config(cc);
}

static int
client_has_above(struct screen_ctx *sc)
{
	struct client_ctx	*cc;

	TAILQ_FOREACH(cc, &sc->clientq, entry) {
		if ((cc->flags & CLIENT_ABOVE) &&
		    !(cc->flags & CLIENT_HIDDEN))
			return 1;
	}
	return 0;
}

static struct client_ctx *
client_lowest_above(struct screen_ctx *sc)
{
	Window			*wins, w0, w1;
	struct client_ctx	*cc = NULL;
	unsigned int		 nwins, i;

	if (!XQueryTree(X_Dpy, sc->rootwin, &w0, &w1, &wins, &nwins))
		return NULL;

	for (i = 0; i < nwins; i++) {
		if ((cc = client_find(wins[i])) == NULL)
			continue;
		if ((cc->flags & CLIENT_ABOVE) &&
		    !(cc->flags & CLIENT_HIDDEN))
			break;
	}
	XFree(wins);

	return((i < nwins) ? cc : NULL);
}

static struct client_ctx *
client_highest_normal(struct screen_ctx *sc)
{
	Window			*wins, w0, w1;
	struct client_ctx	*cc = NULL;
	unsigned int		 nwins, i;

	if (!XQueryTree(X_Dpy, sc->rootwin, &w0, &w1, &wins, &nwins))
		return NULL;

	for (i = nwins; i > 0; i--) {
		if ((cc = client_find(wins[i - 1])) == NULL)
			continue;
		if (!(cc->flags & (CLIENT_ABOVE | CLIENT_HIDDEN)))
			break;
	}
	XFree(wins);

	return((i > 0) ? cc : NULL);
}

void
client_lower(struct client_ctx *cc)
{
	struct client_ctx	*normal;
	XWindowChanges		 wc;

	if ((cc->flags & CLIENT_ABOVE) &&
	    (normal = client_highest_normal(cc->sc)) != NULL) {
		wc.sibling = normal->win;
		wc.stack_mode = Above;
		XConfigureWindow(X_Dpy, cc->win, CWSibling | CWStackMode, &wc);
		return;
	}

	XLowerWindow(X_Dpy, cc->win);
}

void
client_raise(struct client_ctx *cc)
{
	struct client_ctx	*above;
	XWindowChanges		 wc;

	if (cc->flags & CLIENT_ABOVE) {
		XRaiseWindow(X_Dpy, cc->win);
		return;
	}

	if (client_has_above(cc->sc) &&
	    (above = client_lowest_above(cc->sc)) != NULL) {
		wc.sibling = above->win;
		wc.stack_mode = Below;
		XConfigureWindow(X_Dpy, cc->win, CWSibling | CWStackMode, &wc);
		return;
	}

	XRaiseWindow(X_Dpy, cc->win);
}

static int
client_geom_overlap(struct geom *a, struct geom *b)
{
	return(a->x < b->x + b->w && b->x < a->x + a->w &&
	    a->y < b->y + b->h && b->y < a->y + a->h);
}

static int
client_recent_popup_geom(struct screen_ctx *sc, Window win, struct geom *g)
{
	XWindowAttributes	 wa;
	unsigned int		 i;

	for (i = 0; i < nitems(sc->recent_popups); i++) {
		if (sc->recent_popups[i] == None)
			break;
		if (sc->recent_popups[i] != win)
			continue;
		/* The popup may have moved or unmapped since MapNotify. */
		if (!XGetWindowAttributes(X_Dpy, win, &wa) ||
		    !wa.override_redirect || wa.map_state != IsViewable)
			return 0;
		g->x = wa.x;
		g->y = wa.y;
		g->w = wa.width + 2 * wa.border_width;
		g->h = wa.height + 2 * wa.border_width;
		return 1;
	}
	return 0;
}

void
client_clamp_stacking(struct client_ctx *cc, unsigned long *mask,
    XWindowChanges *wc)
{
	Window			*wins, w0, w1;
	XWindowAttributes	 wa;
	struct client_ctx	*ci, *sc_ci = NULL, *low = NULL, *high = NULL;
	struct geom		 g, cg, sg;
	unsigned int		 nwins, i;
	int			 above = (cc->flags & CLIENT_ABOVE) != 0;
	int			 sib = (*mask & CWSibling) != 0;
	int			 mode, cond;
	int			 seen = 0, k = 0, p;
	int			 sk = -1, sabove = 0, svis = 0;
	int			 lowk = -1, highk = -1;
	int			 occluded = 0, occludes = 0;

	if (!(*mask & CWStackMode))
		return;
	mode = wc->stack_mode;
	cond = (mode == TopIf || mode == BottomIf || mode == Opposite);

	/* Nothing to clamp if the layer cannot change. */
	if (!above && (mode == BottomIf || (mode == Below && !sib) ||
	    !client_has_above(cc->sc)))
		return;
	if (above && (mode == TopIf || (mode == Above && !sib)))
		return;
	if (cond && (cc->flags & CLIENT_HIDDEN))
		return;

	g.x = wc->x;
	g.y = wc->y;
	g.w = wc->width + 2 * wc->border_width;
	g.h = wc->height + 2 * wc->border_width;

	if (!XQueryTree(X_Dpy, cc->sc->rootwin, &w0, &w1, &wins, &nwins))
		return;
	for (i = 0; i < nwins; i++) {
		if (wins[i] == cc->win) {
			seen = 1;
			continue;
		}
		ci = client_find(wins[i]);
		if (sib && wins[i] == wc->sibling) {
			sk = k;
			sabove = seen;
			sc_ci = ci;
		}
		if (ci != NULL && !(ci->flags & CLIENT_HIDDEN)) {
			if ((ci->flags & CLIENT_ABOVE) && low == NULL) {
				low = ci;
				lowk = k;
			}
			if (!(ci->flags & CLIENT_ABOVE)) {
				high = ci;
				highk = k;
			}
		}
		k++;

		if (!cond || sib)
			continue;
		if (seen) {
			if (occluded || mode == BottomIf)
				continue;
		} else if (occludes || mode == TopIf || !above)
			continue; /* Normal windows can pass BottomIf to X. */

		if (ci != NULL) {
			if (ci->flags & CLIENT_HIDDEN)
				continue;
			cg = ci->geom;
			cg.w += 2 * ci->bwidth;
			cg.h += 2 * ci->bwidth;
		} else if (!client_recent_popup_geom(cc->sc, wins[i], &cg))
			continue;

		if (client_geom_overlap(&g, &cg)) {
			if (seen)
				occluded = 1;
			else
				occludes = 1;
		}
	}
	XFree(wins);

	if (sib && sk == -1)
		return;

	if (cond) {
		if ((!above && low == NULL) || (above && high == NULL))
			return;
		if (sib) {
			if (sc_ci != NULL) {
				svis = !(sc_ci->flags & CLIENT_HIDDEN);
				sg.x = sc_ci->geom.x;
				sg.y = sc_ci->geom.y;
				sg.w = sc_ci->geom.w + 2 * sc_ci->bwidth;
				sg.h = sc_ci->geom.h + 2 * sc_ci->bwidth;
			} else if (XGetWindowAttributes(X_Dpy, wc->sibling,
			    &wa)) {
				svis = (wa.map_state == IsViewable);
				sg.x = wa.x;
				sg.y = wa.y;
				sg.w = wa.width + 2 * wa.border_width;
				sg.h = wa.height + 2 * wa.border_width;
			}
			if (!svis)
				return;
			if (client_geom_overlap(&g, &sg)) {
				occluded = sabove;
				occludes = !sabove;
			}
		}

		switch (mode) {
		case TopIf:
			mode = occluded ? Above : -1;
			break;
		case BottomIf:
			mode = occludes ? Below : -1;
			break;
		case Opposite:
			if (occluded)
				mode = Above;
			else if (!above) {
				wc->stack_mode = BottomIf;
				return;
			} else
				mode = occludes ? Below : -1;
			break;
		}
		if (mode == -1) {
			*mask &= ~(CWSibling | CWStackMode);
			return;
		}
		*mask &= ~CWSibling;
		wc->stack_mode = mode;
		sib = 0;
	}

	if (sib)
		p = (mode == Above) ? sk + 1 : sk;
	else
		p = (mode == Above) ? k : 0;

	if (!above && low != NULL && lowk < p) {
		wc->sibling = low->win;
		wc->stack_mode = Below;
		*mask |= CWSibling;
	} else if (above && high != NULL && highk >= p) {
		wc->sibling = high->win;
		wc->stack_mode = Above;
		*mask |= CWSibling;
	}
}

void
client_config(struct client_ctx *cc)
{
	XConfigureEvent	 cn;

	(void)memset(&cn, 0, sizeof(cn));
	cn.type = ConfigureNotify;
	cn.event = cc->win;
	cn.window = cc->win;
	cn.x = cc->geom.x;
	cn.y = cc->geom.y;
	cn.width = cc->geom.w;
	cn.height = cc->geom.h;
	cn.border_width = cc->bwidth;
	cn.above = None;
	cn.override_redirect = 0;

	XSendEvent(X_Dpy, cc->win, False, StructureNotifyMask, (XEvent *)&cn);
}

void
client_ptr_inbound(struct client_ctx *cc, int getpos)
{
	if (getpos)
		xu_ptr_get(cc->win, &cc->ptr.x, &cc->ptr.y);

	if (cc->ptr.x < 0)
		cc->ptr.x = 0;
	else if (cc->ptr.x > cc->geom.w - 1)
		cc->ptr.x = cc->geom.w - 1;
	if (cc->ptr.y < 0)
		cc->ptr.y = 0;
	else if (cc->ptr.y > cc->geom.h - 1)
		cc->ptr.y = cc->geom.h - 1;

	client_ptr_warp(cc);
}

void
client_ptr_warp(struct client_ctx *cc)
{
	xu_ptr_set(cc->win, cc->ptr.x, cc->ptr.y);
}

void
client_ptr_save(struct client_ctx *cc)
{
	int	 x, y;

	xu_ptr_get(cc->win, &x, &y);
	if (client_inbound(cc, x, y)) {
		cc->ptr.x = x;
		cc->ptr.y = y;
	} else {
		cc->ptr.x = cc->geom.w / 2;
		cc->ptr.y = cc->geom.h / 2;
	}
}

void
client_hide(struct client_ctx *cc)
{
	if (!(cc->flags & CLIENT_IGNORE))
		client_set_visibility(cc, 0, NULL);
}

void
client_map(struct client_ctx *cc)
{
	client_set_visibility(cc, 1, NULL);
}

void
client_set_visibility(struct client_ctx *cc, int visible,
    const xcb_get_property_cookie_t *cookie)
{
	int	was_hidden = cc->flags & CLIENT_HIDDEN;

	if (visible) {
		XMapWindow(X_Dpy, cc->win);
		cc->flags &= ~CLIENT_HIDDEN;
	} else {
		XUnmapWindow(X_Dpy, cc->win);
		cc->flags |= CLIENT_HIDDEN;
		if (cc->flags & CLIENT_ACTIVE) {
			cc->flags &= ~CLIENT_ACTIVE;
			xu_ewmh_net_active_window(cc->sc, None);
		}
	}

	if (cookie != NULL)
		xu_ewmh_publish_net_wm_state(cc, *cookie);
	else if (was_hidden != (cc->flags & CLIENT_HIDDEN))
		xu_ewmh_set_net_wm_state(cc);
	xu_set_wm_state(cc->win, visible ? NormalState : IconicState);
	if (visible)
		client_draw_border(cc);
}

void
client_show(struct client_ctx *cc)
{
	client_raise(cc);
	client_map(cc);
}

void
client_urgency(struct client_ctx *cc)
{
	if (!(cc->flags & CLIENT_ACTIVE))
		cc->flags |= CLIENT_URGENCY;
}

void
client_draw_border(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	unsigned long		 pixel;

	if (cc->flags & CLIENT_ACTIVE)
		switch (cc->flags & CLIENT_HIGHLIGHT) {
		case CLIENT_GROUP:
			pixel = sc->xftcolor[CWM_COLOR_BORDER_GROUP].pixel;
			break;
		case CLIENT_UNGROUP:
			pixel = sc->xftcolor[CWM_COLOR_BORDER_UNGROUP].pixel;
			break;
		default:
			pixel = sc->xftcolor[CWM_COLOR_BORDER_ACTIVE].pixel;
			break;
		}
	else
		pixel = sc->xftcolor[CWM_COLOR_BORDER_INACTIVE].pixel;

	if (cc->flags & CLIENT_URGENCY)
		pixel = sc->xftcolor[CWM_COLOR_BORDER_URGENCY].pixel;

	XSetWindowBorderWidth(X_Dpy, cc->win, (unsigned int)cc->bwidth);
	XSetWindowBorder(X_Dpy, cc->win, pixel | (0xffu << 24));
}

static void
client_class_hint(struct client_ctx *cc)
{
	XClassHint	ch;

	if (XGetClassHint(X_Dpy, cc->win, &ch)) {
		if (ch.res_class) {
			cc->res_class = xstrdup(ch.res_class);
			XFree(ch.res_class);
		}
		if (ch.res_name) {
			cc->res_name = xstrdup(ch.res_name);
			XFree(ch.res_name);
		}
	}
}

void
client_wm_protocols(struct client_ctx *cc)
{
	Atom	*p;
	int	 i, j;

	cc->flags &= ~(CLIENT_WM_DELETE_WINDOW | CLIENT_WM_TAKE_FOCUS);
	if (XGetWMProtocols(X_Dpy, cc->win, &p, &j)) {
		for (i = 0; i < j; i++) {
			if (p[i] == cwmh[WM_DELETE_WINDOW])
				cc->flags |= CLIENT_WM_DELETE_WINDOW;
			else if (p[i] == cwmh[WM_TAKE_FOCUS])
				cc->flags |= CLIENT_WM_TAKE_FOCUS;
		}
		XFree(p);
	}
}

void
client_wm_hints(struct client_ctx *cc)
{
	XWMHints	*wmh;

	cc->flags &= ~(CLIENT_INPUT | CLIENT_NOINPUT);
	if ((wmh = XGetWMHints(X_Dpy, cc->win)) != NULL) {
		if (wmh->flags & InputHint)
			cc->flags |= wmh->input ? CLIENT_INPUT : CLIENT_NOINPUT;
		if ((wmh->flags & XUrgencyHint))
			client_urgency(cc);
		if ((wmh->flags & StateHint))
			cc->initial_state = wmh->initial_state;
		XFree(wmh);
	}
}

void
client_close(struct client_ctx *cc)
{
	if (cc->flags & CLIENT_IGNORE)
		return;
	if (cc->flags & CLIENT_WM_DELETE_WINDOW)
		xu_send_clientmsg(cc->win, cwmh[WM_DELETE_WINDOW], CurrentTime);
	else
		XKillClient(X_Dpy, cc->win);
}

void
client_set_name(struct client_ctx *cc)
{
	struct winname	*wn, *wnnxt;
	int		 i = 0;

	free(cc->name);
	if (!xu_get_strprop(cc->win, ewmh[_NET_WM_NAME], &cc->name))
		if (!xu_get_strprop(cc->win, XA_WM_NAME, &cc->name))
			cc->name = xstrdup("");

	TAILQ_FOREACH_SAFE(wn, &cc->nameq, entry, wnnxt) {
		if (strcmp(wn->name, cc->name) == 0) {
			TAILQ_REMOVE(&cc->nameq, wn, entry);
			free(wn->name);
			free(wn);
		}
		i++;
	}
	wn = xmalloc(sizeof(*wn));
	wn->name = xstrdup(cc->name);
	TAILQ_INSERT_TAIL(&cc->nameq, wn, entry);

	/* Garbage collection. */
	if ((i + 1) > Conf.nameqlen) {
		wn = TAILQ_FIRST(&cc->nameq);
		TAILQ_REMOVE(&cc->nameq, wn, entry);
		free(wn->name);
		free(wn);
	}
}

static void
client_placement(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;

	/* Ignored windows keep their position; only fix edge alignment. */
	if (cc->flags & CLIENT_IGNORE) {
		if (((cc->obwidth * 2) + cc->geom.x + cc->geom.w) == sc->view.w)
			cc->geom.x += cc->obwidth * 2;
		if (((cc->obwidth * 2) + cc->geom.y + cc->geom.h) == sc->view.h)
			cc->geom.y += cc->obwidth * 2;
		return;
	}

	if (cc->hint.flags & (USPosition | PPosition)) {
		if (cc->geom.x >= sc->view.w)
			cc->geom.x = sc->view.w - cc->bwidth - 1;
		if (cc->geom.x + cc->geom.w + cc->bwidth <= 0)
			cc->geom.x = -(cc->geom.w + cc->bwidth - 1);
		if (cc->geom.y >= sc->view.h)
			cc->geom.y = sc->view.h - cc->bwidth - 1;
		if (cc->geom.y + cc->geom.h + cc->bwidth <= 0)
			cc->geom.y = -(cc->geom.h + cc->bwidth - 1);
	} else {
		struct geom	 area;
		int		 xmouse, ymouse, xslack, yslack;

		xu_ptr_get(sc->rootwin, &xmouse, &ymouse);
		area = screen_area(sc, xmouse, ymouse, 1);

		xmouse = MAX(MAX(xmouse, area.x) - cc->geom.w / 2, area.x);
		ymouse = MAX(MAX(ymouse, area.y) - cc->geom.h / 2, area.y);

		xslack = area.x + area.w - cc->geom.w - cc->bwidth * 2;
		yslack = area.y + area.h - cc->geom.h - cc->bwidth * 2;

		if (xslack >= area.x) {
			cc->geom.x = MAX(MIN(xmouse, xslack), area.x);
		} else {
			cc->geom.x = area.x;
			cc->geom.w = area.x + area.w;
		}
		if (yslack >= area.y) {
			cc->geom.y = MAX(MIN(ymouse, yslack), area.y);
		} else {
			cc->geom.y = area.y;
			cc->geom.h = area.y + area.h;
		}
	}
}

void
client_mtf(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;

	TAILQ_REMOVE(&sc->clientq, cc, entry);
	TAILQ_INSERT_HEAD(&sc->clientq, cc, entry);
}

void
client_get_sizehints(struct client_ctx *cc)
{
	long		 tmp;
	XSizeHints	 size;

	if (!XGetWMNormalHints(X_Dpy, cc->win, &size, &tmp))
		size.flags = 0;

	cc->hint.flags = size.flags;

	if (size.flags & PBaseSize) {
		cc->hint.basew = size.base_width;
		cc->hint.baseh = size.base_height;
	} else if (size.flags & PMinSize) {
		cc->hint.basew = size.min_width;
		cc->hint.baseh = size.min_height;
	}
	if (size.flags & PMinSize) {
		cc->hint.minw = size.min_width;
		cc->hint.minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		cc->hint.minw = size.base_width;
		cc->hint.minh = size.base_height;
	}
	if (size.flags & PMaxSize) {
		cc->hint.maxw = size.max_width;
		cc->hint.maxh = size.max_height;
	}
	if (size.flags & PResizeInc) {
		cc->hint.incw = size.width_inc;
		cc->hint.inch = size.height_inc;
	}
	cc->hint.incw = MAX(1, cc->hint.incw);
	cc->hint.inch = MAX(1, cc->hint.inch);
	cc->hint.minw = MAX(1, cc->hint.minw);
	cc->hint.minh = MAX(1, cc->hint.minh);

	if (size.flags & PAspect) {
		if (size.min_aspect.x > 0)
			cc->hint.mina = (float)size.min_aspect.y /
			    size.min_aspect.x;
		if (size.max_aspect.y > 0)
			cc->hint.maxa = (float)size.max_aspect.x /
			    size.max_aspect.y;
	}
}

void
client_apply_sizehints(struct client_ctx *cc)
{
	Bool		 baseismin;

	baseismin = (cc->hint.basew == cc->hint.minw) &&
	    (cc->hint.baseh == cc->hint.minh);

	/* temporarily remove base dimensions, ICCCM 4.1.2.3 */
	if (!baseismin) {
		cc->geom.w -= cc->hint.basew;
		cc->geom.h -= cc->hint.baseh;
	}

	/* adjust for aspect limits */
	if (cc->hint.mina && cc->hint.maxa &&
	    cc->geom.w > 0 && cc->geom.h > 0) {
		if (cc->hint.maxa < (float)cc->geom.w / cc->geom.h)
			cc->geom.w = cc->geom.h * cc->hint.maxa;
		else if (cc->hint.mina < (float)cc->geom.h / cc->geom.w)
			cc->geom.h = cc->geom.w * cc->hint.mina;
	}

	/* remove base dimensions for increment */
	if (baseismin) {
		cc->geom.w -= cc->hint.basew;
		cc->geom.h -= cc->hint.baseh;
	}

	/* adjust for increment value */
	cc->geom.w -= cc->geom.w % cc->hint.incw;
	cc->geom.h -= cc->geom.h % cc->hint.inch;

	/* restore base dimensions */
	cc->geom.w += cc->hint.basew;
	cc->geom.h += cc->hint.baseh;

	/* adjust for min width/height */
	cc->geom.w = MAX(cc->geom.w, cc->hint.minw);
	cc->geom.h = MAX(cc->geom.h, cc->hint.minh);

	/* adjust for max width/height */
	if (cc->hint.maxw)
		cc->geom.w = MIN(cc->geom.w, cc->hint.maxw);
	if (cc->hint.maxh)
		cc->geom.h = MIN(cc->geom.h, cc->hint.maxh);
}

static void
client_mwm_hints(struct client_ctx *cc)
{
	struct mwm_hints	*mwmh;

	if (xu_get_prop(cc->win, cwmh[_MOTIF_WM_HINTS],
	    cwmh[_MOTIF_WM_HINTS], MWM_HINTS_ELEMENTS,
	    (unsigned char **)&mwmh) <= 0)
		return;

	if ((mwmh->flags & MWM_HINTS_DECORATIONS) &&
	    !(mwmh->decorations & MWM_DECOR_ALL)) {
		if (!(mwmh->decorations & MWM_DECOR_BORDER))
			cc->bwidth = 0;
	}
	XFree(mwmh);
}

static void
client_inherit_ignore(struct client_ctx *cc, struct client_ctx *tc)
{
	int resize = cc->flags & (CLIENT_FULLSCREEN | CLIENT_MAXFLAGS);

	/* Fullscreen may have saved an already maximized geometry. */
	if (cc->flags & CLIENT_FULLSCREEN)
		cc->geom = cc->fullgeom;
	if (cc->flags & CLIENT_HMAXIMIZED) {
		cc->geom.x = cc->savegeom.x;
		cc->geom.w = cc->savegeom.w;
	}
	if (cc->flags & CLIENT_VMAXIMIZED) {
		cc->geom.y = cc->savegeom.y;
		cc->geom.h = cc->savegeom.h;
	}
	cc->flags &= ~(CLIENT_FULLSCREEN | CLIENT_MAXFLAGS | CLIENT_STICKY);
	cc->flags |= CLIENT_IGNORE | CLIENT_FREEZE;
	cc->bwidth = tc->bwidth;
	if (resize)
		client_apply_geom(cc);
}

static void
client_read_transient(struct client_ctx *cc)
{
	struct client_ctx	*tc;

	if (!XGetTransientForHint(X_Dpy, cc->win, &cc->transient_for)) {
		cc->transient_for = None;
		return;
	}
	if ((tc = client_find(cc->transient_for)) != NULL) {
		if (tc->flags & CLIENT_IGNORE)
			client_inherit_ignore(cc, tc);
		if (tc->flags & CLIENT_ABOVE)
			cc->flags |= CLIENT_ABOVE;
	}
}

static void
client_sync_ignore(struct client_ctx *cc)
{
	if (cc->gc)
		group_assign(NULL, cc);
	if (cc->flags & CLIENT_HIDDEN)
		client_show(cc);
	else
		client_draw_border(cc);
	xu_ewmh_set_net_wm_state(cc);
}

/* A transient may already be managed when its parent first becomes ignored. */
static void
client_ignore_transients(struct client_ctx *tc)
{
	SLIST_HEAD(, client_ctx) pending = SLIST_HEAD_INITIALIZER(pending);
	struct client_ctx	*cc;

	SLIST_INSERT_HEAD(&pending, tc, ignore_entry);
	while ((tc = SLIST_FIRST(&pending)) != NULL) {
		SLIST_REMOVE_HEAD(&pending, ignore_entry);
		TAILQ_FOREACH(cc, &tc->sc->clientq, entry) {
			if (cc->transient_for != tc->win ||
			    (cc->flags & CLIENT_IGNORE))
				continue;
			client_inherit_ignore(cc, tc);
			client_sync_ignore(cc);
			SLIST_INSERT_HEAD(&pending, cc, ignore_entry);
		}
	}
}

void
client_transient(struct client_ctx *cc)
{
	int	 oflags = cc->flags;

	client_read_transient(cc);
	if ((cc->flags & CLIENT_ABOVE) && !(oflags & CLIENT_ABOVE)) {
		if (!(cc->flags & CLIENT_HIDDEN))
			client_raise(cc);
		xu_ewmh_set_net_wm_state(cc);
	}
	if ((cc->flags & CLIENT_IGNORE) && !(oflags & CLIENT_IGNORE)) {
		client_sync_ignore(cc);
		client_ignore_transients(cc);
	} else
		client_draw_border(cc);
}

int
client_inbound(struct client_ctx *cc, int x, int y)
{
	return(x < cc->geom.w && x >= 0 &&
	    y < cc->geom.h && y >= 0);
}

int
client_snapcalc(int n0, int n1, int e0, int e1, int snapdist)
{
	int	 s0, s1;

	s0 = s1 = 0;

	if (abs(e0 - n0) <= snapdist)
		s0 = e0 - n0;

	if (abs(e1 - n1) <= snapdist)
		s1 = e1 - n1;

	/* possible to snap in both directions */
	if (s0 != 0 && s1 != 0)
		if (abs(s0) < abs(s1))
			return s0;
		else
			return s1;
	else if (s0 != 0)
		return s0;
	else if (s1 != 0)
		return s1;
	else
		return 0;
}

static int
client_tile_member(struct client_ctx *ci, struct client_ctx *master,
    struct geom *area)
{
	struct geom	region;

	if (ci == master || ci->gc != master->gc ||
	    (ci->flags & (CLIENT_HIDDEN | CLIENT_IGNORE)))
		return 0;
	/* A fullscreen window starts outside the work area if there is a gap. */
	if (ci->flags & CLIENT_FULLSCREEN) {
		region = screen_area(ci->sc, ci->geom.x + ci->geom.w / 2,
		    ci->geom.y + ci->geom.h / 2, 1);
		return region.x == area->x && region.y == area->y;
	}
	return ci->geom.x >= area->x && ci->geom.x <= area->x + area->w &&
	    ci->geom.y >= area->y && ci->geom.y <= area->y + area->h;
}

static void
client_tile(struct client_ctx *cc, int vertical)
{
	struct tile {
		struct client_ctx	*cc;
		struct geom		 geom;
	}			*tiles;
	struct client_ctx	*ci;
	struct screen_ctx	*sc = cc->sc;
	struct geom		 area, master, peer;
	int			 i, n = 0, pct, size, step, border;

	if (cc->flags & CLIENT_IGNORE)
		return;

	area = screen_area(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2, 1);
	if (2LL * cc->sbwidth >= MIN(area.w, area.h))
		return;

	/* Size the master in content pixels, then split the outer rectangles. */
	border = 2 * cc->sbwidth;
	size = (vertical ? area.w : area.h) - border;
	pct = vertical ? Conf.vtile : Conf.htile;
	if (pct > 0)
		size = size * pct / 100;
	else {
		master = (cc->flags & CLIENT_FULLSCREEN) ? cc->fullgeom : cc->geom;
		size = vertical ? master.w : master.h;
	}
	if (size <= 0 || size >= (vertical ? area.w : area.h) - border)
		return;

	TAILQ_FOREACH(ci, &sc->clientq, entry) {
		if (client_tile_member(ci, cc, &area))
			n++;
	}
	if (n == 0)
		return;

	master = peer = area;
	if (vertical) {
		master.w = size + border;
		peer.x += master.w;
		peer.w -= master.w;
		step = area.h / n;
	} else {
		master.h = size + border;
		peer.y += master.h;
		peer.h -= master.h;
		step = area.w / n;
	}

	/* Check every slot before resizing or clearing any window's state. */
	tiles = xreallocarray(NULL, n + 1, sizeof(*tiles));
	tiles[0].cc = cc;
	tiles[0].geom = master;
	i = 0;
	TAILQ_FOREACH(ci, &sc->clientq, entry) {
		if (!client_tile_member(ci, cc, &area))
			continue;
		tiles[++i].cc = ci;
		if (vertical) {
			peer.h = step + (i == n ? area.h % n : 0);
			tiles[i].geom = peer;
			peer.y += peer.h;
		} else {
			peer.w = step + (i == n ? area.w % n : 0);
			tiles[i].geom = peer;
			peer.x += peer.w;
		}
		if (2LL * ci->sbwidth >= MIN(peer.w, peer.h))
			goto out;
	}
	for (i = 0; i <= n; i++) {
		ci = tiles[i].cc;
		ci->geom = tiles[i].geom;
		ci->geom.w -= 2 * ci->sbwidth;
		ci->geom.h -= 2 * ci->sbwidth;
		client_resize(ci, 1);
		if (ci == cc)
			client_ptr_warp(cc);
	}
out:
	free(tiles);
}

void
client_htile(struct client_ctx *cc)
{
	client_tile(cc, 0);
}

void
client_vtile(struct client_ctx *cc)
{
	client_tile(cc, 1);
}
