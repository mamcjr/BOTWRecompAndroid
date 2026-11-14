// The app icon's long-press "Choose driver" shortcut (res/xml/shortcuts.xml): a Vulkan driver package picked before the
// game starts. On a phone whose own driver is too old for the game, the app stops before the start screen
// (runtime/nvn/vulkan/device.c), so Settings > Files > Choose driver is out of reach (issues #2, #5, #6, #9). The
// package goes where that button puts it (BOTWRecompAndroidActivity.installDriver), then the app starts again in a new
// process, as restartApp does: the driver is loaded once, at start.
package com.zbotw.recomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

public class DriverPickActivity extends Activity {
    private static final int REQ_DRIVER = 1;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) return;  // (recreated while the picker is up: its result still comes here)
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, REQ_DRIVER);
        } catch (Exception e) {
            Log.e("BOTWRecompAndroid", "driver picker: " + e);
            finish();
        }
    }

    @Override
    protected void onActivityResult(int req, int result, Intent data) {
        Uri f = result == RESULT_OK && data != null ? data.getData() : null;
        if (req != REQ_DRIVER || f == null) {
            finish();
            return;
        }
        new Thread(() -> {
            String name = BOTWRecompAndroidActivity.installDriver(this, f);
            runOnUiThread(() -> {
                if (name.isEmpty()) {  // (not a .zip or .so: nothing changed)
                    Toast.makeText(this, R.string.driver_not_a_package, Toast.LENGTH_LONG).show();
                    finish();
                    return;
                }
                Log.i("BOTWRecompAndroid", "shortcut: driver " + name);
                Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
                if (i != null) {
                    i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                    startActivity(i);
                }
                System.exit(0);  // (a game already running loaded the old driver)
            });
        }, "driver").start();
    }
}
