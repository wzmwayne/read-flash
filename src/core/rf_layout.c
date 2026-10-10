/* 排版引擎（v3.4 §5.2–§5.5、§8）
 * 渲染行迭代器 + 分页 + 上一页回溯。
 *
 * 关键约定：
 *  - 渲染行的切分只取决于"源行起点 + 列容量"，与页面锚点无关
 *    ⇒ 全文的渲染行序列是固定的；分页 = 在这个序列上按块取行。
 *  - 标题块是原子的：整块放不下时整块留给下一页（RfRow.block_start 标记块首）。
 *  - 上一页 = 满足 f(S) == 当前锚点 的最大块起点 S（f = 一次分页排版）。
 */
#include "rf.h"

/* ---------------- 文档与源行 ---------------- */

void rf_doc_init(RfDoc *d, const u8 *data, u64 len, u64 line_count)
{
    d->data = data;
    d->len = len;
    d->line_count = line_count;
}

u64 rf_count_lines(const u8 *data, u64 len)
{
    u64 n = 0, i;
    for (i = 0; i < len; i++)
        if (data[i] == '\n') n++;
    if (len > 0 && data[len - 1] != '\n') n++;   /* 末尾无 \n 时最后一行有效 */
    return n;
}

u64 rf_line_end(const RfDoc *d, u64 off)
{
    u64 p = off;
    if (p > d->len) return d->len;
    while (p < d->len && d->data[p] != '\n') p++;
    return p;
}

u64 rf_next_line_start(const RfDoc *d, u64 off)
{
    u64 e = rf_line_end(d, off);
    return (e < d->len) ? e + 1 : d->len;
}

u64 rf_line_start_of(const RfDoc *d, u64 off)
{
    u64 p;
    if (off == 0) return 0;
    p = (off > d->len) ? d->len : off;
    while (p > 0 && d->data[p - 1] != '\n') p--;
    return p;
}

u64 rf_prev_line_start(const RfDoc *d, u64 off)
{
    u64 p;
    if (off == 0 || d->len == 0) return 0;
    p = off - 1;                                  /* 上一行的 \n */
    while (p > 0 && d->data[p - 1] != '\n') p--;
    return p;
}

u64 rf_line_index_of(const RfDoc *d, u64 off)
{
    u64 i, n = 0;
    if (d->len == 0) return 0;
    if (off > d->len) off = d->len;
    for (i = 0; i < off; i++)
        if (d->data[i] == '\n') n++;
    return n + 1;
}

/* ---------------- 渲染行迭代器 ---------------- */

static int is_page_break(const u8 *p, u64 n)
{
    return n == 3 && p[0] == '-' && p[1] == '-' && p[2] == '-';
}

/* 标题判定（§5.3）：返回块占用的渲染行数（0 = 非标题） */
static int detect_title(const u8 *p, u64 n, int *kind, int *num, int *den)
{
    int nh = 0;
    *kind = RF_ROW_TEXT; *num = 1; *den = 1;
    if (n == 0 || p[0] != '#') return 0;
    while ((u64)nh < n && p[nh] == '#') nh++;
    if (nh > 3) return 0;
    if ((u64)nh >= n || p[nh] != ' ') return 0;
    if (nh == 1)      { *kind = RF_ROW_TITLE1; *num = 2; *den = 1; return 2; }  /* 2.0×，占 2 行 */
    else if (nh == 2) { *kind = RF_ROW_TITLE2; *num = 3; *den = 2; return 2; }  /* 1.5×，占 2 行 */
    else              { *kind = RF_ROW_TITLE3; *num = 1; *den = 1; return 1; }  /* 1.0×，占 1 行 */
}

void rf_iter_init(RfIter *it, const RfDoc *d, const RfGrid *g, const RfMode *m,
                  u64 off, u64 line)
{
    it->d = d; it->g = g; it->m = m;
    it->pos = off; it->line = line;
    it->line_end = off; it->line_has_nl = 0;
    it->state = 0; it->kind = RF_ROW_TEXT; it->cap = g->cols;
    it->block_rows = 0; it->block_left = 0; it->pad = 0;
}

/* 载入当前块；返回块占用行数（正文 0）。无法产出时返回 -1 并置 *term。 */
int rf_iter_block(RfIter *it, u8 *term)
{
    if (it->state == 1 || it->state == 2) return it->block_rows;   /* 2 = 块内补空行 */
    for (;;) {
        u64 le, llen;
        int kind, num, den, span;
        if (it->pos >= it->d->len) { *term |= RF_TERM_EOF; return -1; }
        le = rf_line_end(it->d, it->pos);
        llen = le - it->pos;
        it->line_end = le;
        it->line_has_nl = (le < it->d->len);
        /* 换页符（仅阅读书，v3.4 A5） */
        if (!it->m->truncate_scroll && is_page_break(it->d->data + it->pos, llen)) {
            it->pos = le + (it->line_has_nl ? 1u : 0u);
            it->line++;
            *term |= RF_TERM_FORCED;
            return -1;
        }
        span = it->m->parse_titles ? detect_title(it->d->data + it->pos, llen, &kind, &num, &den) : 0;
        it->kind = kind;
        it->block_rows = 0;
        it->block_left = 0;
        if (span == 0) {
            it->cap = it->g->cols;
        } else {
            i64 c = (i64)it->g->cols * den / num;      /* C / scale */
            it->cap = (c < 1) ? 1 : (int)c;
        }
        it->state = 1;
        return it->block_rows;
    }
}

