package com.readflash.verify;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * Read-Flash 最小验证（§16）：
 * 在设备上跑 core 自检（网格 / UTF-8 / 断行 / 分页 / 锚点 / 上一页），
 * 结果同时输出到屏幕与 logcat（tag = RFVERIFY）。
 */
public class MainActivity extends Activity {

    static {
        System.loadLibrary("rfverify");
    }

    /** 由 native 层执行自检并返回报告文本；返回 0 表示全部通过。 */
    private static native String runSelfTest();

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);

        final TextView tv = new TextView(this);
        tv.setTextSize(11f);
        tv.setTextColor(0xFFE0E0E0);
        tv.setBackgroundColor(0xFF000000);
        tv.setText("Read-Flash 最小验证运行中…\n5MB 中文语料，约需数秒。");

        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(0xFF000000);
        sv.addView(tv);
        setContentView(sv);

        new Thread(new Runnable() {
            public void run() {
                final String report = runSelfTest();
                Log.i("RFVERIFY", report);
                runOnUiThread(new Runnable() {
                    public void run() {
                        tv.setText(report);
                    }
                });
            }
        }).start();
    }
}
