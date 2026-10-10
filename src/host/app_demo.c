/* 宿主端到端演示：驱动正式应用走一遍书库 → 教程 → 设置 → 导入 → 阅读，
 * 每步把 RGB565 帧缓冲导出为 PPM，便于肉眼检查排版与配色。 */
#include "rf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define W 720
#define H 1280
#define DENS 2000

static u16 fb[W * H];
static RfGrid grid;

static void dump(const char *path)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) { printf("!! 无法写 %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        u16 c = fb[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
        rgb[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
        rgb[2] = (unsigned char)((c & 31) * 255 / 31);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("  导出 %s\n", path);
}

static void step(RfApp *a, const char *name, const char *path)
{
    rf_app_render(a, fb, W, H);
    printf("[%s] 书=%d\n", name, rf_app_book_id(a));
    dump(path);
}

static void tap_center(RfApp *a, int x, int y) { rf_app_tap(a, x, y); }

/* 用应用"当前"网格计算坐标（字号/边距变化后行列会变） */
static void tap_body_row(RfApp *a, int row0)
{
    int wc, rh, bx, by, sw, sh, cols, rows, bary;
    rf_app_metrics(a, &sw, &sh, &wc, &rh, &cols, &rows, &bx, &by, &bary);
    rf_app_tap(a, bx + wc * 3, by + row0 * rh + rh / 2);
}

static void tap_bar(RfApp *a, int i)
{
    int x = 0, wc, rh, bx, by, sw, sh, cols, rows, bary;
    if (!rf_app_bar_hit(a, i, &x)) { printf("  !! 底栏按钮 %d 不存在\n", i); return; }
    rf_app_metrics(a, &sw, &sh, &wc, &rh, &cols, &rows, &bx, &by, &bary);
    rf_app_tap(a, x, bary + rh / 2);
}

static void tap_body_btn(RfApp *a, int i)
{
    int x = 0, y = 0;
    if (!rf_app_body_hit(a, i, &x, &y)) { printf("  !! 正文按钮 %d 不存在\n", i); return; }
    rf_app_tap(a, x, y);
}

static void seed_import_dir(const char *dir)
{
    char p[700];
    FILE *f;
    int i;
    snprintf(p, sizeof p, "%s/%s", dir, "sample.txt");
    f = fopen(p, "wb");
    if (!f) return;
    fputs("# 第一章 科学边界\n\n", f);
    for (i = 0; i < 120; i++) {
        fputs("汪淼觉得，这个世界的物理规律突然变得不可靠了。他想起杨冬临终前的话，"
              "想起那些在黑暗中闪烁的星星。Reading is a quiet pleasure 0123456789。\n", f);
        if (i % 17 == 16) fputs("\n---\n\n", f);
        if (i % 23 == 22) fputs("## 第二节 黑暗森林\n\n", f);
    }
    fputs("### 尾声\n最后一行。\n", f);
    fclose(f);
}

int main(void)
{
    RfStore st;
    RfApp *a;
    int i, act, x, y;
    char cmd[256];

    system("rm -rf /tmp/rfapp && mkdir -p /tmp/rfapp/book /tmp/rfapp/import");
    seed_import_dir("/tmp/rfapp/import");
    pal_set_dirs("/tmp/rfapp", "/tmp/rfapp/import");

    rf_store_init(&st, "/tmp/rfapp");
    rf_settings_load(&st);
    rf_glyph_set_provider(rf_glyph_builtin, NULL);
    rf_grid_build(&grid, &st.settings, W, H, DENS);
    a = rf_app_create(&st, W, H, DENS);

    printf("网格 C=%d V=%d wc=%d rh=%d 边距(%d,%d,%d,%d)\n",
           grid.cols, grid.rows, grid.wc, grid.rh, grid.m_top, grid.m_bot, grid.m_left, grid.m_right);

    /* 1) 书库 */
    tap_center(a, 1, 1);                       /* 顶栏 <：在书库无动作 */
    step(a, "书库", "/tmp/01-library.ppm");

    /* 2) 选中第 0 行 → 按 [打开] → 教程书 → 翻两页 */
    tap_body_row(a, 0);
    tap_bar(a, 0);
    step(a, "教程 P1", "/tmp/02-tutorial.ppm");
    tap_center(a, W - 40, H / 2);              /* 正文右半边：下一页 */
    step(a, "教程 P2", "/tmp/03-tutorial-p2.ppm");

    /* 3) 返回书库 */
    tap_center(a, 8, 8);
    step(a, "书库(返回)", "/tmp/04-library-back.ppm");

    /* 4) 设置书：选中第 1 行 → [打开] → 调字号 → 预览 */
    tap_body_row(a, 2);
    tap_bar(a, 0);
    step(a, "设置", "/tmp/05-settings.ppm");
    tap_body_btn(a, 1);                        /* 字号 [+] */
    step(a, "设置(字号+1)", "/tmp/06-settings-font.ppm");
    tap_bar(a, 0);                             /* [预览] */
    step(a, "设置(预览)", "/tmp/07-preview.ppm");
    tap_center(a, 8, 8);                       /* 返回书库（不保存） */

    /* 5) 导入 sample.txt 并阅读 */
    tap_bar(a, 2);                             /* [导入] */
    step(a, "书库(导入后)", "/tmp/08-library-imported.ppm");

    tap_body_row(a, 6);                        /* 第 3 项 = 导入的书（行 2*3=6） */
    tap_bar(a, 0);
    step(a, "阅读 P1", "/tmp/09-reader.ppm");
    for (i = 0; i < 3; i++) tap_center(a, W - 40, H / 2);   /* 正文右半边：下一页 */
    step(a, "阅读 P4", "/tmp/10-reader-p4.ppm");
    tap_center(a, 40, H / 2);                  /* 上一页 */
    step(a, "阅读 P3(上一页)", "/tmp/11-reader-prev.ppm");

    /* 6) 退出并确认进度落盘 */
    rf_app_destroy(a);
    snprintf(cmd, sizeof cmd, "ls -l /tmp/rfapp/book/*/ | head -20; echo ---; cat /tmp/rfapp/book/*/.progress | head -8");
    printf("== 落盘内容 ==\n");
    fflush(stdout);
    if (system(cmd) != 0) printf("(列目录失败)\n");
    return 0;
}
