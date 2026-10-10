/* PAL：iOS 实现（Objective-C / ARC）
 *   - 数据目录：Library/Application Support/ReadFlash（不对外暴露）
 *   - 导入目录：Documents/import（配合 Info.plist 的 UIFileSharingEnabled
 *     与 LSSupportsOpeningDocumentsInPlace，会出现在"文件 → 我的 iPhone → Read-Flash"）
 *   - 文件选择：UIDocumentPickerViewController（iOS 11+，覆盖 iOS 12–26）
 *   - 字形：CoreText 光栅化到 8 位 alpha 位图（与安卓 Java Paint 同一契约）
 * 注意：本文件需在 macOS + Xcode 下编译，本仓库的 Linux 环境不参与 iOS 构建。
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <CoreText/CoreText.h>
#include "rf.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <mach/mach_time.h>

static char g_data[1024];
static char g_import[1024];
static void (*g_exit_hook)(void);
static void (*g_pick_hook)(void);

void pal_set_dirs(const char *app_data, const char *import_dir)
{
    if (app_data) snprintf(g_data, sizeof g_data, "%s", app_data);
    if (import_dir) snprintf(g_import, sizeof g_import, "%s", import_dir);
}
const char *pal_app_data_dir(void) { return g_data; }
const char *pal_import_dir(void) { return g_import; }

u64 pal_ticks_ms(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (u64)((mach_absolute_time() * tb.numer / tb.denom) / 1000000ull);
}
u64 pal_now_unix(void) { return (u64)time(NULL); }

void pal_set_exit_hook(void (*fn)(void)) { g_exit_hook = fn; }
void pal_set_pick_hook(void (*fn)(void)) { g_pick_hook = fn; }
void pal_app_exit(void) { if (g_exit_hook) g_exit_hook(); else exit(0); }
void pal_pick_files(void) { if (g_pick_hook) g_pick_hook(); }

/* ---------- 目录初始化（App 启动时调用一次） ---------- */
void pal_ios_init_dirs(void)
{
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *support = paths.count ? paths[0] : NSTemporaryDirectory();
    NSString *data = [support stringByAppendingPathComponent:@"ReadFlash"];
    NSArray *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *doc = docs.count ? docs[0] : NSTemporaryDirectory();
    NSString *imp = [doc stringByAppendingPathComponent:@"import"];
    NSFileManager *fm = [NSFileManager defaultManager];
    [fm createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:nil];
    [fm createDirectoryAtPath:imp withIntermediateDirectories:YES attributes:nil error:nil];
    pal_set_dirs([data fileSystemRepresentation], [imp fileSystemRepresentation]);
}

/* ---------- 字形：CoreText → 8 位 alpha ---------- */
int rf_glyph_ios(void *ud, u32 cp, int px, int flags, u8 *alpha, int cap, int *gw, int *gh)
{
    (void)ud;
    if (px < 1) return 0;
    UniChar buf[2];
    int len = 0;
    if (cp < 0x10000) { buf[len++] = (UniChar)cp; }
    else { u32 v = cp - 0x10000; buf[len++] = (UniChar)(0xD800 + (v >> 10)); buf[len++] = (UniChar)(0xDC00 + (v & 0x3FF)); }
    CTFontRef font = CTFontCreateWithName(CFSTR("Menlo"), px, NULL);
    if (!font) return 0;
    if (flags & RF_GLYPH_BOLD) {
        CTFontRef b = CTFontCreateCopyWithSymbolicTraits(font, px, NULL, kCTFontBoldTrait, kCTFontBoldTrait);
        if (b) { CFRelease(font); font = b; }
    }
    int w = (int)(px * (rf_char_cols(cp) == 2 ? 1.05 : 0.6)) + 2;
    int h = px + 4;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 64) w = 64;
    if (h > 64) h = 64;
    if (w * h > cap) { CFRelease(font); return 0; }
    memset(alpha, 0, (size_t)(w * h));
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(alpha, w, h, 8, w, cs, kCGImageAlphaOnly);
    CGColorSpaceRelease(cs);
    if (!ctx) { CFRelease(font); return 0; }
    CGContextSetGrayFillColor(ctx, 1.0, 1.0);
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    CGContextSetTextPosition(ctx, 1, 3);                 /* 基线 */
    CGGlyph glyphs[2];
    if (CTFontGetGlyphsForCharacters(font, buf, glyphs, len)) {
        CGContextShowGlyphsAtPositions(ctx, glyphs, (CGPoint[]){{0, 0}}, len);
    }
    CGContextRelease(ctx);
    CFRelease(font);
    *gw = w;
    *gh = h;
    return 1;
}

/* ---------- 文件选择：UIDocumentPicker ---------- */
@interface RFPicker : NSObject <UIDocumentPickerDelegate>
@end

static RFPicker *g_picker;

@implementation RFPicker
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    int n = 0;
    NSString *imp = [NSString stringWithUTF8String:pal_import_dir()];
    for (NSURL *u in urls) {
        BOOL scoped = [u startAccessingSecurityScopedResource];
        NSURL *dst = [NSURL fileURLWithPath:[imp stringByAppendingPathComponent:u.lastPathComponent]];
        [[NSFileManager defaultManager] removeItemAtURL:dst error:nil];
        if ([[NSFileManager defaultManager] copyItemAtURL:u toURL:dst error:nil]) n++;
        if (scoped) [u stopAccessingSecurityScopedResource];
    }
    rf_app_on_files_picked(n);                            /* 通知 C 侧刷新导入页面 */
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c
{
    rf_app_on_files_picked(0);
}
@end

void pal_ios_pick(void)
{
    UIViewController *root = UIApplication.sharedApplication.keyWindow.rootViewController;
    if (!root) return;
    if (!g_picker) g_picker = [RFPicker new];
    UIDocumentPickerViewController *p =
        [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.plain-text"]
                                                              inMode:UIDocumentPickerModeImport];
    p.delegate = g_picker;
    p.allowsMultipleSelection = YES;
    [root presentViewController:p animated:YES completion:nil];
}
