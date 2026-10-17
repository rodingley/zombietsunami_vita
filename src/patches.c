/* patches.c -- small changes to the game itself, applied right after loading.
 *
 * Friends drawer: the main menu (libcgame.so, Zombie Tsunami 4.7.3 armeabi-v7a)
 * owns two "friends" side panels: one at menu+0x10 (shown once Facebook is
 * connected) and one at menu+0x208 (the "Friends" tab with the connect offer).
 * Each has a mode word (menu+0x1cc and menu+0x3c4); the menu only updates, draws
 * and passes touches to a panel when that word is non-zero.  We change the menu's
 * twelve reads of those words into "movs rX, #0", so to the menu both panels are
 * always switched off: nothing is drawn and nothing reacts to touch.
 * Every instruction is checked before it is changed; on another game version the
 * patch is skipped and logged.
 */
#include <stdint.h>
#include <psp2/kernel/processmgr.h>
#include "so_platform.h"
#include "stage1.h"

typedef struct {
  uint32_t addr; /* offset in libcgame.so */
  uint8_t rt;    /* destination register of the original ldr.w */
  uint16_t imm;  /* field offset the ldr.w reads */
} ldr_site;

static const ldr_site friends_mode_reads[] = {
  {0x17f57e, 0, 0x3c4}, {0x17f58e, 0, 0x1cc}, /* update */
  {0x17f5ee, 1, 0x1cc}, {0x17f61a, 1, 0x3c4}, /* draw */
  {0x17f676, 2, 0x3c4}, {0x17f700, 2, 0x1cc}, /* touch handlers */
  {0x17f784, 2, 0x3c4}, {0x17f7ba, 2, 0x1cc},
  {0x17f868, 0, 0x3c4}, {0x17f8c4, 0, 0x1cc},
  {0x17f9b8, 0, 0x3c4}, {0x17fa14, 0, 0x1cc},
};


/* Options screen: drop "Notifications", "Facebook Log In/Out" and "Privacy Policy".
 * The game already knows how to leave out the first two (it hides them on some
 * builds): its two "show this item" flags (screen+0x37a notifications, +0x379
 * Facebook) are now always read as 0.  "Privacy Policy" is always added, so its
 * label is skipped when the list of labels is built and its button is made
 * invisible and untappable (button+0x214 enabled, +0x215 visible = 0).
 * Each entry: offset, number of 16-bit Thumb halfwords, original code, new code. */
typedef struct {
  uint32_t addr;
  int n;
  uint16_t old[8], neu[8];
} hw_patch;

static const hw_patch options_patches[] = {
  {0x15e20e, 2, {0xf894, 0x137a}, {0x2100, 0xbf00}},   /* layout: notifications flag -> 0 */
  {0x15e252, 2, {0xf894, 0x1379}, {0x2100, 0xbf00}},   /* layout: facebook flag -> 0 */
  {0x15eb2a, 2, {0xf894, 0x037a}, {0x2000, 0xbf00}},
  {0x15ebaa, 2, {0xf894, 0x0379}, {0x2000, 0xbf00}},
  {0x15ed1c, 4, {0xf04f, 0x31ff, 0xf11e, 0xf9a4},      /* privacy button: strh 0 -> enabled+visible */
                {0x2100, 0xf8a0, 0x1214, 0xbf00}},
  {0x15f168, 2, {0xf891, 0x137a}, {0x2100, 0xbf00}},
  {0x15f1b6, 2, {0xf890, 0x0379}, {0x2000, 0xbf00}},
  {0x15f894, 2, {0xf89b, 0x037a}, {0x2000, 0xbf00}},   /* label list: no "Notifications" */
  {0x15fa48, 2, {0xf89b, 0x4379}, {0x2400, 0xbf00}},   /* label list: no "Facebook Log In" */
  {0x15fb02, 8, {0x2010, 0xf213, 0xe884, 0x4604, 0x4885, 0xf884, 0x800e, 0x4478},
                {0x950d, 0xaa0b, 0x9c0c, 0x4294, 0xf000, 0x8092, 0xf000, 0xb827}}, /* skip "Privacy Policy" label */
  {0x160afe, 2, {0xf894, 0x0379}, {0x2000, 0xbf00}},
};

