/* Read-Flash core —— 公共类型与 API
 * 规范：docs/Read-Flash-v3.4-修订条文.md
 * 约定：全程整数运算，不使用浮点（v3.3 P4）；宽度单位 = 半角列。
 */
#ifndef RF_H
#define RF_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;
typedef int64_t  i64;

/* 一页最多支持的渲染行数（2400px / 19px ≈ 126） */
#define RF_MAX_ROWS 160

/* 单个字形位图的最大边长（px）：标题 2× 字号，需留足余量 */
#define RF_GLYPH_MAX 256

/* 版本号（由构建脚本注入 -DRF_VERSION，缺省值用于宿主构建） */
#ifndef RF_VERSION
#define RF_VERSION "0.1.1d1"
#endif

/* ---------------- 设置（§4.1） ---------------- */
#define RF_FONT_DP_MIN       12
#define RF_FONT_DP_MAX       48
#define RF_FONT_DP_DEFAULT   16
#define RF_LINEHT_DP_MIN     8      /* 行高绝对值下限（dp） */
#define RF_LINEHT_DP_MAX     96     /* 行高绝对值上限（dp） */
#define RF_LH_MULT_MIN       10     /* 自动倍数下限 ×10（1.0） */
#define RF_LH_MULT_MAX       30     /* 自动倍数上限 ×10（3.0） */
#define RF_LH_MULT_DEFAULT   12     /* 默认 1.2 */
#define RF_MARGIN_DP_MAX     64
#define RF_MARGIN_DP_DEFAULT 8
#define RF_LINEHT_DP_DEFAULT 19     /* 非自动时的默认行高（约 16dp 字号 × 1.2） */

typedef struct {
    int font_dp;            /* 字号，dp（文字大小）；默认 16 */
    int lh_auto;            /* 行高是否自动（多选框）：1 = 字号 × 倍数，0 = 绝对 dp */
    int lh_tenths;          /* 自动时的倍数 ×10（12 = 1.2，0.1 精度），范围 10–30 */
    int lh_dp;              /* 非自动时的行高绝对值（dp），范围 8–96 */
    int m_top_dp, m_bot_dp, m_left_dp, m_right_dp;   /* 边距 dp（0 表示无边距，非"自动"） */
    int live_edit;          /* 是否实时修改：1 = 调整立即影响渲染（本次会话），0 = 只影响 [预览] */
} RfSettings;

void rf_settings_default(RfSettings *s);

/* ---------------- 网格（§4.2） ---------------- */
typedef struct { int x, y, w, h; } RfRect;

typedef struct {
    int screen_w, screen_h;
    int density;            /* ‰，1000 = 1.0x */
    int fp;                 /* 字号像素 */
    int wc;                 /* 半角列宽（全角 = 2 × wc） */
    int rh;                 /* 行高：行容器高度 = 相邻行推进量（两者现在同一个量） */
    int m_top, m_bot, m_left, m_right;  /* 自动补齐后的边距（只增不减） */
    int cols;               /* C */
    int rows;               /* V */
    RfRect top_bar;         /* 1 个基础行高 */
    RfRect body;            /* C 列 × V 行 */
    RfRect btn_bar;         /* 底栏上行（可选按钮行） */
    RfRect pager_bar;       /* 底栏下行（翻页行） */
    int overflow;           /* 屏幕过小、内容放不下时为 1 */
} RfGrid;

void rf_grid_build(RfGrid *g, const RfSettings *s,
                   int screen_w, int screen_h, int density);

/* ---------------- UTF-8 与字符宽度（§5.1） ---------------- */
#define RF_REPLACEMENT 0xFFFDu

/* 解码一个码点；返回消耗字节数。非法序列返回 1 且 *cp = RF_REPLACEMENT。 */
int rf_utf8_decode(const u8 *p, const u8 *end, u32 *cp);
/* 列宽：全角 = 2，半角 = 1（不存在 0 宽） */
int rf_char_cols(u32 cp);

/* ---------------- 文档与源行（§5.1、§1.3） ---------------- */
typedef struct {
    const u8 *data;
    u64 len;
    u64 line_count;   /* L，源行数（空文件为 0） */
} RfDoc;

