/* updater.c -- the port's auto-updater.
 *
 * As soon as the game starts (while it is still loading), a background thread
 * asks GitHub for the latest release of the port:
 *     https://api.github.com/repos/<UPDATE_REPO>/releases/latest
 * (only if the Vita is on Wi-Fi; otherwise nothing happens).  If its version
 * tag is newer than PORT_VERSION, a Vita pop-up asks whether to update.  On
 * "Yes" that release's VPK is downloaded, with a progress bar.  The VPK is
 * kept in the repository (UPDATE_VPK_PATH, at the release's tag) and fetched
 * through api.github.com: the Vita can make secure connections to GitHub's
 * own servers but not to the servers that hold release files
 * (*.githubusercontent.com), tested on a real Vita.
 *
 * Installing: the VPK is checked (it must contain eboot.bin and be this
 * app: TITLE_ID in sce_sys/param.sfo) and unpacked; then the game starts a
 * small helper app that installs it with the Vita's own package installer,
 * the way VitaShell installs a VPK, and starts the game again (see install()
 * and updater_helper.c).  A failed download or a full memory card leaves the
 * installed game as it was.  The game's data and saves
 * (ux0:data/zombietsunami) are not touched.
 *
 * Downloads go over HTTPS with the Vita checking GitHub's certificate, and
 * only from this repository.  The Vita's own list
 * of trusted certificate authorities is too old for GitHub ("unknown CA"), so the
 * current root certificates are handed to it first (update_certs.h).  "[update]"
 * lines in log.txt say what happened. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/libssl.h>
#include <psp2/message_dialog.h>
#include <psp2/net/http.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "config.h"
#include "so_platform.h"
#include "update_certs.h"
#include "update_promoter.h"
#include "update_util.h"
#include "updater.h"

#define API_URL "https://api.github.com/repos/" UPDATE_REPO "/releases/latest"
#define VPK_URL_FMT "https://api.github.com/repos/" UPDATE_REPO "/contents/" UPDATE_VPK_PATH "?ref=%s"
#define USER_AGENT "ZombieTsunamiVita/" PORT_VERSION
#define UPD_DIR DATA_PATH "/update"
#define UPD_FILE UPD_DIR "/download.tmp"
#define APP_DIR "ux0:app/" PORT_TITLE_ID
#define NOTE_FILE DATA_PATH "/update_log.txt"      /* the updater's own log: kept across restarts */
#define MARK_FILE DATA_PATH "/update_installed.txt" /* the tag of the update just installed */

#define NET_POOL (1024 * 1024)
#define SSL_POOL (300 * 1024)
#define HTTP_POOL (512 * 1024)
#define MAX_HEADER (32 * 1024) /* GitHub's download redirects have very long headers */
#define MAX_URL 4096           /* ...and long signed links to its file servers */
#define MAX_JSON (256 * 1024)
#define MAX_DOWNLOAD (64 * 1024 * 1024)

extern volatile int g_in_run;       /* gfx_vita.c: a run is being played (not a menu) */
extern int ime_busy(void);          /* ime.c */
extern int popup_is_open(void);     /* popup.c */

enum {
  ST_WAIT,        /* not started yet */
  ST_CHECKING,    /* the check thread is asking GitHub */
  ST_FOUND,       /* a newer version: ask when the moment is right */
  ST_ASKING,      /* "Update now?" is on screen */
  ST_WORKING,     /* downloading / installing, progress bar on screen */
  ST_CLOSING,     /* the progress bar is closing */
  ST_RESULT,      /* "installed" / "could not" message on screen */
  ST_DONE         /* nothing more to do until the next start */
};

static volatile int state = ST_WAIT;
static char new_tag[40];
static char news[240];     /* the release's "What's new" list, for the pop-up */
static char mark_tag[40];  /* MARK_FILE: an update installed on an earlier start... */
static int mark_skip;      /* ...that did not take effect, and the player was told */
static int tell_failed;    /* ST_FOUND: tell the player the earlier update did not take effect */
static int asked;         /* the question was shown again after closing by itself */
static int style;         /* which way of writing the question (0 = with the list, 1 = list on one line, 2 = no list) */
static char vpk_url[256];
static long vpk_size; /* from GitHub, for the progress bar and a size check */

/* progress, written by the work thread, shown by the game's thread */
static volatile int work_pct, work_phase, work_done, work_ok;
static int shown_pct = -1, shown_phase = -1;
static char work_error[160];

/* ---- the updater's own log (update_log.txt, kept across restarts) ---- */

