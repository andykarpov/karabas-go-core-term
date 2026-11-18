/* keyboard of the term core: the HID boot report of the MCU (IO_KBD0 / IO_KBD1) -> libvterm keys */
#pragma once
#include "vterm.h"

void kbd_init(VTerm *vt);
void kbd_poll(VTerm *vt);      /* new presses and the key repeat; call often */
int kbd_layout(void);          /* 0 US, 1 RU (Alt+Shift toggles) */
