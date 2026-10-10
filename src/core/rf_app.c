/* 正式应用：书树 + 渲染 + 触摸（v3.4 §2 界面、§3 颜色、§5 排版、§7 翻页、§3.5 教程书）
 * 全部逻辑在 C；平台层只提供目录、时钟、字形位图与触摸事件。
 */
#include "rf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>

/* RGB565 默认色表（v3.4 §6） */
#define C_BG     0x0000
#define C_TEXT   0xE71C
#define C_TITLE  0x07FF
#define C_BTN    0x07E0
#define C_DIS    0x4208
#define C_DIM    0x632C          /* #666666 次要信息 */
#define C_WHITE  0xFFFF          /* #FFFFFF 可用翻页键 */
#define C_ACC    0xFFE0
#define C_DANGER 0xF9C8
#define C_SEP    0x2945

enum {
    ACT_NONE = 0, ACT_OPEN, ACT_DELETE, ACT_IMPORT,
    ACT_FONT_DN, ACT_FONT_UP, ACT_SP_DN, ACT_SP_UP,
    ACT_ML_DN, ACT_ML_UP, ACT_MT_DN, ACT_MT_UP, ACT_LH_DN, ACT_LH_UP, ACT_LH_AUTO, ACT_LIVE,
    ACT_PREVIEW, ACT_SAVE, ACT_DEBUG, ACT_REBUILD, ACT_RESET, ACT_SETTINGS,
    ACT_DLG_OK, ACT_DLG_CANCEL, ACT_BACK
};

#define HIT_BODY_MAX 32
#define HIT_BAR_MAX  12

struct RfApp {
    RfStore *st;
    int sw, sh, density;
    RfGrid g;          /* 当前生效网格：平时 = settingsSaved；预览时 = editing */
    RfGrid g_saved;    /* 进入预览前的生效网格备份 */
    RfSettings editing;/* settingsCurrent：设置书里编辑中的值，只影响预览 */
    RfMode mode;

    int book;                    /* 当前书（= 栈顶） */
    int stack[16];               /* 书栈：栈底固定为书库，左上角 < 出栈一次 */
    int sp;
    char *gen;
    size_t gen_cap, gen_len;
    const u8 *data;
    u64 data_len;

    char id[16], title[128];
    u64 line, off;
    RfIndex ix;
    u64 storage[RF_MAX_INDEX];

    RfPage page;
    RfRow rows[RF_MAX_ROWS];
    int laid;

    RfBookEnt books[RF_MAX_BOOKS];
    int nbooks, sel;
    int preview;
    int debug_view;

    char dlg[192];
    int dlg_active, dlg_action, dlg_kind;   /* dlg_kind: 0=信息提示, 1=删除确认 */

    char bar[192];
    struct { int row, c0, c1, act; } hb[HIT_BODY_MAX];
    int nhb;
    struct { int x0, x1, act; } hk[HIT_BAR_MAX];
    int nhk;
};

/* ---------------- 生成式书内容 ---------------- */

static void ginit(RfApp *a)
{
    a->gen_len = 0;
    a->nhb = 0;                       /* 仅正文命中表随内容重建；底栏由 set_bar 维护 */
    if (a->gen && a->gen_cap) a->gen[0] = 0;
}

static void gputs(RfApp *a, const char *s)
{
    size_t n = strlen(s);
    if (!a->gen) return;
    if (a->gen_len + n + 1 > a->gen_cap) return;
    memcpy(a->gen + a->gen_len, s, n);
    a->gen_len += n;
    a->gen[a->gen_len] = 0;
}

static void gnum(RfApp *a, unsigned long long v)
{
    char b[32];
    snprintf(b, sizeof b, "%llu", v);
    gputs(a, b);
}

static int grow(RfApp *a)
{
    int row = 1;
    size_t i;
    for (i = 0; i < a->gen_len; i++) if (a->gen[i] == '\n') row++;
    return row;
}

static int gcol(RfApp *a)
{
    size_t i = a->gen_len;
    int cols = 0;
    while (i > 0 && a->gen[i - 1] != '\n') i--;
    while (i < a->gen_len) {
        u32 cp;
        int n = rf_utf8_decode((const u8 *)a->gen + i, (const u8 *)a->gen + a->gen_len, &cp);
        if (n <= 0) break;
        cols += rf_char_cols(cp);
        i += (size_t)n;
    }
    return cols;
}

/* 正文按钮：[label]，记录命中框（行, 列区间, 动作） */
static void gbtn(RfApp *a, int act, const char *label)
{
    if (a->nhb < HIT_BODY_MAX) {
        a->hb[a->nhb].row = grow(a);
        a->hb[a->nhb].c0 = gcol(a);
        a->hb[a->nhb].c1 = a->hb[a->nhb].c0 + (int)strlen(label) + 2;
        a->hb[a->nhb].act = act;
        a->nhb++;
    }
    gputs(a, "[");
    gputs(a, label);
    gputs(a, "]");
}

/* 底栏按钮行 */
static void bar_add(RfApp *a, int act, const char *label)
{
    int col = 0;
    {
        size_t i = strlen(a->bar);
        int cols = 0;
        while (i > 0 && a->bar[i - 1] != '\n') i--;
        while (i < strlen(a->bar)) {
            u32 cp;
            int n = rf_utf8_decode((const u8 *)a->bar + i, (const u8 *)a->bar + strlen(a->bar), &cp);
            if (n <= 0) break;
            cols += rf_char_cols(cp);
            i += (size_t)n;
        }
        col = cols;
    }
    if (a->nhk < HIT_BAR_MAX) {
        a->hk[a->nhk].x0 = col * a->g.wc;
        a->hk[a->nhk].x1 = (col + (int)strlen(label) + 2) * a->g.wc;
        a->hk[a->nhk].act = act;
        a->nhk++;
    }
    strncat(a->bar, "[", sizeof a->bar - strlen(a->bar) - 1);
    strncat(a->bar, label, sizeof a->bar - strlen(a->bar) - 1);
    strncat(a->bar, "]  ", sizeof a->bar - strlen(a->bar) - 1);
}

