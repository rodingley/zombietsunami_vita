/* updater_helper.c -- "Zombie Tsunami Updater", a tiny second app (TITLE_ID
 * ZTUPDATER) that installs an update of the game.
 *
 * A running app cannot install a new version of itself, so the game unpacks
 * the update into ux0:data/zt_pkg, installs this app and starts it, then
 * closes (updater.c).  This app makes sure the game is closed, hands the
 * update to the Vita's package installer, writes the result to
 * ux0:data/zombietsunami/update_result.txt and starts the game again: the
 * same steps as VitaShell's own updater app (VSUPDATER).  It shows nothing
 * (a few seconds of black screen).  The game removes it on its next start. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "config.h"
#include "update_promoter.h"

#define NOTE_FILE DATA_PATH "/update_log.txt"

static void note(const char *fmt, ...) {
  char buf[300];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (n > (int)sizeof(buf) - 2)
    n = (int)sizeof(buf) - 2;
  buf[n++] = '\n';
  SceUID fd = sceIoOpen(NOTE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, buf, (SceSize)n);
    sceIoClose(fd);
  }
}

static void remove_tree(const char *path) {
  SceUID d = sceIoDopen(path);
  if (d < 0) {
    sceIoRemove(path);
    return;
  }
  SceIoDirent e;
  memset(&e, 0, sizeof(e));
  while (sceIoDread(d, &e) > 0) {
    char sub[512];
    snprintf(sub, sizeof(sub), "%s/%s", path, e.d_name);
    if (SCE_S_ISDIR(e.d_stat.st_mode))
      remove_tree(sub);
    else
      sceIoRemove(sub);
    memset(&e, 0, sizeof(e));
  }
  sceIoDclose(d);
  sceIoRmdir(path);
}

/* the same steps as VitaShell's own updater (VitaShell/updater/main.c):
 * close the other app, install, start the game, exit */
int main(void) {
  sceAppMgrDestroyOtherApp();

  SceIoStat st;
  int r;
  if (sceIoGetstat(UPD_PKG_DIR "/eboot.bin", &st) < 0) {
    r = -1;
    note("helper: no update waiting in " UPD_PKG_DIR);
  } else {
    r = upd_promoter_start();
    if (r >= 0) {
      r = scePromoterUtilityPromotePkgWithRif(UPD_PKG_DIR, 1);
      upd_promoter_stop();
    }
    if (r >= 0)
      note("helper: the update is installed");
    else
      note("helper: the update could not be installed (%08x)", (unsigned)r);
    if (r < 0)
      remove_tree(UPD_PKG_DIR);
  }
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%08x\n", (unsigned)r);
    SceUID fd = sceIoOpen(UPD_RESULT_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
      sceIoWrite(fd, buf, (SceSize)strlen(buf));
      sceIoClose(fd);
    }
  }

  /* start the game again */
  sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" PORT_TITLE_ID);
  sceKernelExitProcess(0);
  return 0;
}
