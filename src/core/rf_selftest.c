/* Read-Flash §16 最小验证 + 不重不漏自检
 *   1) 网格构建：多种屏幕/密度/字号/行距下的不变量
 *   2) 确定性：同参数同输入 → 结果逐字节一致（含字号切换往返）
 *   3) 锚点：进度往返 + (sourceLine, lineOffset) 恢复 + 上一页回溯
 * 全程整数运算、无随机源（PRNG 为固定种子的 LCG）。
 */
#include "rf.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define DOC_CAP   (6u * 1024u * 1024u)
#define FB_W      720
#define FB_H      1280
#define MAX_PAGES 20000

static u8   g_doc[DOC_CAP];
static u16  g_fb[FB_W * FB_H];
static RfRow g_rows[RF_MAX_ROWS];

typedef struct { u64 anchor_off, anchor_line, next_off; int filled; u8 term; u64 hash; } PageRec;
static PageRec g_pages[MAX_PAGES];

typedef struct { char *buf; size_t cap; size_t len; int fails; } Rpt;

static void rp(Rpt *r, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (r->len + 1 >= r->cap) return;
    va_start(ap, fmt);
    n = vsnprintf(r->buf + r->len, r->cap - r->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= r->cap - r->len) { r->len = r->cap - 1; return; }
    r->len += (size_t)n;
}

static void fail(Rpt *r, const char *fmt, ...)
{
    va_list ap;
    r->fails++;
    rp(r, "      FAIL: ");
    if (r->len + 1 >= r->cap) return;
    va_start(ap, fmt);
    {
        int n = vsnprintf(r->buf + r->len, r->cap - r->len, fmt, ap);
        if (n > 0 && (size_t)n < r->cap - r->len) r->len += (size_t)n;
        else r->len = r->cap - 1;
    }
    va_end(ap);
    rp(r, "\n");
}

/* ---------------- 测试语料（确定性生成） ---------------- */

static u64 g_rng = 0x0123456789ABCDEFULL;

static u32 rnd(void)
{
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(g_rng >> 33);
}

static const char *SENT[] = {
    "汪淼觉得，这个世界的物理规律突然变得不可靠了。",
    "他想起杨冬临终前的话，想起那些在黑暗中闪烁的星星。",
    "智子锁死了人类的基础物理，而人类还在为眼前的小事争吵。",
    "阅读器把每一行都当作真实存在的一行，不合并，也不压缩。",
    "ASCII words and digits 0123456789 mixed with 中文与《书名号》。",
    "这是一条用于验证断行的长句，它必须在视口宽度处被正确切断，"
    "既不能多一个字符，也不能少一个字符；换行渲染下它会占据多个渲染行。",
};
#define NSENT ((int)(sizeof(SENT) / sizeof(SENT[0])))

static u64 app(u8 *b, u64 p, const char *s)
{
    while (*s) b[p++] = (u8)*s++;
    return p;
}

static u64 gen_main(u8 *b, u64 cap)
{
    u64 p = 0;
    while (p + 8192 < cap) {
        u32 k = rnd() % 100;
        if (k < 4)       { p = app(b, p, "---\n"); }
        else if (k < 8)  { p = app(b, p, "# 第一章 科学边界\n"); }
        else if (k < 11) { p = app(b, p, "## 第二节 黑暗森林\n"); }
        else if (k < 13) { p = app(b, p, "### 三体问题\n"); }
        else if (k < 16) { b[p++] = '\n'; }
        else if (k < 19) {                       /* 超长行（>2000 字符） */
            int i, n = 260 + (int)(rnd() % 120);
            for (i = 0; i < n; i++) p = app(b, p, SENT[rnd() % NSENT]);
            b[p++] = '\n';
        } else {                                  /* 普通行 1–3 句 */
            int i, n = 1 + (int)(rnd() % 3);
            for (i = 0; i < n; i++) p = app(b, p, SENT[rnd() % NSENT]);
            if (rnd() % 20 == 0) b[p++] = '\r';   /* \r 视为内容（P2） */
            b[p++] = '\n';
        }
    }
    return p;
}