/* Main menu, top right: the Facebook and Twitter buttons.  When the menu lays
 * them out it calls button->setVisible(1); make that setVisible(0) instead.  An
 * invisible button is neither drawn nor tappable.  The third one (Bluesky) stays
 * and opens the "report bugs" pop-up (see popup.c / jni.c openUrl). */
static const hw_patch social_patches[] = {
  {0x18151e, 1, {0x2101}, {0x2100}},   /* Facebook: movs r1,#1 -> movs r1,#0 */
  {0x1815c4, 1, {0x2101}, {0x2100}},   /* Twitter */
};

/* The "Connect to Facebook" pop-up (... Earn 100 diamonds ...), which the game
 * shows after some runs and in a few other places.  Every one of them opens it
 * the same way: popupManager->showPopup(9, 0, &callback), where 9 is that
 * pop-up's number in the game's pop-up factory (libcgame 0x18b54c: case 9 builds
 * the Facebook pop-up, 0x19ffb8).  The call is removed at each place; none of
 * them uses what the call returns, and each still cleans up its own callback. */
static const hw_patch fb_popup_patches[] = {
  {0x15d63e, 2, {0x2109, 0x47b0}, {0x2109, 0xbf00}},   /* movs r1,#9 ; blx r6  -> movs r1,#9 ; nop */
  {0x15f02c, 2, {0x2109, 0x47e0}, {0x2109, 0xbf00}},   /* blx r12 */
  {0x171648, 2, {0x2109, 0x47b0}, {0x2109, 0xbf00}},   /* after a run (game count checks) */
  {0x178320, 2, {0x2109, 0x47e0}, {0x2109, 0xbf00}},
  {0x1b1e16, 2, {0x2109, 0x47b0}, {0x2109, 0xbf00}},
  {0x1ba5d2, 2, {0x2109, 0x47a0}, {0x2109, 0xbf00}},   /* blx r4 */
};

static int apply_hw_patches(uintptr_t base, const hw_patch *t, int n, const char *what) {
  int done = 0;
  for (int i = 0; i < n; i++) {
    uint16_t *p = (uint16_t *)(base + t[i].addr);
    int ok = 1;
    for (int k = 0; k < t[i].n; k++)
      if (p[k] != t[i].old[k]) ok = 0;
    if (!ok) {
      so_log("[patch] %s %06x: unexpected code, skipped", what, (unsigned)t[i].addr);
      continue;
    }
    so_plat_write(p, t[i].neu, t[i].n * 2);
    done++;
  }
  so_log("[patch] %s (%d of %d patched)", what, done, n);
  return done;
}

/* ---- loading screen: the percentage counts up smoothly ----------------------
 * The game's loading screen prints its counter (loading object + 0x220) with
 * "%02d%%".  The counter itself moves in steps of 5 as each part of the game
 * finishes loading, so on the Vita (slower loading) it sat on 0, then showed
 * 20%, then jumped.  Only the number that is PRINTED is changed: the read at
 * 0x8fb32 (ldr.w r0, [r9, #0x220]) becomes a call to loading_percent_hook,
 * which counts up towards the real value smoothly.  The real counter, which
 * decides when loading moves on, is untouched.
 * The call needs a tiny jump stub within 16 MB of the game's code, so a page
 * of code memory is set up just after libcgame. */
#define LOADING_PCT_SITE 0x8fb32

static float pct_shown;
static uint64_t pct_last;

int loading_percent_hook(const uint8_t *loading) {
  int actual = *(const volatile int *)(loading + 0x220);
  uint64_t now = sceKernelGetProcessTimeWide();
  float dt = pct_last ? (now - pct_last) * 1e-6f : 0.0f;
  pct_last = now;
  if (dt > 0.1f)
    dt = 0.1f; /* after a stall: carry on smoothly, no jump */
  if (actual < pct_shown - 10.0f)
    pct_shown = 0.0f; /* a new loading screen */
  if (pct_shown < actual) {
    /* fast enough to keep up, faster when far behind: 0 -> 20% takes about half a second */
    pct_shown += (15.0f + (actual - pct_shown) * 3.0f) * dt;
    if (pct_shown > actual)
      pct_shown = (float)actual;
  }
  return (int)pct_shown;
}

