/* keyboard of the term core (kbd.h): HID boot report (modifiers + 6 key codes) -> libvterm.
   New key codes in a report are presses; the last one repeats (500 ms, then every 33 ms) while held.
   Layouts: US and RU (ЙЦУКЕН on the same keys), Alt+Shift toggles; Caps Lock for the letters. */
#include <stdint.h>
#include "kbd.h"
#include "mmio_hw.h"

/* US: HID usage 0x04..0x38 -> { normal, shift } */
static const char us[0x35][2] = {
    {'a','A'},{'b','B'},{'c','C'},{'d','D'},{'e','E'},{'f','F'},{'g','G'},{'h','H'},{'i','I'},{'j','J'},
    {'k','K'},{'l','L'},{'m','M'},{'n','N'},{'o','O'},{'p','P'},{'q','Q'},{'r','R'},{'s','S'},{'t','T'},
    {'u','U'},{'v','V'},{'w','W'},{'x','X'},{'y','Y'},{'z','Z'},
    {'1','!'},{'2','@'},{'3','#'},{'4','$'},{'5','%'},{'6','^'},{'7','&'},{'8','*'},{'9','('},{'0',')'},
    {0,0},{0,0},{0,0},{0,0},{' ',' '},                          /* 28 Enter 29 Esc 2a Bksp 2b Tab 2c Space */
    {'-','_'},{'=','+'},{'[','{'},{']','}'},{'\\','|'},{'#','~'},{';',':'},{'\'','"'},{'`','~'},
    {',','<'},{'.','>'},{'/','?'} };

/* RU: the same keys -> { normal, shift } as codepoints */
static const uint16_t ru[0x35][2] = {
    {0x444,0x424},{0x438,0x418},{0x441,0x421},{0x432,0x412},{0x443,0x423},{0x430,0x410},{0x43f,0x41f},
    {0x440,0x420},{0x448,0x428},{0x43e,0x41e},{0x43b,0x41b},{0x434,0x414},{0x44c,0x42c},{0x442,0x422},
    {0x449,0x429},{0x437,0x417},{0x439,0x419},{0x43a,0x41a},{0x44b,0x42b},{0x435,0x415},{0x433,0x413},
    {0x43c,0x41c},{0x446,0x426},{0x447,0x427},{0x43d,0x41d},{0x44f,0x42f},
    {'1','!'},{'2','"'},{'3',0x2116},{'4',';'},{'5','%'},{'6',':'},{'7','?'},{'8','*'},{'9','('},{'0',')'},
    {0,0},{0,0},{0,0},{0,0},{' ',' '},
    {'-','_'},{'=','+'},{0x445,0x425},{0x44a,0x42a},{'\\','/'},{'#','~'},{0x436,0x416},{0x44d,0x42d},
    {0x451,0x401},{0x431,0x411},{0x44e,0x42e},{'.',','} };

static int layout, caps;
static uint8_t held[6], last_mods;
static uint8_t rep_key;
static uint32_t rep_t, kbdn_seen;

int kbd_layout(void) { return layout; }
int (*kbd_hook)(VTermKey key, uint32_t ch, VTermModifier mod, int gui);

static VTermModifier mods_of(uint8_t m) {
    int v = VTERM_MOD_NONE;
    if (m & 0x22) v |= VTERM_MOD_SHIFT;
    if (m & 0x44) v |= VTERM_MOD_ALT;
    if (m & 0x11) v |= VTERM_MOD_CTRL;
    return (VTermModifier)v;
}

