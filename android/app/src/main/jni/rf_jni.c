/* JNI 桥：把 core 自检结果交给 Java 层显示与写 logcat。
 * 不依赖 liblog（避免额外链接），日志由 Java 侧完成。 */
#include <jni.h>
#include "rf.h"

JNIEXPORT jstring JNICALL
Java_com_readflash_verify_MainActivity_runSelfTest(JNIEnv *env, jclass cls)
{
    static char report[16384];
    (void)cls;
    rf_selftest(report, sizeof report);
    return (*env)->NewStringUTF(env, report);
}