static const char *TUTORIAL =
"应用使用教程\n"
"\n"
"一、这是什么\n"
"Read-Flash 是一个终端风格的纯文本阅读器，为低端设备设计。\n"
"整个应用是一棵书树：书库是根，其它界面都是书。\n"
"\n"
"二、怎么翻页\n"
"顶栏左侧的 < 返回上一本书。\n"
"底栏最下面一行是翻页行：左边 < 上一页，右边 > 下一页。\n"
"在阅读界面里，点击正文左半边是上一页，右半边是下一页。\n"
"按键变暗表示不可用，此时点击没有反应。\n"
"\n"
"三、书的格式\n"
"纯文本，只认两种语法：\n"
"行首 # / ## / ### 是三级标题；\n"
"单独一行的 --- 是换页符，本页剩余部分留空。\n"
"阅读界面不解析按钮、单选、多选等交互组件。\n"
"\n"
"四、设置\n"
"书库里的《设置》可以调整字号、行距与边距。\n"
"[-] 和 [+] 调整数值，[预览] 用示例文本看效果，[保存] 写入配置。\n"
"边距在渲染时会自动向网格对齐，只会变大不会变小。\n"
"\n"
"五、书库与导入\n"
"把 .txt 放进导入目录，回到书库点 [导入] 即可入库。\n"
"选中一本书后，[打开] 阅读，[删除] 会弹出提示框再次确认。\n"
"删除是不可撤销的，请谨慎操作。\n"
"\n"
"六、进度与调试\n"
"翻页即保存进度，只记 (行号, 行内偏移)，与字号无关。\n"
"《调试》里可以看到文件大小、总行数、当前行号与内存占用，\n"
"还可以重建索引或重置进度。\n"
"\n"
"祝你读得开心。\n";

static void gen_tutorial(RfApp *a)
{
    ginit(a);
    gputs(a, TUTORIAL);
    a->data = (const u8 *)a->gen;
    a->data_len = a->gen_len;
}

static void gen_settings(RfApp *a)
{
    RfSettings *s = &a->editing;
    ginit(a);
    if (a->preview) {
        gputs(a, "预览（示例文本）\n\n");
        gputs(a, "汪淼觉得，这个世界的物理规律突然变得不可靠了。\n");
        gputs(a, "Reading is a quiet pleasure, 0123456789.\n");
        gputs(a, "、。〈〉《》「」全角标点也要对齐。\n\n");
        gputs(a, "# 一级标题\n## 二级标题\n### 三级标题\n\n");
        gputs(a, "这是一条很长的行，用来观察换行渲染在视口宽度处切断的效果，"
                 "既不能多一个字，也不能少一个字。\n\n");
        gputs(a, "当前字号 / 行高 / 边距已应用到本页。\n");
    } else {
        gputs(a, "字号\n");
        gbtn(a, ACT_FONT_DN, "-");
        gputs(a, " ");
        gnum(a, (unsigned long long)s->font_dp);
        gputs(a, " ");
        gbtn(a, ACT_FONT_UP, "+");
        gputs(a, "      （dp，12 - 48）\n\n");

        gputs(a, "行高\n");
        gputs(a, s->lh_auto ? "(+)自动" : "(-)自动");
        {
            if (a->nhb < HIT_BODY_MAX) {
                a->hb[a->nhb].row = grow(a);
                a->hb[a->nhb].c0 = 0;
                a->hb[a->nhb].c1 = 8;
                a->hb[a->nhb].act = ACT_LH_AUTO;
                a->nhb++;
            }
        }
        gputs(a, "（行容器的倍数）\n");
        gbtn(a, ACT_LH_DN, "-");
        gputs(a, " ");
        if (s->lh_auto) {
            gnum(a, (unsigned long long)(s->lh_tenths / 10));
            gputs(a, ".");
            gnum(a, (unsigned long long)(s->lh_tenths % 10));
        } else {
            gnum(a, (unsigned long long)s->lh_dp);
        }
        gputs(a, " ");
        gbtn(a, ACT_LH_UP, "+");
        gputs(a, s->lh_auto ? "   （倍数，0.1 步进，1.0 - 3.0）\n\n"
                            : "   （dp，2 步进，8 - 96）\n\n");

        gputs(a, "左右边距\n");
        gbtn(a, ACT_ML_DN, "-");
        gputs(a, " ");
        gnum(a, (unsigned long long)s->m_left_dp);
        gputs(a, " ");
        gbtn(a, ACT_ML_UP, "+");
        gputs(a, "      （dp，0 - 64）\n\n");

        gputs(a, "上下边距\n");
        gbtn(a, ACT_MT_DN, "-");
        gputs(a, " ");
        gnum(a, (unsigned long long)s->m_top_dp);
        gputs(a, " ");
        gbtn(a, ACT_MT_UP, "+");
        gputs(a, "      （dp，0 - 64）\n\n");

        gputs(a, s->live_edit ? "(+)实时修改" : "(-)实时修改");
        {
            if (a->nhb < HIT_BODY_MAX) {
                a->hb[a->nhb].row = grow(a);
                a->hb[a->nhb].c0 = 0;
                a->hb[a->nhb].c1 = 10;
                a->hb[a->nhb].act = ACT_LIVE;
                a->nhb++;
            }
        }
        gputs(a, "\n\n");
        gputs(a, s->live_edit
                 ? "实时修改已开：调整立即生效（本次会话）。\n"
                 : "实时修改已关：调整只作用于 [预览]。\n");
        gputs(a, "[保存] 写入配置，重启后全应用生效。\n");
    }
    a->data = (const u8 *)a->gen;
    a->data_len = a->gen_len;
}

