/* 调试工具：复现"上一页回溯"失败的具体页面，打印候选分析。
 * 修复完成后保留为回归排查工具。 */
#include "rf.h"
#include <stdio.h>
#include <stdlib.h>

static u8   doc[6u * 1024u * 1024u];
static RfRow rows[RF_MAX_ROWS];

int main(int argc, char **argv)
{
    RfSettings st;
    RfGrid g;
    RfMode m;
    RfDoc d;
    u64 len, off = 0, line = 1;
    int target = (argc > 1) ? atoi(argv[1]) : 20;
    int i, n = 0;
    static u64  anch[256];
    static u64  next_off[256];
    static u64  anch_line[256];
    static int  filled[256];
    static u8   term[256];

    rf_settings_default(&st);
    m.truncate_scroll = 0;
    rf_grid_build(&g, &st, 720, 1280, 2000);
    len = rf_test_gen_main(doc, 5u * 1024u * 1024u);
    rf_doc_init(&d, doc, len, rf_count_lines(doc, len));
    printf("doc=%llu B, L=%llu, C=%d V=%d wc=%d rh=%d\n",
           (unsigned long long)len, (unsigned long long)d.line_count,
           g.cols, g.rows, g.wc, g.rh);

    for (i = 0; i < 256; i++) {
        RfPage pg;
        rf_page_layout(&d, &g, &m, off, line, &pg, rows);
        anch[i] = pg.anchor_off; next_off[i] = pg.next_off;
        anch_line[i] = pg.anchor_line; filled[i] = pg.rows_filled; term[i] = pg.term;
        n++;
        if (pg.term & RF_TERM_EOF) break;
        off = pg.next_off; line = pg.next_line;
    }
    printf("swept %d pages\n", n);
    if (target >= n) { printf("target out of range\n"); return 1; }

    printf("P%d: anchor=%llu line=%llu rows=%d term=0x%02X next=%llu\n",
           target - 1, (unsigned long long)anch[target - 1], (unsigned long long)anch_line[target - 1],
           filled[target - 1], term[target - 1], (unsigned long long)next_off[target - 1]);
    printf("P%d: anchor=%llu line=%llu rows=%d term=0x%02X next=%llu\n",
           target, (unsigned long long)anch[target], (unsigned long long)anch_line[target],
           filled[target], term[target], (unsigned long long)next_off[target]);

    {
        u64 A = anch[target], Aline = anch_line[target];
        u64 line_start = rf_line_start_of(&d, A), b_off = line_start;
        int moved = 0, idx = 0;
        RfIter it;
        RfPage pp;
        int rc;

        for (i = 0; i < g.rows + 2; i++) {
            u64 prev;
            if (b_off == 0) break;
            prev = rf_prev_line_start(&d, b_off);
            if (prev >= b_off) break;
            b_off = prev; moved++;
        }
        printf("line_start=%llu  b_off=%llu  moved=%d  b_line=%llu\n",
               (unsigned long long)line_start, (unsigned long long)b_off, moved,
               (unsigned long long)(Aline - (u64)moved));

        rc = rf_page_prev(&d, &g, &m, A, Aline, &pp, rows);
        printf("prev() rc=%d anchor=%llu rows=%d  (期望 anchor=%llu rows=%d)\n",
               rc, (unsigned long long)pp.anchor_off, rc == 0 ? pp.rows_filled : -1,
               (unsigned long long)anch[target - 1], filled[target - 1]);

        rf_iter_init(&it, &d, &g, &m, b_off, Aline - (u64)moved);
        printf("--- 窗口内渲染行（最多 80 行）---\n");
        for (;;) {
            RfRow row;
            u8 t = 0;
            if (it.pos >= A) break;
            if (!rf_iter_next(&it, &row, &t)) break;
            if (row.start_off >= A) break;
            if (idx < 80)
                printf("  [%3d] start=%-9llu end=%-9llu line=%-6llu kind=%d block=%d cols=%d\n",
                       idx, (unsigned long long)row.start_off, (unsigned long long)row.end_off,
                       (unsigned long long)row.line, row.kind, row.block_start, row.cols_used);
            idx++;
        }
        printf("窗口共 %d 行\n", idx);
    }
    return 0;
}
