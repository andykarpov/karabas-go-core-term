/* term screen layer on VTermState (see tscreen.h). */
#include <string.h>
#include "tscreen.h"

static uint16_t pack_color(const VTermColor *c, int is_bg) {
    if ((is_bg && VTERM_COLOR_IS_DEFAULT_BG(c)) || (!is_bg && VTERM_COLOR_IS_DEFAULT_FG(c))) return 0x100;
    if (VTERM_COLOR_IS_INDEXED(c)) return c->indexed.idx;
    return 0x8000 | ((c->rgb.red >> 3) << 10) | ((c->rgb.green >> 3) << 5) | (c->rgb.blue >> 3);
}

/* word copy of cells (newlib-nano memmove is bytewise) */
static void cells_move(ts_cell *d, const ts_cell *s, int n) {
    if (d < s) for (int i = 0; i < n; i++) d[i] = s[i];
    else for (int i = n - 1; i >= 0; i--) d[i] = s[i];
}

static void erase_cells(tscreen *s, int row, int c0, int c1) {
    ts_cell *c = tscreen_cell(s, row, 0);
    uint32_t w1 = s->pen_col;
    for (int x = c0; x < c1; x++) { c[x].w0 = 0; c[x].w1 = w1; }
}

static int cb_putglyph(VTermGlyphInfo *info, VTermPos pos, void *user) {
    tscreen *s = user;
    ts_cell *c = tscreen_cell(s, pos.row, pos.col);
    c->w0 = (info->chars[0] & 0x1fffff) | (info->width == 2 ? 1u << 21 : 0) | s->pen_attr;
    c->w1 = s->pen_col;
    if (info->width == 2 && pos.col + 1 < TS_COLS) { c[1].w0 = TS_CONT; c[1].w1 = 0; }
    s->n_put++;
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
    tscreen *s = user;
    (void)oldpos;
    s->cursor = pos;
    (void)visible;                 /* the visibility comes with VTERM_PROP_CURSORVISIBLE */
    return 1;
}

static int cb_erase(VTermRect r, int selective, void *user) {
    tscreen *s = user;
    (void)selective;
    for (int y = r.start_row; y < r.end_row && y < TS_ROWS; y++) erase_cells(s, y, r.start_col, r.end_col);
    s->n_erase++;
    return 1;
}

static int cb_moverect(VTermRect d, VTermRect src, void *user) {
    tscreen *s = user;
    int w = src.end_col - src.start_col, h = src.end_row - src.start_row;
    if (d.start_row < src.start_row || (d.start_row == src.start_row && d.start_col < src.start_col))
        for (int y = 0; y < h; y++)
            cells_move(tscreen_cell(s, d.start_row + y, d.start_col), tscreen_cell(s, src.start_row + y, src.start_col), w);
    else
        for (int y = h - 1; y >= 0; y--)
            cells_move(tscreen_cell(s, d.start_row + y, d.start_col), tscreen_cell(s, src.start_row + y, src.start_col), w);
    s->n_move++;
    return 1;
}

static int cb_scrollrect(VTermRect r, int down, int right, void *user) {
    tscreen *s = user;
    s->n_scroll++;
    if (right || r.start_col != 0 || r.end_col != TS_COLS || down == 0)
        return 0; /* libvterm falls back to moverect + erase */
    int h = r.end_row - r.start_row;
    int n = down > 0 ? down : -down;
    if (n > h) n = h;
    if (!s->altscreen && down > 0 && r.start_row == 0 && r.end_row == TS_ROWS) {
        /* the whole primary screen up: advance the ring, the top rows become scrollback */
        for (int k = 0; k < n; k++) {
            uint16_t nr = (uint16_t)((s->row[TS_ROWS - 1] + 1) % TS_RING);
            memmove(&s->row[0], &s->row[1], (TS_ROWS - 1) * sizeof s->row[0]);
            s->row[TS_ROWS - 1] = nr;
            erase_cells(s, TS_ROWS - 1, 0, TS_COLS);
            s->top++;
        }
        s->n_rot++;
        return 1;
    }
    /* a region: rotate its row table entries, erase the rows that came in */
    uint16_t tmp[TS_ROWS];
    for (int y = 0; y < h; y++) tmp[y] = s->row[r.start_row + (down > 0 ? (y + n) % h : (y - n + h) % h)];
    memcpy(&s->row[r.start_row], tmp, h * sizeof tmp[0]);
    if (down > 0) for (int y = r.end_row - n; y < r.end_row; y++) erase_cells(s, y, 0, TS_COLS);
    else for (int y = r.start_row; y < r.start_row + n; y++) erase_cells(s, y, 0, TS_COLS);
    s->n_rot++;
    return 1;
}

