// BOTWRecompAndroid Android probe (docs/las/ANDROID-PORT.md, Fase 0). Runs once per launch, in this order: the guest window
// placement (before any driver maps memory), the system Vulkan driver, then every custom driver found in
// <external files>/drivers/ (a .so, or an adrenotools .zip). The report is shown, written to
// <external files>/report-<time>.txt (and report-latest.txt) section by section, and logged under the tag LASProbe.
package com.zbotw.recomp.probe;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.widget.HorizontalScrollView;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class MainActivity extends Activity {
    static {
        System.loadLibrary("lasprobe");
    }

    private static final String TAG = "LASProbe";

    private native String memoryProbe();

    private native String vulkanProbe(String nativeLibDir, String driverDir, String driverName);

    private TextView text;
    private final StringBuilder report = new StringBuilder();
    private File reportFile, latestFile;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this);
        text.setTypeface(Typeface.MONOSPACE);
        text.setTextSize(10);
        text.setTextIsSelectable(true);
        text.setPadding(24, 24, 24, 24);
        HorizontalScrollView h = new HorizontalScrollView(this);
        h.addView(text);
        ScrollView v = new ScrollView(this);
        v.addView(h);
        setContentView(v);

        File ext = getExternalFilesDir(null);
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
        reportFile = new File(ext, "report-" + stamp + ".txt");
        latestFile = new File(ext, "report-latest.txt");
        new Thread(this::run, "probe").start();
    }

    private void run() {
        String libDir = getApplicationInfo().nativeLibraryDir;
        section("== Aparelho\n" + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + "), Android "
                + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), SoC " + Build.SOC_MANUFACTURER + " "
                + Build.SOC_MODEL + "\nnativeLibraryDir: " + libDir + "\n");
        section(memoryProbe());
        section(vulkanProbe(libDir, null, null));
        List<File[]> drivers = customDrivers();
        if (drivers.isEmpty())
            section("== Drivers customizados\nnenhum em " + new File(getExternalFilesDir(null), "drivers")
                    + " (copie um libvulkan_freedreno.so ou o .zip do adrenotools para la)\n");
        for (File[] d : drivers)  // d[0] = folder (internal storage), d[1] = the driver library in it
            section(vulkanProbe(libDir, d[0].getAbsolutePath() + "/", d[1].getName()));
        section("== Fim\n");
    }

    // Custom drivers from <external files>/drivers, copied to internal storage (dlopen refuses external storage).
    private List<File[]> customDrivers() {
        List<File[]> out = new ArrayList<>();
        File src = new File(getExternalFilesDir(null), "drivers");
        src.mkdirs();
        File[] files = src.listFiles();
        if (files == null) return out;
        Arrays.sort(files);
        for (File f : files) {
            String name = f.getName();
            if (!name.endsWith(".so") && !name.endsWith(".zip")) continue;
            File dir = new File(getFilesDir(), "drivers/" + name.replaceAll("\\.(so|zip)$", ""));
            deleteTree(dir);
            dir.mkdirs();
            try {
                File lib = name.endsWith(".so") ? copy(new FileInputStream(f), new File(dir, name)) : unzipDriver(f, dir);
                if (lib == null) section("== Driver " + name + "\nERRO: nenhum .so dentro do zip\n");
                else out.add(new File[] {dir, lib});
            } catch (IOException e) {
                section("== Driver " + name + "\nERRO: " + e + "\n");
            }
        }
        return out;
    }

    // An adrenotools driver package: meta.json plus the driver library (its name is in meta.json; the only .so).
    private static File unzipDriver(File zip, File dir) throws IOException {
        File lib = null;
        try (ZipInputStream z = new ZipInputStream(new FileInputStream(zip))) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                String n = new File(e.getName()).getName();
                if (e.isDirectory() || !n.endsWith(".so")) continue;
                lib = copyNoClose(z, new File(dir, n));
            }
        }
        return lib;
    }

    private static File copy(InputStream in, File dst) throws IOException {
        try (InputStream i = in) {
            return copyNoClose(i, dst);
        }
    }

    private static File copyNoClose(InputStream in, File dst) throws IOException {
        try (OutputStream o = new FileOutputStream(dst)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
        }
        return dst;
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    // Shown, logged and saved right away: a driver that crashes the process doesn't take the earlier sections with it.
    private synchronized void section(String s) {
        report.append(s).append('\n');
        for (String line : s.split("\n")) Log.i(TAG, line);
        String all = report.toString();
        for (File f : new File[] {reportFile, latestFile}) {
            try (OutputStream o = new FileOutputStream(f)) {
                o.write(all.getBytes());
            } catch (IOException e) {
                Log.e(TAG, "report: " + e);
            }
        }
        runOnUiThread(() -> text.setText(all));
    }
}
