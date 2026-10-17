/* fmod_log.c -- logging wrappers around a few FMOD calls the game makes (libcgame only) */
#include <stdint.h>
#include "so_platform.h"
#include "stage1.h"

typedef uintptr_t (*fn_t)();

static void *real(const char *name) {
  for (int i = 1; i <= 2; i++) {
    uintptr_t a = so_symbol(&game_mods[i], name);
    if (a)
      return (void *)a;
  }
  return 0;
}

#define REAL(name) static void *r; if (!r) r = real(name)

static int w_create(void **pp, unsigned ver) {
  REAL("_ZN4FMOD6Studio6System6createEPPS1_j");
  int rc = ((int (*)(void **, unsigned))r)(pp, ver);
  so_log("[fmod] Studio::System::create(ver %x) -> %d", ver, rc);
  return rc;
}
static int w_init(void *s, int ch, unsigned sf, unsigned cf, void *x) {
  REAL("_ZN4FMOD6Studio6System10initializeEijjPv");
  int rc = ((int (*)(void *, int, unsigned, unsigned, void *))r)(s, ch, sf, cf, x);
  so_log("[fmod] Studio::initialize(ch %d, studio %x, core %x) -> %d", ch, sf, cf, rc);
  return rc;
}
static int w_setoutput(void *s, int t) {
  REAL("_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE");
  int rc = ((int (*)(void *, int))r)(s, t);
  so_log("[fmod] System::setOutput(%d) -> %d", t, rc);
  return rc;
}
static int w_loadbank(void *s, const char *p, unsigned f, void **b) {
  REAL("_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE");
  int rc = ((int (*)(void *, const char *, unsigned, void **))r)(s, p, f, b);
  so_log("[fmod] loadBankFile(\"%s\") -> %d", p ? p : "?", rc);
  return rc;
}
static int w_getevent(void *s, const char *p, void **e) {
  REAL("_ZNK4FMOD6Studio6System8getEventEPKcPPNS0_16EventDescriptionE");
  int rc = ((int (*)(void *, const char *, void **))r)(s, p, e);
  static int n;
  if (rc != 0 && n++ < 20)
    so_log("[fmod] getEvent(\"%s\") -> %d", p ? p : "?", rc);
  return rc;
}
static int w_update(void *s) {
  REAL("_ZN4FMOD6Studio6System6updateEv");
  int rc = ((int (*)(void *))r)(s);
  static int n;
  if (rc != 0 && n++ < 5)
    so_log("[fmod] Studio::update -> %d", rc);
  return rc;
}
static int w_start(void *s) {
  REAL("_ZN4FMOD6Studio13EventInstance5startEv");
  int rc = ((int (*)(void *))r)(s);
  static int n;
  if (n++ < 15 || rc != 0)
    so_log("[fmod] EventInstance::start -> %d", rc);
  return rc;
}

const so_default_dynlib fmod_log_dynlib[] = {
  {"_ZN4FMOD6Studio6System6createEPPS1_j", (uintptr_t)w_create},
  {"_ZN4FMOD6Studio6System10initializeEijjPv", (uintptr_t)w_init},
  {"_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE", (uintptr_t)w_setoutput},
  {"_ZN4FMOD6Studio6System12loadBankFileEPKcjPPNS0_4BankE", (uintptr_t)w_loadbank},
  {"_ZNK4FMOD6Studio6System8getEventEPKcPPNS0_16EventDescriptionE", (uintptr_t)w_getevent},
  {"_ZN4FMOD6Studio6System6updateEv", (uintptr_t)w_update},
  {"_ZN4FMOD6Studio13EventInstance5startEv", (uintptr_t)w_start},
};
const int fmod_log_dynlib_count = sizeof(fmod_log_dynlib) / sizeof(fmod_log_dynlib[0]);
