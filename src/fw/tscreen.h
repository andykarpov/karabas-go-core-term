/* term screen layer on the libvterm state layer (VTermState): compact 8-byte cells in rows reached
   through a row table, the layout the RTL renderer reads (docs/PLAN.md "Экранный слой").
   - primary screen: a ring of TS_RING physical rows; a full-screen scroll up only advances the ring
     (the rows that leave the screen stay in the ring = scrollback, no copy);
   - scroll regions of full width: rotate the row table entries of the region (no cell copy);
   - partial width: memmove of the cells;
   - alternate screen: its own TS_ROWS rows, no scrollback. */
#pragma once
#include <stdint.h>
#include "vterm.h"

#ifndef TS_ROWS
#define TS_ROWS 48
#endif
#ifndef TS_COLS
#define TS_COLS 128
#endif
#ifndef TS_RING
#define TS_RING 1024          /* physical rows of the primary ring (screen + scrollback) */
#endif

/* w0 = codepoint (21 bits, 0x1fffff = right half of a wide glyph) | wide (bit 21) | attrs (bits 22..31:
   bold, underline 2 bits, italic, blink, reverse, conceal, strike);
   w1 = fg (low 16) | bg (high 16): bit 15 = RGB555, 0x100 = default, else palette index */
typedef struct { uint32_t w0, w1; } ts_cell;

#define TS_CONT 0x1fffffu

typedef struct {
    VTermState *state;
    ts_cell *ring;                 /* TS_RING x TS_COLS */
    ts_cell *alt;                  /* TS_ROWS x TS_COLS */
    uint16_t row[TS_ROWS];         /* screen row -> physical row (ring index, or TS_RING + alt row) */
    uint16_t prim_row[TS_ROWS];    /* the primary table, kept while the alternate screen is shown */
    uint32_t top;                  /* ring rows pushed off the top (scrollback count) */
    int altscreen;
    uint32_t pen_attr, pen_col;    /* the current pen, packed */
    VTermPos cursor;
    int cursor_visible;            /* movecursor / VTERM_PROP_CURSORVISIBLE */
    int cursor_blink;              /* VTERM_PROP_CURSORBLINK */
    int cursor_shape;              /* VTERM_PROP_CURSORSHAPE: 1 block, 2 underline, 3 bar (left) */
    uint32_t bells;                /* BEL received (a counter) */
    int mouse;                     /* VTERM_PROP_MOUSE: 0 off, else the mouse tracking mode of the application */
    uint32_t n_put, n_scroll, n_rot, n_move, n_erase;
} tscreen;

void tscreen_init(tscreen *s, VTermState *st, ts_cell *ring, ts_cell *alt);
static inline ts_cell *tscreen_cell(tscreen *s, int row, int col) {
    uint32_t p = s->row[row];
    return p >= TS_RING ? &s->alt[(p - TS_RING) * TS_COLS + col] : &s->ring[p * TS_COLS + col];
}
