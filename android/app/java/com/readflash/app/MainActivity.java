package com.readflash.app;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;

import java.nio.ByteBuffer;
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

    private static final int MAX_GLYPH = 64;
    private static final Paint PAINT = new Paint(Paint.ANTI_ALIAS_FLAG);
    private static final HashMap<Integer, Bitmap> CACHE = new HashMap<Integer, Bitmap>();
    private static byte[] alpha = new byte[MAX_GLYPH * MAX_GLYPH];

    private RailView view;
    private Bitmap bitmap;
    private int[] pixels;
    private int density;

    /** 由 native 调用：取一个码点的 alpha 位图 */
    @SuppressWarnings("unused")
    private static int glyph(int cp, int px, byte[] out) {
        int key = (cp << 8) | (px & 0xFF);
        Bitmap b = CACHE.get(key);
        if (b == null) {
            String s = new String(Character.toChars(cp));
            Paint p = new Paint(PAINT);
            p.setTextSize(px);
            p.setTypeface(Typeface.MONOSPACE);
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
        density = (int) (getResources().getDisplayMetrics().density * 1000);
        String dataDir = getFilesDir().getAbsolutePath();
        String importDir = getExternalFilesDir(null) != null
                ? getExternalFilesDir(null).getAbsolutePath() + "/import"
                : dataDir + "/import";
        nativeInit(dataDir, importDir, density);
        view = new RailView(this);
        view.setBackgroundColor(0xFF000000);
        setContentView(view);
        Log.i("READFLASH", "init dataDir=" + dataDir + " importDir=" + importDir + " density=" + density);
    }

    @Override
    public void onBackPressed() {
        nativeBack();
        view.redraw();
    }

    /** native 侧渲染时回调取字形 */
    @SuppressWarnings("unused")
    static int glyphProvider(int cp, int px) {
        int r = glyph(cp, px, alpha);
        return r;
    }

    @SuppressWarnings("unused")
    static byte[] alphaBuffer() {
        return alpha;
    }
}
