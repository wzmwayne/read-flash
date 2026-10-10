/* PAL：POSIX 实现（Linux 宿主与 Android 通用；目录由平台层注入） */
#include "rf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static char g_data[512];
static char g_import[512];

void pal_set_dirs(const char *app_data, const char *import_dir)
{
    if (app_data) snprintf(g_data, sizeof g_data, "%s", app_data);
    if (import_dir) snprintf(g_import, sizeof g_import, "%s", import_dir);
}

const char *pal_app_data_dir(void) { return g_data; }
const char *pal_import_dir(void) { return g_import; }

u64 pal_ticks_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000ull + (u64)(ts.tv_nsec / 1000000);
}

u64 pal_now_unix(void)
{
    return (u64)time(NULL);
}

static void (*g_exit_hook)(void);

void pal_set_exit_hook(void (*fn)(void)) { g_exit_hook = fn; }

static void (*g_pick_hook)(void);

void pal_set_pick_hook(void (*fn)(void)) { g_pick_hook = fn; }

void pal_pick_files(void)
{
    if (g_pick_hook) g_pick_hook();      /* 平台弹系统选择器（异步） */
}

void pal_app_exit(void)
{
    if (g_exit_hook) g_exit_hook();      /* 平台接管（安卓：结束 Activity） */
    else exit(0);                        /* 宿主/iOS 兜底 */
}
