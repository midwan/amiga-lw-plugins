#ifndef PANEL_LAYOUT_H
#define PANEL_LAYOUT_H

/*
 * Line up LWPanels controls.
 *
 * LWPanels stacks controls in one column and left-aligns their labels, so
 * every field, slider and popup starts at a different x. These helpers
 * move each control so its labels end right-aligned and every control in a
 * column starts at the same x (CTL_HOTX), then size the panel to fit.
 * Check boxes carry their label on the right, so they start at that x too.
 *
 * Call after all controls exist and before opening the panel. The CON_*
 * and PAN_* macros all go through the caller's shared ival; these use
 * their own value, so they can't clobber a value the caller holds.
 *
 * Include after <lwpanel.h>, which has no include guard.
 */

#ifdef __GNUC__
#define PL_UNUSED __attribute__((unused))
#else
#define PL_UNUSED
#endif

#define PL_ROW_GAP    8
#define PL_COLUMN_GAP 16

static int PL_UNUSED
pl_get(LWControl *ctl, cTag tag)
{
	LWValue v = { LWT_INTEGER };
	(*ctl->get)(ctl, tag, &v);
	return v.intv.value;
}

static void PL_UNUSED
pl_move(LWControl *ctl, int x, int y)
{
	LWValue v = { LWT_INTEGER };
	v.intv.value = x;
	(*ctl->set)(ctl, CTL_X, &v);
	v.intv.value = y;
	(*ctl->set)(ctl, CTL_Y, &v);
}

/*
 * Place count auto-placed controls as one column with its left edge at x
 * and its rows moved by dy. sameRow (may be NULL) marks controls that go
 * to the right of the previous one; such a row starts at x, and the rows
 * below move up by the rows freed.
 */
static void PL_UNUSED
pl_place_column(LWControl **con, const unsigned char *sameRow, int count,
                int x, int dy)
{
	int i, label, hot = 0, lift = 0, prevAuto = 0;

	for (i = 0; i < count; i++) {
		if (sameRow && sameRow[i]) continue;
		label = pl_get(con[i], CTL_HOTX) - pl_get(con[i], CTL_X);
		if (label > hot) hot = label;
	}
	for (i = 0; i < count; i++) {
		int autoY = pl_get(con[i], CTL_Y);
		if (sameRow && sameRow[i] && i > 0) {
			pl_move(con[i],
				pl_get(con[i - 1], CTL_X) + pl_get(con[i - 1], CTL_W) + PL_ROW_GAP,
				pl_get(con[i - 1], CTL_Y));
			lift += autoY - prevAuto;
		} else if (sameRow && i + 1 < count && sameRow[i + 1]) {
			pl_move(con[i], x, autoY - lift + dy);
		} else {
			label = pl_get(con[i], CTL_HOTX) - pl_get(con[i], CTL_X);
			pl_move(con[i], x + hot - label, autoY - lift + dy);
		}
		prevAuto = autoY;
	}
}

static int PL_UNUSED
pl_panel_get(LWPanelFuncs *panl, LWPanelID pan, pTag tag)
{
	int v = 0;
	(*panl->get)(pan, tag, &v);
	return v;
}

static void PL_UNUSED
pl_panel_set(LWPanelFuncs *panl, LWPanelID pan, pTag tag, int v)
{
	(*panl->set)(pan, tag, &v);
}

/*
 * Lay out con[0..count) as two columns: the first leftCount controls on
 * the left, the rest on the right starting level with the first one.
 * Sizes the panel to the columns, keeping the space LWPanels leaves under
 * the controls for its OK/Cancel row. With leftCount == count it gives one
 * column.
 */
static void PL_UNUSED
pl_columns(LWPanelFuncs *panl, LWPanelID pan, LWControl **con,
           const unsigned char *sameRow, int count, int leftCount)
{
	int i, edge, autoBottom = 0, bottom = 0, right = 0, margin, dy = 0;

	for (i = 0; i < count; i++) {
		edge = pl_get(con[i], CTL_Y) + pl_get(con[i], CTL_H);
		if (edge > autoBottom) autoBottom = edge;
	}
	autoBottom = pl_panel_get(panl, pan, PAN_H) - autoBottom;
	margin = pl_get(con[0], CTL_X);
	if (leftCount < count)
		dy = pl_get(con[0], CTL_Y) - pl_get(con[leftCount], CTL_Y);

	pl_place_column(con, sameRow, leftCount, margin, 0);
	for (i = 0; i < leftCount; i++) {
		edge = pl_get(con[i], CTL_X) + pl_get(con[i], CTL_W);
		if (edge > right) right = edge;
	}
	if (leftCount < count)
		pl_place_column(con + leftCount, sameRow ? sameRow + leftCount : 0,
			count - leftCount, right + PL_COLUMN_GAP, dy);

	for (i = 0; i < count; i++) {
		edge = pl_get(con[i], CTL_X) + pl_get(con[i], CTL_W);
		if (edge > right) right = edge;
		edge = pl_get(con[i], CTL_Y) + pl_get(con[i], CTL_H);
		if (edge > bottom) bottom = edge;
	}
	pl_panel_set(panl, pan, PAN_W, right + margin);
	pl_panel_set(panl, pan, PAN_H, bottom + autoBottom);
}

#endif