/* one page of code memory just after libcgame, for the small jump stubs the
 * hooks below need (a Thumb call or jump reaches 16 MB) */
static uintptr_t hooks_page_addr;
static uintptr_t hooks_page(uintptr_t base) {
  if (hooks_page_addr)
    return hooks_page_addr;
  static const uintptr_t tries[] = { 0x9B400000, 0x9B500000, 0x9AF00000, 0x9B600000 };
  for (unsigned i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
    int blk = -1;
    uintptr_t got = 0;
    if (so_plat_alloc("zt_hooks", 0x1000, tries[i], 1, &blk, &got) < 0)
      continue;
    int32_t dist = (int32_t)(got - base);
    if (dist > 0xF00000 || dist < 0) { /* not where we asked: too far for a call */
      so_plat_free(blk);
      continue;
    }
    hooks_page_addr = got;
    break;
  }
  return hooks_page_addr;
}

static void thumb_bl(uintptr_t from, uintptr_t to, uint16_t out[2]) {
  int32_t off = (int32_t)(to - (from + 4));
  uint32_t s = (off >> 24) & 1, i1 = (off >> 23) & 1, i2 = (off >> 22) & 1;
  uint32_t j1 = (1 - i1) ^ s, j2 = (1 - i2) ^ s;
  out[0] = (uint16_t)(0xF000 | (s << 10) | ((off >> 12) & 0x3ff));
  out[1] = (uint16_t)(0xD000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7ff));
}

static void patch_loading_percent(uintptr_t base) {
  uint16_t *site = (uint16_t *)(base + LOADING_PCT_SITE);
  if (site[0] != 0xf8d9 || site[1] != 0x0220) { /* ldr.w r0, [r9, #0x220] */
    so_log("[patch] loading percentage: unexpected code, skipped");
    return;
  }
  uintptr_t page = hooks_page(base);
  if (!page) {
    so_log("[patch] loading percentage: no code memory near the game, skipped");
    return;
  }
  /* stub: mov r0, r9 ; nop ; ldr.w pc, [pc, #0] ; .word loading_percent_hook */
  uint32_t hook = (uint32_t)(uintptr_t)&loading_percent_hook;
  uint16_t stub[6] = { 0x4648, 0xbf00, 0xf8df, 0xf000, (uint16_t)hook, (uint16_t)(hook >> 16) };
  so_plat_write((void *)page, stub, sizeof(stub));
  so_plat_flush(page, sizeof(stub));
  uint16_t bl[2];
  thumb_bl(base + LOADING_PCT_SITE, page, bl);
  so_plat_write(site, bl, sizeof(bl));
  so_log("[patch] loading percentage counts up smoothly (stub at %08x)", (unsigned)page);
}


/* ---- game states and run starts: logged only (nothing is changed) ----
 * GameManager::SetState (libcgame 0x97650) changes screens.  From the game's
 * state -> screen table (0x8ed00), its buttons and the test log:
 *   2 main menu; 3/4/5 the screen-1 menus (5 reached from the main menu, 4
 *   from the results screen); 6/7/8 the shop (from the menu or screen 5, from
 *   screen 4, from the pause menu); 9 the results screen after a run;
 *   12/13 the run itself (playing / paused); 15/16 the missions & trophies
 *   screen (from the menu / screen 1).
 *
 * A normal run is started by StartNormalRun (0x982a0), which switches to 12 by
 * itself, not through SetState; its start is logged too (nothing changed).
 * The boosters pop-up comes up inside the run, after it has started.
 *
 * Both hooks work the same way: the function's first two instructions are
 * replaced by a jump to a small stub that calls the C hook, then runs those two
 * instructions and carries on with the function. */
