/* The system on-screen keyboard (IME dialog). The dialog draws itself
 * during gfx_end() while the app keeps rendering frames. */
#ifndef DXG_IME_H
#define DXG_IME_H

#include <stddef.h>

typedef enum {
    IME_TEXT,
    IME_URL,
    IME_NUMBER /* digits, '.', '-' */
} ime_kind;

/* Call once after gfx init: sets the system language and enter button for
 * dialogs. */
void ime_setup(void);
/* Opens the keyboard. Returns 0 on success. */
int ime_open(const char *title, const char *initial, ime_kind kind, int max_len);
int ime_active(void);
/* Closes the keyboard as if cancelled. */
void ime_cancel(void);
/* 0 while open; 1 when confirmed (text in `out`); -1 when cancelled. */
int ime_poll(char *out, size_t n);

#endif
