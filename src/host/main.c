/* 宿主（Linux/aarch64）验证入口：直接跑 core 自检并打印报告。
 * 用途：在不依赖 Android 设备的情况下验证 §16 的三项结论。 */
#include "rf.h"
#include <stdio.h>

int main(void)
{
    static char report[16384];
    int rc = rf_selftest(report, sizeof report);
    fputs(report, stdout);
    return rc;
}