void rf_doc_init(RfDoc *d, const u8 *data, u64 len, u64 line_count);
u64  rf_count_lines(const u8 *data, u64 len);      /* 统计 L */
u64  rf_line_end(const RfDoc *d, u64 off);         /* 行内容末尾（不含 \n） */
u64  rf_next_line_start(const RfDoc *d, u64 off);  /* 下一行起点（跳过 \n） */
u64  rf_line_start_of(const RfDoc *d, u64 off);    /* off 所在行的起点 */
u64  rf_prev_line_start(const RfDoc *d, u64 off);  /* 上一行起点（off 须为行首） */
u64  rf_line_index_of(const RfDoc *d, u64 off);    /* off 所在行号（1-based，空文件为 0） */

/* ---------------- 排版（§5.2–§5.5、§8） ---------------- */
#define RF_TERM_FILLED 0x01
#define RF_TERM_FORCED 0x02
#define RF_TERM_EOF    0x04

#define RF_ROW_TEXT   0
#define RF_ROW_TITLE1 1   /* "# "    占 2 渲染行，2.0× */
#define RF_ROW_TITLE2 2   /* "## "   占 2 渲染行，1.5× */
#define RF_ROW_TITLE3 3   /* "### "  占 1 渲染行，1.0× */
#define RF_ROW_BLANK  4   /* 空白补齐 / 换页符填充 */

typedef struct {
    u64 start_off, end_off;  /* 本渲染行对应的源字节区间 */
    u64 line;                /* 该行所属源行号（1-based） */
    u16 cols_used;
    u8  kind;
    u8  truncated;           /* 截断模式下该行被截断（行尾有 …） */
    u8  block_start;         /* 1 = 本行是一个块（正文行 / 标题块首行）的起点 */
    u8  pad[3];
} RfRow;

typedef struct {
    u64 anchor_line, anchor_off;   /* 页首锚点（进度保存的就是这两个值） */
    u64 next_line,  next_off;      /* 下一页起点（= 本页已渲染字节的末尾） */
    int row_count;                 /* 恒 = V */
    int rows_filled;               /* 其中有内容的行数 */
    u8  term;                      /* RF_TERM_* 位标志 */
} RfPage;

typedef struct {
    int truncate_scroll;   /* 1 = 截断 + 行尾 … + 点击滚动（仅生成式书，v3.4.1） */
    int parse_titles;      /* 1 = 解析 # 标题；0 = 按普通文本渲染（目录页用） */
} RfMode;

/* --- 渲染行迭代器（逐渲染行前进，供翻页与回溯共用） --- */
typedef struct {
    const RfDoc *d;
    const RfGrid *g;
    const RfMode *m;
    u64 pos;          /* 当前字节位置 */
    u64 line;         /* pos 所在源行号（1-based） */
    u64 line_end;
    int line_has_nl;
    int state;        /* 0 = 待载入源行；1 = 行内续排 */
    int pad;          /* 1 = 待输出一个"标题后自动空行"（不消耗源字节） */
    int kind;         /* 当前块的行类型 */
    int cap;          /* 当前块的列容量 */
    int block_rows;   /* 当前块占用的渲染行数（标题 > 1，正文 = 0） */
    int block_left;   /* 当前块剩余渲染行数 */
} RfIter;

void rf_iter_init(RfIter *it, const RfDoc *d, const RfGrid *g, const RfMode *m,
                  u64 off, u64 line);
/* 载入当前块（如需要）；返回该块占用的渲染行数（正文为 0）。
 * 若遇到换页符或文件末尾而无法产出，返回 -1 并置 *term。 */
int rf_iter_block(RfIter *it, u8 *term);
/* 产出下一个渲染行；返回 1 成功、0 结束（*term 说明原因）。 */
int rf_iter_next(RfIter *it, RfRow *row, u8 *term);

/* 从 anchor 起排版一页。rows 需能容纳 g->rows 个元素；返回有内容的行数。 */
int rf_page_layout(const RfDoc *d, const RfGrid *g, const RfMode *m,
                   u64 anchor_off, u64 anchor_line, RfPage *pg, RfRow *rows);

/* 上一页（§8.3）：B = max(1, cur_line − (V + 2))，累积渲染行后取
 * "结束于当前锚点之前的最后 V 行"。返回 0 成功；1 = 已在第一页。 */
int rf_page_prev(const RfDoc *d, const RfGrid *g, const RfMode *m,
                 u64 cur_anchor_off, u64 cur_anchor_line,
                 RfPage *pg, RfRow *rows);

/* 翻页键状态（§8.1，v3.4 修正 A2） */
static inline int rf_can_prev(const RfPage *pg) { return pg->anchor_off > 0; }
static inline int rf_can_next(const RfPage *pg) { return (pg->term & RF_TERM_EOF) == 0; }