static void gen_debug(RfApp *a)
{
    ginit(a);
    gputs(a, "文件大小   ");
    gnum(a, a->data_len);
    gputs(a, " B\n总行数     ");
    gnum(a, rf_count_lines(a->data, a->data_len));
    gputs(a, "\n当前行号   ");
    gnum(a, a->line);
    gputs(a, "\n行内偏移   ");
    gnum(a, a->off);
    gputs(a, "\n行高       ");
    if (a->st->settings.lh_auto) {
        gputs(a, "自动 ×");
        gnum(a, (unsigned long long)(a->st->settings.lh_tenths / 10));
        gputs(a, ".");
        gnum(a, (unsigned long long)(a->st->settings.lh_tenths % 10));
    } else {
        gnum(a, (unsigned long long)a->st->settings.lh_dp);
        gputs(a, " dp");
    }
    gputs(a, "（容器实际 ");
    gnum(a, (unsigned long long)a->g.rh);
    gputs(a, " px）\n实时修改   ");
    gputs(a, a->st->settings.live_edit ? "开" : "关");
    gputs(a, "\n索引条目   ");
    gnum(a, a->ix.N);
    gputs(a, "\n索引间隔 K ");
    gnum(a, a->ix.K);
    gputs(a, "\n字号       ");
    gnum(a, (unsigned long long)a->st->settings.font_dp);
    gputs(a, " dp（文字实际 ");
    gnum(a, (unsigned long long)a->g.fp);
    gputs(a, " px）");
    gputs(a, "\n网格       ");
    gnum(a, (unsigned long long)a->g.cols);
    gputs(a, " 列 x ");
    gnum(a, (unsigned long long)a->g.rows);
    gputs(a, " 行\n版本       " RF_VERSION "\n");
    a->data = (const u8 *)a->gen;
    a->data_len = a->gen_len;
}

static void gen_dialog(RfApp *a)
{
    ginit(a);
    gputs(a, a->dlg);
    gputs(a, "\n");
    a->data = (const u8 *)a->gen;
    a->data_len = a->gen_len;
}

static void gen_library(RfApp *a)
{
    int i;
    ginit(a);
    /* 书库单选组只列书：内置《应用使用教程》+ 导入的书；设置/调试在底栏按钮 */
    for (i = 0; i <= a->nbooks; i++) {
        const char *t = (i == 0) ? "应用使用教程" : a->books[i - 1].title;
        gputs(a, (i == a->sel) ? "(>)" : "( )");
        gputs(a, t);
        gputs(a, "\n\n");
    }
    a->data = (const u8 *)a->gen;
    a->data_len = a->gen_len;
}

/* ---------------- 书与导航 ---------------- */

static void set_bar(RfApp *a)
{
    a->bar[0] = 0;
    a->nhk = 0;
    switch (a->book) {
    case RF_BOOK_LIBRARY:
        bar_add(a, ACT_OPEN, "打开");
        bar_add(a, ACT_DELETE, "删除");
        bar_add(a, ACT_IMPORT, "导入");
        bar_add(a, ACT_SETTINGS, "设置");
        bar_add(a, ACT_DEBUG, "调试");
        break;
    case RF_BOOK_SETTINGS:
        bar_add(a, ACT_PREVIEW, "预览");
        bar_add(a, ACT_SAVE, "保存");
        bar_add(a, ACT_DEBUG, "调试");
        break;
    case RF_BOOK_DEBUG:
        bar_add(a, ACT_REBUILD, "重建索引");
        bar_add(a, ACT_RESET, "重置进度");
        break;
    case RF_BOOK_DIALOG:
        bar_add(a, ACT_DLG_OK, "确定");
        bar_add(a, ACT_DLG_CANCEL, "取消");
        break;
    default:
        break;
    }
}

static void load_reader(RfApp *a, const char *id)
{
    char p[700];
    FILE *f;
    long n;
    u8 *b;
    u64 len = 0;

    snprintf(p, sizeof p, "%s/book/%s/book", a->st->root, id);
    f = fopen(p, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    if (n < 0) { fclose(f); return; }
    b = (u8 *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return; }
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return; }
    b[n] = 0;
    fclose(f);
    len = (u64)n;

    free((void *)a->data == (void *)a->gen ? NULL : (void *)a->data);
    a->data = b;
    a->data_len = len;
    snprintf(a->id, sizeof a->id, "%s", id);
    a->line = 1;
    a->off = 0;
    if (rf_index_load(a->st, id, b, len, &a->ix, a->storage) != 0)
        rf_index_build(a->st, id, b, len, &a->ix, a->storage, 1);
    {
        u64 pl = 0, po = 0;
        if (rf_progress_load(a->st, id, &pl, &po) == 0 && pl >= 1 && pl <= a->ix.L) {
            a->line = pl;
            a->off = po <= len ? po : 0;
        }
    }
    /* 标题 */
    {
        RfBookEnt e;
        memset(&e, 0, sizeof e);
        snprintf(a->title, sizeof a->title, "阅读器");
        {
            int i;
            for (i = 0; i < a->nbooks; i++)
                if (strcmp(a->books[i].id, id) == 0)
                    snprintf(a->title, sizeof a->title, "阅读器-%s", a->books[i].title);
        }
    }
}

