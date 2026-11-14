package com.zbotw.recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Base64;
import android.view.PixelCopy;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

/** Opt-in, loopback-only snapshots of the game's minimap. No guest-memory or video stream access. */
final class WatchMapServer {
    static final String ACTION = "com.zbotw.recomp.action.WATCH_MAP";
    private static final int PORT = 18880, SIZE = 240;
    private final Activity activity;
    private final SharedPreferences prefs;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final String token;
    private volatile boolean resumed;
    private volatile ServerSocket listener;
    private volatile byte[] cached;
    private volatile byte[] cachedLegacy;
    private volatile long capturedAt;
    private static native String nativeMapState();

    WatchMapServer(Activity activity) {
        this.activity = activity;
        prefs = activity.getSharedPreferences("watch_map", Context.MODE_PRIVATE);
        String saved = prefs.getString("token", null);
        token = saved != null ? saved : UUID.randomUUID().toString().replace("-", "");
        if (saved == null) prefs.edit().putString("token", token).apply();
    }

    void resume() {
        resumed = true;
        if (prefs.getBoolean("enabled", false)) start();
    }

    void pause() { resumed = false; stop(); }

    private synchronized void start() {
        if (listener != null) return;
        try {
            ServerSocket server = new ServerSocket(PORT, 2, InetAddress.getByName("127.0.0.1"));
            listener = server;
            Thread worker = new Thread(() -> serve(server), "watch-map");
            worker.setDaemon(true);
            worker.start();
        } catch (IOException e) {
            Toast.makeText(activity, "Não foi possível abrir a conexão do relógio (porta 18880).", Toast.LENGTH_LONG).show();
        }
    }

    private synchronized void stop() {
        ServerSocket server = listener;
        listener = null;
        cached = null;
        cachedLegacy = null;
        if (server != null) try { server.close(); } catch (IOException ignored) { }
    }

    private void serve(ServerSocket server) {
        while (!server.isClosed()) {
            try (Socket client = server.accept()) {
                client.setSoTimeout(3000);
                handle(client, server);
            } catch (IOException | RuntimeException ignored) { /* closed, disconnected or malformed client */ }
        }
    }

