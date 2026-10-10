/* 存储层（v3.4 §4.1 设置、§9 存储格式、§9.4 索引）
 * 目录：<root>/config、<root>/book/<ID>/{book,.config,.rfidx,.progress}
 * 全部落盘为 ASCII 行文本 + CRC32 末行，写入一律 tmp + rename（原子替换）。
 */
#include "rf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

/* ---------------- 基础工具 ---------------- */

static int write_atomic(const char *path, const void *buf, size_t len)
{
    char tmp[700];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(buf, 1, len, f) != len) { fclose(f); remove(tmp); return -1; }
    if (fclose(f) != 0) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

static u8 *read_file(const char *path, u64 *out_len)
{
    FILE *f = fopen(path, "rb");
    long n;
    u8 *b;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    b = (u8 *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = 0;
    fclose(f);
    *out_len = (u64)n;
    return b;
}

static void join(char *dst, size_t cap, const char *a, const char *b)
{
    snprintf(dst, cap, "%s/%s", a, b);
}

/* 行迭代器：最后一行也必须以 \n 结尾（§9.1） */
typedef struct { const char *p; size_t len, off; } LineIt;
static int li_next(LineIt *it, const char **s, size_t *n)
{
    size_t s0 = it->off, e;
    if (s0 >= it->len) return 0;
    e = s0;
    while (e < it->len && it->p[e] != '\n') e++;
    if (e >= it->len) return 0;
    *s = it->p + s0; *n = e - s0;
    it->off = e + 1;
    return 1;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int hex_get(const char *s, size_t n, int digits, u64 *out)
{
    u64 v = 0;
    int i, d;
    if (n < (size_t)digits) return -1;
    for (i = 0; i < digits; i++) {
        d = hex_val(s[i]);
        if (d < 0) return -1;
        v = (v << 4) | (u64)d;
    }
    *out = v;
    return 0;
}

static size_t put_hex(char *b, size_t off, u64 v, int digits)
{
    static const char *H = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--) { b[off + (size_t)i] = H[v & 15]; v >>= 4; }
    b[off + (size_t)digits] = '\n';
    return off + (size_t)digits + 1;
}

static size_t put_str(char *b, size_t off, const char *s)
{
    size_t n = strlen(s);
    memcpy(b + off, s, n);
    b[off + n] = '\n';
    return off + n + 1;
}

static int crc_ok(const char *buf, size_t crc_line_off, const char *crc_s, size_t crc_n)
{
    u64 v;
    if (hex_get(crc_s, crc_n, 8, &v) < 0) return 0;
    return (u32)v == rf_crc32((const u8 *)buf, crc_line_off);
}

/* ---------------- 设置 ---------------- */

void rf_store_init(RfStore *st, const char *root)
{
    size_t n;
    memset(st, 0, sizeof *st);
    n = strlen(root);
    if (n >= sizeof st->root) n = sizeof st->root - 1;
    memcpy(st->root, root, n);
    rf_settings_default(&st->settings);
}

int rf_settings_load(RfStore *st)
{
    char p[700];
    u8 *b;
    u64 len = 0;
    LineIt it;
    const char *s;
    size_t n;
    u64 v;
    int line = 0;

    rf_settings_default(&st->settings);
    join(p, sizeof p, st->root, "config");
    b = read_file(p, &len);
    if (!b) return 1;                                   /* 无配置：用默认值 */
    it.p = (const char *)b; it.len = (size_t)len; it.off = 0;
    while (li_next(&it, &s, &n)) {
        line++;
        if (line == 1) {
            if (n != 8 || memcmp(s, "RFSET002", 8) != 0) { free(b); return 1; }
        } else if (line >= 2 && line <= 8) {
            if (hex_get(s, n, 4, &v) < 0) { free(b); return 1; }
            switch (line) {
            case 2: st->settings.font_dp = (int)v; break;
            case 3: st->settings.line_pitch_dp = (int)v; break;
            case 4: st->settings.line_height_dp = (int)v; break;
            case 5: st->settings.m_top_dp = (int)v; break;
            case 6: st->settings.m_bot_dp = (int)v; break;
            case 7: st->settings.m_left_dp = (int)v; break;
            case 8: st->settings.m_right_dp = (int)v; break;
            }
        } else if (line == 9) {
            if (!crc_ok((const char *)b, (size_t)it.off - n - 1, s, n)) { free(b); return 1; }
        }
    }
    free(b);
    /* 范围钳制 */
    if (st->settings.font_dp < RF_FONT_DP_MIN || st->settings.font_dp > RF_FONT_DP_MAX)
        st->settings.font_dp = RF_FONT_DP_DEFAULT;
    if (st->settings.line_pitch_dp < 0 || st->settings.line_pitch_dp > RF_PITCH_DP_MAX)
        st->settings.line_pitch_dp = 0;
    if (st->settings.line_height_dp < 0 || st->settings.line_height_dp > RF_LINEHT_DP_MAX)
        st->settings.line_height_dp = 0;
    return 0;
}

int rf_settings_save(RfStore *st, const RfSettings *s)
{
    char buf[256], p[700];
    size_t off = 0;
    off = put_str(buf, off, "RFSET002");
    off = put_hex(buf, off, (u64)s->font_dp, 4);
    off = put_hex(buf, off, (u64)s->line_pitch_dp, 4);
    off = put_hex(buf, off, (u64)s->line_height_dp, 4);
    off = put_hex(buf, off, (u64)s->m_top_dp, 4);
    off = put_hex(buf, off, (u64)s->m_bot_dp, 4);
    off = put_hex(buf, off, (u64)s->m_left_dp, 4);
    off = put_hex(buf, off, (u64)s->m_right_dp, 4);
    off = put_hex(buf, off, (u64)rf_crc32((const u8 *)buf, off), 8);
    join(p, sizeof p, st->root, "config");
    return write_atomic(p, buf, off);
}

/* ---------------- 书与索引 ---------------- */

static void book_path(char *dst, size_t cap, const RfStore *st, const char *id, const char *name)
{
    snprintf(dst, cap, "%s/book/%s/%s", st->root, id, name);
}

/* 读 .config：书名、S、L */
static int config_read(const RfStore *st, const char *id, RfBookEnt *e)
{
    char p[700];
    u8 *b;
    u64 len = 0;
    LineIt it;
    const char *s;
    size_t n;
    u64 v;
    int line = 0;

    book_path(p, sizeof p, st, id, ".config");
    b = read_file(p, &len);
    if (!b) return -1;
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    it.p = (const char *)b; it.len = (size_t)len; it.off = 0;
    while (li_next(&it, &s, &n)) {
        line++;
        if (line == 1) {
            if (n != 9 || memcmp(s, "RFBOOK001", 9) != 0) { free(b); return -1; }
        } else if (line == 2) {
            size_t k = n < sizeof e->title - 1 ? n : sizeof e->title - 1;
            memcpy(e->title, s, k);
            e->title[k] = 0;
        } else if (line == 3) {
            if (hex_get(s, n, 16, &v) < 0) { free(b); return -1; }
            e->size = v;
        } else if (line == 5) {
            if (hex_get(s, n, 16, &v) < 0) { free(b); return -1; }
            e->lines = v;
        }
    }
    free(b);
    return 0;
}

int rf_books_scan(RfStore *st, RfBookEnt *out, int max)
{
    char dir[700];
    DIR *d;
    struct dirent *de;
    int n = 0;

    join(dir, sizeof dir, st->root, "book");
    d = opendir(dir);
    if (!d) return 0;
    while ((de = readdir(d)) != NULL && n < max) {
        char full[900];
        struct stat sb;
        if (de->d_name[0] == '.') continue;
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        if (stat(full, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        if (config_read(st, de->d_name, &out[n]) == 0) n++;
    }
    closedir(d);
    return n;
}

static int compute_k(u64 L, u64 *k_out, u64 *n_out)
{
    u64 K = 256, N;
    if (L == 0) { *k_out = 256; *n_out = 0; return 0; }
    {
        u64 x = (L + 4095) / 4096;                      /* ceil(L/4096) */
        u64 p = 1;
        while (p < x) p <<= 1;                          /* 2^ceil(log2(x)) */
        K = p < 256 ? 256 : p;
        if (K > (1u << 24)) K = 1u << 24;
    }
    N = (L + K - 1) / K;
    while (N > RF_MAX_INDEX && K < (1u << 28)) {        /* 防御：缓冲上限 */
        K <<= 1;
        N = (L + K - 1) / K;
    }
    *k_out = K;
    *n_out = N;
    return N <= RF_MAX_INDEX ? 0 : -1;
}

static void index_fill(const u8 *data, u64 len, u64 K, u64 N, u64 *arr)
{
    u64 line = 1, off = 0, j = 0, i;
    if (N == 0) return;
    arr[0] = 0;
    j = 1;
    for (i = 0; i < len && j < N; i++) {
        if (data[i] == '\n') {
            line++;
            off = i + 1;
            if (((line - 1) % K) == 0) arr[j++] = off;  /* line ≡ 1 (mod K) */
        }
    }
    while (j < N) arr[j++] = len;
}

static int index_write(const RfStore *st, const char *id, const RfIndex *ix,
                       const u8 *data, u64 len)
{
    char p[700], *buf;
    size_t cap = 512 + (size_t)ix->N * 17 + 32, off = 0;
    u32 h1, h2;
    u64 n1 = len < 4096 ? len : 4096, n2 = len < 4096 ? len : 4096;
    u64 i;

    buf = (char *)malloc(cap);
    if (!buf) return -1;
    off = put_str(buf, off, "RFIDX001");
    off = put_hex(buf, off, ix->L, 16);
    off = put_hex(buf, off, ix->S, 16);
    h1 = rf_crc32(data, (size_t)n1);
    h2 = rf_crc32(data + (len - n2), (size_t)n2);
    off = put_hex(buf, off, h1, 8);
    off = put_hex(buf, off, h2, 8);
    off = put_hex(buf, off, ix->K, 8);
    off = put_hex(buf, off, ix->N, 8);
    for (i = 0; i < ix->N; i++) off = put_hex(buf, off, ix->arr[i], 16);
    off = put_hex(buf, off, (u64)rf_crc32((const u8 *)buf, off), 8);
    book_path(p, sizeof p, st, id, ".rfidx");
    i = (u64)write_atomic(p, buf, off);
    free(buf);
    return (int)i;
}

int rf_index_build(RfStore *st, const char *id, const u8 *data, u64 len, RfIndex *ix,
                   u64 *storage, int write_file)
{
    u64 K, N;
    if (compute_k(rf_count_lines(data, len), &K, &N) != 0) return -1;
    ix->L = rf_count_lines(data, len);
    ix->S = len;
    ix->K = K;
    ix->N = N;
    ix->arr = storage;
    index_fill(data, len, K, N, storage);
    if (write_file) return index_write(st, id, ix, data, len);
    return 0;
}

int rf_index_load(RfStore *st, const char *id, const u8 *data, u64 len, RfIndex *ix,
                  u64 *storage)
{
    char p[700];
    u8 *b;
    u64 flen = 0, v;
    LineIt it;
    const char *s;
    size_t n;
    u64 line = 0, i = 0;
    u32 h1, h2;
    u64 n1 = len < 4096 ? len : 4096, n2 = len < 4096 ? len : 4096;

    book_path(p, sizeof p, st, id, ".rfidx");
    b = read_file(p, &flen);
    if (!b) return -1;
    it.p = (const char *)b; it.len = (size_t)flen; it.off = 0;
    ix->arr = storage;
    while (li_next(&it, &s, &n)) {
        line++;
        if (line == 1) {
            if (n != 8 || memcmp(s, "RFIDX001", 8) != 0) goto bad;
        } else if (line == 2) { if (hex_get(s, n, 16, &ix->L) < 0) goto bad; }
        else if (line == 3)   { if (hex_get(s, n, 16, &ix->S) < 0) goto bad; }
        else if (line == 4)   { if (hex_get(s, n, 8, &v) < 0) goto bad; h1 = (u32)v; }
        else if (line == 5)   { if (hex_get(s, n, 8, &v) < 0) goto bad; h2 = (u32)v; }
        else if (line == 6)   { if (hex_get(s, n, 8, &v) < 0) goto bad; ix->K = v; }
        else if (line == 7)   { if (hex_get(s, n, 8, &v) < 0) goto bad; ix->N = v; }
        else if (line >= 8 && i < ix->N) {
            if (hex_get(s, n, 16, &storage[i]) < 0) goto bad;
            i++;
        } else {
            /* 校验行 */
            if (!crc_ok((const char *)b, (size_t)it.off - n - 1, s, n)) goto bad;
            free(b);
            /* 自校验（§9.4 八项） */
            if (ix->K < 256 || ix->K > (1u << 24) || (ix->K & (ix->K - 1)) != 0) return -2;
            if (ix->S != len) return -2;
            if (ix->N != (ix->L == 0 ? 0 : (ix->L + ix->K - 1) / ix->K)) return -2;
            if (ix->N > RF_MAX_INDEX) return -2;
            if (ix->N > 0) {
                u64 k;
                if (storage[0] != 0) return -2;
                for (k = 1; k < ix->N; k++) if (storage[k] <= storage[k - 1]) return -2;
                if (storage[ix->N - 1] >= len) return -2;
            }
            if (rf_crc32(data, (size_t)n1) != h1) return -2;
            if (rf_crc32(data + (len - n2), (size_t)n2) != h2) return -2;
            return 0;
        }
    }
bad:
    free(b);
    return -1;
}

u64 rf_index_locate(const RfIndex *ix, const u8 *data, u64 off_hint, u64 line)
{
    u64 j, pos, remaining, log2k = 0, K = ix->K;
    (void)off_hint;
    if (ix->N == 0 || line == 0) return 0;
    while ((1ull << log2k) < K) log2k++;
    j = (line - 1) >> log2k;
    if (j >= ix->N) j = ix->N - 1;
    pos = ix->arr[j];
    remaining = (line - 1) & (K - 1);
    while (remaining > 0 && pos < ix->S) {
        while (pos < ix->S && data[pos] != '\n') pos++;
        pos++;
        remaining--;
    }
    return pos > ix->S ? ix->S : pos;
}

/* ---------------- 导入 / 删除 / 进度 ---------------- */

static size_t put_hex_raw(char *b, size_t off, u64 v, int digits)
{
    static const char *H = "0123456789ABCDEF";
    int i;
    for (i = digits - 1; i >= 0; i--) { b[off + (size_t)i] = H[v & 15]; v >>= 4; }
    return off + (size_t)digits;
}

/* 写 <id>/.config：书名 / S / 全文 CRC / L / 目录条目数 T / 目录 / CRC */
static int config_write(const RfStore *st, const char *id, const u8 *data, u64 len,
                        const char *src_path, u32 h)
{
    char path[700], title[160];
    char *cfg;
    size_t coff = 0, tlen, dot, cap;
    const char *base = strrchr(src_path, '/');
    u64 L = rf_count_lines(data, len), entries = 0, i, line_no;

    base = base ? base + 1 : src_path;
    tlen = strlen(base);
    dot = tlen;
    while (dot > 0 && base[dot - 1] != '.') dot--;
    if (dot > 0) dot--;
    if (dot == 0) dot = tlen;
    if (dot >= sizeof title) dot = sizeof title - 1;
    memcpy(title, base, dot);
    title[dot] = 0;
    tlen = dot;

    for (i = 0, line_no = 1; i < len && entries < 256; ) {      /* 统计目录条目 */
        u64 e = i;
        while (e < len && data[e] != '\n') e++;
        if (e > i && data[i] == '#' && (e - i) < 120) entries++;
        i = (e < len) ? e + 1 : len;
        line_no++;
    }

    cap = 512 + (size_t)entries * 152;
    cfg = (char *)malloc(cap);
    if (!cfg) return -1;
    coff = put_str(cfg, coff, "RFBOOK001");
    memcpy(cfg + coff, title, tlen);
    cfg[coff + tlen] = '\n';
    coff += tlen + 1;
    coff = put_hex(cfg, coff, len, 16);
    coff = put_hex(cfg, coff, (u64)h, 8);
    coff = put_hex(cfg, coff, L, 16);
    coff = put_hex(cfg, coff, entries, 8);
    for (i = 0, line_no = 1; i < len; ) {                        /* 目录：`#` 标题 */
        u64 e = i;
        while (e < len && data[e] != '\n') e++;
        if (e > i && data[i] == '#' && (e - i) < 120) {
            coff = put_hex_raw(cfg, coff, line_no, 16);
            cfg[coff++] = ' ';
            memcpy(cfg + coff, data + i, (size_t)(e - i));
            coff += (size_t)(e - i);
            cfg[coff++] = '\n';
        }
        i = (e < len) ? e + 1 : len;
        line_no++;
    }
    coff = put_hex(cfg, coff, (u64)rf_crc32((const u8 *)cfg, coff), 8);
    book_path(path, sizeof path, st, id, ".config");
    i = (u64)write_atomic(path, cfg, coff);
    free(cfg);
    return (int)i;
}

int rf_book_import(RfStore *st, const char *src_path, char *out_id, size_t cap)
{
    u8 *data;
    u64 len = 0;
    u32 h;
    char id[16], dir[700], dst[700];
    int seq;
    static u64 storage[RF_MAX_INDEX];
    RfIndex ix;

    data = read_file(src_path, &len);
    if (!data) return -1;
    h = rf_crc32(data, (size_t)len);

    memset(id, 0, sizeof id);
    for (seq = 0; seq < 256; seq++) {                   /* ID = 哈希高 3 字节 + 冲突序号 */
        struct stat sb;
        snprintf(id, sizeof id, "%06X%02X", (unsigned)(h >> 8), (unsigned)seq);
        snprintf(dir, sizeof dir, "%s/book/%s", st->root, id);
        if (stat(dir, &sb) != 0) break;
    }
    if (seq >= 256) { free(data); return -1; }

    snprintf(dir, sizeof dir, "%s/book", st->root);
    mkdir(dir, 0770);
    snprintf(dir, sizeof dir, "%s/book/%s", st->root, id);
    if (mkdir(dir, 0770) != 0) { free(data); return -1; }

    book_path(dst, sizeof dst, st, id, "book");
    if (write_atomic(dst, data, (size_t)len) != 0) { free(data); return -1; }
    if (config_write(st, id, data, len, src_path, h) != 0) { free(data); return -1; }
    if (rf_index_build(st, id, data, len, &ix, storage, 1) != 0) { free(data); return -1; }

    free(data);
    if (out_id && cap) snprintf(out_id, cap, "%s", id);
    return 0;
}

int rf_book_delete(RfStore *st, const char *id)
{
    char dir[700], dst[700], cmd[1600];
    snprintf(dir, sizeof dir, "%s/book/%s", st->root, id);
    snprintf(dst, sizeof dst, "%s/trash/%s", st->root, id);
    {
        char trash[700];
        snprintf(trash, sizeof trash, "%s/trash", st->root);
        mkdir(trash, 0770);
    }
    if (rename(dir, dst) != 0) return -1;
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dst);
    if (system(cmd) != 0) return -1;                    /* 删除失败：目录仍在 trash，可人工恢复 */
    return 0;
}

int rf_progress_save(RfStore *st, const char *id, u64 line, u64 off, u64 total)
{
    char buf[160], p[700];
    int n = rf_progress_encode(buf, sizeof buf, line, off, total, pal_now_unix());
    if (n <= 0) return -1;
    book_path(p, sizeof p, st, id, ".progress");
    return write_atomic(p, buf, (size_t)n);
}

int rf_progress_load(RfStore *st, const char *id, u64 *line, u64 *off)
{
    char p[700];
    u8 *b;
    u64 len = 0, tl = 0;
    int rc;
    book_path(p, sizeof p, st, id, ".progress");
    b = read_file(p, &len);
    if (!b) return -1;
    rc = rf_progress_decode((const char *)b, (size_t)len, line, off, &tl);
    free(b);
    return rc;
}
