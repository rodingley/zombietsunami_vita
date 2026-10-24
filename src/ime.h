#ifndef IME_H
#define IME_H

/* the Vita's on-screen keyboard for the game's promo code box (ime.c) */
void ime_request_open(void);  /* the game asked for the keyboard (JNI openIMEKeyboard) */
void ime_request_close(void); /* the game asked to close it (JNI closeIMEKeyboard) */
void ime_frame(void *env);    /* once a frame, on the game's thread, before the game's frame */
int ime_busy(void);           /* a system dialog is up (or about to be): no input to the game */

#endif