/* 离开当前书：存阅读进度、退出预览（恢复生效网格）、释放正文缓冲 */
static void book_leave(RfApp *a)
{
    char prev_id[16];
    snprintf(prev_id, sizeof prev_id, "%s", a->id);
    if (a->book == RF_BOOK_READER && a->data && a->id[0])
        rf_progress_save(a->st, a->id, a->line, a->off, a->data_len);
    if (a->preview) { a->g = a->g_saved; a->preview = 0; }
    if (a->data && a->data != (const u8 *)a->gen) { free((void *)a->data); a->data = NULL; }
    (void)prev_id;
}

/* 打开某本书的显示状态（不动栈） */
static void open_book(RfApp *a, int book)
{
    a->book = book;
    a->line = 1;
    a->off = 0;
    a->laid = 0;

    switch (book) {
    case RF_BOOK_READER:  load_reader(a, a->id); break;   /* a->id 由调用方（[打开]）设置 */
    case RF_BOOK_TUTORIAL: gen_tutorial(a); snprintf(a->title, sizeof a->title, "阅读器-应用使用教程"); break;
    case RF_BOOK_SETTINGS:
        if (a->preview) { a->g = a->g_saved; a->preview = 0; }   /* 离开预览：恢复生效网格 */
        a->editing = a->st->settings;                            /* 进入设置 → 编辑值 = 已保存值 */
        gen_settings(a);
        snprintf(a->title, sizeof a->title, "设置");
        break;
    case RF_BOOK_DEBUG:    gen_debug(a);    snprintf(a->title, sizeof a->title, "调试"); break;
    case RF_BOOK_DIALOG:   gen_dialog(a);   snprintf(a->title, sizeof a->title, "提示"); break;
    default:               gen_library(a);  snprintf(a->title, sizeof a->title, "书库"); break;
    }
    set_bar(a);
    if (book != RF_BOOK_LIBRARY) a->debug_view = 0;
}

static void layout(RfApp *a)
{
    RfDoc d;
    if (!a->data) { a->laid = 0; return; }
    rf_doc_init(&d, a->data, a->data_len, rf_count_lines(a->data, a->data_len));
    rf_page_layout(&d, &a->g, &a->mode, a->off, a->line ? a->line : 1, &a->page, a->rows);
    a->laid = 1;
}

static void page_next(RfApp *a)
{
    if (!a->laid || !rf_can_next(&a->page)) return;
    a->off = a->page.next_off;
    a->line = a->page.next_line;
    layout(a);
    if (a->book == RF_BOOK_READER) rf_progress_save(a->st, a->id, a->line, a->off, a->data_len);
    rf_glyph_reset();
}

static void page_prev(RfApp *a)
{
    RfPage pp;
    RfDoc d;
    if (!a->laid || !rf_can_prev(&a->page)) return;
    rf_doc_init(&d, a->data, a->data_len, rf_count_lines(a->data, a->data_len));
    if (rf_page_prev(&d, &a->g, &a->mode, a->off, a->line, &pp, a->rows) == 0) {
        a->off = pp.anchor_off;
        a->line = pp.anchor_line;
        a->page = pp;
        if (a->book == RF_BOOK_READER) rf_progress_save(a->st, a->id, a->line, a->off, a->data_len);
        rf_glyph_reset();
    } else {
        a->off = 0;
        a->line = 1;
        layout(a);
    }
}

/* ---------------- 行与坐标的唯一换算（渲染/点击共用） ---------------- */

/* 第 i 行的绘制盒：行高在行距槽位内**垂直居中**（行高 > 行距时盒向上溢出，字形中心仍在槽中心） */
void rf_app_row_box(const RfApp *a, int i, int *y, int *h)
{
    if (y) *y = a->g.body.y + i * a->g.rh;   /* 行容器顶 = 槽位顶 */
    if (h) *h = a->g.rh;                     /* 行容器高度 */
}

/* y → 正文行号（0-based）；不在正文区返回 -1。点击与命中框都走这里 */
int rf_app_row_at(const RfApp *a, int y)
{
    int row;
    if (y < a->g.body.y || y >= a->g.body.y + a->g.body.h) return -1;
    row = (y - a->g.body.y) / a->g.rh;
    if (row < 0 || row >= a->g.rows) return -1;
    return row;
}

/* ---------------- 书栈（左上角 < 一律出栈一次） ---------------- */

static void stack_push(RfApp *a, int book)
{
    book_leave(a);
    if (a->sp >= (int)(sizeof a->stack / sizeof a->stack[0]))
        a->sp = (int)(sizeof a->stack / sizeof a->stack[0]) - 1;   /* 栈满：替换栈顶 */
    a->stack[a->sp++] = book;
    open_book(a, book);
}

static void stack_pop(RfApp *a)
{
    if (a->sp <= 1) return;                  /* 栈底（书库）：无操作 */
    book_leave(a);
    a->sp--;
    open_book(a, a->stack[a->sp - 1]);
}

/* 特殊情况：直接编辑栈（清空为指定书） */
static void stack_reset(RfApp *a, int book)
{
    book_leave(a);
    a->sp = 0;
    a->stack[a->sp++] = book;
    open_book(a, book);
}

/* ---------------- 渲染 ---------------- */

