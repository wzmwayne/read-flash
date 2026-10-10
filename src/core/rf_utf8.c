/* UTF-8 自建解码器与列宽分类（v3.4 §5.1，A13/D7/D8）
 * 宽度只有两档：全角 = 2 列，半角 = 1 列。非法序列 → 替换字符（半角）。
 */
#include "rf.h"

int rf_utf8_decode(const u8 *p, const u8 *end, u32 *cp)
{
    u8 b0;
    if (p >= end) { *cp = RF_REPLACEMENT; return 0; }
    b0 = p[0];

    if (b0 < 0x80) { *cp = b0; return 1; }

    if (b0 >= 0xC2 && b0 <= 0xDF) {                     /* 2 字节 */
        if (p + 1 < end && (p[1] & 0xC0) == 0x80) {
            *cp = ((u32)(b0 & 0x1F) << 6) | (u32)(p[1] & 0x3F);
            return 2;
        }
        *cp = RF_REPLACEMENT; return 1;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {                     /* 3 字节 */
        if (p + 2 < end && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            u32 c = ((u32)(b0 & 0x0F) << 12) |
                    ((u32)(p[1] & 0x3F) << 6) |
                     (u32)(p[2] & 0x3F);
            if (c < 0x800) { *cp = RF_REPLACEMENT; return 1; }   /* 过长编码 */
            if (c >= 0xD800 && c <= 0xDFFF) { *cp = RF_REPLACEMENT; return 1; } /* 代理区 */
            *cp = c; return 3;
        }
        *cp = RF_REPLACEMENT; return 1;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {                     /* 4 字节 */
        if (p + 3 < end && (p[1] & 0xC0) == 0x80 &&
            (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            u32 c = ((u32)(b0 & 0x07) << 18) |
                    ((u32)(p[1] & 0x3F) << 12) |
                    ((u32)(p[2] & 0x3F) << 6) |
                     (u32)(p[3] & 0x3F);
            if (c < 0x10000 || c > 0x10FFFF) { *cp = RF_REPLACEMENT; return 1; }
            *cp = c; return 4;
        }
        *cp = RF_REPLACEMENT; return 1;
    }
    *cp = RF_REPLACEMENT; return 1;                     /* 0x80–0xC1、0xF5–0xFF */
}

/* 全角码点区间（v3.4 §5.1 表；不在表内一律半角） */
static int rf_is_wide(u32 c)
{
    if (c >= 0x1100  && c <= 0x115F)  return 1;
    if (c >= 0x2E80  && c <= 0xA4CF)  return 1;
    if (c >= 0xAC00  && c <= 0xD7A3)  return 1;
    if (c >= 0xF900  && c <= 0xFAFF)  return 1;
    if (c >= 0xFE10  && c <= 0xFE19)  return 1;
    if (c >= 0xFE30  && c <= 0xFE6F)  return 1;
    if (c >= 0xFF00  && c <= 0xFF60)  return 1;
    if (c >= 0xFFE0  && c <= 0xFFE6)  return 1;
    if (c >= 0x1F300 && c <= 0x1F64F) return 1;
    if (c >= 0x1F900 && c <= 0x1F9FF) return 1;
    if (c >= 0x20000 && c <= 0x3FFFD) return 1;
    return 0;
}

int rf_char_cols(u32 cp)
{
    return rf_is_wide(cp) ? 2 : 1;
}
