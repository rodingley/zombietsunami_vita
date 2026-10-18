/* update_util.c -- see update_util.h */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "update_util.h"
#include "zlib/zlib.h"

/* ---- versions ---- */

/* "v1.2.3-beta" -> {1,2,3,0}; returns how many numbers were read */
static int parse_version(const char *s, int v[4]) {
  int n = 0;
  memset(v, 0, 4 * sizeof(int));
  if (!s)
    return 0;
  while (*s == ' ')
    s++;
  if (*s == 'v' || *s == 'V')
    s++;
  while (n < 4 && isdigit((unsigned char)*s)) {
    int x = 0;
    while (isdigit((unsigned char)*s) && x < 100000)
      x = x * 10 + (*s++ - '0');
    v[n++] = x;
    if (*s != '.')
      break;
    s++;
  }
  return n;
}

int upd_version_newer(const char *tag, const char *ours) {
  int a[4], b[4];
  if (!parse_version(tag, a) || !parse_version(ours, b))
    return 0;
  for (int i = 0; i < 4; i++)
    if (a[i] != b[i])
      return a[i] > b[i];
  return 0;
}

/* ---- GitHub's answer (JSON) ---- */

/* copies the JSON string that starts at p (just after its opening quote) */
static int copy_json_string(const char *p, char *out, int outlen) {
  int n = 0;
  while (*p && *p != '"') {
    char c = *p++;
    if (c == '\\') {
      c = *p++;
      if (c == 'n')
        c = '\n';
      else if (c == 't')
        c = '\t';
      else if (c == 'u') { /* \uXXXX: not needed for what we read; keep it out */
        for (int i = 0; i < 4 && *p; i++)
          p++;
        c = '?';
      } else if (!c)
        break;
    }
    if (n < outlen - 1)
      out[n++] = c;
    else
      return -1; /* too long */
  }
  out[n] = 0;
  return *p == '"' ? 0 : -1;
}

/* finds "key" : "  and returns a pointer just after the value's opening quote */
static const char *find_key(const char *json, const char *key) {
  size_t kl = strlen(key);
  const char *p = json;
  while ((p = strchr(p, '"')) != NULL) {
    if (!strncmp(p + 1, key, kl) && p[1 + kl] == '"') {
      const char *q = p + 2 + kl;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
        q++;
      if (*q == ':') {
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
          q++;
        if (*q == '"')
          return q + 1;
      }
    }
    p++;
  }
  return NULL;
}

int upd_json_string(const char *json, const char *key, char *out, int outlen) {
  const char *p = json ? find_key(json, key) : NULL;
  if (!p)
    return -1;
  return copy_json_string(p, out, outlen);
}

static int ends_with(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  if (a < b)
    return 0;
  for (size_t i = 0; i < b; i++)
    if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suf[i]))
      return 0;
  return 1;
}

int upd_json_number(const char *json, const char *key, long *out) {
  size_t kl = strlen(key);
  const char *p = json;
  while (p && (p = strchr(p, '"')) != NULL) {
    if (!strncmp(p + 1, key, kl) && p[1 + kl] == '"') {
      const char *q = p + 2 + kl;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
        q++;
      if (*q == ':') {
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
          q++;
        if (isdigit((unsigned char)*q)) {
          long v = 0;
          while (isdigit((unsigned char)*q) && v < 100000000L)
            v = v * 10 + (*q++ - '0');
          *out = v;
          return 0;
        }
      }
    }
    p++;
  }
  return -1;
}

int upd_tag_ok(const char *tag) {
  int n = 0;
  for (; tag[n]; n++)
    if (!isalnum((unsigned char)tag[n]) && tag[n] != '.' && tag[n] != '-' && tag[n] != '_')
      return 0;
  return n > 0 && n < 32;
}

/* ---- param.sfo ---- */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int upd_sfo_string(const uint8_t *sfo, uint32_t len, const char *key, char *out, int outlen) {
  if (len < 20 || memcmp(sfo, "\0PSF", 4))
    return -1;
  uint32_t keyofs = rd32(sfo + 8), valofs = rd32(sfo + 12), count = rd32(sfo + 16);
  if (count > 1000 || 20 + count * 16 > len)
    return -1;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t *e = sfo + 20 + i * 16;
    uint32_t name = keyofs + rd16(e), vsize = rd32(e + 4), data = valofs + rd32(e + 12);
    if (name >= len || data >= len || data + vsize > len)
      return -1;
    if (strncmp((const char *)sfo + name, key, len - name) != 0)
      continue;
    int n = 0;
    while (n < (int)vsize && n < outlen - 1 && sfo[data + n])
      out[n] = (char)sfo[data + n], n++;
    out[n] = 0;
    return 0;
  }
  return -1;
}

/* ---- zip ---- */

typedef struct {
  char name[256];
  int method;
  uint32_t crc, csize, usize;
  const uint8_t *data; /* compressed data */
} zentry;