static void note(const char *fmt, ...) {
  char buf[1200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  so_log("[update] %s", buf);
  SceUID fd = sceIoOpen(NOTE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd < 0)
    return;
  if (sceIoLseek(fd, 0, SCE_SEEK_END) > 64 * 1024) { /* keep it small */
    sceIoClose(fd);
    fd = sceIoOpen(NOTE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
      return;
  }
  sceIoWrite(fd, buf, (SceSize)strlen(buf));
  sceIoWrite(fd, "\n", 1);
  sceIoClose(fd);
}

static long file_size(const char *path) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return -1;
  long n = (long)sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoClose(fd);
  return n;
}

/* MARK_FILE holds "<tag>" after an update was installed, "<tag> skip" once the
 * player was told that it did not take effect (so it is not offered again) */
static void read_mark(void) {
  mark_tag[0] = 0;
  mark_skip = 0;
  SceUID fd = sceIoOpen(MARK_FILE, SCE_O_RDONLY, 0);
  if (fd < 0)
    return;
  char buf[64];
  int n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0)
    return;
  buf[n] = 0;
  int i = 0;
  while (i < (int)sizeof(mark_tag) - 1 && buf[i] && buf[i] != ' ' && buf[i] != '\n')
    mark_tag[i] = buf[i], i++;
  mark_tag[i] = 0;
  mark_skip = strstr(buf, "skip") != NULL;
}

static void write_mark(const char *tag, int skip) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s%s\n", tag, skip ? " skip" : "");
  SceUID fd = sceIoOpen(MARK_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, buf, (SceSize)strlen(buf));
    sceIoClose(fd);
  }
}

static uint8_t *read_file(const char *path, uint32_t *len);

/* after an update: what the helper app said, and remove it and the files */
static void remove_tree(const char *path);
static void clean_up(void) {
  char buf[16] = "";
  SceUID fd = sceIoOpen(UPD_RESULT_FILE, SCE_O_RDONLY, 0);
  if (fd >= 0) {
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    buf[n > 0 ? n : 0] = 0;
    if (n > 0 && buf[n - 1] == '\n')
      buf[n - 1] = 0;
    sceIoRemove(UPD_RESULT_FILE);
    note("the update helper answered %s", buf);
  }
  remove_tree(UPD_PKG_DIR);
  remove_tree(UPD_HELPER_DIR);
  /* the helper app stays open in the background after starting the game:
   * close it first, then remove it (as VitaShell does with its updater) */
  sceAppMgrDestroyOtherApp();
  int r = upd_promoter_start();
  if (r >= 0) {
    r = scePromoterUtilityDeletePkg(UPD_HELPER_ID);
    upd_promoter_stop();
  }
  note("removing the update helper app: %08x", (unsigned)r);
}

/* Copies installed from a VPK made before the content ID was set were
 * registered by the Vita as "ABCD99999" (the SDK's placeholder) in
 * ux0:app/ABCD99999.  Once this copy runs under its own ID, that old copy (a
 * second bubble) is removed -- only if it really is this game. */
#define OLD_ID "ABCD99999"
static void remove_old_copy(void) {
  uint32_t l = 0, hl = 0;
  uint8_t *sfo = read_file("ux0:app/" OLD_ID "/sce_sys/param.sfo", &l);
  if (!sfo)
    return;
  char tid[16] = "";
  upd_sfo_string(sfo, l, "TITLE_ID", tid, sizeof(tid));
  free(sfo);
  /* and this copy is not that one (its package header names this game's ID) */
  uint8_t *head = read_file("app0:" UPD_HEAD_BIN, &hl);
  int ours = 0;
  for (uint32_t i = 0x30; head && hl > 0x60 && i + 9 <= 0x60; i++)
    if (!memcmp(head + i, PORT_TITLE_ID, 9))
      ours = 1;
  free(head);
  if (strcmp(tid, PORT_TITLE_ID) != 0 || !ours)
    return;
  int r = upd_promoter_start();
  if (r >= 0) {
    r = scePromoterUtilityDeletePkg(OLD_ID);
    upd_promoter_stop();
  }
  note("removed the old copy of the game (" OLD_ID "): %08x", (unsigned)r);
}

/* ---- networking ---- */

static void *net_pool;
static int net_inited, netctl_inited, ssl_inited, http_inited, mods_loaded;

/* ---- root certificates (update_certs.h), as PEM text ---- */
static int certs_loaded;

static void load_certs(void) {
  static SceHttpsData data[UPDATE_CERT_COUNT];
  static const SceHttpsData *list[UPDATE_CERT_COUNT];
  for (int i = 0; i < UPDATE_CERT_COUNT; i++) {
    data[i].ptr = (char *)cert_pem[i];
    data[i].size = (unsigned int)strlen(cert_pem[i]);
    list[i] = &data[i];
  }
  int r = sceHttpsLoadCert(UPDATE_CERT_COUNT, list, NULL, NULL);
  certs_loaded = r >= 0;
  if (r < 0)
    so_log("[update] the root certificates could not be loaded (%08x)", (unsigned)r);
}

