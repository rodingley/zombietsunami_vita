/* ime.c -- the PS Vita's own on-screen keyboard for the game's text box (the
 * "Redeem code" promo code box in the Market), and the port's own promo code.
 *
 * On Android the game asks Java to show the phone's keyboard
 * (GameGLSurfaceView.openIMEKeyboard) and gets the typed text back through
 * Cocos2dxEditText.nativeTextInputDone.  Here openIMEKeyboard opens the Vita's
 * keyboard (the system IME dialog).  The game then sends every code to
 * Mobigame's coupon server, which the Vita cannot reach, so the code is checked
 * here instead:
 *   - the port's own codes (config.h): "rodi" gives 100,000 diamonds and
 *     "vita" 100,000 coins, every time they are typed, and a message says so;
 *   - any other code gets "That promo code isn't valid";
 *   - either way the game is told the box was closed (nativeTextInputCancel),
 *     its normal "cancel" path, so it never waits for a server answer.
 * Everything runs on the game's thread, at the start of a frame (stage2b.c).
 * While a dialog is up the game gets no buttons or touches, and every frame is
 * shown with the system dialog drawn over it (gfx_swap). */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/message_dialog.h>
#include <psp2/system_param.h>

#include "config.h"
#include "ime.h"
#include "jni_fake.h"
#include "so_platform.h"
#include "stage1.h"

#define LIBCGAME (NUM_GAME_LIBS - 1)
/* the game's currency numbers, as its own purchase code uses them: a coin pack
 * adds to 3, a diamond pack to 0x1b */
#define COINS 3
#define DIAMONDS 0x1b

static const struct {
  const char *code;
  int currency;
  int amount;
  const char *message;
} promo_codes[] = {
  { PROMO_CODE_DIAMONDS, DIAMONDS, 100000, "Promo code accepted!\nYou got 100,000 diamonds." },
  { PROMO_CODE_COINS, COINS, 100000, "Promo code accepted!\nYou got 100,000 coins." },
};

static volatile int ime_wanted;
static int ime_running, msg_running, inited;
static uint16_t ime_title[32];
static uint16_t ime_initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint16_t ime_text[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

static void (*text_cancel_fn)(void *, void *);
static void *(*get_content_fn)(void *, void *);
static void *(*wallet_get)(void);

void ime_request_open(void) { ime_wanted = 1; }

void ime_request_close(void) {
  ime_wanted = 0;
  if (ime_running)
    sceImeDialogAbort();
}

int ime_busy(void) { return ime_running || msg_running || ime_wanted; }

static void utf8_to_16(const char *s, uint16_t *out, int max) {
  int n = 0;
  while (s && *s && n < max - 1) {
    unsigned c = (unsigned char)*s++;
    if (c >= 0xe0 && s[0] && s[1]) {
      c = ((c & 0x0f) << 12) | ((s[0] & 0x3f) << 6) | (s[1] & 0x3f);
      s += 2;
    } else if (c >= 0xc0 && s[0]) {
      c = ((c & 0x1f) << 6) | (s[0] & 0x3f);
      s += 1;
    }
    out[n++] = (uint16_t)c;
  }
  out[n] = 0;
}

static void utf16_to_8(const uint16_t *s, char *out, int max) {
  int n = 0;
  for (; *s && n < max - 4; s++) {
    unsigned c = *s;
    if (c < 0x80) {
      out[n++] = (char)c;
    } else if (c < 0x800) {
      out[n++] = (char)(0xc0 | (c >> 6));
      out[n++] = (char)(0x80 | (c & 0x3f));
    } else {
      out[n++] = (char)(0xe0 | (c >> 12));
      out[n++] = (char)(0x80 | ((c >> 6) & 0x3f));
      out[n++] = (char)(0x80 | (c & 0x3f));
    }
  }
  out[n] = 0;
}

static void ime_init_once(void) {
  if (inited)
    return;
  inited = 1;
  SceAppUtilInitParam ip;
  SceAppUtilBootParam bp;
  memset(&ip, 0, sizeof(ip));
  memset(&bp, 0, sizeof(bp));
  sceAppUtilInit(&ip, &bp); /* may already be done (fine) */
  SceCommonDialogConfigParam cfg;
  sceCommonDialogConfigParamInit(&cfg);
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cfg.language);
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cfg.enterButtonAssign);
  sceCommonDialogSetConfigParam(&cfg);

  so_module *g = &game_mods[LIBCGAME];
  text_cancel_fn = (void (*)(void *, void *))so_symbol(g, "Java_net_mobigame_artemis_Cocos2dxEditText_nativeTextInputCancel");
  get_content_fn = (void *(*)(void *, void *))so_symbol(g, "Java_net_mobigame_artemis_GameGLSurfaceView_nativeGetContentText");
  /* the player's wallet: libcgame 0xafa44 returns it (ldr r0,[pc,#4]; add r0,pc; ldr r0,[r0,#4]; bx lr) */
  static const uint16_t sig[] = {0x4801, 0x4478, 0x6840, 0x4770};
  if (!memcmp((const void *)(g->text_base + 0xafa44), sig, sizeof(sig)))
    wallet_get = (void *(*)(void))(g->text_base + 0xafa44 + 1);
  else
    so_log("[promo] wallet code not found: the promo code cannot give diamonds");
}