/* ---------------- 校验与渲染（验证用） ---------------- */
u32 rf_crc32(const u8 *p, size_t n);
u64 rf_fnv1a(const void *p, size_t n);
u64 rf_fnv1a_u64(u64 h, u64 v);

/* 验证用渲染：把每个字符画成其网格单元内的方块（不含真实字形），
 * 用于验证网格几何与渲染确定性。返回整帧的 FNV-1a。 */
u64 rf_render_page(u16 *fb, int fw, int fh, const RfGrid *g,
                   const RfRow *rows, int nrows, const RfDoc *d);

/* 页哈希（确定性比对用） */
u64 rf_hash_page(const RfPage *pg, const RfRow *rows);

/* ---------------- 进度文件（§9.3 RFBOOK/RFPROG 格式，验证用） ---------------- */
/* 编码为文本行格式，返回写入的字节数；buf 需 ≥ 128 字节。 */
int rf_progress_encode(char *buf, size_t cap,
                       u64 source_line, u64 line_offset, u64 total_lines, u64 ts);
/* 解析并校验 CRC32；返回 0 成功。 */
int rf_progress_decode(const char *buf, size_t len,
                       u64 *source_line, u64 *line_offset, u64 *total_lines);

/* ---------------- 自检 ---------------- */
/* 测试辅助：生成与自检完全一致的确定性语料（供 tools/debug_prev.c 复现） */
u64 rf_test_gen_main(u8 *buf, u64 cap);

/* 运行 §16 最小验证 + 不重不漏；返回 0 全部通过。out 写入报告文本。 */
int rf_selftest(char *out, size_t outsz);

/* ---------------- 平台（PAL） ---------------- */
/* 由平台层注入应用数据目录；返回静态字符串。 */
void pal_set_dirs(const char *app_data, const char *import_dir);
const char *pal_app_data_dir(void);
const char *pal_import_dir(void);
u64 pal_ticks_ms(void);
u64 pal_now_unix(void);
/* 退出应用：平台层实现（安卓 finishAffinity、iOS 退出主循环）；宿主直接 exit(0) */
void pal_app_exit(void);
void pal_set_exit_hook(void (*fn)(void));
/* 平台文件选择器（Android: SAF / iOS: UIDocumentPicker）。异步：启动后返回 0；
 * 平台把选中的文件拷进导入目录后，调用 C 侧 rf_app_on_files_picked() 通知。 */
void pal_set_pick_hook(void (*fn)(void));
void pal_pick_files(void);
/* 通知应用：已导入 n 个文件到导入目录（平台在选择完成后调用） */
void rf_app_on_files_picked(int n);

/* ---------------- 字形提供者（D8：排版不依赖字体，字形按网格约束绘制） ---------------- */
/* 取一个码点的 alpha 位图：alpha 缓冲容量 cap 字节，返回写入的宽高。成功返回 1。 */
#define RF_GLYPH_BOLD 1     /* 位标志：加粗（标题三档都要加粗，#/## 另有放大） */
typedef int (*RfGlyphFn)(void *ud, u32 cp, int px, int flags, u8 *alpha, int cap, int *gw, int *gh);
void rf_glyph_set_provider(RfGlyphFn fn, void *ud);
void rf_glyph_reset(void);
int  rf_glyph_get(u32 cp, int px, int flags, const u8 **alpha, int *w, int *h);
/* 宿主验证用：FreeType + 真字体（文泉驿等宽正黑优先，可用 RF_FONT 覆盖） */
int  rf_glyph_ft_init(const char *path, int face_index);
int  rf_glyph_ft(void *ud, u32 cp, int px, int flags, u8 *alpha, int cap, int *gw, int *gh);

/* 内置占位字体（仅当没有真字体时使用）；画描边方块。 */
int  rf_glyph_builtin(void *ud, u32 cp, int px, int flags, u8 *alpha, int cap, int *gw, int *gh);

/* ---------------- 存储（§4.1 设置、§9 存储格式） ---------------- */
#define RF_MAX_BOOKS 256
#define RF_MAX_INDEX 4096          /* index1 条目上限（K ≥ 2^24 时才可能更多） */

typedef struct {
    char root[512];                /* 应用数据目录 */
    RfSettings settings;           /* 当前设置（settingsSaved） */
    char status[192];              /* 底栏提示行文本 */
} RfStore;