static void net_stop(void) {
  if (certs_loaded)
    sceHttpsUnloadCert();
  certs_loaded = 0;
  if (http_inited)
    sceHttpTerm();
  if (ssl_inited)
    sceSslTerm();
  if (netctl_inited)
    sceNetCtlTerm();
  if (net_inited)
    sceNetTerm();
  if (mods_loaded) {
    sceSysmoduleUnloadModule(SCE_SYSMODULE_HTTPS);
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
  }
  http_inited = ssl_inited = netctl_inited = net_inited = mods_loaded = 0;
  free(net_pool);
  net_pool = NULL;
}

/* 0 = online and ready; -1 = no Wi-Fi or the network could not start */
static int net_start(void) {
  if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0 || sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS) < 0) {
    so_log("[update] the network modules could not be loaded");
    return -1;
  }
  mods_loaded = 1;
  net_pool = malloc(NET_POOL);
  if (!net_pool)
    return -1;
  SceNetInitParam p;
  p.memory = net_pool;
  p.size = NET_POOL;
  p.flags = 0;
  int r = sceNetInit(&p);
  if (r < 0 && r != (int)SCE_NET_ERROR_EBUSY) {
    so_log("[update] sceNetInit failed (%08x)", (unsigned)r);
    return -1;
  }
  net_inited = r >= 0;
  r = sceNetCtlInit();
  netctl_inited = r >= 0;
  int st = 0;
  if (sceNetCtlInetGetState(&st) < 0 || st != SCE_NETCTL_STATE_CONNECTED) {
    so_log("[update] the Vita is not online: no update check");
    return -1;
  }
  r = sceSslInit(SSL_POOL);
  if (r < 0 && r != (int)SCE_SSL_ERROR_ALREADY_INITED) {
    so_log("[update] sceSslInit failed (%08x)", (unsigned)r);
    return -1;
  }
  ssl_inited = r >= 0;
  r = sceHttpInit(HTTP_POOL);
  if (r < 0 && r != (int)SCE_HTTP_ERROR_ALREADY_INITED) {
    so_log("[update] sceHttpInit failed (%08x)", (unsigned)r);
    return -1;
  }
  http_inited = r >= 0;
  /* the Vita checks the server's certificate (who it really is), against its own
   * list plus the current root certificates */
  load_certs();
  sceHttpsEnableOption(SCE_HTTPS_FLAG_SERVER_VERIFY);
  return 0;
}

typedef struct {
  int tmpl, conn, req;
} http_req;

static void http_close(http_req *h) {
  if (h->req >= 0)
    sceHttpDeleteRequest(h->req);
  if (h->conn >= 0)
    sceHttpDeleteConnection(h->conn);
  if (h->tmpl >= 0)
    sceHttpDeleteTemplate(h->tmpl);
  h->req = h->conn = h->tmpl = -1;
}

/* GET url (following up to 5 redirects to other https addresses); on success
 * the response body can be read with sceHttpReadData(h->req).  *len = its size
 * (0 if not given).  0 = HTTP 200 */
