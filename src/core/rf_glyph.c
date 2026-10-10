/* 字形提供者与缓存（v3.4 D8：排版不依赖字体，字形按网格约束绘制）
 * 平台通过 rf_glyph_set_provider 注入取字形的实现（Android 用系统字体光栅化）。
 */
#include "rf.h"
#include <string.h>

#define G_POOL   (256u * 1024u)     /* alpha 位图池 */
#define G_SLOTS  512

static RfGlyphFn g_fn;
static void     *g_ud;
static u8        g_pool[G_POOL];
static size_t    g_used;
static int       g_n;
static struct { u32 cp; int px, flags, w, h; size_t off; } g_slot[G_SLOTS];
static u8        g_tmp[RF_GLYPH_MAX * RF_GLYPH_MAX];

void rf_glyph_set_provider(RfGlyphFn fn, void *ud)
{
    g_fn = fn;
    g_ud = ud;
    g_used = 0;
    g_n = 0;
}

void rf_glyph_reset(void)
{
    g_used = 0;
    g_n = 0;
}

int rf_glyph_get(u32 cp, int px, int flags, const u8 **alpha, int *w, int *h)
{
    int i;
    if (px < 1) px = 1;
    for (i = 0; i < g_n; i++) {
        if (g_slot[i].cp == cp && g_slot[i].px == px && g_slot[i].flags == flags) {
            *alpha = g_pool + g_slot[i].off;
            *w = g_slot[i].w;
            *h = g_slot[i].h;
            return 1;
        }
    }
    if (!g_fn) return 0;
    {
        int gw = 0, gh = 0;
        if (!g_fn(g_ud, cp, px, flags, g_tmp, (int)sizeof g_tmp, &gw, &gh)) return 0;
        if (gw <= 0 || gh <= 0 || gw > RF_GLYPH_MAX || gh > RF_GLYPH_MAX) return 0;
        if ((size_t)(gw * gh) > sizeof g_tmp) return 0;
        if (g_n >= G_SLOTS || g_used + (size_t)(gw * gh) > G_POOL) {
            g_used = 0;
            g_n = 0;                        /* 按页重建：整表清空 */
        }
        memcpy(g_pool + g_used, g_tmp, (size_t)(gw * gh));
        g_slot[g_n].cp = cp;
        g_slot[g_n].px = px;
        g_slot[g_n].flags = flags;
        g_slot[g_n].w = gw;
        g_slot[g_n].h = gh;
        g_slot[g_n].off = g_used;
        g_used += (size_t)(gw * gh);
        *alpha = g_pool + g_slot[g_n].off;
        *w = gw;
        *h = gh;
        g_n++;
        return 1;
    }
}

/* 宿主/无平台字体时的占位字形：按列宽画一个描边方块（用于验证网格几何） */
int rf_glyph_builtin(void *ud, u32 cp, int px, int flags, u8 *alpha, int cap, int *gw, int *gh)
{
    int w = (rf_char_cols(cp) == 2) ? px : (px + 1) / 2;
    int h = px;
    int x, y;
    (void)ud;
    (void)flags;
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    if (w > RF_GLYPH_MAX) w = RF_GLYPH_MAX;
    if (h > RF_GLYPH_MAX) h = RF_GLYPH_MAX;
    if ((size_t)(w * h) > (size_t)cap) return 0;
    memset(alpha, 0, (size_t)(w * h));
    for (y = 1; y < h - 1; y++)
        for (x = 1; x < w - 1; x++)
            if (x == 1 || y == 1 || x == w - 2 || y == h - 2 ||
                (flags & RF_GLYPH_BOLD && (x == 2 || y == 2)))
                alpha[y * w + x] = 220;
    *gw = w;
    *gh = h;
    return 1;
}