static u64 gen_single_line(u8 *b, u64 cap)
{
    u64 p = 0;
    while (p + 8192 < cap) p = app(b, p, SENT[rnd() % NSENT]);
    return p;                                     /* 全文无 \n */
}

static u64 gen_newlines(u8 *b, u64 cap)
{
    u64 p = 0;
    while (p + 1 < cap) b[p++] = '\n';
    return p;
}

/* ---------------- 1) 网格不变量 ---------------- */

static void check_grid(Rpt *r)
{
    static const int SW[7] = {240, 320, 480, 720, 1080, 1440, 2400};
    static const int SH[7] = {320, 480, 800, 1280, 1920, 2560, 1080};
    static const int DN[5] = {1000, 1500, 2000, 2750, 3000};
    static const int FN[5] = {12, 16, 24, 32, 48};
    static const int SP[4] = {1000, 1200, 2000, 3000};
    int combos = 0, bad0 = r->fails, skipped = 0, overflow = 0;
    size_t i;
    int d, f, s;

    for (i = 0; i < 7; i++)
    for (d = 0; d < 5; d++)
    for (f = 0; f < 5; f++)
    for (s = 0; s < 4; s++) {
        RfSettings st;
        RfGrid g;
        int want_r, want_b;
        char tag[80];

        rf_settings_default(&st);
        st.font_dp = FN[f];
        st.spacing_permille = SP[s];
        rf_grid_build(&g, &st, SW[i], SH[i], DN[d]);
        combos++;
        if (g.rows > RF_MAX_ROWS) { skipped++; continue; }
        if (g.overflow) overflow++;

        snprintf(tag, sizeof tag, "%dx%d d=%d f=%d sp=%d", SW[i], SH[i], DN[d], FN[f], SP[s]);

        if (g.fp < 1 || g.wc < 1 || g.rh < 1 || g.cols < 1 || g.rows < 1)
            fail(r, "%s: 基本量非正 (fp=%d wc=%d rh=%d C=%d V=%d)", tag, g.fp, g.wc, g.rh, g.cols, g.rows);
        if (g.wc != (g.fp + 1) / 2)
            fail(r, "%s: 半角列宽 != ceil(fp/2)", tag);
        if (g.body.w != g.cols * g.wc || g.body.h != g.rows * g.rh)
            fail(r, "%s: 正文矩形与网格不符", tag);
        if (g.top_bar.h != g.rh || g.btn_bar.h != g.rh || g.pager_bar.h != g.rh)
            fail(r, "%s: 顶栏/底栏高度 != 基础行高", tag);
        if (g.pager_bar.y + g.rh != SH[i] || g.btn_bar.y != SH[i] - 2 * g.rh)
            fail(r, "%s: 底栏未贴屏幕底部", tag);
        if (g.body.y != g.rh + g.m_top)
            fail(r, "%s: 正文 y != 顶栏 + 上边距", tag);

        if (!g.overflow) {
            want_r = (st.m_right_dp * DN[d] + 500) / 1000;
            want_b = (st.m_bot_dp * DN[d] + 500) / 1000;
            if (g.m_right < want_r || g.m_bot < want_b)
                fail(r, "%s: 自动边距反而变小", tag);
            if (g.body.x + g.body.w + g.m_right != SW[i])
                fail(r, "%s: 水平方向未铺满（网格未对齐）", tag);
            if (g.body.y + g.body.h + g.m_bot + 2 * g.rh != SH[i])
                fail(r, "%s: 垂直方向未铺满（网格未对齐）", tag);
            if (g.body.y + g.body.h > g.btn_bar.y)
                fail(r, "%s: 正文侵占底栏", tag);
        }
    }
    rp(r, "[1/3] 网格不变量: %d 组合, 失败 %d, 溢出降级 %d, 超 V 跳过 %d\n",
       combos, r->fails - bad0, overflow, skipped);
}

