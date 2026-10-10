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

/* 统计矩形内的非黑像素数（判断文字是否渲染到了某处） */
static long ink(int x0, int y0, int x1, int y1)
{
    long n = 0;
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (y >= 0 && y < H && x >= 0 && x < W && fb[y * W + x]) n++;
    return n;
}

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

/* 点顶栏 < 返回：顶栏在边距之内，y = 正文顶部 − 行高/2 */
static void tap_back(RfApp *a)
{
    int wc, rh, bx, by, sw, sh, cols, rows, bary;
    rf_app_metrics(a, &sw, &sh, &wc, &rh, &cols, &rows, &bx, &by, &bary);
    rf_app_tap(a, bx + wc, by - rh / 2);
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
    if (rf_glyph_ft_init(NULL, 0) == 0) {
        rf_glyph_set_provider(rf_glyph_ft, NULL);      /* 真字形：能看到真实文字 */
    } else {
        rf_glyph_set_provider(rf_glyph_builtin, NULL);
        printf("!! 未找到可用中文字体，退回占位方块（设 RF_FONT 指定字体）\n");
    }
    rf_grid_build(&grid, &st.settings, W, H, DENS);
    a = rf_app_create(&st, W, H, DENS);

    printf("网格 C=%d V=%d wc=%d rh=%d 边距(%d,%d,%d,%d)\n",
           grid.cols, grid.rows, grid.wc, grid.rh, grid.m_top, grid.m_bot, grid.m_left, grid.m_right);

    /* 1) 书库 */
    tap_center(a, 1, 1);                       /* 顶栏 <：在书库无动作 */
    step(a, "书库", "/tmp/01-library.ppm");

    /* 1.4) 顶栏热区：点标题文字（非 < 号）也应触发返回 */
    {
        int sw, sh, wc, rh, cols, rows, bx, by, bary;
        int b1, b2;
        rf_app_metrics(a, &sw, &sh, &wc, &rh, &cols, &rows, &bx, &by, &bary);
        tap_body_row(a, 0);                 /* 选中教程 */
        tap_bar(a, 0);                      /* [打开] → 教程书 */
        b1 = rf_app_book_id(a);
        rf_app_tap(a, bx + wc * 8, by - rh / 2);   /* 顶栏中部（标题文字上） */
        b2 = rf_app_book_id(a);
        printf("  顶栏点标题: 打开后=%d 点标题后=%d\n", b1, b2);
        printf("  判定: 标题也算返回=%d\n", (b1 == 4 && b2 == 0));
    }

    /* 1.5) 根页面（书库）返回 → 退出确认框 → 取消 */
    {
        int b1, b2;
        tap_back(a);
        rf_app_render(a, fb, W, H);
        b1 = rf_app_book_id(a);
        dump("/tmp/15-exit-confirm.ppm");
        tap_bar(a, 1);                      /* [取消] */
        b2 = rf_app_book_id(a);
        printf("  根页面返回: 确认框=%d（期望 5） 取消后=%d（期望 0）\n", b1, b2);
        printf("  判定: 退出确认=%d\n", (b1 == 5 && b2 == 0));
    }

    /* 2) 选中第 0 行 → 按 [打开] → 教程书 → 翻两页 */
    tap_body_row(a, 0);
    tap_bar(a, 0);
    step(a, "教程 P1", "/tmp/02-tutorial.ppm");
    tap_center(a, W - 40, H / 2);              /* 正文右半边：下一页 */
    step(a, "教程 P2", "/tmp/03-tutorial-p2.ppm");

    /* 3) 返回书库 */
    tap_back(a);
    step(a, "书库(返回)", "/tmp/04-library-back.ppm");

    /* 4) 设置书：底栏 [设置] 按钮（不再作为单选项） */
    tap_bar(a, 3);
    step(a, "设置", "/tmp/05-settings.ppm");
    tap_body_btn(a, 1);                        /* 字号 [+] */
    step(a, "设置(字号+1)", "/tmp/06-settings-font.ppm");
    tap_bar(a, 0);                             /* [预览] */
    step(a, "设置(预览)", "/tmp/07-preview.ppm");
    tap_back(a);                               /* 返回书库（不保存） */

    /* 5) 导入 sample.txt 并阅读 */
    tap_bar(a, 2);                             /* [导入] → 进入导入页面（复选列表） */
    {
        int bi = rf_app_book_id(a);            /* 期望 6 = 导入页面 */
        rf_app_render(a, fb, W, H);
        dump("/tmp/16-import-page.ppm");
        printf("  导入页面书=%d（期望 6）\n", bi);
        tap_bar(a, 1);                         /* [导入这些] → 提示框 */
        {
            int bd = rf_app_book_id(a);
            const char *txt = rf_app_dialog_text(a);
            printf("  导入结果提示框=%d（期望 5）文本含 null=%d\n",
                   bd, strstr(txt, "(null)") != NULL);
            tap_bar(a, 0);                     /* [确定] */
            printf("  判定: 导入页面与结果提示=%d\n", bi == 6 && bd == 5);
        }
    }
    step(a, "书库(导入后)", "/tmp/08-library-imported.ppm");

    tap_body_row(a, 2);                        /* 第 2 项 = 导入的书（行 2*1=2） */
    tap_bar(a, 0);
    step(a, "阅读 P1", "/tmp/09-reader.ppm");
    for (i = 0; i < 3; i++) tap_center(a, W - 40, H / 2);   /* 正文右半边：下一页 */
    step(a, "阅读 P4", "/tmp/10-reader-p4.ppm");
    tap_center(a, 40, H / 2);                  /* 上一页 */
    step(a, "阅读 P3(上一页)", "/tmp/11-reader-prev.ppm");

    /* 5.5) 翻页行点击：左右键必须真的翻页（此前被 enabled 缓存卡死） */
    {
        int sw, sh, wc, rh, cols, rows, bx, by, bary;
        unsigned long long h0, h1, h2;
        int i2;
        rf_app_metrics(a, &sw, &sh, &wc, &rh, &cols, &rows, &bx, &by, &bary);
        rf_app_render(a, fb, W, H);
        h0 = 0; for (i2 = 0; i2 < W * H; i2++) h0 = h0 * 131 + fb[i2];
        rf_app_tap(a, W - 40, H - rh - 4);              /* 翻页行右键 → 下一页 */
        rf_app_render(a, fb, W, H);
        h1 = 0; for (i2 = 0; i2 < W * H; i2++) h1 = h1 * 131 + fb[i2];
        rf_app_tap(a, 40, H - rh - 4);                  /* 翻页行左键 → 上一页 */
        rf_app_render(a, fb, W, H);
        h2 = 0; for (i2 = 0; i2 < W * H; i2++) h2 = h2 * 131 + fb[i2];
        printf("  翻页键: 右键换页=%d 左键回退=%d\n", h1 != h0, h2 != h1);
        printf("  判定: 翻页键可用=%d\n", (h1 != h0) && (h2 != h1));
    }

    /* 5.6) 目录：阅读底栏 [目录] → 目录页（扁平、含 # 原文）→ 点条目跳转回阅读 */
    {
        int sw, sh, wc, rh, cols, rows, bx, by, bary;
        int b0, b1, b2;
        rf_app_metrics(a, &sw, &sh, &wc, &rh, &cols, &rows, &bx, &by, &bary);
        b0 = rf_app_book_id(a);
        tap_bar(a, 0);                              /* 阅读底栏 [目录]（第 0 个按钮） */
        rf_app_render(a, fb, W, H);
        b1 = rf_app_book_id(a);
        dump("/tmp/17-toc.ppm");
        rf_app_tap(a, bx + wc, by + rh + rh / 2);   /* 点目录第 1 条 */
        b2 = rf_app_book_id(a);
        printf("  目录: 阅读=%d → 目录页=%d → 跳转后=%d\n", b0, b1, b2);
        printf("  判定: 目录页与跳转=%d\n", (b0 == 1 && b1 == 7 && b2 == 1));
    }

    /* 6) 设置语义回归：编辑只影响预览 → 保存 → 重启才生效 */
    {
        int wc0, rh0, wc1, rh1, wc2, rh2, wc3, rh3, wc4, rh4;
        int c0, c1, c2, c3, c4, r0, r1, r2, r3, r4, sw, sh, bx, by, bary;
        tap_back(a);                       /* 回到书库 */
        tap_bar(a, 3);                     /* [设置] */
        rf_app_metrics(a, &sw, &sh, &wc0, &rh0, &c0, &r0, &bx, &by, &bary);
        tap_body_btn(a, 1);                /* 字号 +1（仅编辑） */
        rf_app_metrics(a, &sw, &sh, &wc1, &rh1, &c1, &r1, &bx, &by, &bary);
        printf("  编辑后（应不变）: wc %d→%d  rh %d→%d\n", wc0, wc1, rh0, rh1);
        tap_bar(a, 0);                     /* [预览] */
        rf_app_metrics(a, &sw, &sh, &wc2, &rh2, &c2, &r2, &bx, &by, &bary);
        printf("  预览中（应变）  : wc %d→%d  rh %d→%d\n", wc0, wc2, rh0, rh2);
        tap_bar(a, 1);                     /* [保存] */
        tap_back(a);                       /* 回书库：应恢复生效网格 */
        rf_app_metrics(a, &sw, &sh, &wc3, &rh3, &c3, &r3, &bx, &by, &bary);
        printf("  返回书库（应恢复）: wc %d→%d  rh %d→%d\n", wc2, wc3, rh2, rh3);
        rf_app_destroy(a);
        rf_settings_load(&st);             /* 模拟重启：重新读配置 */
        a = rf_app_create(&st, W, H, DENS);
        rf_app_metrics(a, &sw, &sh, &wc4, &rh4, &c4, &r4, &bx, &by, &bary);
        printf("  重启后（应生效）  : wc %d (期望 %d)  rh %d (期望 %d)\n",
               wc4, wc2, rh4, rh2);
        printf("  判定: 编辑不影响=%d 预览生效=%d 返回恢复=%d 重启生效=%d\n",
               wc0 == wc1 && rh0 == rh1,
               wc2 != wc0 || rh2 != rh0,
               wc3 == wc0 && rh3 == rh0,
               wc4 == wc2 && rh4 == rh2);
    }

    /* 6.5) 文字高于行容器：顶部对齐 + 溢出照常渲染 */
    {
        int sw, sh, wc, rh, cols, rows, bx, by, bary;
        long in_container, below;
        tap_back(a);                        /* 回书库 */
        tap_bar(a, 3);                      /* [设置] */
        tap_body_btn(a, 2);                 /* 取消"行高自动" */
        for (int k = 0; k < 5; k++) tap_body_btn(a, 3);   /* 行高 -2dp ×5 → 容器远小于字形 */
        tap_bar(a, 0);                      /* [预览]：按新行高重排 */
        rf_app_metrics(a, &sw, &sh, &wc, &rh, &cols, &rows, &bx, &by, &bary);
        rf_app_render(a, fb, W, H);
        in_container = ink(bx, by, bx + wc * cols, by + rh);
        below = ink(bx, by + rh, bx + wc * cols, by + rh * 3);
        printf("  行容器=%dpx 字号像素=32px（自动已取消）：容器内墨点=%ld 溢出墨点=%ld\n",
               rh, in_container, below);
        printf("  判定: 顶部有字=%d 溢出仍渲染=%d\n", in_container > 0, below > 0);
        dump("/tmp/12-overflow.ppm");
    }

    /* 6.6) "实时修改"开关：开 → 调整立即生效（无需 [预览]） */
    {
        int sw, sh, wc0, rh0, wc1, rh1, c, r, bx, by, bary;
        tap_back(a);                        /* 回书库 */
        tap_bar(a, 3);                      /* [设置] */
        tap_body_btn(a, 9);                 /* (+)实时修改 → 开 */
        rf_app_metrics(a, &sw, &sh, &wc0, &rh0, &c, &r, &bx, &by, &bary);
        tap_body_btn(a, 1);                 /* 字号 +1 */
        rf_app_metrics(a, &sw, &sh, &wc1, &rh1, &c, &r, &bx, &by, &bary);
        printf("  实时修改=开：字号+1 → wc %d→%d rh %d→%d\n", wc0, wc1, rh0, rh1);
        printf("  判定: 实时生效=%d\n", (wc1 != wc0 || rh1 != rh0));
        tap_body_btn(a, 9);                 /* 关掉，便于后续观察 */
    }

    /* 6.7) 书栈：设置 → [调试] → 返回应回到设置（出栈一次） */
    {
        int b1, b2, b3;
        tap_back(a);                        /* 回书库 */
        tap_bar(a, 3);                      /* [设置] 入栈 */
        b1 = rf_app_book_id(a);
        tap_bar(a, 2);                      /* [调试] 入栈 */
        b2 = rf_app_book_id(a);
        tap_back(a);                        /* 出栈一次 */
        b3 = rf_app_book_id(a);
        printf("  书栈: 设置=%d → 调试=%d → 出栈=%d\n", b1, b2, b3);
        printf("  判定: 出栈回到上一本=%d\n", (b1 == 2 && b2 == 3 && b3 == 2));
        tap_back(a);                        /* 回书库 */
    }

    /* 6.8) 乱码回归：内容长度变化后必须重新排版（此前的 bug：旧行区间指向新缓冲区） */
    {
        int v1, v2, v3;
        tap_back(a);                        /* 回书库 */
        tap_bar(a, 3);                      /* [设置] */
        rf_app_render(a, fb, W, H);
        v1 = rf_app_rows_valid(a);
        tap_body_btn(a, 2);                 /* 取消"行高自动"（内容长度变化） */
        rf_app_render(a, fb, W, H);
        v2 = rf_app_rows_valid(a);
        tap_body_btn(a, 2);                 /* 再勾回自动 */
        rf_app_render(a, fb, W, H);
        v3 = rf_app_rows_valid(a);
        printf("  行区间有效: 初始=%d 取消自动后=%d 勾回自动后=%d\n", v1, v2, v3);
        printf("  判定: 无乱码=%d\n", v1 && v2 && v3);
        tap_back(a);
    }

    /* 6.9) 保存 → 提示框 → 确定回设置；并导出带标题的预览画面以便肉眼检查 */
    {
        int b1, b2;
        tap_back(a);                        /* 回书库 */
        tap_bar(a, 3);                      /* [设置] */
        tap_body_btn(a, 1);                 /* 字号 +1（编辑值） */
        tap_bar(a, 1);                      /* [保存] → 应弹出提示框 */
        rf_app_render(a, fb, W, H);
        b1 = rf_app_book_id(a);
        dump("/tmp/13-save-dialog.ppm");
        tap_bar(a, 0);                      /* [确定] */
        b2 = rf_app_book_id(a);
        printf("  保存后书=%d（期望 5=提示框） 确定后书=%d（期望 2=设置）\n", b1, b2);
        printf("  判定: 保存提示框=%d\n", (b1 == 5 && b2 == 2));
        tap_bar(a, 0);                      /* [预览]：导出带标题的样例 */
        rf_app_render(a, fb, W, H);
        dump("/tmp/14-titles.ppm");
        tap_back(a);
    }

    /* 6.10) 组件注册表：不得有重复 id（同 id 不同动作） */
    {
        int dups;
        tap_back(a);                        /* 回书库 */
        tap_bar(a, 3);                      /* [设置]：注册一堆组件 */
        rf_app_render(a, fb, W, H);
        dups = rf_app_comp_dups(a);
        printf("  组件重复 id 次数=%d\n", dups);
        printf("  判定: 组件 id 唯一=%d\n", dups == 0);
        tap_back(a);
    }

    /* 7) 退出并确认进度落盘 */
    rf_app_destroy(a);
    snprintf(cmd, sizeof cmd,
             "echo '--- 配置（实时保存）---'; cat /tmp/rfapp/config; "
             "echo '--- 进度 ---'; cat /tmp/rfapp/book/*/.progress | head -3");
    printf("== 落盘内容 ==\n");
    fflush(stdout);
    if (system(cmd) != 0) printf("(列目录失败)\n");
    return 0;
}
