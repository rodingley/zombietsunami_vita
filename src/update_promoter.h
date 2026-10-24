/* update_promoter.h -- shared by the game (updater.c) and the update helper
 * app (updater_helper.c): where the update is put, and the Vita's package
 * installer (the "promoter", the same one VitaShell uses to install a VPK). */
#ifndef UPDATE_PROMOTER_H
#define UPDATE_PROMOTER_H

#include <stdint.h>
#include <string.h>

#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

#define UPD_PKG_DIR "ux0:data/zt_pkg"           /* the new version, unpacked by the game */
#define UPD_HELPER_DIR "ux0:data/zt_helper_pkg" /* the helper app's package, before it is installed */
#define UPD_HELPER_ID "ZTUPDATER"           /* the helper app's TITLE_ID */
#define UPD_HEAD_BIN "sce_sys/package/head.bin"
#define UPD_RESULT_FILE "ux0:data/zombietsunami/update_result.txt" /* the helper's answer */

static int upd_paf_loaded;

/* started and stopped exactly as VitaShell does (package_installer.c) */
static int upd_paf_start(void) {
  static uint32_t argp[] = {0x180000, 0xFFFFFFFF, 0xFFFFFFFF, 1, 0xFFFFFFFF, 0xFFFFFFFF};
  int result = -1;
  uint32_t buf[4];
  buf[0] = sizeof(buf);
  buf[1] = (uint32_t)(uintptr_t)&result;
  buf[2] = 0xFFFFFFFF;
  buf[3] = 0xFFFFFFFF;
  return sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof(argp), argp, (const SceSysmoduleOpt *)buf);
}

static void upd_paf_stop(void) {
  uint32_t buf = 0;
  sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, (const SceSysmoduleOpt *)&buf);
}

/* 0 = ready (call upd_promoter_stop afterwards) */
static int upd_promoter_start(void) {
  upd_paf_loaded = 0;
  if (sceSysmoduleIsLoadedInternal(SCE_SYSMODULE_INTERNAL_PAF) < 0) /* the game may have it already */
    upd_paf_loaded = upd_paf_start() >= 0;
  int r = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
  if (r >= 0)
    r = scePromoterUtilityInit();
  if (r < 0 && upd_paf_loaded) {
    upd_paf_stop();
    upd_paf_loaded = 0;
  }
  return r < 0 ? r : 0;
}

static void upd_promoter_stop(void) {
  scePromoterUtilityExit();
  sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
  if (upd_paf_loaded)
    upd_paf_stop();
  upd_paf_loaded = 0;
}

#endif
