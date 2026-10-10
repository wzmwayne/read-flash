/* 网格构建（v3.4 §4.2）——全整数运算 */
#include "rf.h"

void rf_settings_default(RfSettings *s)
{
    s->font_dp = RF_FONT_DP_DEFAULT;
    s->line_height_dp = RF_LINEHT_DP_DEFAULT;
    s->m_top_dp = s->m_bot_dp = s->m_left_dp = s->m_right_dp = RF_MARGIN_DP_DEFAULT;
}

/* 整数四舍五入：dp → px */
static int dp2px(int dp, int density)
{
    int v;
    if (dp <= 0) return 0;
    v = (dp * density + 500) / 1000;
    return v < 0 ? 0 : v;
}

void rf_grid_build(RfGrid *g, const RfSettings *s,
                   int screen_w, int screen_h, int density)
{
    int avail_w, avail_h, extra;

    g->screen_w = screen_w;
    g->screen_h = screen_h;
    g->density  = density;
    g->overflow = 0;

    /* 字号像素（≥1） */
    g->fp = dp2px(s->font_dp, density);
    if (g->fp < 1) g->fp = 1;

    /* 半角列宽 = ceil(fp / 2)；全角 = 2 × wc */
    g->wc = (g->fp + 1) / 2;
    if (g->wc < 1) g->wc = 1;

    /* 行高 = 行容器高度（0 = 自动 = 字号 × 1.2）；文本大小由字号决定，不受行高约束 */
    g->rh = (s->line_height_dp > 0) ? dp2px(s->line_height_dp, density)
                                    : (g->fp * 1200 + 999) / 1000;
    if (g->rh < 1) g->rh = 1;

    /* 四向边距（像素） */
    g->m_top   = dp2px(s->m_top_dp,   density);
    g->m_bot   = dp2px(s->m_bot_dp,   density);
    g->m_left  = dp2px(s->m_left_dp,  density);
    g->m_right = dp2px(s->m_right_dp, density);

    /* 列数 C（退化时保留设置边距，只标记 overflow） */
    avail_w = screen_w - g->m_left - g->m_right;
    if (avail_w < 0) avail_w = 0;
    g->cols = avail_w / g->wc;
    if (g->cols < 1) { g->cols = 1; g->overflow = 1; }

    /* 纵向：上下边距之内按"行距"排槽位 —— 顶栏 1 + 正文 V + 底栏 2 */
    avail_h = screen_h - g->m_top - g->m_bot;
    if (avail_h < 0) avail_h = 0;
    g->rows = avail_h / g->rh - 3;
    if (g->rows < 1) { g->rows = 1; g->overflow = 1; }

    /* 自动边距：余数全部给右/下（只增不减，A12） */
    extra = (screen_w - g->m_left - g->m_right) - g->cols * g->wc;
    if (extra > 0) g->m_right += extra;
    extra = (screen_h - g->m_top - g->m_bot) - (g->rows + 3) * g->rh;
    if (extra > 0) g->m_bot += extra;

    /* 区域矩形：上下左右边距作用于**整个绘制面**（顶栏/正文/底栏都在边距之内） */
    g->top_bar.x = g->m_left;            g->top_bar.y = g->m_top;
    g->top_bar.w = g->cols * g->wc;      g->top_bar.h = g->rh;

    g->body.x = g->m_left;               g->body.y = g->m_top + g->rh;
    g->body.w = g->cols * g->wc;         g->body.h = g->rows * g->rh;

    g->btn_bar.x = g->m_left;            g->btn_bar.y = g->body.y + g->body.h;
    g->btn_bar.w = g->cols * g->wc;      g->btn_bar.h = g->rh;

    g->pager_bar.x = g->m_left;          g->pager_bar.y = g->btn_bar.y + g->rh;
    g->pager_bar.w = g->cols * g->wc;    g->pager_bar.h = g->rh;
}