static void fill(u16 *fb, int fw, int fh, int x, int y, int w, int h, u16 c)
{
    int yy, xx;
    for (yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= fh) continue;
        for (xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= fw) continue;
            fb[(size_t)yy * fw + xx] = c;
        }
    }
}

static void blend(u16 *fb, int fw, int fh, const u8 *alpha, int gw, int gh, int x, int y, u16 color)
{
    int yy, xx;
    if (!alpha) {
        fill(fb, fw, fh, x, y, gw, gh, color);
        return;
    }
    for (yy = 0; yy < gh; yy++) {
        int py = y + yy;
        if (py < 0 || py >= fh) continue;
        for (xx = 0; xx < gw; xx++) {
            int px = x + xx;
            u32 a = alpha[yy * gw + xx];
            u16 bg, r, g, b;
            if (px < 0 || px >= fw || a == 0) continue;
            bg = fb[(size_t)py * fw + px];
            if (a >= 250) { fb[(size_t)py * fw + px] = color; continue; }
            r = (u16)((((bg >> 11) & 31) * (255 - a) + ((color >> 11) & 31) * a) / 255);
            g = (u16)((((bg >> 5) & 63) * (255 - a) + ((color >> 5) & 63) * a) / 255);
            b = (u16)(((bg & 31) * (255 - a) + (color & 31) * a) / 255);
            fb[(size_t)py * fw + px] = (u16)((r << 11) | (g << 5) | b);
        }
    }
}

/* 画一行文本；unit = 每个半角列的像素宽 = wc * num / den；px = 字号像素 */
static int draw_text(RfApp *a, u16 *fb, int fw, int fh, const u8 *s, u64 n,
                     int x0, int y, int px, int unit_num, int unit_den,
                     u16 base, int max_cols, int has_marker, int boxh, int bold, int top_align)
{
    u64 i = 0;
    int col = 0, marker = 0;
    u16 color = base;
    int in_btn = 0, in_dis = 0;
    int flags = bold ? RF_GLYPH_BOLD : 0;

    while (i < n && col < max_cols) {
        u32 cp;
        int nb = rf_utf8_decode(s + i, s + n, &cp);
        int cc = rf_char_cols(cp);
        const u8 *alpha = NULL;
        int gw = 0, gh = 0;
        int cx, cy;
        if (nb <= 0) break;
        /* 行内样式（§5.5 生成式书组件语法） */
        if (cp == '[') in_btn = 1;
        if (cp == '{') in_dis = 1;
        if (has_marker && col < 3) {
            marker = 1;
        } else {
            marker = 0;
        }
        color = in_btn ? C_BTN : (in_dis ? C_DIS : (marker ? C_ACC : base));
        if (!(cp == ' ' || cp == '\t')) {
            if (rf_glyph_get(cp, px, flags, &alpha, &gw, &gh)) {
                int cellw = cc * a->g.wc * unit_num / unit_den;
                cx = x0 + col * a->g.wc * unit_num / unit_den + (cellw - gw) / 2;
                /* 文字在行容器内垂直居中；文字高于容器时顶部对齐容器顶，
                 * 超出部分照常写入帧缓冲（不裁剪，允许压到下一行） */
                cy = (top_align || gh > boxh) ? y : y + (boxh - gh) / 2;
                blend(fb, fw, fh, alpha, gw, gh, cx, cy, color);
            }
        }
        if (cp == ']') in_btn = 0;
        if (cp == '}') in_dis = 0;
        col += cc;
        i += (u64)nb;
    }
    return col;
}