static int http_open(const char *url, const char *accept, http_req *h, uint64_t *len) {
  static char cur[MAX_URL];
  snprintf(cur, sizeof(cur), "%s", url);
  h->tmpl = h->conn = h->req = -1;
  for (int hop = 0; hop < 6; hop++) {
    if (strncmp(cur, "https://", 8) != 0) {
      so_log("[update] refusing a non-https address");
      return -1;
    }
    {
      const char *host = cur + 8, *end = strchr(host, '/');
      so_log("[update] connecting to %.*s", (int)(end ? end - host : (int)strlen(host)), host);
    }
    h->tmpl = sceHttpCreateTemplate(USER_AGENT, SCE_HTTP_VERSION_1_1, SCE_TRUE);
    if (h->tmpl < 0) {
      so_log("[update] sceHttpCreateTemplate failed (%08x)", (unsigned)h->tmpl);
      return -1;
    }
    sceHttpSetAutoRedirect(h->tmpl, 0); /* redirects are followed below, so each one is checked */
    sceHttpSetResponseHeaderMaxSize(h->tmpl, MAX_HEADER);
    sceHttpSetResolveTimeOut(h->tmpl, 10 * 1000 * 1000);
    sceHttpSetConnectTimeOut(h->tmpl, 15 * 1000 * 1000);
    sceHttpSetRecvTimeOut(h->tmpl, 30 * 1000 * 1000);
    h->conn = sceHttpCreateConnectionWithURL(h->tmpl, cur, SCE_TRUE);
    if (h->conn < 0) {
      so_log("[update] sceHttpCreateConnectionWithURL failed (%08x)", (unsigned)h->conn);
      http_close(h);
      return -1;
    }
    h->req = sceHttpCreateRequestWithURL(h->conn, SCE_HTTP_METHOD_GET, cur, 0);
    if (h->req < 0) {
      so_log("[update] sceHttpCreateRequestWithURL failed (%08x)", (unsigned)h->req);
      http_close(h);
      return -1;
    }
    if (accept)
      sceHttpAddRequestHeader(h->req, "Accept", accept, SCE_HTTP_HEADER_OVERWRITE);
    int r = sceHttpSendRequest(h->req, NULL, 0);
    if (r < 0) {
      int err = 0;
      unsigned int detail = 0;
      sceHttpsGetSslError(h->req, &err, &detail);
      so_log("[update] request failed (%08x, ssl error %08x detail %08x)", (unsigned)r, (unsigned)err, detail);
      http_close(h);
      return -1;
    }
    int status = 0;
    sceHttpGetStatusCode(h->req, &status);
    if (status == 200) {
      unsigned long long l = 0;
      if (sceHttpGetResponseContentLength(h->req, &l) < 0)
        l = 0;
      *len = l;
      return 0;
    }
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      char *hdr = NULL;
      unsigned int hl = 0;
      const char *val = NULL;
      unsigned int vl = 0;
      if (sceHttpGetAllResponseHeaders(h->req, &hdr, &hl) >= 0 &&
          sceHttpParseResponseHeader(hdr, hl, "Location", &val, &vl) >= 0 && vl > 0 && vl < sizeof(cur)) {
        static char next[MAX_URL];
        memcpy(next, val, vl);
        next[vl] = 0;
        http_close(h);
        snprintf(cur, sizeof(cur), "%s", next);
        continue;
      }
    }
    so_log("[update] the server answered %d", status);
    if (status >= 300 && status < 400)
      so_log("[update] (a redirect whose address could not be read)");
    http_close(h);
    return -1;
  }
  so_log("[update] too many redirects");
  return -1;
}

/* the whole response in a malloc'ed, 0-terminated buffer */
static int http_get_text(const char *url, const char *accept, char **out) {
  http_req h;
  uint64_t len = 0;
  if (http_open(url, accept, &h, &len))
    return -1;
  char *buf = malloc(MAX_JSON + 1);
  int n = 0;
  if (!buf) {
    http_close(&h);
    return -1;
  }
  while (n < MAX_JSON) {
    int r = sceHttpReadData(h.req, buf + n, MAX_JSON - n);
    if (r < 0) {
      so_log("[update] reading the answer failed (%08x)", (unsigned)r);
      free(buf);
      http_close(&h);
      return -1;
    }
    if (r == 0)
      break;
    n += r;
  }
  http_close(&h);
  buf[n] = 0;
  *out = buf;
  return n;
}

/* ---- files ---- */

static void make_dirs(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 5; *p; p++) /* skip "ux0:/" style prefixes */
    if (*p == '/') {
      *p = 0;
      sceIoMkdir(tmp, 0777);
      *p = '/';
    }
}

static int write_file(const char *path, const void *data, uint32_t len) {
  make_dirs(path);
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return -1;
  const uint8_t *p = data;
  uint32_t done = 0;
  while (done < len) {
    int w = sceIoWrite(fd, p + done, len - done > 256 * 1024 ? 256 * 1024 : len - done);
    if (w <= 0) {
      sceIoClose(fd);
      sceIoRemove(path);
      return -1;
    }
    done += (uint32_t)w;
  }
  sceIoClose(fd);
  return 0;
}

static uint8_t *read_file(const char *path, uint32_t *len) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0)
    return NULL;
  int size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);
  if (size <= 0 || size > MAX_DOWNLOAD) {
    sceIoClose(fd);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)size);
  int got = buf ? sceIoRead(fd, buf, (SceSize)size) : -1;
  sceIoClose(fd);
  if (got != size) {
    free(buf);
    return NULL;
  }
  *len = (uint32_t)size;
  return buf;
}

/* ---- the check (background thread) ---- */