/* ---------------- 2) 确定性 ---------------- */

static u64 sweep(const u8 *doc, u64 len, const RfGrid *g, const RfMode *m,
                 int *pages, int *stalled)
{
    RfDoc d;
    RfPage pg;
    u64 h = 1469598103934665603ULL, off = 0, line = 1;
    int n = 0;
    rf_doc_init(&d, doc, len, 0);
    *stalled = 0;
    for (;;) {
        if (n >= MAX_PAGES) { *stalled = 2; break; }
        rf_page_layout(&d, g, m, off, line, &pg, g_rows);
        h = rf_fnv1a_u64(h, pg.anchor_off);
        h = rf_fnv1a_u64(h, pg.next_off);
        h = rf_fnv1a_u64(h, (u64)pg.rows_filled);
        h = rf_fnv1a_u64(h, (u64)pg.term);
        n++;
        if (pg.term & RF_TERM_EOF) break;
        if (pg.next_off <= off) { *stalled = 1; break; }
        off = pg.next_off;
        line = pg.next_line;
    }
    *pages = n;
    return h;
}

static void check_determinism(Rpt *r, u64 len)
{
    RfSettings st;
    RfGrid g, g2;
    RfMode m;
    RfDoc d;
    RfPage pg1, pg2;
    int bad0 = r->fails, p1 = 0, p2 = 0, st1 = 0, st2 = 0;
    u64 h1, h2, hA, hB, hC, f1, f2;

    rf_settings_default(&st);
    m.truncate_scroll = 0;
    rf_grid_build(&g, &st, FB_W, FB_H, 2000);
    rf_doc_init(&d, g_doc, len, 0);

    h1 = sweep(g_doc, len, &g, &m, &p1, &st1);
    h2 = sweep(g_doc, len, &g, &m, &p2, &st2);
    if (h1 != h2 || p1 != p2) fail(r, "两次重排结果不同 (%llu vs %llu)", (unsigned long long)h1, (unsigned long long)h2);
    if (st1) fail(r, "扫描未收敛 stalled=%d", st1);

    rf_page_layout(&d, &g, &m, 0, 1, &pg1, g_rows);
    f1 = rf_render_page(g_fb, FB_W, FB_H, &g, g_rows, pg1.rows_filled, &d);
    rf_page_layout(&d, &g, &m, 0, 1, &pg2, g_rows);
    f2 = rf_render_page(g_fb, FB_W, FB_H, &g, g_rows, pg2.rows_filled, &d);
    if (f1 != f2) fail(r, "同页两次渲染帧哈希不同");
    if (pg1.anchor_off != pg2.anchor_off || pg1.next_off != pg2.next_off ||
        pg1.anchor_line != pg2.anchor_line || pg1.rows_filled != pg2.rows_filled ||
        pg1.term != pg2.term)
        fail(r, "同页两次排版 PageState 不同");

    /* 字号往返：16 → 24 → 16 必须回到同一结果 */
    hA = sweep(g_doc, len, &g, &m, &p1, &st1);
    st.font_dp = 24;
    rf_grid_build(&g2, &st, FB_W, FB_H, 2000);
    hB = sweep(g_doc, len, &g2, &m, &p2, &st2);
    st.font_dp = 16;
    rf_grid_build(&g2, &st, FB_W, FB_H, 2000);
    hC = sweep(g_doc, len, &g2, &m, &p2, &st2);
    if (hA != hC) fail(r, "字号往返后结果不一致");
    if (hA == hB) fail(r, "字号变化后结果竟然相同（断行未随网格变化）");

    rp(r, "[2/3] 确定性: %d 页 x2 一致, 帧哈希 0x%llX, 字号往返一致, 失败 %d\n",
       p1, (unsigned long long)f1, r->fails - bad0);
}