#define SETSTATE 0x97650

#define START_NORMAL_RUN 0x982a0

static int state_hook(int new_state, uint32_t caller, const uint8_t *game) {
  int old_state = game ? *(const volatile int *)(game + 0x44) : -1;
  unsigned from = (unsigned)((caller & ~1u) - (uint32_t)game_mods[NUM_GAME_LIBS - 1].text_base - 4);
  so_log("[state] %d -> %d (from libcgame+%06x)", old_state, new_state, from);
  return new_state;
}

/* called at the start of StartNormalRun: 1 = start the run, 0 = don't.  Only
 * logs: the boosters pop-up is part of the run itself (it shows up after the
 * run has started), so nothing is redirected. */
static int run_hook(uint8_t *game) {
  int st = game ? *(volatile int *)(game + 0x44) : -1;
  so_log("[state] a normal run starts (from state %d)", st);
  return 1; /* always start it: only logged */
}

static void thumb_bw(uintptr_t from, uintptr_t to, uint16_t out[2]) {
  thumb_bl(from, to, out);
  out[1] &= ~0x4000; /* B.W instead of BL: bit 14 of the second halfword cleared */
}

static void patch_state_log(uintptr_t base) {
  uint16_t *site = (uint16_t *)(base + SETSTATE);
  if (site[0] != 0xb5d0 || site[1] != 0xaf02 || site[2] != 0x4604) {
    so_log("[patch] state log: unexpected code, skipped");
    return;
  }
  uintptr_t page = hooks_page(base);
  if (!page) {
    so_log("[patch] state log: no code memory near the game, skipped");
    return;
  }
  uintptr_t stub = page + 0x40;
  uint32_t fn = (uint32_t)(uintptr_t)&state_hook, back = (uint32_t)(base + SETSTATE + 4) | 1;
  uint16_t code[] = {
    0xe92d, 0x500f, /* 00 push.w {r0-r3, r12, lr} */
    0x4602,         /* 04 mov r2, r0      (the game manager) */
    0x4608,         /* 06 mov r0, r1      (the new state) */
    0x4671,         /* 08 mov r1, lr      (where it was called from) */
    0xf8df, 0xc014, /* 0a ldr.w r12, [pc, #20] -> 20 */
    0x47e0,         /* 0e blx r12 */
    0x9001,         /* 10 str r0, [sp, #4]  (the state to use goes back into r1) */
    0xe8bd, 0x500f, /* 12 pop.w {r0-r3, r12, lr} */
    0xb5d0,         /* 16 push {r4, r6, r7, lr}   (the two instructions replaced) */
    0xaf02,         /* 18 add r7, sp, #8 */
    0xf8df, 0xf008, /* 1a ldr.w pc, [pc, #8] -> 24 */
    0xbf00,         /* 1e nop (keeps the words below aligned) */
    (uint16_t)fn, (uint16_t)(fn >> 16),     /* 20 */
    (uint16_t)back, (uint16_t)(back >> 16), /* 24 */
  };
  so_plat_write((void *)stub, code, sizeof(code));
  so_plat_flush(stub, sizeof(code));
  uint16_t jump[2];
  thumb_bw(base + SETSTATE, stub, jump);
  so_plat_write(site, jump, sizeof(jump));
  so_plat_flush(base + SETSTATE, sizeof(jump));
  so_log("[patch] state changes are logged (stub at %08x)", (unsigned)stub);
}

/* NOT APPLIED (kept for reference): the second test showed the boosters screen
 * does appear before every run -- it is shown as part of the run start, inside
 * StartNormalRun, not as a separate state -- so redirecting the call is wrong. */