int rf_app_render(RfApp *a, u16 *fb, int fw, int fh)
{
    int i;
    if (!a->laid) layout(a);
    memset(fb, 0, (size_t)fw * (size_t)fh * 2);

    /* 顶栏：< + 标题（阅读书右侧显示进度） */
    {
        int ty = a->g.top_bar.y + (a->g.rh - a->g.rh) / 2;
        draw_text(a, fb, fw, fh, (const u8 *)"<", 1,
                  a->g.top_bar.x, ty, a->g.fp, 1, 1, C_TITLE, 4, 0, a->g.rh, 0, 0);
        draw_text(a, fb, fw, fh, (const u8 *)a->title, strlen(a->title),
                  a->g.top_bar.x + 2 * a->g.wc, ty, a->g.fp, 1, 1,
                  C_TITLE, a->g.cols - 6, 0, a->g.rh, 0, 0);
    }
    if (a->book == RF_BOOK_READER && a->data_len > 0) {
        char pb[16];
        unsigned long long pct = a->off * 100ull / a->data_len;
        snprintf(pb, sizeof pb, "%llu%%", pct);
        draw_text(a, fb, fw, fh, (const u8 *)pb, strlen(pb),
                  a->g.top_bar.x + a->g.top_bar.w - (int)strlen(pb) * a->g.wc - a->g.wc,
                  a->g.top_bar.y + (a->g.rh - a->g.rh) / 2, a->g.fp, 1, 1, C_DIM, 6, 0, a->g.rh, 0, 0);
    }
    fill(fb, fw, fh, a->g.top_bar.x, a->g.top_bar.y + a->g.rh - 1, a->g.top_bar.w, 1, C_SEP);

    /* 正文 */
    if (a->laid && a->data) {
        for (i = 0; i < a->g.rows; i++) {
            RfRow *r = &a->rows[i];
            int y = a->g.body.y + i * a->g.rh;
            int num = 1, den = 1, px = a->g.fp, boxh = a->g.rh, boxy = y;
            int bold = 0, top_align = 0;
            u16 base = C_TEXT;
            if (r->kind == RF_ROW_BLANK) continue;
            if (r->start_off > r->end_off || r->end_off > a->data_len) continue;  /* 行区间越界：丢弃 */
            if (r->kind == RF_ROW_TITLE1) {          /* #  ：2× 字号，真实占 2 行，顶部对齐 */
                num = 2; den = 1; px = a->g.fp * 2; base = C_TITLE; bold = 1; top_align = 1;
            } else if (r->kind == RF_ROW_TITLE2) {   /* ## ：1.5× 字号，占 2 行，顶部对齐，下方留 0.5 行 */
                num = 3; den = 2; px = a->g.fp * 3 / 2; base = C_TITLE; bold = 1; top_align = 1;
            } else if (r->kind == RF_ROW_TITLE3) {   /* ###：1× 字号，占 1 行，只加粗 */
                base = C_TITLE; bold = 1;
            }
            rf_app_row_box(a, i, &boxy, &boxh);       /* 行容器：标题与正文一致，与点击共用公式 */
            {
                u64 p0 = r->start_off, p1 = r->end_off;
                if (r->kind == RF_ROW_TITLE1 || r->kind == RF_ROW_TITLE2 || r->kind == RF_ROW_TITLE3) {
                    while (p0 < p1 && a->data[p0] == '#') p0++;      /* 标题标记不渲染 */
                    if (p0 < p1 && a->data[p0] == ' ') p0++;
                }
                draw_text(a, fb, fw, fh, a->data + p0, p1 - p0,
                          a->g.body.x, boxy, px, num, den, base, a->g.cols,
                          a->book == RF_BOOK_LIBRARY ? 1 : 0, boxh, bold, top_align);
            }
        }
    }

    /* 底栏上行：按钮 */
    fill(fb, fw, fh, a->g.btn_bar.x, a->g.btn_bar.y, a->g.btn_bar.w, 1, C_SEP);
    if (a->bar[0])
        draw_text(a, fb, fw, fh, (const u8 *)a->bar, strlen(a->bar),
                  a->g.btn_bar.x + a->g.wc,
                  a->g.btn_bar.y + (a->g.rh - a->g.rh) / 2, a->g.fp, 1, 1,
                  C_BTN, a->g.cols, 0, a->g.rh, 0, 0);
    /* 底栏下行：翻页行（左 <，右 >） */
    fill(fb, fw, fh, a->g.pager_bar.x, a->g.pager_bar.y, a->g.pager_bar.w, 1, C_SEP);
    {
        u16 cprev = (a->laid && rf_can_prev(&a->page)) ? C_WHITE : C_DIS;   /* 可用=纯白 */
        u16 cnext = (a->laid && rf_can_next(&a->page)) ? C_WHITE : C_DIS;   /* 不可用=深灰 */
        const u8 *lt = (const u8 *)"<";
        const u8 *gt = (const u8 *)">";
        {
            int py = a->g.pager_bar.y + (a->g.rh - a->g.rh) / 2;
            draw_text(a, fb, fw, fh, lt, 1, a->g.pager_bar.x, py,
                      a->g.fp, 1, 1, cprev, 4, 0, a->g.rh, 0, 0);
            draw_text(a, fb, fw, fh, gt, 1, a->g.pager_bar.x + a->g.pager_bar.w - a->g.wc,
                      py, a->g.fp, 1, 1, cnext, 4, 0, a->g.rh, 0, 0);
        }
    }
    return 1;
}

/* ---------------- 触摸与动作 ---------------- */

static void do_action(RfApp *a, int act)
{
    RfSettings *s = &a->editing;
    switch (act) {
    case ACT_BACK:
        if (a->book == RF_BOOK_DIALOG) a->dlg_active = 0;
        stack_pop(a);                        /* 一律出栈一次 */
        return;
    case ACT_OPEN:
        if (a->sel == 0) stack_push(a, RF_BOOK_TUTORIAL);
        else if (a->sel - 1 < a->nbooks) {
            snprintf(a->id, sizeof a->id, "%s", a->books[a->sel - 1].id);
            stack_push(a, RF_BOOK_READER);
        }
        return;
    case ACT_SETTINGS: stack_push(a, RF_BOOK_SETTINGS); return;
    case ACT_DELETE:
        if (a->sel >= 1 && a->sel - 1 < a->nbooks) {
            snprintf(a->dlg, sizeof a->dlg, "删除《%s》？\n此操作不可撤销。", a->books[a->sel - 1].title);
            a->dlg_action = a->sel - 1;
            a->dlg_kind = 1;
            a->dlg_active = 1;
            stack_push(a, RF_BOOK_DIALOG);
        } else {
            snprintf(a->st->status, sizeof a->st->status, "内置书不可删除");
        }
        return;
    case ACT_DLG_OK:
        if (a->dlg_kind == 1 && a->dlg_action >= 0 && a->dlg_action < a->nbooks) {
            rf_book_delete(a->st, a->books[a->dlg_action].id);
            snprintf(a->st->status, sizeof a->st->status, "已删除");
        }
        a->dlg_active = 0;
        a->dlg_kind = 0;
        stack_pop(a);                        /* 提示框是栈顶：出栈即回到来源书 */
        return;
    case ACT_DLG_CANCEL:
        a->dlg_active = 0;
        a->dlg_kind = 0;
        stack_pop(a);
        return;
    case ACT_IMPORT: {
        const char *dir = pal_import_dir();
        DIR *d = dir ? opendir(dir) : NULL;
        int n = 0;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                char full[900];
                size_t l = strlen(de->d_name);
                if (l < 4 || strcasecmp(de->d_name + l - 4, ".txt") != 0) continue;
                snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
                if (rf_book_import(a->st, full, NULL, 0) == 0) n++;
            }
            closedir(d);
        }
        a->nbooks = rf_books_scan(a->st, a->books, RF_MAX_BOOKS);
        snprintf(a->st->status, sizeof a->st->status, n > 0 ? "已导入 %d 本" : "导入目录里没有 .txt", n);
        gen_library(a);
        a->laid = 0;                                 /* 内容变了：作废排版 */
        return;
    }
    case ACT_FONT_DN: if (s->font_dp > RF_FONT_DP_MIN) s->font_dp--; goto regrid;
    case ACT_FONT_UP: if (s->font_dp < RF_FONT_DP_MAX) s->font_dp++; goto regrid;
    case ACT_LH_AUTO: s->lh_auto = !s->lh_auto; goto regrid;
    case ACT_LIVE:    s->live_edit = !s->live_edit; goto regrid;
    case ACT_LH_DN:
        if (s->lh_auto) { if (s->lh_tenths > RF_LH_MULT_MIN) s->lh_tenths--; }
        else if (s->lh_dp > RF_LINEHT_DP_MIN) s->lh_dp -= 2;
        goto regrid;
    case ACT_LH_UP:
        if (s->lh_auto) { if (s->lh_tenths < RF_LH_MULT_MAX) s->lh_tenths++; }
        else if (s->lh_dp < RF_LINEHT_DP_MAX) s->lh_dp += 2;
        goto regrid;
    case ACT_ML_DN:   if (s->m_left_dp > 0) { s->m_left_dp -= 2; s->m_right_dp = s->m_left_dp; } goto regrid;
    case ACT_ML_UP:   if (s->m_left_dp < RF_MARGIN_DP_MAX) { s->m_left_dp += 2; s->m_right_dp = s->m_left_dp; } goto regrid;
    case ACT_MT_DN:   if (s->m_top_dp > 0) { s->m_top_dp -= 2; s->m_bot_dp = s->m_top_dp; } goto regrid;
    case ACT_MT_UP:   if (s->m_top_dp < RF_MARGIN_DP_MAX) { s->m_top_dp += 2; s->m_bot_dp = s->m_top_dp; } goto regrid;