/* ---------------- 3) 锚点 / 进度 / 不重不漏 / 上一页 ---------------- */

/* 扫描并存页记录；返回页数；*bad 记录违反不变量的次数 */
static int survey(Rpt *r, const u8 *doc, u64 len, const RfGrid *g, const RfMode *m,
                  int *prev_checked, int *prev_bad, int max_prev)
{
    RfDoc d;
    RfPage pg;
    u64 off = 0, line = 1, covered = 0;
    int n = 0, eof_seen = 0;
    char pbuf[128];

    rf_doc_init(&d, doc, len, rf_count_lines(doc, len));
    for (;;) {
        if (n >= MAX_PAGES) { fail(r, "页数超过上限 %d，疑似未前进", MAX_PAGES); break; }
        rf_page_layout(&d, g, m, off, line, &pg, g_rows);

        if (n == 0 && pg.anchor_off != 0) fail(r, "首页锚点 != 0");
        if (pg.anchor_off != covered) fail(r, "第 %d 页锚点 %llu != 已覆盖 %llu", n, (unsigned long long)pg.anchor_off, (unsigned long long)covered);
        if (!(pg.term & RF_TERM_EOF) && pg.next_off <= pg.anchor_off)
            fail(r, "第 %d 页未前进且非 EOF", n);
        if (pg.term & RF_TERM_EOF) eof_seen++;
        if (pg.anchor_off < len) {
            u8 b0 = doc[pg.anchor_off];
            if ((b0 & 0xC0) == 0x80) fail(r, "第 %d 页锚点未对齐 UTF-8 字符边界", n);
        }
        if (pg.rows_filled > g->rows) fail(r, "第 %d 页行数越界", n);

        if (n < MAX_PAGES) {
            g_pages[n].anchor_off = pg.anchor_off;
            g_pages[n].anchor_line = pg.anchor_line;
            g_pages[n].next_off = pg.next_off;
            g_pages[n].filled = pg.rows_filled;
            g_pages[n].term = pg.term;
            g_pages[n].hash = rf_hash_page(&pg, g_rows);
        }

        /* 进度往返：编码 → 解析 → 重排必须完全一致 */
        if ((n % 97) == 0) {
            u64 sl, lo, tl;
            int nb = rf_progress_encode(pbuf, sizeof pbuf, pg.anchor_line, pg.anchor_off,
                                        d.line_count, 1700000000ULL + (u64)n);
            if (nb <= 0) fail(r, "进度编码失败");
            else if (rf_progress_decode(pbuf, (size_t)nb, &sl, &lo, &tl) != 0)
                fail(r, "进度解码/校验失败 (页 %d)", n);
            else if (sl != pg.anchor_line || lo != pg.anchor_off || tl != d.line_count)
                fail(r, "进度往返不一致 (页 %d)", n);
            else {
                RfPage pg2;
                rf_page_layout(&d, g, m, lo, sl, &pg2, g_rows);
                if (rf_hash_page(&pg2, g_rows) != g_pages[n].hash)
                    fail(r, "按进度恢复后页面与保存时不同 (页 %d)", n);
            }
        }

        covered = pg.next_off;
        n++;
        if (pg.term & RF_TERM_EOF) break;
        off = pg.next_off;
        line = pg.next_line;
    }

    if (eof_seen != 1) fail(r, "EOF 页数 = %d（应为 1）", eof_seen);
    if (covered != len) fail(r, "覆盖不完整：%llu / %llu", (unsigned long long)covered, (unsigned long long)len);

    /* 反向翻页：从末页起连续回溯，必须不重不漏地铺满 [0, 锚点)（§8.3 / A16） */
    if (prev_checked && n > 0 && max_prev > 0) {
        u64 cursor_off = g_pages[n - 1].anchor_off;
        u64 cursor_line = g_pages[n - 1].anchor_line;
        int steps = 0, reached_first = 0;

        for (;;) {
            RfPage pp;
            int rc;
            if (cursor_off == 0) { reached_first = 1; break; }
            if (steps >= max_prev) break;
            rc = rf_page_prev(&d, g, m, cursor_off, cursor_line, &pp, g_rows);
            steps++;
            (*prev_checked)++;
            if (rc == 1) { reached_first = 1; break; }
            if (rc != 0) { (*prev_bad)++; fail(r, "反向翻页失败 rc=%d @%llu", rc, (unsigned long long)cursor_off); break; }
            if (pp.next_off != cursor_off) {
                (*prev_bad)++;
                fail(r, "反向翻页出现缝隙: next=%llu cursor=%llu", (unsigned long long)pp.next_off, (unsigned long long)cursor_off);
                break;
            }
            if (pp.anchor_off >= cursor_off) { (*prev_bad)++; fail(r, "反向翻页未前进 @%llu", (unsigned long long)cursor_off); break; }
            if (pp.rows_filled <= 0) { (*prev_bad)++; fail(r, "反向翻页得到空页 @%llu", (unsigned long long)cursor_off); break; }
            if (pp.rows_filled > g->rows) { (*prev_bad)++; fail(r, "反向翻页行数越界"); break; }
            if (!g_rows[0].block_start) { (*prev_bad)++; fail(r, "反向翻页页首不在块首 @%llu", (unsigned long long)pp.anchor_off); break; }
            cursor_off = pp.anchor_off;
            cursor_line = pp.anchor_line;
        }
        if (reached_first && cursor_off != 0) { (*prev_bad)++; fail(r, "反向到首页但锚点=%llu", (unsigned long long)cursor_off); }
        if (max_prev >= n && !reached_first) { (*prev_bad)++; fail(r, "反向翻页 %d 步未到达开头", max_prev); }
    }
    return n;
}