static void __attribute__((unused)) patch_run_start(uintptr_t base) {
  uint16_t *site = (uint16_t *)(base + START_NORMAL_RUN);
  /* push {r4-r7, lr} ; add r7, sp, #0xc ; str r8, [sp, #-4]! */
  if (site[0] != 0xb5f0 || site[1] != 0xaf03 || site[2] != 0xf84d || site[3] != 0x8d04) {
    so_log("[patch] run start log: unexpected code, skipped");
    return;
  }
  uintptr_t page = hooks_page(base);
  if (!page) {
    so_log("[patch] run start log: no code memory near the game, skipped");
    return;
  }
  uintptr_t stub = page + 0x80;
  uint32_t fn = (uint32_t)(uintptr_t)&run_hook, back = (uint32_t)(base + START_NORMAL_RUN + 4) | 1;
  uint16_t code[] = {
    0xe92d, 0x500f, /* 00 push.w {r0-r3, r12, lr} */
    0xf8df, 0xc018, /* 04 ldr.w r12, [pc, #24] -> 20 */
    0x47e0,         /* 08 blx r12        (run_hook(game): game is already in r0) */
    0x2800,         /* 0a cmp r0, #0 */
    0xe8bd, 0x500f, /* 0c pop.w {r0-r3, r12, lr}   (flags are kept) */
    0xd100,         /* 10 bne 14         (1: start the run) */
    0x4770,         /* 12 bx lr          (0: return without starting it) */
    0xb5f0,         /* 14 push {r4-r7, lr}   (the two instructions replaced) */
    0xaf03,         /* 16 add r7, sp, #0xc */
    0xf8df, 0xf008, /* 18 ldr.w pc, [pc, #8] -> 24 */
    0xbf00, 0xbf00, /* 1c nops (keep the words below aligned) */
    (uint16_t)fn, (uint16_t)(fn >> 16),     /* 20 */
    (uint16_t)back, (uint16_t)(back >> 16), /* 24 */
  };
  so_plat_write((void *)stub, code, sizeof(code));
  so_plat_flush(stub, sizeof(code));
  uint16_t jump[2];
  thumb_bw(base + START_NORMAL_RUN, stub, jump);
  so_plat_write(site, jump, sizeof(jump));
  so_plat_flush(base + START_NORMAL_RUN, sizeof(jump));
  so_log("[patch] run starts are logged (stub at %08x)", (unsigned)stub);
}

/* ---- which boosters the run's booster buttons offer ----
 * At the start of a run the game shows its boosters (Pack of civilians, Double
 * zombies, +10 seconds, coin bomb, coin car).  For each one it asks
 * IsBoosterAvailable(game, type) (libcgame 0x96b54; only the run's booster
 * buttons call it, 0x129d8a / 0x12a002).  Types: 0x24 civilians, 0x25 double
 * zombies, 0x26 +10 s, 0x27 coin bomb, 0x28 coin car, 0x29 last chance.
 * The game has two rules for it:
 *   - "unlock flags" (what the Vita was using): a booster shows once its intro
 *     pop-up has run (wallet flags 0x1e-0x22); on the Vita only the civilians'
 *     flag ever got set, so only Pack of civilians showed;
 *   - "level-up by games played" (game+0x24 == 1): +10 s after 3 games, coin
 *     bomb after 8, coin car after 13, double zombies after 18.
 * Chosen for the Vita (like the phone): Pack of civilians and Double zombies
 * from the start, then the game's own level-up for the other three (more than
 * 3 / 8 / 13 games played, the game's games-played counter, stat 10).
 * Anything else (last chance, ...) keeps the game's own answer. */
#define BOOSTER_AVAILABLE 0x96b54
#define STATS_GET 0xaf764      /* returns the stats object */
#define STATS_VALUE 0xaf90c    /* (stats, id) -> int; id 10 = games played */

static int booster_hook(void *game, int type) {
  (void)game;
  uintptr_t base = game_mods[NUM_GAME_LIBS - 1].text_base;
  if (type == 0x24 || type == 0x25)
    return 1;
  if (type < 0x26 || type > 0x28)
    return -1; /* the game's own rule */
  void *stats = ((void *(*)(void))(base + STATS_GET + 1))();
  int games = stats ? ((int (*)(void *, int))(base + STATS_VALUE + 1))(stats, 10) : 0;
  static int logged_games = -1;
  if (games != logged_games) {
    logged_games = games;
    so_log("[state] boosters in a run (%d games played): civilians, double zombies%s%s%s", games,
           games > 3 ? ", +10 seconds" : "", games > 8 ? ", coin bomb" : "", games > 13 ? ", coin car" : "");
  }
  return type == 0x26 ? games > 3 : type == 0x27 ? games > 8 : games > 13;
}