static int check_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  int found = 0;
  note("start: this is v%s", PORT_VERSION);
  remove_old_copy();
  /* did an update installed on an earlier start take effect? */
  read_mark();
  if (mark_tag[0] && !mark_skip)
    clean_up(); /* the first start after an update: remove the helper app */
  if (mark_tag[0] && !upd_version_newer(mark_tag, PORT_VERSION)) {
    note("the update to %s worked: this is now v%s", mark_tag, PORT_VERSION);
    sceIoRemove(MARK_FILE);
    mark_tag[0] = 0;
  } else if (mark_tag[0]) {
    note("the update to %s was installed earlier, but this is still v%s", mark_tag, PORT_VERSION);
  }
  if (net_start() == 0) {
    char *json = NULL;
    if (http_get_text(API_URL, "application/vnd.github+json", &json) > 0) {
      if (upd_json_string(json, "tag_name", new_tag, sizeof(new_tag)) != 0) {
        so_log("[update] GitHub's answer has no version (no release yet?)");
      } else if (!upd_version_newer(new_tag, PORT_VERSION)) {
        so_log("[update] up to date (this is v%s, the latest release is %s)", PORT_VERSION, new_tag);
      } else if (!upd_tag_ok(new_tag)) {
        so_log("[update] the latest release has an unusual tag: ignored");
      } else {
        /* its VPK, in the repository at that tag (its size comes with the download) */
        snprintf(vpk_url, sizeof(vpk_url), VPK_URL_FMT, new_tag);
        vpk_size = 0;
        note("release %s is newer than v%s", new_tag, PORT_VERSION);
        found = 1;
        /* its "What's new" list, for the pop-up */
        static char body[4096];
        news[0] = 0;
        if (upd_json_text(json, "body", body, sizeof(body)) == 0)
          upd_whats_new(body, news, sizeof(news), 5);
        /* the same update was installed before and did not take effect: do not loop */
        if (mark_tag[0] && !strcmp(mark_tag, new_tag)) {
          if (mark_skip) {
            note("not offering %s again: installing it did not take effect before", new_tag);
            found = 0;
          } else {
            tell_failed = 1;
          }
        }
      }
    }
    free(json);
  }
  if (!found)
    net_stop();
  state = found ? ST_FOUND : ST_DONE;
  return sceKernelExitDeleteThread(0);
}

/* ---- download and install (background thread) ---- */

static void fail(const char *why) {
  snprintf(work_error, sizeof(work_error), "%s", why);
  note("%s", why);
}

static int download(const char *url, const char *path) {
  http_req h;
  uint64_t len = 0;
  if (http_open(url, "application/vnd.github.raw+json", &h, &len)) {
    fail("The update could not be downloaded.");
    return -1;
  }
  if (!len)
    len = (uint64_t)vpk_size; /* for the progress bar */
  if (len > MAX_DOWNLOAD) {
    http_close(&h);
    fail("The update file is too big.");
    return -1;
  }
  make_dirs(path);
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) {
    http_close(&h);
    fail("The update could not be saved on the memory card.");
    return -1;
  }
  static uint8_t buf[64 * 1024];
  uint64_t got = 0;
  int ok = 1;
  for (;;) {
    int r = sceHttpReadData(h.req, buf, sizeof(buf));
    if (r < 0) {
      so_log("[update] download interrupted (%08x)", (unsigned)r);
      ok = 0;
      break;
    }
    if (r == 0)
      break;
    if (sceIoWrite(fd, buf, (SceSize)r) != r) {
      ok = 0;
      fail("The memory card is full (or could not be written).");
      break;
    }
    got += (uint64_t)r;
    if (got > MAX_DOWNLOAD) {
      ok = 0;
      break;
    }
    if (len)
      work_pct = (int)(got * 80 / len);
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
  }
  sceIoClose(fd);
  http_close(&h);
  if (ok && ((len && got != len) || (vpk_size && got != (uint64_t)vpk_size)))
    ok = 0;
  if (!ok) {
    sceIoRemove(path);
    if (!work_error[0])
      fail("The download was interrupted. Check the Wi-Fi and try again.");
    return -1;
  }
  so_log("[update] downloaded %llu bytes", (unsigned long long)got);
  return 0;
}

/* Installing goes through the Vita's own package installer (the "promoter",
 * the same one VitaShell uses for a VPK): files written straight into
 * ux0:app/<TITLE_ID> are not what the Vita starts (tested), and a running app
 * cannot install a new version of itself.  So the new version is unpacked
 * into UPD_PKG_DIR, a tiny second app (updater_helper.c, shipped in the VPK as
 * app0:updater/) is installed, and after the game closes that app installs the
 * update and starts the game again.  The installer needs a package header
 * (head.bin) for each app: VitaShell made one for the game when it was
 * installed; it only depends on the TITLE_ID, so it is used again, and turned
 * into the helper's one (update_util.c). */
#define HEAD_BIN UPD_HEAD_BIN

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

