#ifndef CONFIG_H
#define CONFIG_H

/* the port's version: the auto-updater (updater.c) offers a GitHub release
 * whose tag (e.g. "v1.1") is newer than this.  Keep VITA_VERSION in
 * CMakeLists.txt in step ("01.00" = 1.0). */
#define PORT_VERSION "1.0"
#define PORT_TITLE_ID "ZMBT00001"              /* = VITA_TITLEID in CMakeLists.txt */
#define UPDATE_REPO "rodingley/zombietsunami_vita" /* where the releases are */
/* the VPK of each release, committed in the repository at that release's tag
 * (the Vita cannot reach GitHub's release file servers, only api.github.com) */
#define UPDATE_VPK_PATH "release/zombietsunami.vpk"

/* Where the user puts the files on the Vita */
#define DATA_PATH "ux0:data/zombietsunami"
#define LIB_PATH DATA_PATH "/lib"
#define LOG_PATH DATA_PATH "/log.txt"
#define ASSET_PATH DATA_PATH "/assets"

/* the port's own promo codes, typed in the shop's Redeem box (ime.c); each
 * works every time it is typed, letters in any case */
#define PROMO_CODE_DIAMONDS "rodi" /* 100,000 diamonds */
#define PROMO_CODE_COINS "vita"    /* 100,000 coins */

/* Fixed load addresses, 16 MB apart. Order matters: a library must be
 * loaded after the libraries it needs. */
#define LOAD_ADDR_LIBCXX 0x98000000
#define LOAD_ADDR_FMOD 0x99000000
#define LOAD_ADDR_FMODSTUDIO 0x9A000000
#define LOAD_ADDR_GAME 0x9B000000

#endif