static void patch_boosters(uintptr_t base) {
  uint16_t *site = (uint16_t *)(base + BOOSTER_AVAILABLE);
  /* push {r4, r6, r7, lr} ; add r7, sp, #8 ; ldr r0, [r0, #0x24] ; mov r4, r1 */
  if (site[0] != 0xb5d0 || site[1] != 0xaf02 || site[2] != 0x6a40 || site[3] != 0x460c ||
      *(const uint16_t *)(base + STATS_GET) != 0x4801 || *(const uint16_t *)(base + STATS_VALUE) != 0xeb00) {
    so_log("[patch] boosters: unexpected code, skipped");
    return;
  }
  uintptr_t page = hooks_page(base);
  if (!page) {
    so_log("[patch] boosters: no code memory near the game, skipped");
    return;
  }
  uintptr_t stub = page + 0xc0;
  uint32_t fn = (uint32_t)(uintptr_t)&booster_hook, back = (uint32_t)(base + BOOSTER_AVAILABLE + 4) | 1;
  uint16_t code[] = {
    0xe92d, 0x500f, /* 00 push.w {r0-r3, r12, lr} */
    0xf8df, 0xc01c, /* 04 ldr.w r12, [pc, #28] -> 24 */
    0x47e0,         /* 08 blx r12            booster_hook(game, type) */
    0x1c41,         /* 0a adds r1, r0, #1    (zero when the hook said -1) */
    0xd003,         /* 0c beq 16             -1: ask the game's own rule */
    0x9000,         /* 0e str r0, [sp]       the answer goes back in r0 */
    0xe8bd, 0x500f, /* 10 pop.w {r0-r3, r12, lr} */
    0x4770,         /* 14 bx lr */
    0xe8bd, 0x500f, /* 16 pop.w {r0-r3, r12, lr} */
    0xb5d0,         /* 1a push {r4, r6, r7, lr}   (the two instructions replaced) */
    0xaf02,         /* 1c add r7, sp, #8 */
    0xf8df, 0xf008, /* 1e ldr.w pc, [pc, #8] -> 28 */
    0xbf00,         /* 22 nop */
    (uint16_t)fn, (uint16_t)(fn >> 16),     /* 24 */
    (uint16_t)back, (uint16_t)(back >> 16), /* 28 */
  };
  so_plat_write((void *)stub, code, sizeof(code));
  so_plat_flush(stub, sizeof(code));
  uint16_t jump[2];
  thumb_bw(base + BOOSTER_AVAILABLE, stub, jump);
  so_plat_write(site, jump, sizeof(jump));
  so_plat_flush(base + BOOSTER_AVAILABLE, sizeof(jump));
  so_log("[patch] boosters: civilians + double zombies from the start, the rest by games played (stub at %08x)",
         (unsigned)stub);
}

/* Is the boosters bar on screen (the row of boosters shown when a run starts)?
 * While it is up the game sends taps to the booster buttons and does not jump
 * (its own jump code asks the same thing, libcgame+0x93fbc), so the X button's
 * jump tap must not be sent then: it landed on a booster.
 *   App (one object, made at start-up; pointer at libcgame+0x395840)
 *     +0x220  the game screen (made together with the App)
 *       +0x1f70  the boosters bar (vtable libcgame+0x37d4e4)
 *         +0x8e4  showing (sliding in / waiting)   +0x8e5  sliding out
 * = the bar's own check, libcgame+0x12ae84.  Only plain memory reads, so it is
 * safe from the input code; the bar's vtable is checked before trusting it. */