typedef struct {
  int written, total;
  int failed;
} unpack_ctx;

static int count_one(void *ctx, const char *name, const uint8_t *data, uint32_t len) {
  (void)name;
  (void)data;
  (void)len;
  ((unpack_ctx *)ctx)->total++;
  return 0;
}

/* every file of the VPK into UPD_PKG_DIR */
static int unpack_one(void *ctx, const char *name, const uint8_t *data, uint32_t len) {
  unpack_ctx *c = ctx;
  char path[512];
  snprintf(path, sizeof(path), UPD_PKG_DIR "/%s", name);
  if (write_file(path, data, len)) {
    note("could not write %s", path);
    c->failed = 1;
    return 1;
  }
  c->written++;
  work_pct = 84 + (c->total ? c->written * 8 / c->total : 0);
  sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
  return 0;
}

static int install(const uint8_t *vpk, uint32_t vlen) {
  /* is it really this app? */
  uint8_t *sfo = NULL, *eboot = NULL;
  uint32_t sl = 0, el = 0;
  char tid[16] = "";
  if (upd_zip_find(vpk, vlen, "sce_sys/param.sfo", 1, &sfo, &sl) || upd_sfo_string(sfo, sl, "TITLE_ID", tid, sizeof(tid)) ||
      strcmp(tid, PORT_TITLE_ID) != 0) {
    free(sfo);
    fail("The downloaded file is not an update for this game.");
    return -1;
  }
  free(sfo);
  if (upd_zip_find(vpk, vlen, "eboot.bin", 1, &eboot, &el) || el < 1024) {
    free(eboot);
    fail("The downloaded update is incomplete.");
    return -1;
  }
  free(eboot);

  /* the head.bin made when the game was installed, and the helper app */
  uint32_t hl = 0, hel = 0, hsl = 0;
  uint8_t *head = read_file("app0:" HEAD_BIN, &hl);
  uint8_t *helper_head = NULL;
  uint8_t *helper_eboot = read_file("app0:updater/eboot.bin", &hel);
  uint8_t *helper_sfo = read_file("app0:updater/param.sfo", &hsl);
  int sums_ok = 0;
  if (head && (helper_head = malloc(hl)) != NULL) {
    /* both made for the right title ID: a copy installed by VitaShell from an
     * older VPK has the SDK's placeholder ID (HB0001-ABCD99999) in it, and the
     * Vita registers the app under that ID, so it cannot be found as the game */
    memcpy(helper_head, head, hl);
    if (upd_head_make(head, hl, PORT_TITLE_ID, &sums_ok) || upd_head_make(helper_head, hl, UPD_HELPER_ID, NULL)) {
      free(helper_head);
      helper_head = NULL;
    }
  }
  note("head.bin %u bytes (checksums %s), helper %u + %u bytes", head ? (unsigned)hl : 0u,
       helper_head ? (sums_ok ? "as expected" : "NOT as expected") : "unreadable", (unsigned)hel, (unsigned)hsl);
  if (!head || !helper_head || !helper_eboot || !helper_sfo) {
    free(head);
    free(helper_head);
    free(helper_eboot);
    free(helper_sfo);
    fail("This copy of the game cannot install updates by itself. Please install the update with VitaShell.");
    return -1;
  }

  /* the new version, unpacked */
  remove_tree(UPD_PKG_DIR);
  remove_tree(UPD_HELPER_DIR);
  unpack_ctx uc = {0, 0, 0};
  upd_zip_each(vpk, vlen, count_one, &uc);
  int bad = upd_zip_each(vpk, vlen, unpack_one, &uc) != 0;
  bad = bad || write_file(UPD_PKG_DIR "/" HEAD_BIN, head, hl);
  /* the helper app's package */
  bad = bad || write_file(UPD_HELPER_DIR "/eboot.bin", helper_eboot, hel) ||
        write_file(UPD_HELPER_DIR "/sce_sys/param.sfo", helper_sfo, hsl) ||
        write_file(UPD_HELPER_DIR "/" HEAD_BIN, helper_head, hl);
  free(head);
  free(helper_head);
  free(helper_eboot);
  free(helper_sfo);
  if (bad) {
    remove_tree(UPD_PKG_DIR);
    remove_tree(UPD_HELPER_DIR);
    fail(uc.failed ? "The update could not be written (is the memory card full?)." : "The downloaded update is damaged.");
    return -1;
  }
  note("unpacked %d files into " UPD_PKG_DIR, uc.written);
  work_pct = 94;

  /* install the helper app */
  int r = upd_promoter_start();
  if (r >= 0) {
    r = scePromoterUtilityPromotePkgWithRif(UPD_HELPER_DIR, 1);
    upd_promoter_stop();
  }
  note("installing the update helper: %08x", (unsigned)r);
  if (r < 0) {
    remove_tree(UPD_PKG_DIR);
    remove_tree(UPD_HELPER_DIR);
    snprintf(work_error, sizeof(work_error), "The update could not be installed (error %08X). Please install it with VitaShell.",
             (unsigned)r);
    note("%s", work_error);
    return -1;
  }
  return 0;
}

