/*
 * calmwm - the calm window manager
 *
 * Copyright (c) 2004 Andy Adamson <dros@monkey.org>
 * Copyright (c) 2004,2005 Marius Aamodt Eriksen <marius@monkey.org>
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

static void		 group_insert_sorted(struct client_ctx **, int *,
			     struct client_ctx *);
static struct group_ctx	*group_next(struct group_ctx *);
static struct group_ctx	*group_prev(struct group_ctx *);
static void		 group_restack(struct group_ctx *);
static int		 group_restackable(struct group_ctx *,
			     struct client_ctx *);
static void		 group_set_active(struct group_ctx *);

void
group_assign(struct group_ctx *gc, struct client_ctx *cc)
{
	if ((gc != NULL) && (gc->num == 0))
		gc = NULL;

	/* Ignored windows cannot be hidden, so they live on every group. */
	if (cc->flags & CLIENT_IGNORE)
		gc = NULL;

	cc->gc = gc;

	xu_ewmh_set_net_wm_desktop(cc);
}

static void
group_set_visibility(struct group_ctx *gc, int visible)
{
	struct {
		struct client_ctx		*cc;
		xcb_get_property_cookie_t	 cookie;
	} batch[64];
	struct client_ctx	*cc;
	size_t			 i, n;
	int			 hidden = visible ? 0 : CLIENT_HIDDEN;

	/* Send the reads together; consume every reply within this group action. */
	cc = TAILQ_FIRST(&gc->sc->clientq);
	while (cc != NULL) {
		for (n = 0; cc != NULL && n < nitems(batch);
		    cc = TAILQ_NEXT(cc, entry)) {
			if (cc->gc != gc ||
			    (cc->flags & (CLIENT_STICKY | CLIENT_IGNORE)) ||
			    (cc->flags & CLIENT_HIDDEN) == hidden)
				continue;
			batch[n].cc = cc;
			batch[n].cookie = xu_ewmh_request_net_wm_state(cc);
			n++;
		}
		for (i = 0; i < n; i++)
			client_set_visibility(batch[i].cc, visible,
			    &batch[i].cookie);
	}
}

void
group_hide(struct group_ctx *gc)
{
	screen_updatestackingorder(gc->sc);
	group_set_visibility(gc, 0);
}

void
group_show(struct group_ctx *gc)
{
	group_restack(gc);
	group_set_visibility(gc, 1);
	group_set_active(gc);
}

static void
group_insert_sorted(struct client_ctx **list, int *n, struct client_ctx *cc)
{
	int	 j;

	for (j = *n; j > 0; j--) {
		if (list[j - 1]->stackingorder >= cc->stackingorder)
			break;
		list[j] = list[j - 1];
	}
	list[j] = cc;
	(*n)++;
}

static int
group_restackable(struct group_ctx *gc, struct client_ctx *cc)
{
	/* Hidden sticky windows will not be mapped by group_show(). */
	return(cc->gc == gc && (!(cc->flags & CLIENT_HIDDEN) ||
	    !(cc->flags & CLIENT_STICKY)));
}