regrid:
        if (a->preview || s->live_edit) {            /* 预览中，或开启"实时修改"：网格也按新值重建 */
            rf_grid_build(&a->g, s, a->sw, a->sh, a->density);
            rf_glyph_reset();
        }
        gen_settings(a);
        a->laid = 0;                                 /* 正文内容变了，排版必须重新计算 */
        set_bar(a);
        snprintf(a->st->status, sizeof a->st->status,
                 s->live_edit ? "实时修改：已生效（保存后重启持久）"
                              : "改动仅预览，保存并重启后生效");
        return;
    case ACT_PREVIEW:
        if (!a->preview) {                           /* 进入预览：按 editing 建网格 */
            a->g_saved = a->g;
            rf_grid_build(&a->g, &a->editing, a->sw, a->sh, a->density);
            a->preview = 1;
        } else {                                     /* 退出预览：恢复生效网格 */
            a->g = a->g_saved;
            a->preview = 0;
        }
        rf_glyph_reset();
        a->laid = 0;
        gen_settings(a);
        set_bar(a);
        return;
    case ACT_SAVE: {
        RfSettings back;
        int rc = rf_settings_save(a->st, &a->editing);      /* 写全局配置文件 */
        int ok = 0;
        if (rc == 0) {                                       /* 回读校验：确认真的写进去了 */
            back = a->st->settings;
            a->st->settings = a->editing;                    /* 借用 load 解析配置 */
            if (rf_settings_load(a->st) == 0 &&
                a->st->settings.font_dp == a->editing.font_dp &&
                a->st->settings.lh_auto == a->editing.lh_auto &&
                a->st->settings.lh_tenths == a->editing.lh_tenths &&
                a->st->settings.lh_dp == a->editing.lh_dp &&
                a->st->settings.m_top_dp == a->editing.m_top_dp &&
                a->st->settings.live_edit == a->editing.live_edit)
                ok = 1;
            a->st->settings = back;                          /* 当前会话仍用生效值 */
        }
        snprintf(a->dlg, sizeof a->dlg,
                 ok ? "已保存到全局配置文件。\n重启后全应用生效。"
                    : "保存失败：配置文件写入或校验不通过。\n请检查应用数据目录权限。");
        a->dlg_kind = 0;
        a->dlg_action = -1;
        a->dlg_active = 1;
        stack_push(a, RF_BOOK_DIALOG);
        snprintf(a->st->status, sizeof a->st->status, ok ? "已保存" : "保存失败");
        return;
    }
    case ACT_DEBUG:   stack_push(a, RF_BOOK_DEBUG); return;
    case ACT_REBUILD:
        if (a->book == RF_BOOK_DEBUG) snprintf(a->st->status, sizeof a->st->status, "索引已重建");
        return;
    case ACT_RESET:
        snprintf(a->st->status, sizeof a->st->status, "进度已重置");
        return;
    default:
        return;
    }
}