static void check_bounds(Rpt *r, const char *name, const char *text)
{
    RfSettings st;
    RfGrid g;
    RfMode m;
    RfDoc d;
    RfPage pg;
    u64 len = (u64)strlen(text);

    rf_settings_default(&st);
    m.truncate_scroll = 0;
    rf_grid_build(&g, &st, 320, 480, 1500);
    memcpy(g_doc, text, (size_t)len);
    rf_doc_init(&d, g_doc, len, rf_count_lines(g_doc, len));
    rf_page_layout(&d, &g, &m, 0, 1, &pg, g_rows);
    rp(r, "      %-14s L=%-3llu 首页行数=%d term=0x%02X canPrev=%d canNext=%d\n",
       name, (unsigned long long)d.line_count, pg.rows_filled, pg.term,
       rf_can_prev(&pg), rf_can_next(&pg));
}

/* 供调试工具复现同一份语料 */
u64 rf_test_gen_main(u8 *buf, u64 cap)
{
    g_rng = 0x0123456789ABCDEFULL;
    return gen_main(buf, cap);
}

/* ---------------- 入口 ---------------- */

int rf_selftest(char *out, size_t outsz)
{
    Rpt r;
    RfSettings st;
    RfGrid g;
    RfMode m;
    u64 len_main, len_single, len_nl;
    int pages = 0, stalled = 0, pv_checked = 0, pv_bad = 0;
    int bad0;
    u64 hglobal;

    r.buf = out; r.cap = outsz; r.len = 0; r.fails = 0;
    if (outsz == 0) return 1;
    out[0] = 0;

    rf_settings_default(&st);
    m.truncate_scroll = 0;
    rf_grid_build(&g, &st, FB_W, FB_H, 2000);

    rp(&r, "Read-Flash 最小验证（§16）  网格 %dx%d d=2000 字号 %ddp 行距 %d‰\n",
       FB_W, FB_H, st.font_dp, st.spacing_permille);
    rp(&r, "C=%d 列  V=%d 行  半角列宽=%dpx  基础行高=%dpx  边距=(%d,%d,%d,%d)\n",
       g.cols, g.rows, g.wc, g.rh, g.m_top, g.m_bot, g.m_left, g.m_right);

    check_grid(&r);

    g_rng = 0x0123456789ABCDEFULL;
    len_main = gen_main(g_doc, 5u * 1024u * 1024u);
    rp(&r, "语料: 混合正文 %llu B / %llu 行\n",
       (unsigned long long)len_main, (unsigned long long)rf_count_lines(g_doc, len_main));

    check_determinism(&r, len_main);

    bad0 = r.fails;
    pages = survey(&r, g_doc, len_main, &g, &m, &pv_checked, &pv_bad, 200000);
    rp(&r, "[3/3] 锚点/不重不漏: %d 页连续覆盖 %llu B, 反向翻页 %d 步铺满, 失败 %d, 本项失败 %d\n",
       pages, (unsigned long long)len_main, pv_checked, pv_bad, r.fails - bad0);

    /* 单行大文件（A2 场景：无 \n，一个源行跨多页） */
    g_rng = 0x0FEDCBA987654321ULL;
    len_single = gen_single_line(g_doc, 5u * 1024u * 1024u);
    bad0 = r.fails;
    pv_checked = 0; pv_bad = 0;
    pages = survey(&r, g_doc, len_single, &g, &m, &pv_checked, &pv_bad, 40);
    rp(&r, "      单行 5MB: %d 页连续覆盖, 失败 %d\n", pages, r.fails - bad0);
    {
        /* 单独抽样测上一页（每次回溯从 0 起扫，只测前若干页以控时） */
        RfDoc d;
        RfPage first, second, pp;
        int rc;
        rf_doc_init(&d, g_doc, len_single, 1);
        rf_page_layout(&d, &g, &m, 0, 1, &first, g_rows);
        rf_page_layout(&d, &g, &m, first.next_off, first.next_line, &second, g_rows);
        rc = rf_page_prev(&d, &g, &m, second.anchor_off, second.anchor_line, &pp, g_rows);
        if (rc != 0 || rf_hash_page(&pp, g_rows) != rf_hash_page(&first, g_rows))
            fail(&r, "单行大文件：第 2 页回溯不回到第 1 页 (rc=%d)", rc);
        rp(&r, "      单行大文件 canPrev(第1页)=%d canNext=%d；第2页回溯 ok=%d\n",
           rf_can_prev(&first), rf_can_next(&first), rc == 0);
    }

    /* 大量连续 \n */
    len_nl = gen_newlines(g_doc, 400u * 1024u);
    bad0 = r.fails;
    pages = survey(&r, g_doc, len_nl, &g, &m, NULL, NULL, 0);
    rp(&r, "      连续 \\n: %llu 行 → %d 页连续覆盖, 失败 %d\n",
       (unsigned long long)rf_count_lines(g_doc, len_nl), pages, r.fails - bad0);

    rp(&r, "边界: \n");
    check_bounds(&r, "空文件", "");
    check_bounds(&r, "单换行", "\n");
    check_bounds(&r, "无尾换行", "abc");
    check_bounds(&r, "仅换页符", "---\n");
    check_bounds(&r, "双换页符", "---\n---\n");
    check_bounds(&r, "换页符+正文", "---\nabc\n");
    check_bounds(&r, "标题三档", "# H1\n## H2\n### H3\nbody\n");
    check_bounds(&r, "CRLF", "a\r\nb");
    check_bounds(&r, "全角标点", "、。〈〉《》「」\n");

    hglobal = rf_fnv1a(out, r.len);
    rp(&r, "GLOBAL %016llX\n", (unsigned long long)hglobal);
    rp(&r, "RESULT: %s (失败 %d)\n", r.fails == 0 ? "PASS" : "FAIL", r.fails);
    (void)pages; (void)stalled;
    return r.fails == 0 ? 0 : 1;
}
