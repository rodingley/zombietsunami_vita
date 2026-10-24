/* update_util.c -- see update_util.h */

#include <ctype.h>
#include <stdio.h>
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

/* one Unicode character as UTF-8 (BMP only); returns the bytes written */
static int put_utf8(unsigned c, char *o) {
  if (c < 0x80) {
    o[0] = (char)c;
    return 1;
  }
  if (c < 0x800) {
    o[0] = (char)(0xc0 | (c >> 6));
    o[1] = (char)(0x80 | (c & 0x3f));
    return 2;
  }
  o[0] = (char)(0xe0 | (c >> 12));
  o[1] = (char)(0x80 | ((c >> 6) & 0x3f));
  o[2] = (char)(0x80 | (c & 0x3f));
  return 3;
}

int upd_json_text(const char *json, const char *key, char *out, int outlen) {
  const char *p = json ? find_key(json, key) : NULL;
  if (!p || outlen < 1)
    return -1;
  int n = 0;
  while (*p && *p != '"') {
    char tmp[4];
    int k = 0;
    char c = *p++;
    if (c == '\\') {
      c = *p++;
      if (!c)
        break;
      if (c == 'n')
        tmp[k++] = '\n';
      else if (c == 'r')
        ; /* dropped: lines end with \n */
      else if (c == 't')
        tmp[k++] = ' ';
      else if (c == 'u') {
        unsigned v = 0;
        int i = 0;
        for (; i < 4 && isxdigit((unsigned char)*p); i++, p++)
          v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
        if (i == 4 && (v < 0xd800 || v > 0xdfff))
          k = put_utf8(v ? v : '?', tmp);
      } else
        tmp[k++] = c;
    } else
      tmp[k++] = c;
    if (n + k > outlen - 1)
      break; /* cut to fit */
    memcpy(out + n, tmp, (size_t)k);
    n += k;
  }
  out[n] = 0;
  return 0;
}

static int has_whats_new(const char *line, int len) {
  char low[96];
  int n = 0;
  for (int i = 0; i < len && n < (int)sizeof(low) - 1; i++) {
    unsigned char c = (unsigned char)line[i];
    if (c == '\'' || c == 0xe2 || c == 0x80 || c == 0x99) /* ' and the UTF-8 bytes of a curly apostrophe */
      continue;
    low[n++] = (char)tolower(c);
  }
  low[n] = 0;
  return strstr(low, "whats new") != NULL;
}

int upd_whats_new(const char *body, char *out, int outlen, int max_items) {
  int n = 0, items = 0, in_list = 0;
  if (outlen > 0)
    out[0] = 0;
  if (!body || outlen < 8)
    return -1;
  const char *p = body;
  while (*p) {
    const char *eol = strchr(p, '\n');
    int len = eol ? (int)(eol - p) : (int)strlen(p);
    const char *s = p;
    int sl = len;
    while (sl && (*s == ' ' || *s == '\t'))
      s++, sl--;
    if (!in_list) {
      if (has_whats_new(s, sl))
        in_list = 1;
    } else if (sl >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') {
      if (items < max_items) {
        /* "- " + the text without markdown marks, cut to fit */
        char line[160];
        int ln = 0;
        line[ln++] = '-';
        line[ln++] = ' ';
        for (int i = 2; i < sl && ln < (int)sizeof(line) - 2; i++) {
          char c = s[i];
          if (c == '*' || c == '`' || c == '_' || c == '#')
            continue;
          line[ln++] = c;
        }
        while (ln > 2 && line[ln - 1] == ' ')
          ln--;
        if (n + ln + 1 >= outlen)
          break;
        if (n)
          out[n++] = '\n';
        memcpy(out + n, line, (size_t)ln);
        n += ln;
        out[n] = 0;
        items++;
      }
    } else if (sl == 0) {
      if (items)
        break; /* blank line after the list: done */
    } else if (items || s[0] == '#') {
      break; /* the list is over (or another heading came first) */
    }
    if (!eol)
      break;
    p = eol + 1;
  }
  return items ? 0 : -1;
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

/* ---- head.bin (the package header the Vita's installer needs) ---- */

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const uint8_t *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 80; i++)
    w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20)
      f = (b & c) | (~b & d), k = 0x5A827999;
    else if (i < 40)
      f = b ^ c ^ d, k = 0x6ED9EBA1;
    else if (i < 60)
      f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
    else
      f = b ^ c ^ d, k = 0xCA62C1D6;
    uint32_t t = ROL(a, 5) + f + e + k + w[i];
    e = d, d = c, c = ROL(b, 30), b = a, a = t;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
}