static void
group_restack(struct group_ctx *gc)
{
	struct screen_ctx	*sc = gc->sc;
	struct client_ctx	*cc, **cclist;
	Window			*wins, *winlist, w0, w1;
	Window			 lowest_above = None;
	Window			 highest_above = None;
	XWindowChanges		 wc;
	unsigned int		 ntree, i;
	int			 j, n = 0, nabove = 0, na = 0, nn = 0;
	int			 seen_top = 0, top_below = 0;

	TAILQ_FOREACH(cc, &sc->clientq, entry) {
		if (!group_restackable(gc, cc))
			continue;
		n++;
		if (cc->flags & CLIENT_ABOVE)
			nabove++;
	}
	if (n == 0)
		return;

	cclist = xreallocarray(NULL, n, sizeof(*cclist));
	winlist = xreallocarray(NULL, n, sizeof(*winlist));

	/* Above windows first, then the normal ones, each top to bottom. */
	TAILQ_FOREACH(cc, &sc->clientq, entry) {
		if (!group_restackable(gc, cc))
			continue;
		if (cc->flags & CLIENT_ABOVE)
			group_insert_sorted(cclist, &na, cc);
		else
			group_insert_sorted(cclist + nabove, &nn, cc);
	}
	for (j = 0; j < n; j++)
		winlist[j] = cclist[j]->win;

	/* Use other groups' visible above windows only as anchors. */
	if (XQueryTree(X_Dpy, sc->rootwin, &w0, &w1, &wins, &ntree)) {
		for (i = 0; i < ntree; i++) {
			if (na > 0 && wins[i] == winlist[0])
				seen_top = 1;
			if ((cc = client_find(wins[i])) == NULL ||
			    cc->gc == gc || !(cc->flags & CLIENT_ABOVE) ||
			    (cc->flags & CLIENT_HIDDEN))
				continue;
			if (lowest_above == None)
				lowest_above = cc->win;
			if (na == 0)
				break;
			highest_above = cc->win;
			if (seen_top)
				top_below = 1;
		}
		XFree(wins);
	}

	if (na > 0) {
		if (top_below) {
			wc.sibling = highest_above;
			wc.stack_mode = Above;
			XConfigureWindow(X_Dpy, winlist[0],
			    CWSibling | CWStackMode, &wc);
		} else if (highest_above == None &&
		    (cclist[0]->flags & CLIENT_HIDDEN))
			XRaiseWindow(X_Dpy, winlist[0]);
		if (na > 1)
			XRestackWindows(X_Dpy, winlist, na);
	}

	if (nn > 0) {
		/* A visible normal window is already below every above one. */
		if (cclist[na]->flags & CLIENT_HIDDEN) {
			if (lowest_above == None && na > 0)
				lowest_above = winlist[na - 1];
			if (lowest_above != None) {
				wc.sibling = lowest_above;
				wc.stack_mode = Below;
				XConfigureWindow(X_Dpy, winlist[na],
				    CWSibling | CWStackMode, &wc);
			} else
				XRaiseWindow(X_Dpy, winlist[na]);
		}
		if (nn > 1)
			XRestackWindows(X_Dpy, winlist + na, nn);
	}

	free(cclist);
	free(winlist);
}

void
group_init(struct screen_ctx *sc, int num, const char *name)
{
	struct group_ctx	*gc;

	gc = xmalloc(sizeof(*gc));
	gc->sc = sc;
	gc->name = xstrdup(name);
	gc->num = num;
	TAILQ_INSERT_TAIL(&sc->groupq, gc, entry);

	if (num == 1)
		group_set_active(gc);
}

void
group_set_active(struct group_ctx *gc)
{
	struct screen_ctx	*sc = gc->sc;

	sc->group_active = gc;

	xu_ewmh_net_current_desktop(sc);
}

void
group_movetogroup(struct client_ctx *cc, int idx)
{
	struct screen_ctx	*sc = cc->sc;
	struct group_ctx	*gc;

	if (cc->flags & CLIENT_IGNORE) {
		group_assign(NULL, cc);
		return;
	}

	TAILQ_FOREACH(gc, &sc->groupq, entry) {
		if (gc->num == idx) {
			if (cc->gc == gc)
				return;
			if (gc->num != 0 && group_holds_only_hidden(gc))
				client_hide(cc);
			group_assign(gc, cc);
		}
	}
}

void
group_toggle_membership(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct group_ctx	*gc = sc->group_active;

	if (cc->flags & CLIENT_IGNORE)
		return;

	if (cc->gc == gc) {
		group_assign(NULL, cc);
		cc->flags |= CLIENT_UNGROUP;
	} else {
		group_assign(gc, cc);
		cc->flags |= CLIENT_GROUP;
	}
	client_draw_border(cc);
}

int
group_holds_only_sticky(struct group_ctx *gc)
{
	struct screen_ctx	*sc = gc->sc;
	struct client_ctx	*cc;

	TAILQ_FOREACH(cc, &sc->clientq, entry) {
		if (cc->gc != gc)
			continue;
		if (!(cc->flags & CLIENT_STICKY))
			return 0;
	}
	return 1;
}

int
group_holds_only_hidden(struct group_ctx *gc)
{
	struct screen_ctx	*sc = gc->sc;
	struct client_ctx	*cc;

	TAILQ_FOREACH(cc, &sc->clientq, entry) {
		if (cc->gc != gc)
			continue;
		if (!(cc->flags & (CLIENT_HIDDEN | CLIENT_STICKY)))
			return 0;
	}
	return 1;
}