int rf_iter_next(RfIter *it, RfRow *row, u8 *term)
{
    u64 row_start, p;
    int used = 0, truncated = 0, limit, block_start;

    if (it->pad) {                              /* 标题后自动插入的空渲染行（不消耗源字节） */
        it->pad = 0;
        row->start_off = row->end_off = it->pos;
        row->line = it->line;
        row->cols_used = 0;
        row->kind = RF_ROW_BLANK;
        row->truncated = 0;
        row->block_start = 1;
        return 1;
    }
    if (rf_iter_block(it, term) < 0) return 0;

    row_start = p = it->pos;
    block_start = (it->block_left == it->block_rows) ? 1 : 0;
    limit = it->cap;
    /* 截断模式（仅生成式书）：正文行占 1 行，末尾留 1 个半角列给省略号 */
    if (it->m->truncate_scroll && it->block_left == 0)
        limit = (it->g->cols > 1) ? it->g->cols - 1 : 1;

    while (p < it->line_end) {
        u32 cp;
        int nb = rf_utf8_decode(it->d->data + p, it->d->data + it->line_end, &cp);
        int c;
        if (nb <= 0) break;
        c = rf_char_cols(cp);
        if (used + c > limit) break;
        used += c;
        p += (u64)nb;
    }
    if (p == row_start && p < it->line_end) {   /* 单字符也放不下：强制放一个，避免死循环 */
        u32 cp;
        int nb = rf_utf8_decode(it->d->data + p, it->d->data + it->line_end, &cp);
        if (nb > 0) { used += rf_char_cols(cp); p += (u64)nb; }
    }
    if (it->m->truncate_scroll && it->block_left == 0 && p < it->line_end)
        truncated = 1;

    row->start_off = row_start;
    row->end_off = p;
    row->line = it->line;
    row->cols_used = (u16)used;
    row->kind = (u8)it->kind;
    row->truncated = (u8)truncated;
    row->block_start = 1;
    (void)block_start;

    /* 标题只占一个渲染行：排出本行后，剩余内容按正文续排 */
    if (it->kind != RF_ROW_TEXT) {
        if (it->kind == RF_ROW_TITLE1 || it->kind == RF_ROW_TITLE2)
            it->pad = 1;                        /* # / ## 之后自动加一个空行 */
        it->kind = RF_ROW_TEXT;
        it->cap = it->g->cols;
    }
    it->pos = p;
    if (it->pos >= it->line_end || truncated) { /* 本源行结束 */
        it->pos = it->line_end + (it->line_has_nl ? 1u : 0u);
        it->line++;
        it->state = 0;
    }
    return 1;
}

/* ---------------- 分页 ---------------- */

int rf_page_layout(const RfDoc *d, const RfGrid *g, const RfMode *m,
                   u64 anchor_off, u64 anchor_line, RfPage *pg, RfRow *rows)
{
    RfIter it;
    u8 term = 0;
    int r = 0, i;
    const int V = g->rows;

    rf_iter_init(&it, d, g, m, anchor_off, anchor_line);
    while (r < V) {
        int br = rf_iter_block(&it, &term);
        if (br < 0) {
            /* 页首连续换页符：忽略，不产生空页（v3.3 §15「后续空页忽略」，A5） */
            if ((term & RF_TERM_FORCED) && !(term & RF_TERM_EOF) && r == 0) {
                term &= (u8)~RF_TERM_FORCED;
                continue;
            }
            break;                               /* 换页符或文件末尾 */
        }
        if (br > 1 && r + br > V) {
            if (r > 0) break;                    /* 整块留给下一页 */
            it.block_rows = 1;                   /* 屏幕过小：降级为单行，保证前进 */
            it.block_left = 1;
        }
        if (!rf_iter_next(&it, &rows[r], &term)) break;
        r++;
    }
    if (r == V && r > 1 &&
        (rows[r - 1].kind == RF_ROW_TITLE1 || rows[r - 1].kind == RF_ROW_TITLE2)) {
        /* 标题行与其自动空行成对，不拆到两页：把标题留给下一页 */
        r--;
        it.pos = rows[r].start_off;
        it.line = rows[r].line;
        it.state = 0;
        it.pad = 0;
        rows[r].kind = RF_ROW_BLANK;
        rows[r].start_off = rows[r].end_off = it.pos;
    }
    if (it.pos >= d->len) term |= RF_TERM_EOF;
    if (r == V)           term |= RF_TERM_FILLED;

    for (i = r; i < V; i++) {                    /* 空白补齐（A5：换页符填到页底） */
        rows[i].start_off = it.pos;
        rows[i].end_off = it.pos;
        rows[i].line = it.line;
        rows[i].cols_used = 0;
        rows[i].kind = RF_ROW_BLANK;
        rows[i].truncated = 0;
        rows[i].block_start = 0;
    }
    pg->anchor_line = anchor_line;
    pg->anchor_off = anchor_off;
    pg->next_line = it.line;
    pg->next_off = it.pos;
    pg->row_count = V;
    pg->rows_filled = r;
    pg->term = term;
    return r;
}