void upd_sha1(const uint8_t *data, uint32_t len, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  uint32_t i = 0;
  for (; i + 64 <= len; i += 64)
    sha1_block(h, data + i);
  uint8_t last[128];
  uint32_t rest = len - i;
  memset(last, 0, sizeof(last));
  memcpy(last, data + i, rest);
  last[rest] = 0x80;
  uint32_t blocks = rest + 9 > 64 ? 2 : 1;
  uint64_t bits = (uint64_t)len * 8;
  for (int b = 0; b < 8; b++)
    last[blocks * 64 - 1 - b] = (uint8_t)(bits >> (8 * b));
  for (uint32_t b = 0; b < blocks; b++)
    sha1_block(h, last + 64 * b);
  for (int b = 0; b < 5; b++) {
    out[4 * b] = (uint8_t)(h[b] >> 24);
    out[4 * b + 1] = (uint8_t)(h[b] >> 16);
    out[4 * b + 2] = (uint8_t)(h[b] >> 8);
    out[4 * b + 3] = (uint8_t)h[b];
  }
}

/* the checksum used in the header of a home-made ("fake") package, as VitaShell
 * writes it: SHA-1 of the data, rearranged, SHA-1 again, first 16 bytes */
static void head_sum(const uint8_t *data, uint32_t len, uint8_t out[16]) {
  uint8_t d[20], blk[64];
  upd_sha1(data, len, d);
  memset(blk, 0, sizeof(blk));
  memcpy(blk, d + 4, 8);
  memcpy(blk + 8, d + 4, 8);
  memcpy(blk + 16, d + 12, 4);
  blk[20] = d[16];
  blk[21] = d[1];
  blk[22] = d[2];
  blk[23] = d[3];
  memcpy(blk + 24, blk + 16, 8);
  upd_sha1(blk, 64, d);
  memcpy(out, d, 16);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* the three checksums of a head.bin (header, package info, everything), in
 * this order because the later ones cover the earlier ones.  write = 0: 1 if
 * they are all right, 0 if not; write = 1: puts them in.  -1 = not a head.bin */
static int head_sums(uint8_t *h, uint32_t len, int write) {
  if (len < 0x100)
    return -1;
  uint32_t hdr = be32(h + 0xD0), off = be32(h + 0x08), info = be32(h + 0x10), info_out = be32(h + 0xD4),
           all = be32(h + 0xE8);
  if (hdr + 16 > len || info < 64 || off > len || info - 64 > len - off || info_out + 16 > len || all + 16 > len)
    return -1;
  int ok = 1;
  uint8_t m[16];
  head_sum(h, hdr, m);
  if (write)
    memcpy(h + hdr, m, 16);
  else if (memcmp(h + hdr, m, 16))
    ok = 0;
  head_sum(h + off, info - 64, m);
  if (write)
    memcpy(h + info_out, m, 16);
  else if (memcmp(h + info_out, m, 16))
    ok = 0;
  head_sum(h, all, m);
  if (write)
    memcpy(h + all, m, 16);
  else if (memcmp(h + all, m, 16))
    ok = 0;
  return ok;
}

int upd_head_make(uint8_t *head, uint32_t len, const char *title_id, int *was_ok) {
  int ok = head_sums(head, len, 0);
  if (ok < 0 || strlen(title_id) != 9)
    return -1;
  if (was_ok)
    *was_ok = ok;
  /* the content ID, at 0x30 (48 bytes): the Vita's installer registers the app
   * under the title ID in it (not the one in param.sfo) */
  char cid[49];
  snprintf(cid, sizeof(cid), "EP9000-%s_00-0000000000000000", title_id);
  memset(head + 0x30, 0, 48);
  memcpy(head + 0x30, cid, strlen(cid));
  head_sums(head, len, 1);
  return 0;
}
