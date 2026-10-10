/* JNI 桥：把 C 应用层接到 Java 外壳上。
 *   - nativeInit/nativeResize/nativeTap/nativeBack：输入与生命周期
 *   - nativeRender(int[])：C 渲染 RGB565 → 展开成 ARGB 交给 Java 上屏
 *   - 字形：C 通过 rf_glyph_set_provider 回调进 Java（系统字体光栅化）
 */
#include <jni.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "rf.h"

static JavaVM  *g_vm;
static jclass   g_cls;
static jmethodID g_mid_glyph;
static jbyteArray g_alpha;
static RfStore  g_store;
static RfApp   *g_app;
static u16     *g_fb;
static int      g_fw, g_fh, g_density;
static char     g_data_dir[512], g_import_dir[512];

static JNIEnv *env_get(void)
{
    JNIEnv *env = NULL;
    if (!g_vm) return NULL;
    (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    return env;
}

/* ---- 字形提供者：调用 Java 静态方法 glyph(int,int,byte[]) ---- */
static int android_glyph(void *ud, u32 cp, int px, int flags, u8 *alpha, int cap, int *gw, int *gh)
{
    JNIEnv *env = env_get();
    jint r;
    (void)ud;
    if (!env || !g_cls || !g_mid_glyph || !g_alpha) return 0;
    /* flags 交给 Java：位0 = 加粗（标题三档） */
    r = (*env)->CallStaticIntMethod(env, g_cls, g_mid_glyph, (jint)cp, (jint)px, (jint)flags, g_alpha);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return 0; }
    if (r == 0) return 0;
    *gw = (r >> 16) & 0xFFFF;
    *gh = r & 0xFFFF;
    if (*gw <= 0 || *gh <= 0 || (*gw) * (*gh) > cap) return 0;
    (*env)->GetByteArrayRegion(env, g_alpha, 0, (*gw) * (*gh), (jbyte *)alpha);
    return 1;
}