/* ---------------- 上一页（§8.3 / A16） ---------------- */

int rf_page_prev(const RfDoc *d, const RfGrid *g, const RfMode *m,
                 u64 cur_off, u64 cur_line, RfPage *pg, RfRow *rows)
{
    static RfRow win[RF_MAX_ROWS];        /* 最近 V 个渲染行（环形） */
    static RfRow saved[RF_MAX_ROWS];      /* 换页符之前的快照（环形展开后） */
    RfIter it;
    u64 b_off, b_line, line_start;
    int moved = 0, i, wcount = 0, wnext = 0;
    int saved_count = 0, have_saved = 0;
    const int V = g->rows;

    if (cur_off == 0) return 1;                       /* 已是第一页 */
    if (V > RF_MAX_ROWS) return 3;

    /* B = max(1, cur_line − (V + 2))：按行回溯 */
    line_start = rf_line_start_of(d, cur_off);
    b_off = line_start;
    for (i = 0; i < V + 2; i++) {
        u64 prev;
        if (b_off == 0) break;
        prev = rf_prev_line_start(d, b_off);
        if (prev >= b_off) break;
        b_off = prev;
        moved++;
    }
    b_line = (cur_line > (u64)moved) ? (cur_line - (u64)moved) : 1;

    /* 从 B 起累积渲染行，直到抵达当前锚点 */
    rf_iter_init(&it, d, g, m, b_off, b_line);
    for (;;) {
        RfRow row;
        u8 t = 0;
        if (it.pos >= cur_off) break;
        if (!rf_iter_next(&it, &row, &t)) {
            if (t & RF_TERM_FORCED) {                 /* 换页符：清窗口，保留最近一次非空快照 */
                if (wcount > 0) {
                    int idx = (wnext + V - wcount) % V;
                    for (i = 0; i < wcount; i++) saved[i] = win[(idx + i) % V];
                    saved_count = wcount;
                    have_saved = 1;
                }
                wcount = 0; wnext = 0;
                continue;
            }
            break;                                    /* EOF */
        }
        if (row.start_off >= cur_off) break;
        win[wnext] = row;
        wnext = (wnext + 1) % V;
        if (wcount < V) wcount++;
    }

    /* 组装上一页：锚点之前的最后 V 个渲染行，起点对齐到块首。
     * - 窗口（或换页符快照）已按"最近 V 行"保存；
     * - 若窗口首行落在标题块中间，则丢弃该块的残余行，保证页首一定是块首；
     * - 反向连续翻页据此严格铺满 [0, 锚点)，不重不漏。 */
    {
        int use_saved = (wcount == 0 && have_saved);
        int count = use_saved ? saved_count : wcount;
        int base = use_saved ? 0 : ((wnext + V - wcount) % V);
        int start = 0;

        if (count <= 0) return 1;                     /* 没有上一页 */
        while (start < count &&
               !(use_saved ? saved[start].block_start : win[(base + start) % V].block_start))
            start++;
        if (start >= count) return 2;                 /* 理论不应发生：窗口内无块首 */

        for (i = start; i < count; i++)
            rows[i - start] = use_saved ? saved[i] : win[(base + i) % V];
        count -= start;

        {
            u64 first_off = rows[0].start_off;
            u64 first_line = rows[0].line;
            pg->anchor_line = first_line;
            pg->anchor_off = first_off;
            pg->next_line = cur_line;
            pg->next_off = cur_off;
            pg->row_count = V;
            pg->rows_filled = count;
            pg->term = RF_TERM_FILLED;                /* 上一页之后必有内容 */
            for (i = count; i < V; i++) {
                rows[i].start_off = cur_off; rows[i].end_off = cur_off;
                rows[i].line = cur_line; rows[i].cols_used = 0;
                rows[i].kind = RF_ROW_BLANK; rows[i].truncated = 0; rows[i].block_start = 0;
            }
        }
    }
    return 0;
}