static void repack_pen(tscreen *s, VTermState *st);
static VTermState *g_state;

static int cb_initpen(void *user) { repack_pen(user, g_state); return 1; }
static int cb_setpenattr(VTermAttr a, VTermValue *v, void *user) { (void)a; (void)v; repack_pen(user, g_state); return 1; }

static int cb_settermprop(VTermProp p, VTermValue *v, void *user) {
    tscreen *s = user;
    if (p == VTERM_PROP_ALTSCREEN) {
        if (v->boolean && !s->altscreen) {
            memcpy(s->prim_row, s->row, sizeof s->row);
            for (int y = 0; y < TS_ROWS; y++) s->row[y] = (uint16_t)(TS_RING + y);
        } else if (!v->boolean && s->altscreen) {
            memcpy(s->row, s->prim_row, sizeof s->row);
        }
        s->altscreen = v->boolean;
    } else if (p == VTERM_PROP_CURSORVISIBLE) s->cursor_visible = v->boolean;
    else if (p == VTERM_PROP_CURSORBLINK) s->cursor_blink = v->boolean;
    else if (p == VTERM_PROP_CURSORSHAPE) s->cursor_shape = v->number;
    return 1;
}

static void repack_pen(tscreen *s, VTermState *st) {
    VTermValue v;
    uint32_t a = 0;
    vterm_state_get_penattr(st, VTERM_ATTR_BOLD, &v); a |= v.boolean ? 1 : 0;
    vterm_state_get_penattr(st, VTERM_ATTR_UNDERLINE, &v); a |= (v.number & 3) << 1;
    vterm_state_get_penattr(st, VTERM_ATTR_ITALIC, &v); a |= v.boolean ? 8 : 0;
    vterm_state_get_penattr(st, VTERM_ATTR_BLINK, &v); a |= v.boolean ? 16 : 0;
    vterm_state_get_penattr(st, VTERM_ATTR_REVERSE, &v); a |= v.boolean ? 32 : 0;
    vterm_state_get_penattr(st, VTERM_ATTR_CONCEAL, &v); a |= v.boolean ? 64 : 0;
    vterm_state_get_penattr(st, VTERM_ATTR_STRIKE, &v); a |= v.boolean ? 128 : 0;
    s->pen_attr = a << 22;
    VTermValue fg, bg;
    vterm_state_get_penattr(st, VTERM_ATTR_FOREGROUND, &fg);
    vterm_state_get_penattr(st, VTERM_ATTR_BACKGROUND, &bg);
    s->pen_col = pack_color(&fg.color, 0) | (uint32_t)pack_color(&bg.color, 1) << 16;
}

static const VTermStateCallbacks cbs = {
    .putglyph = cb_putglyph, .movecursor = cb_movecursor, .scrollrect = cb_scrollrect, .moverect = cb_moverect,
    .erase = cb_erase, .initpen = cb_initpen, .setpenattr = cb_setpenattr, .settermprop = cb_settermprop,
};

void tscreen_init(tscreen *s, VTermState *st, ts_cell *ring, ts_cell *alt) {
    memset(s, 0, sizeof *s);
    s->ring = ring;
    s->alt = alt;
    g_state = st;
    s->cursor_visible = 1;
    s->cursor_blink = 1;
    s->cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
    for (int y = 0; y < TS_ROWS; y++) s->row[y] = (uint16_t)y;
    vterm_state_set_callbacks(st, &cbs, s);
    vterm_state_reset(st, 1);
    repack_pen(s, st);
    for (int y = 0; y < TS_ROWS; y++) erase_cells(s, y, 0, TS_COLS);
    memset(alt, 0, sizeof(ts_cell) * TS_ROWS * TS_COLS);
}
