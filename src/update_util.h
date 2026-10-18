/* update_util.h -- the parts of the auto-updater that do not touch the Vita
 * (so they can be tested on a computer): reading GitHub's answer, comparing
 * versions, reading param.sfo and unzipping. */
#ifndef UPDATE_UTIL_H
#define UPDATE_UTIL_H

#include <stdint.h>

/* 1 if the release tag (e.g. "v1.2" or "1.2.1") is newer than our version
 * (e.g. "1.0"); 0 if not or if the tag is not a version number */
int upd_version_newer(const char *tag, const char *ours);

/* the string value of "key" in a JSON text (first one found); 0 = found */
int upd_json_string(const char *json, const char *key, char *out, int outlen);

/* the number value of "key" in a JSON text (first one found); 0 = found */
int upd_json_number(const char *json, const char *key, long *out);

/* 1 if a release tag is safe to put in an address (letters, digits, . - _ only) */
int upd_tag_ok(const char *tag);

/* a value from a param.sfo (e.g. "TITLE_ID"); 0 = found */
int upd_sfo_string(const uint8_t *sfo, uint32_t len, const char *key, char *out, int outlen);

/* zip files (a .vpk is a zip).  Names are checked: no "..", no absolute paths. */
typedef int (*upd_zip_fn)(void *ctx, const char *name, const uint8_t *data, uint32_t len);
/* calls fn for every file in the zip (folders are skipped) with its unpacked
 * contents (CRC checked); stops at the first fn that returns non-zero and
 * returns that.  -1 = the zip is broken. */
int upd_zip_each(const uint8_t *zip, uint32_t len, upd_zip_fn fn, void *ctx);
/* the unpacked contents of the first file whose name ends with `suffix`
 * (exact = 1: whose name is `suffix`), ignoring case; *out is malloc'ed.  0 = found */
int upd_zip_find(const uint8_t *zip, uint32_t len, const char *suffix, int exact, uint8_t **out, uint32_t *outlen);
/* number of files in the zip, -1 if broken */
int upd_zip_count(const uint8_t *zip, uint32_t len);

#endif
