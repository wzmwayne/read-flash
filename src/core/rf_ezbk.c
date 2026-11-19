/* rf_ezbk.c — ezbk（easy_e-book）单文件读取（v0.2 规范，stable 等级）
 *
 * 规范要点（~/date/neobook/notes/ezbk-spec-v0.1.md）：
 *   第一行 EZBK/1（可带 BOM）；头���是连续的 "@key value" 行，遇非 @ 行即正文；
 *   正文每行首字符决定语义：'\\' 正文、'#' 卷、'##' 章、'---' 分隔、'@' 指令。
 * 本模块把 ezbk 转成应用内部文本（与 .txt 导入同一套渲染）：
 *   卷 → "# 标题"、章 → "## 标题"、'###'+ → "### 标题"（容错，规范里非法）
 *   正文去掉一个 '\\'；正文里的 <br/> 还原成换行；@image → "[图片]"；未知 @ 指令丢弃。
 * 图片与封面（base64）不解析——应用是纯文本阅读器。
 */
#include "rf.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    u8 *p;
    u64 n, cap;
    int oom;
} Buf;

static void buf_reserve(Buf *b, u64 need)
{
    u64 cap;
    u8 *np;
    if (b->oom || b->n + need <= b->cap) return;
    cap = b->cap ? b->cap : 4096;
    while (cap < b->n + need) cap *= 2;
    np = (u8 *)realloc(b->p, (size_t)cap);
    if (!np) { b->oom = 1; return; }
    b->p = np;
    b->cap = cap;
}

static void buf_put(Buf *b, const void *s, u64 n)
{
    buf_reserve(b, n);
    if (b->oom) return;
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n;
}

static void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
static void buf_ch(Buf *b, char c) { buf_put(b, &c, 1); }

/* 元数据反转义：\\ → \\、\n → 换行、丢弃 \r */
static void meta_unescape(const u8 *s, u64 n, char *out, size_t cap)
{
    u64 i, o = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            if (s[i + 1] == 'n') { if (o + 1 < cap) out[o++] = '\n'; i++; continue; }
            if (s[i + 1] == '\\') { if (o + 1 < cap) out[o++] = '\\'; i++; continue; }
        }
        if (s[i] == '\r') continue;
        if (o + 1 < cap) out[o++] = (char)s[i];
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
}

/* 行首/行尾空白修剪（空格与制表符） */
static void trim_span(const u8 *s, u64 *off, u64 *n)
{
    while (*n && (s[*off] == ' ' || s[*off] == '\t')) { (*off)++; (*n)--; }
    while (*n) {
        u8 c = s[*off + *n - 1];
        if (c == ' ' || c == '\t' || c == '\r') (*n)--;
        else break;
    }
}

int rf_ezbk_is(const u8 *data, u64 len)
{
    if (!data) return 0;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3; len -= 3;
    }
    return len >= 6 && memcmp(data, "EZBK/1", 6) == 0;
}

int rf_ezbk_to_text(const u8 *in, u64 len, u8 **out, u64 *out_len,
                    char *title, size_t title_cap,
                    char *writer, size_t writer_cap)
{
    Buf b;
    u64 pos = 0;
    int in_body = 0;

    if (!rf_ezbk_is(in, len)) return 0;
    if (len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) { in += 3; len -= 3; }

    memset(&b, 0, sizeof b);
    if (title && title_cap) title[0] = 0;
    if (writer && writer_cap) writer[0] = 0;

    /* 跳过 magic 行 */
    while (pos < len && in[pos] != '\n') pos++;
    if (pos < len) pos++;

    while (pos < len) {
        u64 s = pos, e;
        while (pos < len && in[pos] != '\n') pos++;
        e = pos;
        if (pos < len) pos++;
        {
            u64 off = s, n = e - s, i;
            trim_span(in, &off, &n);

            if (!in_body && n > 0 && in[off] == '@') {          /* 头部元数据 */
                u64 k = off + 1, ke = k;
                while (ke < off + n && in[ke] != ' ') ke++;
                if (ke + 1 <= off + n) {
                    u64 vs = ke < off + n ? ke + 1 : ke;
                    u64 vn = (off + n > vs) ? (off + n - vs) : 0;
                    if (ke - k == 5 && memcmp(in + k, "title", 5) == 0 && title && title_cap)
                        meta_unescape(in + vs, vn, title, title_cap);
                    else if (ke - k == 6 && memcmp(in + k, "writer", 6) == 0 && writer && writer_cap)
                        meta_unescape(in + vs, vn, writer, writer_cap);
                    else if (ke - k == 6 && memcmp(in + k, "author", 6) == 0 && writer && writer_cap && !writer[0])
                        meta_unescape(in + vs, vn, writer, writer_cap);
                }
                continue;                                        /* 其余 @key 忽略 */
            }
            if (n > 0 && in[off] == '@') in_body = 1;
            else in_body = 1;

            if (n == 0) { buf_ch(&b, '\n'); continue; }

            if (n == 3 && memcmp(in + off, "---", 3) == 0) {      /* 分隔 / 换页 */
                buf_str(&b, "---\n");
                continue;
            }
            if (in[off] == '@') {                                /* @image 等指令 */
                if (n >= 6 && memcmp(in + off, "@image", 6) == 0) buf_str(&b, "[图片]\n");
                continue;
            }
            if (in[off] == '#') {                                /* 卷 / 章 / 三级标题 */
                u64 h = 0;
                while (h < n && in[off + h] == '#') h++;
                if (h <= 3) {
                    u64 ts = off + h, tn = n - h;
                    trim_span(in, &ts, &tn);
                    buf_put(&b, in + off, h);                    /* 原样输出 1~3 个 # */
                    buf_ch(&b, ' ');
                    for (i = 0; i < tn; i++) {
                        if (in[ts + i] == '\r') continue;
                        buf_ch(&b, (char)in[ts + i]);
                    }
                    buf_ch(&b, '\n');
                    continue;
                }
                /* #### 以上：按正文处理（规范里非法，容错） */
            }
            {                                                   /* 正文行（含裸行容错） */
                u64 bs = off, bn = n;
                if (in[bs] == '\\') { bs++; bn--; }
                for (i = 0; i < bn; i++) {
                    if (i + 4 < bn && memcmp(in + bs + i, "<br/>", 5) == 0) {  /* 还原换行 */
                        buf_ch(&b, '\n');
                        i += 4;
                        continue;
                    }
                    if (in[bs + i] == '\r') continue;
                    buf_ch(&b, (char)in[bs + i]);
                }
                buf_ch(&b, '\n');
            }
        }
    }

    if (b.oom) { free(b.p); return 0; }
    if (!b.p) { b.p = (u8 *)malloc(1); if (!b.p) return 0; }
    *out = b.p;
    *out_len = b.n;
    return 1;
}