/* 退出应用：交给 Java 结束 Activity（不使用 exit，避免安卓下进程被硬杀） */
static void android_exit(void)
{
    JNIEnv *env = env_get();
    if (!env || !g_cls) return;
    {
        jmethodID mid = (*env)->GetStaticMethodID(env, g_cls, "finishApp", "()V");
        if (mid) (*env)->CallStaticVoidMethod(env, g_cls, mid);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
}

/* 平台文件选择：交给 Java 的 SAF 选择器（异步） */
static void android_pick(void)
{
    JNIEnv *env = env_get();
    jmethodID mid;
    if (!env || !g_cls) return;
    mid = (*env)->GetStaticMethodID(env, g_cls, "pickFiles", "()V");
    if (mid) (*env)->CallStaticVoidMethod(env, g_cls, mid);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static void fb_alloc(int w, int h)
{
    if (g_fb) { free(g_fb); g_fb = NULL; }
    g_fb = (u16 *)calloc((size_t)w * (size_t)h, 2);
    g_fw = w;
    g_fh = h;
}

JNIEXPORT void JNICALL
Java_com_readflash_app_MainActivity_nativeInit(JNIEnv *env, jclass cls,
                                               jstring dataDir, jstring importDir, jint density)
{
    const char *d, *i;
    char p[700];

    (*env)->GetJavaVM(env, &g_vm);
    g_cls = (jclass)(*env)->NewGlobalRef(env, cls);
    g_mid_glyph = (*env)->GetStaticMethodID(env, g_cls, "glyph", "(III[B)I");
    g_alpha = (jbyteArray)(*env)->NewGlobalRef(env, (*env)->NewByteArray(env, 64 * 64));

    d = (*env)->GetStringUTFChars(env, dataDir, NULL);
    i = (*env)->GetStringUTFChars(env, importDir, NULL);
    snprintf(g_data_dir, sizeof g_data_dir, "%s", d ? d : "/data/local/tmp");
    snprintf(g_import_dir, sizeof g_import_dir, "%s", i ? i : "/data/local/tmp");
    if (d) (*env)->ReleaseStringUTFChars(env, dataDir, d);
    if (i) (*env)->ReleaseStringUTFChars(env, importDir, i);

    mkdir(g_data_dir, 0770);
    snprintf(p, sizeof p, "%s/book", g_data_dir);   mkdir(p, 0770);
    snprintf(p, sizeof p, "%s/trash", g_data_dir);  mkdir(p, 0770);
    snprintf(p, sizeof p, "%s", g_import_dir);      mkdir(p, 0770);

    pal_set_dirs(g_data_dir, g_import_dir);
    pal_set_exit_hook(android_exit);
    pal_set_pick_hook(android_pick);
    rf_glyph_set_provider(android_glyph, NULL);

    rf_store_init(&g_store, g_data_dir);
    rf_settings_load(&g_store);

    g_density = density;
    if (g_app) { rf_app_destroy(g_app); g_app = NULL; }
    g_app = rf_app_create(&g_store, 720, 1280, density);   /* 真实尺寸在 resize 时校正 */
    if (!g_app) return;
    fb_alloc(720, 1280);
    rf_app_render(g_app, g_fb, g_fw, g_fh);
}

JNIEXPORT void JNICALL
Java_com_readflash_app_MainActivity_nativeResize(JNIEnv *env, jclass cls, jint w, jint h, jint density)
{
    (void)env; (void)cls;
    if (w <= 0 || h <= 0) return;
    g_density = density;
    if (!g_app) g_app = rf_app_create(&g_store, w, h, density);
    else rf_app_resize(g_app, w, h, density);
    fb_alloc(w, h);
}

JNIEXPORT void JNICALL
Java_com_readflash_app_MainActivity_nativeTap(JNIEnv *env, jclass cls, jint x, jint y)
{
    (void)env; (void)cls;
    if (g_app) rf_app_tap(g_app, x, y);
}

JNIEXPORT void JNICALL
Java_com_readflash_app_MainActivity_nativeBack(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    if (g_app) rf_app_key_back(g_app);
}

JNIEXPORT void JNICALL
Java_com_readflash_app_MainActivity_nativeRender(JNIEnv *env, jclass cls, jintArray px)
{
    jint *buf;
    int i, n;
    (void)cls;
    if (!g_app || !g_fb || !px) return;
    n = g_fw * g_fh;
    if ((*env)->GetArrayLength(env, px) < n) return;
    rf_app_render(g_app, g_fb, g_fw, g_fh);
    buf = (jint *)(*env)->GetPrimitiveArrayCritical(env, px, NULL);
    if (!buf) return;
    for (i = 0; i < n; i++) {
        u16 c = g_fb[i];
        unsigned r = (unsigned)((c >> 11) & 31) * 255 / 31;
        unsigned g = (unsigned)((c >> 5) & 63) * 255 / 63;
        unsigned b = (unsigned)(c & 31) * 255 / 31;
        buf[i] = (jint)(0xFF000000u | (r << 16) | (g << 8) | b);
    }
    (*env)->ReleasePrimitiveArrayCritical(env, px, buf, 0);
}

/* Java 侧选择完成后回调：刷新导入页面 */
JNIEXPORT void JNICALL
Java_com_readflash_app_MainActivity_nativeOnPicked(JNIEnv *env, jclass cls, jint n)
{
    (void)env; (void)cls;
    rf_app_on_files_picked((int)n);
}

/* 便于用 rish 抓运行时信息 */
JNIEXPORT jstring JNICALL
Java_com_readflash_app_MainActivity_nativeStatus(JNIEnv *env, jclass cls)
{
    char b[256];
    (void)cls;
    snprintf(b, sizeof b, "book=%d fb=%dx%d density=%d books=%d",
             g_app ? rf_app_book_id(g_app) : -1, g_fw, g_fh, g_density, 0);
    return (*env)->NewStringUTF(env, b);
}