#define APP_PTR 0x395840
#define APP_PTR_LOAD 0x8d76c   /* App getter: ldr r6, [pc, #0x68] ; add r6, pc */
#define APP_GAME_SCREEN 0x220
#define GAME_BOOSTER_BAR 0x1f70
#define BAR_VTABLE 0x37d4e4
#define BAR_IS_UP 0x12ae84
static int bar_check_ok;

int boosters_bar_up(void) {
  if (!bar_check_ok)
    return 0;
  uintptr_t base = game_mods[NUM_GAME_LIBS - 1].text_base;
  const char *app = *(const char *const *)(base + APP_PTR);
  if (!app)
    return 0;
  const char *screen = *(const char *const *)(app + APP_GAME_SCREEN);
  if (!screen)
    return 0;
  const char *bar = *(const char *const *)(screen + GAME_BOOSTER_BAR);
  if (!bar || *(const uintptr_t *)bar != base + BAR_VTABLE)
    return 0;
  return bar[0x8e4] || bar[0x8e5];
}

static void check_boosters_bar(uintptr_t base) {
  const uint16_t *get = (const uint16_t *)(base + APP_PTR_LOAD);
  const uint16_t *up = (const uint16_t *)(base + BAR_IS_UP);
  uint32_t lit = *(const uint32_t *)(base + APP_PTR_LOAD + 4 + 0x68); /* the getter's literal */
  /* ldr r6,[pc,#0x68] ; add r6,pc ... and ldrb.w r1,[r0,#0x8e4] ; ldrb.w r0,[r0,#0x8e5] */
  if (get[0] != 0x4e1a || get[1] != 0x447e || base + APP_PTR_LOAD + 6 + lit != base + APP_PTR ||
      up[0] != 0xf890 || up[1] != 0x18e4 || up[2] != 0xf890 || up[3] != 0x08e5 ||
      *(const uint32_t *)(base + BAR_VTABLE + 0x148) != (uint32_t)(base + 0x12ae98 + 1)) {
    so_log("[patch] boosters bar check: unexpected code, X may tap the boosters bar");
    return;
  }
  bar_check_ok = 1;
  so_log("[patch] boosters bar check ready (X does not jump while the bar is up)");
}

void patches_apply(void) {
  so_module *game = &game_mods[NUM_GAME_LIBS - 1];
  uintptr_t base = game->text_base;
  const int n = sizeof(friends_mode_reads) / sizeof(friends_mode_reads[0]);
  int done = 0;
  for (int i = 0; i < n; i++) {
    const ldr_site *s = &friends_mode_reads[i];
    uint16_t *p = (uint16_t *)(base + s->addr);
    /* ldr.w Rt, [Rn, #imm12] = F8D<Rn> <Rt><imm12> */
    if ((p[0] & 0xfff0) != 0xf8d0 || (p[1] & 0x0fff) != s->imm || (p[1] >> 12) != s->rt) {
      so_log("[patch] %06x: unexpected code %04x %04x, skipped", (unsigned)s->addr, p[0], p[1]);
      continue;
    }
    uint16_t repl[2] = {(uint16_t)(0x2000 | (s->rt << 8)), 0xbf00}; /* movs Rt, #0 ; nop */
    so_plat_write(p, repl, sizeof(repl));
    done++;
  }
  so_log("[patch] friends tab removed (%d of %d menu checks patched)", done, n);

  apply_hw_patches(base, options_patches, sizeof(options_patches) / sizeof(options_patches[0]),
                   "options: no notifications / facebook / privacy");
  apply_hw_patches(base, social_patches, sizeof(social_patches) / sizeof(social_patches[0]),
                   "main menu: no facebook / twitter buttons");
  apply_hw_patches(base, fb_popup_patches, sizeof(fb_popup_patches) / sizeof(fb_popup_patches[0]),
                   "no \"connect to Facebook\" pop-up");
  patch_loading_percent(base);
  patch_state_log(base);
  /* patch_run_start(base) is not used: the boosters screen is part of the run
   * start itself (it appears without any state change), so there is nothing to
   * redirect -- see the note above patch_run_start. */
  patch_boosters(base);
  check_boosters_bar(base);
}