    private void handle(Socket client, ServerSocket server) throws IOException {
        InputStream in = client.getInputStream();
        ByteArrayOutputStream head = new ByteArrayOutputStream();
        int tail = 0;
        boolean complete = false;
        for (int n = 0; n < 8192; n++) {
            int b = in.read();
            if (b < 0) break;
            head.write(b);
            tail = (tail << 8) | b;
            if (tail == 0x0d0a0d0a) { complete = true; break; }
        }
        if (!complete) { reply(client, 400, "text/plain", "Invalid request".getBytes(StandardCharsets.UTF_8)); return; }
        String[] first = head.toString(StandardCharsets.US_ASCII.name()).split("\r\n", 2)[0].split(" ");
        if (first.length != 3 || !"GET".equals(first[0]) || !first[1].startsWith("/")) {
            reply(client, 400, "text/plain", new byte[0]); return;
        }
        Uri uri = Uri.parse(first[1]);
        if (!token.equals(uri.getQueryParameter("token"))) {
            reply(client, 403, "text/plain", new byte[0]); return;
        }
        boolean legacy = "/v1/map.json".equals(uri.getPath());
        boolean state = "/v1/state.json".equals(uri.getPath());
        if (!state && !legacy && !"/v1/map.png".equals(uri.getPath())) {
            reply(client, 404, "text/plain", new byte[0]); return;
        }
        if (!resumed || listener != server || !prefs.getBoolean("enabled", false)) {
            reply(client, 503, "text/plain", new byte[0]); return;
        }
        if (state) {
            try { reply(client, 200, "application/json", nativeMapState().getBytes(StandardCharsets.UTF_8)); }
            catch (LinkageError error) { reply(client, 503, "text/plain", new byte[0]); }
            return;
        }
        try {
            long now = SystemClock.elapsedRealtime();
            if (cached == null || now - capturedAt >= 2000) {
                CompletableFuture<Bitmap> result = new CompletableFuture<>();
                ui.post(() -> capture(result));
                Bitmap bitmap;
                try { bitmap = result.get(3, TimeUnit.SECONDS); }
                catch (Exception e) { result.cancel(false); throw e; }
                try {
                    ByteArrayOutputStream data = new ByteArrayOutputStream();
                    if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, data)) throw new IOException("PNG");
                    int[] pixels = new int[SIZE * SIZE];
                    bitmap.getPixels(pixels, 0, SIZE, 0, 0, SIZE, SIZE);
                    byte[] legacyData = ("{\"encoding\":\"tga-palette-332\",\"width\":240,\"height\":240,\"data\":\"" +
                        Base64.encodeToString(WatchMapImage.encode(pixels), Base64.NO_WRAP) + "\"}").getBytes(StandardCharsets.UTF_8);
                    synchronized (this) {
                        if (!resumed || listener != server) throw new IOException("Paused");
                        cached = data.toByteArray();
                        cachedLegacy = legacyData;
                        capturedAt = SystemClock.elapsedRealtime();
                    }
                } finally { bitmap.recycle(); }
            }
            byte[] frame = legacy ? cachedLegacy : cached;
            if (!resumed || listener != server || frame == null) throw new IOException("Paused");
            reply(client, 200, legacy ? "application/json" : "image/png", frame);
        } catch (Exception e) {
            reply(client, 503, "text/plain", "Abra o jogo e deixe o minimapa visível.".getBytes(StandardCharsets.UTF_8));
        }
    }

    private void capture(CompletableFuture<Bitmap> result) {
        SurfaceView surface = findSurface(activity.getWindow().getDecorView());
        if (!resumed || result.isDone() || surface == null || !surface.getHolder().getSurface().isValid()) {
            result.completeExceptionally(new IOException("No active game surface")); return;
        }
        int w = surface.getWidth(), h = surface.getHeight();
        float left = 0, top = 0, vw = w, vh = h;
        if (prefs.getBoolean("letterbox", true)) {
            vw = Math.min(w, h * 16f / 9f);
            vh = Math.min(h, w * 9f / 16f);
            left = (w - vw) / 2f;
            top = (h - vh) / 2f;
        }
        Rect crop = new Rect(Math.round(left + vw * prefs.getFloat("left", .79f)),
            Math.round(top + vh * prefs.getFloat("top", .70f)),
            Math.round(left + vw * prefs.getFloat("right", .95f)),
            Math.round(top + vh * prefs.getFloat("bottom", .98f)));
        if (w < 1 || h < 1 || crop.width() < 1 || crop.height() < 1) {
            result.completeExceptionally(new IOException("Invalid crop")); return;
        }
        Bitmap bitmap = Bitmap.createBitmap(SIZE, SIZE, Bitmap.Config.ARGB_8888);
        try {
            PixelCopy.request(surface, crop, bitmap, status -> {
                if (status != PixelCopy.SUCCESS || !resumed) {
                    bitmap.recycle();
                    result.completeExceptionally(new IOException("Capture unavailable"));
                } else if (!result.complete(bitmap)) bitmap.recycle();
            }, ui);
        } catch (RuntimeException e) {
            bitmap.recycle();
            result.completeExceptionally(e);
        }
    }

    private static SurfaceView findSurface(View root) {
        if (root instanceof SurfaceView) return (SurfaceView) root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); i++) {
                SurfaceView found = findSurface(group.getChildAt(i));
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void reply(Socket client, int status, String type, byte[] body) throws IOException {
        String reason = status == 200 ? "OK" : status == 503 ? "Service Unavailable" : "Error";
        OutputStream out = client.getOutputStream();
        out.write(("HTTP/1.1 " + status + " " + reason + "\r\nContent-Type: " + type +
            "\r\nContent-Length: " + body.length + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n")
            .getBytes(StandardCharsets.US_ASCII));
        out.write(body);
        out.flush();
    }

    void showSettings() {
        LinearLayout panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(32, 12, 32, 8);
        CheckBox enabled = new CheckBox(activity);
        enabled.setText("Compartilhar posição e marcadores com o relógio");
        enabled.setChecked(prefs.getBoolean("enabled", false));
        panel.addView(enabled);
        TextView code = new TextView(activity);
        code.setText("Código de conexão (cole nas configurações do app Recomp Mapa no Zepp):\n" + token);
        code.setTextIsSelectable(true);
        panel.addView(code);
        CheckBox letterbox = new CheckBox(activity);
        letterbox.setText("Jogo em proporção 16:9 (com barras laterais)");
        letterbox.setChecked(prefs.getBoolean("letterbox", true));
        panel.addView(letterbox);
        TextView label = new TextView(activity);
        label.setText("Área do minimapa em %: esquerda, topo, direita, base.\nMantenha o minimapa visível; menus também aparecem na captura.");
        panel.addView(label);
        EditText crop = new EditText(activity);
        crop.setSingleLine(true);
        crop.setText(String.format(Locale.US, "%.1f, %.1f, %.1f, %.1f", prefs.getFloat("left", .79f) * 100,
            prefs.getFloat("top", .70f) * 100, prefs.getFloat("right", .95f) * 100, prefs.getFloat("bottom", .98f) * 100));
        panel.addView(crop);
        AlertDialog dialog = new AlertDialog.Builder(activity).setTitle("Mapa no relógio").setView(panel)
            .setNegativeButton("Cancelar", null).setNeutralButton("Copiar código", null).setPositiveButton("Salvar", null).create();
        dialog.setOnShowListener(ignored -> {
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> {
                ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
                clipboard.setPrimaryClip(ClipData.newPlainText("Recomp Map", token));
                Toast.makeText(activity, "Código copiado", Toast.LENGTH_SHORT).show();
            });
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
                try {
                    String[] values = crop.getText().toString().split(",");
                    if (values.length != 4) throw new IllegalArgumentException();
                    float[] a = new float[4];
                    for (int i = 0; i < 4; i++) {
                        a[i] = Float.parseFloat(values[i].trim()) / 100f;
                        if (!Float.isFinite(a[i]) || a[i] < 0 || a[i] > 1) throw new IllegalArgumentException();
                    }
                    if (a[0] >= a[2] || a[1] >= a[3]) throw new IllegalArgumentException();
                    prefs.edit().putBoolean("enabled", enabled.isChecked()).putBoolean("letterbox", letterbox.isChecked())
                        .putFloat("left", a[0]).putFloat("top", a[1]).putFloat("right", a[2]).putFloat("bottom", a[3]).apply();
                    stop();
                    if (resumed && enabled.isChecked()) start();
                    dialog.dismiss();
                } catch (IllegalArgumentException e) { crop.setError("Use quatro valores de 0 a 100, com esquerda < direita e topo < base."); }
            });
        });
        dialog.show();
    }
}