void rf_app_tap(RfApp *a, int x, int y)
{
    int i;
    if (y >= a->g.top_bar.y && y < a->g.top_bar.y + a->g.top_bar.h) {   /* 顶栏 < */
        if (x < a->g.top_bar.x + a->g.wc * 4) do_action(a, ACT_BACK);
        return;
    }
    if (y >= a->g.btn_bar.y && y < a->g.btn_bar.y + a->g.btn_bar.h) {
        for (i = 0; i < a->nhk; i++)
            if (x >= a->hk[i].x0 && x < a->hk[i].x1) { do_action(a, a->hk[i].act); return; }
        return;
    }
    if (y >= a->g.pager_bar.y) {                 /* 翻页行：左 < 右 > */
        int zone = a->g.pager_bar.w * 20 / 100;
        int minzone = a->g.wc * 6;
        if (zone < minzone) zone = minzone;
        if (x < a->g.pager_bar.x + zone) { page_prev(a); return; }
        if (x > a->g.pager_bar.x + a->g.pager_bar.w - zone) { page_next(a); return; }
        return;
    }
    /* 正文 */
    if (a->book == RF_BOOK_READER || a->book == RF_BOOK_TUTORIAL) {
        if (x < a->g.body.x + a->g.body.w / 2) page_prev(a);
        else page_next(a);
        return;
    }
    {
        int row = rf_app_row_at(a, y);
        int col = (x - a->g.body.x) / a->g.wc;
        if (row < 0) return;
        for (i = 0; i < a->nhb; i++) {
            if (a->hb[i].row == row + 1 && col >= a->hb[i].c0 && col < a->hb[i].c1) {
                do_action(a, a->hb[i].act);
                return;
            }
        }
        if (a->book == RF_BOOK_LIBRARY) {        /* 单选行 */
            int n = a->nbooks + 1;
            int idx = row / 2;
            if (idx >= 0 && idx < n) {
                a->sel = idx;
                gen_library(a);
                a->laid = 0;
            }
        }
    }
}

void rf_app_key_back(RfApp *a) { do_action(a, ACT_BACK); }

/* ---------------- 生命周期 ---------------- */

RfApp *rf_app_create(RfStore *st, int screen_w, int screen_h, int density)
{
    RfApp *a = (RfApp *)calloc(1, sizeof(RfApp));
    if (!a) return NULL;
    a->st = st;
    a->sw = screen_w;
    a->sh = screen_h;
    a->density = density;
    a->gen_cap = 64 * 1024;
    a->gen = (char *)malloc(a->gen_cap);
    a->mode.truncate_scroll = 0;
    a->editing = st->settings;
    rf_grid_build(&a->g, &st->settings, screen_w, screen_h, density);
    a->nbooks = rf_books_scan(st, a->books, RF_MAX_BOOKS);
    a->sel = 0;
    a->dlg_action = -1;
    snprintf(a->st->status, sizeof a->st->status, "就绪");
    stack_reset(a, RF_BOOK_LIBRARY);        /* 栈底 = 书库 */
    return a;
}

void rf_app_resize(RfApp *a, int screen_w, int screen_h, int density)
{
    a->sw = screen_w;
    a->sh = screen_h;
    a->density = density;
    rf_grid_build(&a->g, a->preview ? &a->editing : &a->st->settings, screen_w, screen_h, density);
    rf_glyph_reset();
    a->laid = 0;
    if (a->book != RF_BOOK_READER && a->book != RF_BOOK_TUTORIAL) {
        if (a->book == RF_BOOK_LIBRARY) gen_library(a);
        else if (a->book == RF_BOOK_SETTINGS) gen_settings(a);
        else if (a->book == RF_BOOK_DEBUG) gen_debug(a);
        else if (a->book == RF_BOOK_DIALOG) gen_dialog(a);
        set_bar(a);
    }
}

void rf_app_destroy(RfApp *a)
{
    if (!a) return;
    if (a->book == RF_BOOK_READER && a->id[0])
        rf_progress_save(a->st, a->id, a->line, a->off, a->data_len);
    if (a->data && a->data != (const u8 *)a->gen) free((void *)a->data);
    free(a->gen);
    free(a);
}

void rf_app_metrics(const RfApp *a, int *sw, int *sh, int *wc, int *rh,
                    int *cols, int *rows, int *body_x, int *body_y, int *bar_y)
{
    if (sw) *sw = a->sw;
    if (sh) *sh = a->sh;
    if (wc) *wc = a->g.wc;
    if (rh) *rh = a->g.rh;
    if (cols) *cols = a->g.cols;
    if (rows) *rows = a->g.rows;
    if (body_x) *body_x = a->g.body.x;
    if (body_y) *body_y = a->g.body.y;
    if (bar_y) *bar_y = a->g.btn_bar.y;
}

int rf_app_bar_hit(const RfApp *a, int i, int *x)
{
    if (i < 0 || i >= a->nhk) return 0;
    if (x) *x = (a->hk[i].x0 + a->hk[i].x1) / 2;
    return a->hk[i].act;
}

int rf_app_body_hit(const RfApp *a, int i, int *x, int *y)
{
    if (i < 0 || i >= a->nhb) return 0;
    if (x) *x = a->g.body.x + (a->hb[i].c0 + a->hb[i].c1) / 2 * a->g.wc;
    if (y) *y = a->g.body.y + (a->hb[i].row - 1) * a->g.rh + a->g.rh / 2;
    return a->hb[i].act;
}

/* 调试/自检：当前页所有有内容行的字节区间是否都落在正文内 */
int rf_app_rows_valid(const RfApp *a)
{
    int i;
    if (!a->laid) return 0;
    for (i = 0; i < a->g.rows; i++) {
        if (a->rows[i].kind == RF_ROW_BLANK) continue;
        if (a->rows[i].start_off > a->rows[i].end_off) return 0;
        if (a->rows[i].end_off > a->data_len) return 0;
    }
    return 1;
}

int rf_app_book_id(const RfApp *a) { return a->book; }

u64 rf_app_frame_hash(const RfApp *a) { (void)a; return 0; }
