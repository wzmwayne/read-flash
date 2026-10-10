/* CRC32 / FNV-1a / 进度文件编解码（v3.4 §9.1、§9.3） */
#include "rf.h"

u32 rf_crc32(const u8 *p, size_t n)
{
    u32 crc = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (u32)(-(i32)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

u64 rf_fnv1a(const void *p, size_t n)
{
    const u8 *b = (const u8 *)p;
    u64 h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

u64 rf_fnv1a_u64(u64 h, u64 v)
{
    int i;
    for (i = 0; i < 8; i++) { h ^= (u8)(v >> (i * 8)); h *= 1099511628211ULL; }
    return h;
}

/* ---------------- 存储文件格式（ASCII 十六进制行文本） ---------------- */

static int hex_put(char *dst, u64 v, int digits)
{
    static const char *H = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--) { dst[i] = H[v & 0xF]; v >>= 4; }
    return digits;
}

static int hex_get(const char *src, size_t len, int digits, u64 *out)
{
    u64 v = 0;
    int i;
    if (len < (size_t)digits) return -1;
    for (i = 0; i < digits; i++) {
        char c = src[i];
        u64 d;
        if (c >= '0' && c <= '9') d = (u64)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (u64)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (u64)(c - 'a' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return digits;
}

static int put_line(char *buf, size_t cap, size_t *off, const char *s, int n)
{
    if (*off + (size_t)n + 1 > cap) return -1;
    { int i; for (i = 0; i < n; i++) buf[*off + i] = s[i]; }
    *off += (size_t)n;
    buf[(*off)++] = '\n';
    return 0;
}

int rf_progress_encode(char *buf, size_t cap,
                       u64 source_line, u64 line_offset, u64 total_lines, u64 ts)
{
    size_t off = 0;
    char tmp[24];
    if (put_line(buf, cap, &off, "RFPROG001", 9)) return -1;
    hex_put(tmp, source_line, 16); if (put_line(buf, cap, &off, tmp, 16)) return -1;
    hex_put(tmp, line_offset, 16); if (put_line(buf, cap, &off, tmp, 16)) return -1;
    hex_put(tmp, total_lines, 16); if (put_line(buf, cap, &off, tmp, 16)) return -1;
    hex_put(tmp, ts, 16);          if (put_line(buf, cap, &off, tmp, 16)) return -1;
    {
        u32 crc = rf_crc32((const u8 *)buf, off);
        hex_put(tmp, crc, 8);
        if (put_line(buf, cap, &off, tmp, 8)) return -1;
    }
    return (int)off;
}

/* 按行切分 */
static int take_line(const char *buf, size_t len, size_t *off, const char **p, size_t *n)
{
    size_t s = *off, e;
    if (s >= len) return -1;
    e = s;
    while (e < len && buf[e] != '\n') e++;
    if (e >= len) return -1;              /* 最后一行也必须以 \n 结尾 */
    *p = buf + s; *n = e - s; *off = e + 1;
    return 0;
}

int rf_progress_decode(const char *buf, size_t len,
                       u64 *source_line, u64 *line_offset, u64 *total_lines)
{
    size_t off = 0, n;
    const char *p;
    u64 v;
    char magic[9];
    size_t crc_off;

    if (take_line(buf, len, &off, &p, &n)) return -1;
    if (n != 9) return -1;
    { int i; for (i = 0; i < 9; i++) magic[i] = p[i]; }
    if (magic[0] != 'R' || magic[1] != 'F' || magic[2] != 'P') return -1;

    if (take_line(buf, len, &off, &p, &n)) return -1;
    if (hex_get(p, n, 16, &v) < 0) return -1;
    *source_line = v;
    if (take_line(buf, len, &off, &p, &n)) return -1;
    if (hex_get(p, n, 16, &v) < 0) return -1;
    *line_offset = v;
    if (take_line(buf, len, &off, &p, &n)) return -1;
    if (hex_get(p, n, 16, &v) < 0) return -1;
    *total_lines = v;
    if (take_line(buf, len, &off, &p, &n)) return -1;   /* timestamp 不校验内容 */
    crc_off = off;                                       /* CRC 覆盖到校验行之前 */
    if (take_line(buf, len, &off, &p, &n)) return -1;
    if (hex_get(p, n, 8, &v) < 0) return -1;
    if ((u32)v != rf_crc32((const u8 *)buf, crc_off)) return -1;
    return 0;
}
