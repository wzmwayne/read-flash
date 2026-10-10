/* 验证用渲染器：把每个字符画成其网格单元内的方块（不含真实字形）。
 * 目的：验证网格几何与渲染确定性，而不是字形效果。 */
#include "rf.h"

static void fill_rect(u16 *fb, int fw, int fh, int x, int y, int w, int h, u16 color)
{
    int yy, xx;
    for (yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= fh) continue;
        for (xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= fw) continue;
            fb[(size_t)yy * (size_t)fw + (size_t)xx] = color;
        }
    }
}

static u16 kind_color(int kind)
{
    switch (kind) {
    case RF_ROW_TITLE1:
    case RF_ROW_TITLE2:
    case RF_ROW_TITLE3: return 0x07FF;   /* 青 */
    default:            return 0xE71C;   /* #E0E0E0 */
    }
}

u64 rf_render_page(u16 *fb, int fw, int fh, const RfGrid *g,
                   const RfRow *rows, int nrows, const RfDoc *d)
{
    size_t i, n = (size_t)fw * (size_t)fh;
    int r;

    for (i = 0; i < n; i++) fb[i] = 0;                 /* 黑底（§3.3） */

    /* 顶栏：1 个基础行高 + 左侧 < 占位 */
    fill_rect(fb, fw, fh, 0, g->top_bar.h - 1, g->top_bar.w, 1, 0x4208);
    fill_rect(fb, fw, fh, 1, 2, g->wc, g->rh - 4, 0x07FF);

    /* 正文 */
    for (r = 0; r < nrows && r < g->rows; r++) {
        int y = g->body.y + r * g->rh;
        int col = 0;
        u64 p = rows[r].start_off;
        u16 color;

        if (rows[r].kind == RF_ROW_BLANK) continue;
        color = kind_color(rows[r].kind);
        while (p < rows[r].end_off && col < g->cols) {
            u32 cp;
            int nb = rf_utf8_decode(d->data + p, d->data + rows[r].end_off, &cp);
            int c, cw;
            if (nb <= 0) break;
            c = rf_char_cols(cp);
            cw = c * g->wc;
            if (col + c > g->cols) break;
            fill_rect(fb, fw, fh,
                      g->body.x + col * g->wc + 1, y + 1,
                      (cw > 2 ? cw - 2 : cw), (g->rh > 2 ? g->rh - 2 : g->rh), color);
            col += c;
            p += (u64)nb;
        }
        if (rows[r].truncated) {                        /* 行尾省略号（1 个半角列） */
            fill_rect(fb, fw, fh,
                      g->body.x + (g->cols - 1) * g->wc + 1,
                      y + g->rh / 2, g->wc > 1 ? g->wc - 2 : 1, 2, color);
        }
    }

    /* 底栏 2 行：按钮行（占位）+ 翻页行（< 左对齐、> 右对齐） */
    fill_rect(fb, fw, fh, 0, g->btn_bar.y, g->btn_bar.w, 1, 0x4208);
    fill_rect(fb, fw, fh, g->wc * 2, g->btn_bar.y + 2, g->wc * 4, g->rh - 4, 0x3E0);
    fill_rect(fb, fw, fh, 0, g->pager_bar.y, g->pager_bar.w, 1, 0x4208);
    fill_rect(fb, fw, fh, 1, g->pager_bar.y + 2, g->wc, g->rh - 4, 0x6660);
    fill_rect(fb, fw, fh, fw - g->wc - 1, g->pager_bar.y + 2, g->wc, g->rh - 4, 0x6660);

    return rf_fnv1a(fb, n * 2);
}

u64 rf_hash_page(const RfPage *pg, const RfRow *rows)
{
    u64 h = 1469598103934665603ULL;
    int i;
    h = rf_fnv1a_u64(h, pg->anchor_off);
    h = rf_fnv1a_u64(h, pg->anchor_line);
    h = rf_fnv1a_u64(h, pg->next_off);
    h = rf_fnv1a_u64(h, pg->rows_filled);
    h = rf_fnv1a_u64(h, pg->term);
    for (i = 0; i < pg->row_count; i++) {
        h = rf_fnv1a_u64(h, rows[i].start_off);
        h = rf_fnv1a_u64(h, rows[i].end_off);
        h = rf_fnv1a_u64(h, rows[i].cols_used);
        h = rf_fnv1a_u64(h, rows[i].kind);
    }
    return h;
}