/* the End Of Central Directory record */
static int zip_dir(const uint8_t *z, uint32_t len, uint32_t *cd_off, uint32_t *count) {
  if (len < 22)
    return -1;
  uint32_t stop = len > 22 + 65535 ? len - 22 - 65535 : 0;
  for (uint32_t i = len - 22 + 1; i-- > stop;) {
    if (rd32(z + i) == 0x06054b50) {
      *count = rd16(z + i + 10);
      *cd_off = rd32(z + i + 16);
      return *cd_off < len ? 0 : -1;
    }
  }
  return -1;
}

static int name_ok(const char *n) {
  if (!n[0] || n[0] == '/' || n[0] == '\\' || strchr(n, ':'))
    return 0;
  for (const char *p = n; *p; p++)
    if (p[0] == '.' && p[1] == '.' && (p == n || p[-1] == '/' || p[-1] == '\\') &&
        (p[2] == 0 || p[2] == '/' || p[2] == '\\'))
      return 0;
  return 1;
}

/* entry number i (walks the central directory from *pos) */
static int zip_next(const uint8_t *z, uint32_t len, uint32_t *pos, zentry *e) {
  uint32_t p = *pos;
  if (p + 46 > len || rd32(z + p) != 0x02014b50)
    return -1;
  e->method = rd16(z + p + 10);
  e->crc = rd32(z + p + 16);
  e->csize = rd32(z + p + 20);
  e->usize = rd32(z + p + 24);
  uint32_t nl = rd16(z + p + 28), xl = rd16(z + p + 30), cl = rd16(z + p + 32), lho = rd32(z + p + 42);
  if (p + 46 + nl > len || nl >= sizeof(e->name))
    return -1;
  memcpy(e->name, z + p + 46, nl);
  e->name[nl] = 0;
  *pos = p + 46 + nl + xl + cl;
  if (lho + 30 > len || rd32(z + lho) != 0x04034b50)
    return -1;
  uint32_t lnl = rd16(z + lho + 26), lxl = rd16(z + lho + 28);
  uint32_t d = lho + 30 + lnl + lxl;
  if (d > len || e->csize > len - d)
    return -1;
  e->data = z + d;
  return 0;
}

/* unpacks an entry into a malloc'ed buffer, checking its CRC */
static int zip_unpack(const zentry *e, uint8_t **out) {
  if (e->usize > 64 * 1024 * 1024)
    return -1;
  uint8_t *buf = malloc(e->usize ? e->usize : 1);
  if (!buf)
    return -1;
  if (e->method == 0) {
    if (e->csize != e->usize) {
      free(buf);
      return -1;
    }
    memcpy(buf, e->data, e->usize);
  } else if (e->method == 8) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
      free(buf);
      return -1;
    }
    zs.next_in = (Bytef *)e->data;
    zs.avail_in = e->csize;
    zs.next_out = buf;
    zs.avail_out = e->usize;
    int r = inflate(&zs, Z_FINISH);
    uLong got = zs.total_out;
    inflateEnd(&zs);
    if (r != Z_STREAM_END || got != e->usize) {
      free(buf);
      return -1;
    }
  } else {
    free(buf);
    return -1;
  }
  if (crc32(crc32(0L, Z_NULL, 0), buf, e->usize) != e->crc) {
    free(buf);
    return -1;
  }
  *out = buf;
  return 0;
}

static int is_dir(const char *name) {
  size_t n = strlen(name);
  return n && (name[n - 1] == '/' || name[n - 1] == '\\');
}

int upd_zip_count(const uint8_t *zip, uint32_t len) {
  uint32_t cd, count, pos;
  if (zip_dir(zip, len, &cd, &count))
    return -1;
  pos = cd;
  int files = 0;
  for (uint32_t i = 0; i < count; i++) {
    zentry e;
    if (zip_next(zip, len, &pos, &e))
      return -1;
    if (!is_dir(e.name))
      files++;
  }
  return files;
}

int upd_zip_each(const uint8_t *zip, uint32_t len, upd_zip_fn fn, void *ctx) {
  uint32_t cd, count, pos;
  if (zip_dir(zip, len, &cd, &count))
    return -1;
  pos = cd;
  for (uint32_t i = 0; i < count; i++) {
    zentry e;
    if (zip_next(zip, len, &pos, &e))
      return -1;
    if (is_dir(e.name))
      continue;
    if (!name_ok(e.name))
      return -1;
    uint8_t *data;
    if (zip_unpack(&e, &data))
      return -1;
    int r = fn(ctx, e.name, data, e.usize);
    free(data);
    if (r)
      return r;
  }
  return 0;
}

int upd_zip_find(const uint8_t *zip, uint32_t len, const char *suffix, int exact, uint8_t **out, uint32_t *outlen) {
  uint32_t cd, count, pos;
  if (zip_dir(zip, len, &cd, &count))
    return -1;
  pos = cd;
  for (uint32_t i = 0; i < count; i++) {
    zentry e;
    if (zip_next(zip, len, &pos, &e))
      return -1;
    if (is_dir(e.name))
      continue;
    int match = exact ? (strlen(e.name) == strlen(suffix) && ends_with(e.name, suffix)) : ends_with(e.name, suffix);
    if (!match)
      continue;
    if (zip_unpack(&e, out))
      return -1;
    *outlen = e.usize;
    return 0;
  }
  return -1;
}