static int work_thread(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  int ok = 0;
  work_phase = 0;
  if (download(vpk_url, UPD_FILE) == 0) {
    work_phase = 1;
    work_pct = 82;
    uint32_t vlen = 0;
    uint8_t *vpk = read_file(UPD_FILE, &vlen);
    if (!vpk) {
      fail("The downloaded update could not be read.");
    } else {
      work_pct = 85;
      ok = install(vpk, vlen) == 0;
      free(vpk);
      if (ok)
        write_mark(new_tag, 0); /* the next start checks that it took effect */
    }
    sceIoRemove(UPD_FILE);
  }
  net_stop();
  work_pct = 100;
  work_ok = ok;
  work_done = 1;
  return sceKernelExitDeleteThread(0);
}

/* ---- pop-ups (game's thread) ---- */

static void dialog_setup(void) {
  static int done;
  if (done)
    return;
  done = 1;
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
}

static char dialog_text[512];

static int show_message(int buttons, const char *text) {
  dialog_setup();
  snprintf(dialog_text, sizeof(dialog_text), "%s", text);
  static SceMsgDialogUserMessageParam um;
  memset(&um, 0, sizeof(um));
  um.buttonType = buttons;
  um.msg = (const SceChar8 *)dialog_text;
  SceMsgDialogParam mp;
  sceMsgDialogParamInit(&mp);
  mp.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
  mp.userMsgParam = &um;
  int r = sceMsgDialogInit(&mp);
  if (r < 0)
    so_log("[update] the pop-up could not open (%08x)", (unsigned)r);
  return r;
}

static int show_progress(const char *text) {
  dialog_setup();
  snprintf(dialog_text, sizeof(dialog_text), "%s", text);
  static SceMsgDialogProgressBarParam pb;
  memset(&pb, 0, sizeof(pb));
  pb.barType = SCE_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
  pb.msg = (const SceChar8 *)dialog_text;
  SceMsgDialogParam mp;
  sceMsgDialogParamInit(&mp);
  mp.mode = SCE_MSG_DIALOG_MODE_PROGRESS_BAR;
  mp.progBarParam = &pb;
  int r = sceMsgDialogInit(&mp);
  if (r < 0)
    so_log("[update] the progress pop-up could not open (%08x)", (unsigned)r);
  return r;
}

/* the update is unpacked and the helper app installed: start it (it installs
 * the update and starts the game again) and close */
static void start_helper(void) {
  note("starting the update helper");
  so_log_flush();
  /* as VitaShell starts its updater app */
  sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" UPD_HELPER_ID);
  sceKernelExitProcess(0);
}

int updater_busy(void) { return state == ST_ASKING || state == ST_WORKING || state == ST_CLOSING || state == ST_RESULT; }

