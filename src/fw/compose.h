/* combining marks: a base character and marks after it -> one precomposed BMP codepoint (NFC pairs,
   compose_tab.h), applied in turn; a mark without a pair is dropped (the RTL shows one glyph per cell). */
#pragma once
#include <stdint.h>

uint32_t compose(const uint32_t *chars, int n);   /* chars[0] base, then marks; n <= 6, stops at 0 */
