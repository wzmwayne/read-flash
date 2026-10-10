/* 宿主验证用字形提供者：FreeType + 真字体（文泉驿等宽正黑优先）。
 * 目的：宿主自检/预览必须能看到**真实文字**，否则错字与断行问题看不出来。
 * 安卓端不用本文件（那边由 Java 系统字体光栅化，见 android/app）。
 */
#include "rf.h"
#include <stdio.h>
#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

static FT_Library g_lib;
static FT_Face    g_face;
static int        g_ready;

/* 默认字体候选：规范选定文泉驿等宽正黑，其次 Noto Sans CJK */
static const char *DEFAULT_FONTS[] = {
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
    "/system/fonts/NotoSansCJK-Regular.ttc",
    NULL
};

int rf_glyph_ft_init(const char *path, int face_index)
{
    const char *p = path;
    int i;
    if (g_ready) return 0;
    if (FT_Init_FreeType(&g_lib) != 0) return -1;
    if (!p) {
        const char *env = getenv("RF_FONT");
        if (env && *env) p = env;
    }
    if (!p) {
        for (i = 0; DEFAULT_FONTS[i]; i++) {
            FILE *f = fopen(DEFAULT_FONTS[i], "rb");
            if (f) { fclose(f); p = DEFAULT_FONTS[i]; break; }
        }
    }
    if (!p) return -1;
    if (FT_New_Face(g_lib, p, face_index, &g_face) != 0) return -1;
    g_ready = 1;
    fprintf(stderr, "[glyph] FreeType 字体: %s (face %d, %ld 字形)\n",
            p, face_index, (long)g_face->num_glyphs);
    return 0;
}

int rf_glyph_ft(void *ud, u32 cp, int px, int flags, u8 *alpha, int cap, int *gw, int *gh)
{
    FT_UInt idx;
    int x, y;
    (void)ud;
    if (!g_ready || px < 1) return 0;
    if (FT_Set_Pixel_Sizes(g_face, 0, (FT_UInt)px) != 0) return 0;
    idx = FT_Get_Char_Index(g_face, cp);
    if (idx == 0 && cp != ' ') return 0;             /* 字体没有该码点 */
    if (FT_Load_Glyph(g_face, idx, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) return 0;
    if (flags & RF_GLYPH_BOLD) FT_GlyphSlot_Embolden(g_face->glyph);   /* 合成加粗 */
    {
        FT_Bitmap *bm = &g_face->glyph->bitmap;
        int w = (int)bm->width, h = (int)bm->rows;
        if (w <= 0 || h <= 0) {                          /* 空格等：给 1x1 透明 */
            if (cap < 1) return 0;
            alpha[0] = 0;
            *gw = 1;
            *gh = 1;
            return 1;
        }
        if (w > RF_GLYPH_MAX) w = RF_GLYPH_MAX;
        if (h > RF_GLYPH_MAX) h = RF_GLYPH_MAX;
        if (w * h > cap) return 0;
        for (y = 0; y < h; y++) {
            const unsigned char *src = bm->buffer + (size_t)y * bm->pitch;
            for (x = 0; x < w; x++) {
                u8 v = bm->pixel_mode == FT_PIXEL_MODE_MONO
                     ? (u8)((src[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0)
                     : src[x];
                alpha[y * w + x] = v;
            }
        }
        *gw = w;
        *gh = h;
        return 1;
    }
}