void updater_frame(void) {
  switch (state) {
  case ST_WAIT:
    /* straight away, on the first frame: the check runs while the game loads */
    {
      SceUID t = sceKernelCreateThread("zt_update_check", check_thread, 0x10000100 + 30, 0x8000, 0, 0, NULL);
      if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
        state = ST_DONE;
        return;
      }
    }
    state = ST_CHECKING;
    return;

  case ST_FOUND: {
    /* ask at once (over the loading screen or a menu), but never in the middle
     * of a run or over another pop-up */
    if (g_in_run || ime_busy() || popup_is_open())
      return;
    char text[SCE_MSG_DIALOG_USER_MSG_SIZE];
    if (tell_failed) {
      /* installed on an earlier start, but this is still the old version */
      net_stop();
      snprintf(text, sizeof(text),
               "The update to %s did not take effect.\n\n"
               "Please install it with VitaShell:\ngithub.com/" UPDATE_REPO,
               new_tag);
      write_mark(new_tag, 1); /* told once: not offered again */
      if (show_message(SCE_MSG_DIALOG_BUTTON_TYPE_OK, text) < 0) {
        state = ST_DONE;
        return;
      }
      work_ok = 0;
      state = ST_RESULT;
      return;
    }
    /* the Vita refuses some messages (error 80100A01, the pop-up never shows):
     * if it does, the question is asked again written more simply */
    if (!news[0] && style < 2)
      style = 2;
    if (style == 0) {
      snprintf(text, sizeof(text),
               "Zombie Tsunami Vita %s is available! (You have v%s.)\n\n"
               "What's new:\n%s\n\n"
               "Install it now? Your progress is kept.",
               new_tag, PORT_VERSION, news);
    } else if (style == 1) {
      /* the list on one line: "a, b, c." */
      char line[sizeof(news)];
      int n = 0;
      for (const char *p = news; *p && n < (int)sizeof(line) - 3;) {
        if (p[0] == '-' && p[1] == ' ')
          p += 2;
        while (*p && *p != '\n' && n < (int)sizeof(line) - 3)
          line[n++] = *p++;
        if (*p == '\n') {
          p++;
          if (*p)
            line[n++] = ',', line[n++] = ' ';
        }
      }
      line[n] = 0;
      snprintf(text, sizeof(text),
               "Zombie Tsunami Vita %s is available! (You have v%s.)\n\n"
               "What's new: %s.\n\n"
               "Install it now? Your progress is kept.",
               new_tag, PORT_VERSION, line);
    } else {
      snprintf(text, sizeof(text),
               "Zombie Tsunami Vita %s is available! (You have v%s.)\n\n"
               "Install it now? Your progress is kept.",
               new_tag, PORT_VERSION);
    }
    if (show_message(SCE_MSG_DIALOG_BUTTON_TYPE_YESNO, text) < 0) {
      net_stop();
      state = ST_DONE;
      return;
    }
    state = ST_ASKING;
    return;
  }

  case ST_ASKING: {
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
      return;
    SceMsgDialogResult res;
    memset(&res, 0, sizeof(res));
    sceMsgDialogGetResult(&res);
    sceMsgDialogTerm();
    if (res.result < 0 && style < 2) {
      /* the Vita refused the message: write it more simply */
      note("the Vita refused pop-up style %d (%08x): trying a simpler one", style, (unsigned)res.result);
      style++;
      state = ST_FOUND;
      return;
    }
    if (res.buttonId != SCE_MSG_DIALOG_BUTTON_ID_YES && res.buttonId != SCE_MSG_DIALOG_BUTTON_ID_NO && !asked) {
      /* closed without an answer: ask once more */
      note("the question closed without an answer (result %d, button %d): asking again", (int)res.result,
           (int)res.buttonId);
      asked = 1;
      state = ST_FOUND;
      return;
    }
    if (res.buttonId != SCE_MSG_DIALOG_BUTTON_ID_YES) {
      note("the player chose not to update now (pop-up style %d, result %08x, button %d)", style, (unsigned)res.result,
           (int)res.buttonId);
      net_stop();
      state = ST_DONE;
      return;
    }
    work_pct = work_done = work_ok = 0;
    work_error[0] = 0;
    shown_pct = shown_phase = -1;
    if (show_progress("Downloading the update...") < 0) {
      net_stop();
      state = ST_DONE;
      return;
    }
    SceUID t = sceKernelCreateThread("zt_update_work", work_thread, 0x10000100 + 30, 0x10000, 0, 0, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
      fail("The update could not start.");
      work_done = 1;
    }
    note("the player chose to update to %s (pop-up style %d)", new_tag, style);
    state = ST_WORKING;
    return;
  }

  case ST_WORKING:
    if (work_pct != shown_pct) {
      shown_pct = work_pct;
      sceMsgDialogProgressBarSetValue(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT, (SceUInt32)shown_pct);
    }
    if (work_phase != shown_phase) {
      shown_phase = work_phase;
      if (shown_phase == 1)
        sceMsgDialogProgressBarSetMsg(SCE_MSG_DIALOG_PROGRESSBAR_TARGET_BAR_DEFAULT,
                                      (const SceChar8 *)"Installing the update...\nThe game will restart.");
    }
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    if (work_done) {
      sceMsgDialogClose();
      state = ST_CLOSING;
    }
    return;

  case ST_CLOSING:
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
      return;
    sceMsgDialogTerm();
    if (work_ok)
      start_helper(); /* straight on: the progress bar said the game restarts */
    {
      char text[300];
      snprintf(text, sizeof(text),
                 "%s\n\nYou can keep playing this version. The game will offer the update again next time.",
                 work_error[0] ? work_error : "The update could not be installed.");
      if (show_message(SCE_MSG_DIALOG_BUTTON_TYPE_OK, text) < 0) {
        state = ST_DONE;
        return;
      }
    }
    state = ST_RESULT;
    return;

  case ST_RESULT:
    if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
      return;
    sceMsgDialogTerm();
    if (work_ok)
      start_helper();
    state = ST_DONE;
    return;

  default:
    return;
  }
}