static void show_message(const char *text) {
  SceMsgDialogUserMessageParam um;
  memset(&um, 0, sizeof(um));
  um.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
  um.msg = (const SceChar8 *)text;
  SceMsgDialogParam mp;
  sceMsgDialogParamInit(&mp);
  mp.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
  mp.userMsgParam = &um;
  if (sceMsgDialogInit(&mp) >= 0)
    msg_running = 1;
  else
    so_log("[promo] could not show the message: %s", text);
}

static int promo_matches(const char *typed, const char *want) {
  /* letters compared without caring about case; spaces ignored */
  for (;;) {
    while (*typed == ' ')
      typed++;
    if (!*typed || !*want)
      break;
    if (toupper((unsigned char)*typed) != toupper((unsigned char)*want))
      return 0;
    typed++;
    want++;
  }
  while (*typed == ' ')
    typed++;
  return !*typed && !*want;
}

/* ARM Android code takes float arguments in core registers (softfp) */
typedef void (__attribute__((pcs("aapcs"))) *wallet_add_fn)(void *wallet, int currency, float amount);
typedef void (*wallet_save_fn)(void *wallet, int arg);

static void handle_code(const char *typed) {
  int which = -1;
  for (unsigned i = 0; i < sizeof(promo_codes) / sizeof(promo_codes[0]); i++)
    if (promo_matches(typed, promo_codes[i].code))
      which = (int)i;
  if (which < 0) {
    so_log("[promo] code \"%s\" is not one of the port's codes", typed);
    show_message("That promo code isn't valid.");
    return;
  }
  void *wallet = wallet_get ? wallet_get() : NULL;
  if (!wallet) {
    show_message("The reward could not be added right now. Try again from the main menu.");
    return;
  }
  void **vt = *(void ***)wallet;
  /* add, then save -- exactly as the game's own rewards do */
  ((wallet_add_fn)vt[0x1c / 4])(wallet, promo_codes[which].currency, (float)promo_codes[which].amount);
  ((wallet_save_fn)vt[0x8 / 4])(wallet, 0);
  so_log("[promo] code \"%s\" accepted: %d added to currency %d", promo_codes[which].code,
         promo_codes[which].amount, promo_codes[which].currency);
  show_message(promo_codes[which].message);
}

static void tell_game_closed(void *env) {
  if (text_cancel_fn)
    text_cancel_fn(env, jni_find_class("net/mobigame/artemis/Cocos2dxEditText"));
}

void ime_frame(void *env) {
  if (msg_running) {
    if (sceMsgDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
      sceMsgDialogTerm();
      msg_running = 0;
    }
    return;
  }
  if (ime_running) {
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
      return;
    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    ime_running = 0;
    tell_game_closed(env);
    if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
      char typed[SCE_IME_DIALOG_MAX_TEXT_LENGTH * 3 + 4];
      utf16_to_8(ime_text, typed, sizeof(typed));
      handle_code(typed);
    } else {
      so_log("[promo] keyboard closed without a code");
    }
    return;
  }
  if (!ime_wanted)
    return;
  ime_wanted = 0;
  ime_init_once();
  const char *initial = "";
  if (get_content_fn) {
    void *s = get_content_fn(env, jni_find_class("net/mobigame/artemis/GameGLSurfaceView"));
    const char *t = jni_string_utf(s);
    if (t)
      initial = t;
  }
  utf8_to_16("Promo code", ime_title, 32);
  utf8_to_16(initial, ime_initial, SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1);
  memset(ime_text, 0, sizeof(ime_text));
  SceImeDialogParam p;
  sceImeDialogParamInit(&p);
  p.supportedLanguages = 0;
  p.languagesForced = SCE_FALSE;
  p.type = SCE_IME_TYPE_BASIC_LATIN;
  p.option = 0;
  p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
  p.title = ime_title;
  p.maxTextLength = 32;
  p.initialText = ime_initial;
  p.inputTextBuffer = ime_text;
  int r = sceImeDialogInit(&p);
  if (r < 0) {
    so_log("[promo] the Vita keyboard could not open (%08x)", (unsigned)r);
    tell_game_closed(env);
    return;
  }
  ime_running = 1;
  so_log("[promo] Vita keyboard open");
}
