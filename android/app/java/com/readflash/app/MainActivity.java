package com.readflash.app;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;

/**
 * Read-Flash 安卓外壳。
 * Java 只做三件事：把 C 渲染的像素搬上屏、把触摸/返回键转给 C、用系统字体光栅化字形。
 * 应用逻辑（书树、排版、翻页、进度、存储）全部在 src/core 的 C 代码里。
 */
public class MainActivity extends Activity {

    static {
        System.loadLibrary("readflash");
    }

    private static native void nativeInit(String dataDir, String importDir, int density);

    private static native void nativeResize(int w, int h, int density);

    private static native void nativeTap(int x, int y);

    private static native void nativeBack();

    private static native void nativeRender(int[] px);

    /* 字形光栅化：返回 (w << 16) | h，0 表示失败；alpha 写入 out（每像素 1 字节） */
    private static native int nativeGlyph(int cp, int px, byte[] out);

    private static native void nativeOnPicked(int n);

    private static final int MAX_GLYPH = 256;
    private static final Paint PAINT = new Paint(Paint.ANTI_ALIAS_FLAG);
    private static final HashMap<Integer, Bitmap> CACHE = new HashMap<Integer, Bitmap>();
    private static byte[] alpha = new byte[MAX_GLYPH * MAX_GLYPH];

    private static final int REQ_PICK = 1001;
    private static MainActivity inst;
    private String importDir;
    private RailView view;
    private Bitmap bitmap;
    private int[] pixels;
    private int density;

    /** 由 native 调用：取一个码点的 alpha 位图 */
    @SuppressWarnings("unused")
    private static int glyph(int cp, int px, int flags, byte[] out) {
        int key = (cp << 16) | (px << 4) | (flags & 0xF);
        Bitmap b = CACHE.get(key);
        if (b == null) {
            String s = new String(Character.toChars(cp));
            Paint p = new Paint(PAINT);
            p.setTextSize(px);
            p.setTypeface(Typeface.MONOSPACE);
            p.setFakeBoldText((flags & 1) != 0);      /* 位0 = 加粗（标题三档） */
            int w = (int) Math.ceil(p.measureText(s)) + 2;
            int h = px + 4;
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (w > MAX_GLYPH) w = MAX_GLYPH;
            if (h > MAX_GLYPH) h = MAX_GLYPH;
            b = Bitmap.createBitmap(w, h, Bitmap.Config.ALPHA_8);
            Canvas c = new Canvas(b);
            p.setColor(0xFFFFFFFF);
            c.drawText(s, 1, h - 4, p);
            if (CACHE.size() > 900) CACHE.clear();
            CACHE.put(key, b);
        }
        int w = b.getWidth(), h = b.getHeight();
        if (w * h > out.length) return 0;
        b.copyPixelsToBuffer(ByteBuffer.wrap(out));
        return (w << 16) | h;
    }

    private final class RailView extends View {
        RailView(Activity a) {
            super(a);
        }

        @Override
        protected void onSizeChanged(int w, int h, int ow, int oh) {
            super.onSizeChanged(w, h, ow, oh);
            if (w <= 0 || h <= 0) return;
            bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            pixels = new int[w * h];
            nativeResize(w, h, density);
            redraw();
        }

        @Override
        protected void onDraw(Canvas c) {
            if (bitmap != null) c.drawBitmap(bitmap, 0, 0, null);
        }

        void redraw() {
            if (pixels == null) return;
            nativeRender(pixels);
            bitmap.setPixels(pixels, 0, bitmap.getWidth(), 0, 0, bitmap.getWidth(), bitmap.getHeight());
            invalidate();
        }

        private long downMs;
        private float downX, downY;

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downMs = System.currentTimeMillis();
                    downX = e.getX();
                    downY = e.getY();
                    return true;
                case MotionEvent.ACTION_UP: {
                    long dt = System.currentTimeMillis() - downMs;
                    float dx = Math.abs(e.getX() - downX), dy = Math.abs(e.getY() - downY);
                    if (dt <= 300 && dx < 10 * density / 1000f && dy < 10 * density / 1000f) {
                        nativeTap((int) e.getX(), (int) e.getY());
                        redraw();
                    }
                    return true;
                }
                default:
                    return super.onTouchEvent(e);
            }
        }
    }

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        inst = this;
        density = (int) (getResources().getDisplayMetrics().density * 1000);
        String dataDir;
        try {
            dataDir = getFilesDir().getCanonicalPath();
        } catch (Exception e) {
            dataDir = getFilesDir().getAbsolutePath();
        }
        File ext = getExternalFilesDir(null);
        try {
            importDir = (ext != null ? ext.getCanonicalPath() : dataDir) + "/import";
        } catch (Exception e) {
            importDir = (ext != null ? ext.getAbsolutePath() : dataDir) + "/import";
        }
        nativeInit(dataDir, importDir, density);
        view = new RailView(this);
        view.setBackgroundColor(0xFF000000);
        setContentView(view);
        Log.i("READFLASH", "init dataDir=" + dataDir + " importDir=" + importDir + " density=" + density);
    }

    /** 由 native 调用：打开系统文件选择器（SAF），选中后拷进导入目录 */
    @SuppressWarnings("unused")
    static void pickFiles() {
        MainActivity a = inst;
        if (a == null) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("text/plain");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        try {
            a.startActivityForResult(i, REQ_PICK);
        } catch (Exception e) {
            Log.w("READFLASH", "no document picker", e);
            nativeOnPicked(0);
        }
    }

    private String displayName(Uri u) {
        Cursor c = null;
        try {
            c = getContentResolver().query(u, null, null, null, null);
            if (c != null && c.moveToFirst()) {
                int k = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (k >= 0) return c.getString(k);
            }
        } catch (Exception ignored) {
        } finally {
            if (c != null) c.close();
        }
        return null;
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_PICK) return;
        int n = 0;
        if (res == RESULT_OK && data != null) {
            ArrayList<Uri> uris = new ArrayList<Uri>();
            if (data.getClipData() != null) {
                for (int k = 0; k < data.getClipData().getItemCount(); k++)
                    uris.add(data.getClipData().getItemAt(k).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
            File dir = new File(importDir);
            dir.mkdirs();
            for (Uri u : uris) {
                try {
                    String name = displayName(u);
                    if (name == null) name = "picked" + n + ".txt";
                    InputStream in = getContentResolver().openInputStream(u);
                    if (in == null) continue;
                    FileOutputStream out = new FileOutputStream(new File(dir, name));
                    byte[] buf = new byte[8192];
                    int r;
                    while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
                    out.close();
                    in.close();
                    n++;
                } catch (Exception e) {
                    Log.w("READFLASH", "copy failed", e);
                }
            }
        }
        nativeOnPicked(n);
        view.redraw();
    }

    /** 由 native 调用：根页面确认退出后结束应用 */
    @SuppressWarnings("unused")
    static void finishApp() {
        final MainActivity a = inst;
        if (a == null) return;
        a.runOnUiThread(new Runnable() {
            public void run() {
                a.finishAffinity();
            }
        });
    }

    @Override
    public void onBackPressed() {
        nativeBack();
        view.redraw();
    }
}