void
group_only(struct screen_ctx *sc, int idx)
{
	struct group_ctx	*gc;

	if (sc->group_last != sc->group_active)
		sc->group_last = sc->group_active;

	TAILQ_FOREACH(gc, &sc->groupq, entry) {
		if (gc->num == idx)
			group_show(gc);
		else
			group_hide(gc);
	}
}

void
group_toggle(struct screen_ctx *sc, int idx)
{
	struct group_ctx	*gc;

	TAILQ_FOREACH(gc, &sc->groupq, entry) {
		if (gc->num == idx) {
			if (group_holds_only_hidden(gc))
				group_show(gc);
			else
				group_hide(gc);
		}
	}
}

void
group_toggle_all(struct screen_ctx *sc)
{
	struct group_ctx	*gc;

	TAILQ_FOREACH(gc, &sc->groupq, entry) {
		if (sc->hideall)
			group_show(gc);
		else
			group_hide(gc);
	}
	sc->hideall = !sc->hideall;
}

void
group_close(struct screen_ctx *sc, int idx)
{
	struct group_ctx	*gc;
	struct client_ctx	*cc;

	TAILQ_FOREACH(gc, &sc->groupq, entry) {
		if (gc->num == idx) {
			TAILQ_FOREACH(cc, &sc->clientq, entry) {
				if (cc->gc != gc)
					continue;
				client_close(cc);
			}
		}
	}
}

void
group_cycle(struct screen_ctx *sc, int flags)
{
	struct group_ctx	*newgc, *oldgc, *showgroup = NULL;

	oldgc = sc->group_active;

	newgc = oldgc;
	for (;;) {
		newgc = (flags & CWM_CYCLE_REVERSE) ? group_prev(newgc) :
		    group_next(newgc);

		if (newgc == oldgc)
			break;

		if (!group_holds_only_sticky(newgc) && showgroup == NULL)
			showgroup = newgc;
		else if (!group_holds_only_hidden(newgc))
			group_hide(newgc);
	}
	if (showgroup == NULL)
		return;

	group_hide(oldgc);

	if (group_holds_only_hidden(showgroup))
		group_show(showgroup);
	else
		group_set_active(showgroup);
}

static struct group_ctx *
group_next(struct group_ctx *gc)
{
	struct screen_ctx	*sc = gc->sc;
	struct group_ctx	*newgc;

	return(((newgc = TAILQ_NEXT(gc, entry)) != NULL) ?
	    newgc : TAILQ_FIRST(&sc->groupq));
}

static struct group_ctx *
group_prev(struct group_ctx *gc)
{
	struct screen_ctx	*sc = gc->sc;
	struct group_ctx	*newgc;

	return(((newgc = TAILQ_PREV(gc, group_q, entry)) != NULL) ?
	    newgc : TAILQ_LAST(&sc->groupq, group_q));
}

int
group_restore(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct group_ctx	*gc;
	int			 num;
	long			 grpnum;

	if (!xu_ewmh_get_net_wm_desktop(cc, &grpnum))
		return 0;

	num = (grpnum == -1) ? 0 : grpnum;
	num = MIN(num, (Conf.ngroups - 1));

	TAILQ_FOREACH(gc, &sc->groupq, entry) {
		if (gc->num == num) {
			group_assign(gc, cc);
			return 1;
		}
	}
	return 0;
}

int
group_autogroup(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct autogroup	*ag;
	struct group_ctx	*gc;
	int			 num = -1, both_match = 0;

	if (cc->res_class == NULL || cc->res_name == NULL)
		return 0;

	TAILQ_FOREACH(ag, &Conf.autogroupq, entry) {
		if (strcmp(ag->class, cc->res_class) == 0) {
			if ((ag->name != NULL) &&
			    (strcmp(ag->name, cc->res_name) == 0)) {
				num = ag->num;
				both_match = 1;
			} else if (ag->name == NULL && !both_match)
				num = ag->num;
		}
	}

	TAILQ_FOREACH(gc, &sc->groupq, entry) {
		if (gc->num == num) {
			group_assign(gc, cc);
			return 1;
		}
	}
	return 0;
}