typedef struct {
    char id[16];                   /* book/<ID>/ */
    char title[128];               /* .config 书名 */
    u64  size;                     /* 正文字节数 */
    u64  lines;                    /* 行数 L */
} RfBookEnt;

typedef struct {
    u64 L, S, K, N;
    u64 *arr;                      /* 指向外部提供的 RF_MAX_INDEX 数组 */
} RfIndex;

void rf_store_init(RfStore *st, const char *root);
int  rf_settings_load(RfStore *st);
int  rf_settings_save(RfStore *st, const RfSettings *s);   /* 只写配置，不改当前生效设置 */
int  rf_books_scan(RfStore *st, RfBookEnt *out, int max);
int  rf_book_import(RfStore *st, const char *src_path, char *out_id, size_t cap);
int  rf_book_delete(RfStore *st, const char *id);

int  rf_index_build(RfStore *st, const char *id, const u8 *data, u64 len, RfIndex *ix,
                    u64 *storage, int write_file);
int  rf_index_load(RfStore *st, const char *id, const u8 *data, u64 len, RfIndex *ix,
                   u64 *storage);
u64  rf_index_locate(const RfIndex *ix, const u8 *data, u64 off_hint, u64 line);

int  rf_progress_save(RfStore *st, const char *id, u64 line, u64 off, u64 total);
int  rf_progress_load(RfStore *st, const char *id, u64 *line, u64 *off);

/* ---------------- 应用（书树 + 渲染 + 触摸） ---------------- */
#define RF_BOOK_LIBRARY  0
#define RF_BOOK_READER   1
#define RF_BOOK_SETTINGS 2
#define RF_BOOK_DEBUG    3
#define RF_BOOK_TUTORIAL 4
#define RF_BOOK_DIALOG   5
#define RF_BOOK_IMPORT   6
#define RF_BOOK_TOC      7

typedef struct RfApp RfApp;

RfApp *rf_app_create(RfStore *st, int screen_w, int screen_h, int density);
void   rf_app_resize(RfApp *a, int screen_w, int screen_h, int density);
void   rf_app_tap(RfApp *a, int x, int y);
void   rf_app_key_back(RfApp *a);
/* 渲染到 RGB565 帧缓冲（尺寸须与 app 的屏幕尺寸一致）；返回 1 表示画面有变化 */
int    rf_app_render(RfApp *a, u16 *fb, int fw, int fh);
void   rf_app_destroy(RfApp *a);

/* 行与坐标的**唯一**换算（渲染与点击必须共用，防止两者不一致）：
 *   rf_app_row_box(i) → 第 i 行的绘制盒（槽内居中，行高>行距时允许向上溢出）
 *   rf_app_row_at(y)  → y 落在第几行（-1 = 不在正文区） */
void   rf_app_row_box(const RfApp *a, int i, int *y, int *h);
int    rf_app_row_at(const RfApp *a, int y);
/* 调试/自检：当前页行区间是否全部有效（防乱码回归） */
int    rf_app_rows_valid(const RfApp *a);
int    rf_app_comp_dups(const RfApp *a);
int    rf_app_last_action(const RfApp *a);   /* 调试：最近派发的动作 */
int    rf_app_toc_count(const RfApp *a);
int    rf_app_book_count(const RfApp *a);     /* 调试/自检：目录条目数 */
int    rf_app_cur_line(const RfApp *a);      /* 调试/自检：当前阅读行 */
int    rf_app_first_row_line(const RfApp *a);/* 调试/自检：当前页首渲染行的源行号 */
u64    rf_app_toc_line(const RfApp *a, int i);/* 调试/自检：目录第 i 条的行号 */
const char *rf_app_dialog_text(const RfApp *a);   /* 调试/自检：当前提示框文本 */   /* 调试/自检：重复组件 id 次数（应为 0） */

/* 宿主预览/自检用：暴露当前网格（字号变化后坐标会变，测试必须用它） */
void   rf_app_metrics(const RfApp *a, int *sw, int *sh, int *wc, int *rh,
                      int *cols, int *rows, int *body_x, int *body_y, int *bar_y);

/* 宿主预览/自检用：命中框查询（返回动作 id，0 表示无） */
int    rf_app_bar_hit(const RfApp *a, int i, int *x);
int    rf_app_body_hit(const RfApp *a, int i, int *x, int *y);

/* 宿主预览/自检用：取当前画面信息 */
int    rf_app_book_id(const RfApp *a);
u64    rf_app_frame_hash(const RfApp *a);

#endif /* RF_H */