static void press(VTerm *vt, uint8_t k, uint8_t m) {
    VTermModifier md = mods_of(m);
    int shift = (m & 0x22) != 0;
    VTermKey key = VTERM_KEY_NONE;
    switch (k) {
    case 0x28: key = VTERM_KEY_ENTER; break;
    case 0x29: key = VTERM_KEY_ESCAPE; break;
    case 0x2a: key = VTERM_KEY_BACKSPACE; break;
    case 0x2b: key = VTERM_KEY_TAB; break;
    case 0x39: caps = !caps; return;
    case 0x49: key = VTERM_KEY_INS; break;
    case 0x4a: key = VTERM_KEY_HOME; break;
    case 0x4b: key = VTERM_KEY_PAGEUP; break;
    case 0x4c: key = VTERM_KEY_DEL; break;
    case 0x4d: key = VTERM_KEY_END; break;
    case 0x4e: key = VTERM_KEY_PAGEDOWN; break;
    case 0x4f: key = VTERM_KEY_RIGHT; break;
    case 0x50: key = VTERM_KEY_LEFT; break;
    case 0x51: key = VTERM_KEY_DOWN; break;
    case 0x52: key = VTERM_KEY_UP; break;
    case 0x54: key = VTERM_KEY_KP_DIVIDE; break;
    case 0x55: key = VTERM_KEY_KP_MULT; break;
    case 0x56: key = VTERM_KEY_KP_MINUS; break;
    case 0x57: key = VTERM_KEY_KP_PLUS; break;
    case 0x58: key = VTERM_KEY_KP_ENTER; break;
    case 0x62: key = VTERM_KEY_KP_0; break;
    case 0x63: key = VTERM_KEY_KP_PERIOD; break;
    default:
        if (k >= 0x3a && k <= 0x45) key = VTERM_KEY_FUNCTION(k - 0x3a + 1);
        else if (k >= 0x59 && k <= 0x61) key = (VTermKey)(VTERM_KEY_KP_1 + (k - 0x59));
        break;
    }
    int gui = (m & 0x88) != 0;
    if (key != VTERM_KEY_NONE) { if (!(kbd_hook && kbd_hook(key, 0, md, gui))) vterm_keyboard_key(vt, key, md); return; }
    if (k < 0x04 || k > 0x38) return;
    int i = k - 0x04;
    int letter = k <= 0x1d || (layout && ru[i][0] >= 0x400);
    int up = shift ^ (caps && letter);
    uint32_t c;
    if (layout && !(m & 0x11)) c = ru[i][up];   /* Ctrl+key sends the Latin control code */
    else c = (uint8_t)us[i][up];
    if (!c) return;
    /* the shift is in the character; Ctrl / Alt go as modifiers */
    if (kbd_hook && kbd_hook(VTERM_KEY_NONE, c, md, gui)) return;
    vterm_keyboard_unichar(vt, c, (VTermModifier)(md & ~VTERM_MOD_SHIFT));
}

void kbd_init(VTerm *vt) {
    (void)vt;
    kbdn_seen = IO_KBDN;
}

void kbd_poll(VTerm *vt) {
    uint32_t now = IO_MS;
    uint32_t n = IO_KBDN;
    if (n != kbdn_seen) {
        kbdn_seen = n;
        uint32_t k0 = IO_KBD0, k1 = IO_KBD1;
        uint8_t m = (uint8_t)k0;
        uint8_t keys[6] = { (uint8_t)(k0 >> 8), (uint8_t)(k0 >> 16), (uint8_t)(k0 >> 24),
                            (uint8_t)k1, (uint8_t)(k1 >> 8), (uint8_t)(k1 >> 16) };
        /* Alt+Shift: a new Shift with Alt held, or a new Alt with Shift held */
        uint8_t newm = m & ~last_mods;
        if (((newm & 0x22) && (m & 0x44)) || ((newm & 0x44) && (m & 0x22))) layout = !layout;
        last_mods = m;
        int rep_held = 0;
        for (int i = 0; i < 6; i++) {
            uint8_t k = keys[i];
            if (k < 0x04) continue;
            int was = 0;
            for (int j = 0; j < 6; j++) if (held[j] == k) was = 1;
            if (!was) { press(vt, k, m); rep_key = k; rep_t = now + 500; }
            if (k == rep_key) rep_held = 1;
        }
        if (!rep_held) rep_key = 0;
        for (int i = 0; i < 6; i++) held[i] = keys[i];
    } else if (rep_key && rep_key != 0x39 && (int32_t)(now - rep_t) >= 0) {
        press(vt, rep_key, last_mods);
        rep_t = now + 33;
    }
}
