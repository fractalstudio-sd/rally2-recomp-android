package org.srally2;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * First screen: makes sure the game data exists, then starts the game (MainActivity, the SDL host).
 * The data is built on the phone from the user's own srally2.zip (picked with the system file picker, or dropped by hand
 * as Android/data/org.srally2/files/srally2.zip). Nothing from the ROMs ships inside the APK's Java side.
 */
public class LauncherActivity extends Activity {
    static final String APP_NAME = "Rally Recomp";   // neutral name: no third-party trademarks
    private static final int REQ_PICK = 42;
    private File base, images;
    private TextView status;
    private Button pick, browse;
    private View mainView;
    private boolean isTv;
    private volatile boolean busy = false;
    private volatile ServerSocket server;
    private volatile String hint = "";

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        images = new File(base, "images");
        isTv = getPackageManager().hasSystemFeature("android.software.leanback");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (RomBuilder.isComplete(images)) { startGame(); return; }
        startUploader(base);
        buildUi();
        File zip = new File(base, "srally2.zip");
        if (zip.isFile()) {
            try { runBuild(new FileInputStream(zip), zip); } catch (Exception e) { fail(e); }
        } else if (!isTv) {
            openPicker();        // phones: straight to the system picker
        } else {
            openBrowser();       // TVs: the built-in file browser (many have no system picker), or the upload page
        }
    }

    private void buildUi() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setGravity(Gravity.CENTER);
        l.setBackgroundColor(Color.BLACK);
        int pad = (int) (getResources().getDisplayMetrics().density * 48);
        l.setPadding(pad, pad, pad, pad);   // generous: TVs overscan
        TextView title = new TextView(this);
        title.setText(APP_NAME);
        title.setTextColor(Color.WHITE); title.setTextSize(28); title.setGravity(Gravity.CENTER);
        status = new TextView(this);
        status.setTextColor(Color.LTGRAY); status.setTextSize(18); status.setGravity(Gravity.CENTER);
        status.setPadding(0, 32, 0, 32);
        status.setText(waitingText());
        browse = new Button(this);
        browse.setText("Sfoglia cartelle e chiavette USB");
        browse.setOnClickListener(new View.OnClickListener() { @Override public void onClick(View v) { openBrowser(); } });
        pick = new Button(this);
        pick.setText("Selettore file di sistema");
        pick.setOnClickListener(new View.OnClickListener() { @Override public void onClick(View v) { openPicker(); } });
        LinearLayout.LayoutParams wrap = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        l.addView(title, wrap); l.addView(status, wrap);
        if (isTv) { l.addView(browse, wrap); l.addView(pick, wrap); } else { l.addView(pick, wrap); l.addView(browse, wrap); }
        mainView = l;
        setContentView(l);
        (isTv ? browse : pick).requestFocus();
        if (isTv && server == null) startServer();   // TVs only
    }

    private String waitingText() {
        return "Serve il file srally2.zip (il tuo set ROM).\nVerr\u00e0 letto una volta sola e preparato qui.\n\n" + hint;
    }

    /** Upload from another device on the same Wi-Fi: Android TVs often have no file picker at all. */
    private void startServer() {
        new Thread(new Runnable() { @Override public void run() {
            ServerSocket ss;
            try { ss = new ServerSocket(8080); } catch (Exception e) { try { ss = new ServerSocket(0); } catch (Exception e2) { return; } }
            server = ss;
            String ip = localIp();
            hint = ip == null ? "" : "Oppure, da telefono o PC sulla stessa rete Wi-Fi,\napri nel browser:  http://" + ip + ":" + ss.getLocalPort() + "\ne carica srally2.zip.";
            runOnUiThread(new Runnable() { @Override public void run() { if (!busy && status != null) status.setText(waitingText()); } });
            while (!ss.isClosed()) {
                try { final Socket c = ss.accept(); new Thread(new Runnable() { @Override public void run() { serve(c); } }).start(); }
                catch (Exception e) { break; }
            }
        } }, "upload-server").start();
    }

    private static String localIp() {
        try {
            for (NetworkInterface n : Collections.list(NetworkInterface.getNetworkInterfaces()))
                for (InetAddress a : Collections.list(n.getInetAddresses()))
                    if (!a.isLoopbackAddress() && a instanceof java.net.Inet4Address && a.isSiteLocalAddress()) return a.getHostAddress();
        } catch (Exception ignored) { }
        return null;
    }

    private static final String PAGE = "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>" + APP_NAME + "</title>"
        + "<body style='font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:2em'><h2>" + APP_NAME + "</h2>"
        + "<p>Scegli <b>srally2.zip</b> (il tuo set ROM).</p><input type=file id=f accept='.zip'><p><button id=b style='font-size:1.2em;padding:.5em 1.5em'>Invia</button></p>"
        + "<progress id=p value=0 max=100 style='width:80%'></progress><p id=m></p>"
        + "<script>b.onclick=function(){var f=document.getElementById('f').files[0];if(!f){m.textContent='Scegli prima il file';return}"
        + "var x=new XMLHttpRequest();x.open('PUT','/upload');x.upload.onprogress=function(e){p.value=100*e.loaded/e.total;m.textContent=Math.round(100*e.loaded/e.total)+'%'};"
        + "x.onload=function(){m.textContent=x.status==200?'Fatto! Guarda la TV.':'Errore: '+x.responseText};x.onerror=function(){m.textContent='Errore di rete'};x.send(f)}</script>";

    private void serve(Socket c) {
        try {
            c.setSoTimeout(60000);
            InputStream in = c.getInputStream(); OutputStream out = c.getOutputStream();
            StringBuilder head = new StringBuilder(); int b, state = 0;
            while (head.length() < 16384 && (b = in.read()) >= 0) {
                head.append((char) b);
                state = (b == '\r' || b == '\n') ? state + 1 : 0;
                if (head.length() >= 4 && head.substring(head.length() - 4).equals("\r\n\r\n")) break;
            }
            String h = head.toString(); String line = h.split("\r\n")[0];
            if (line.startsWith("PUT ")) {
                long len = -1; for (String x : h.split("\r\n")) if (x.toLowerCase().startsWith("content-length:")) len = Long.parseLong(x.substring(15).trim());
                if (busy) { reply(out, 409, "occupato"); return; }
                if (len <= 0 || len > (3L << 30)) { reply(out, 400, "dimensione non valida"); return; }
                final File part = new File(base, "srally2.zip.part");
                FileOutputStream fo = new FileOutputStream(part);
                byte[] buf = new byte[1 << 16]; long got = 0; int n;
                while (got < len && (n = in.read(buf, 0, (int) Math.min(buf.length, len - got))) > 0) { fo.write(buf, 0, n); got += n; }
                fo.close();
                if (got < len) { part.delete(); reply(out, 400, "upload interrotto"); return; }
                final File zip = new File(base, "srally2.zip"); zip.delete();
                if (!part.renameTo(zip)) { reply(out, 500, "impossibile salvare"); return; }
                reply(out, 200, "ok");
                runOnUiThread(new Runnable() { @Override public void run() {
                    try { runBuild(new FileInputStream(zip), zip); } catch (Exception e) { fail(e); }
                } });
            } else {
                byte[] body = PAGE.getBytes("UTF-8");
                out.write(("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + body.length + "\r\nConnection: close\r\n\r\n").getBytes());
                out.write(body); out.flush();
            }
        } catch (Exception ignored) {
        } finally { try { c.close(); } catch (Exception ignored) { } }
    }

    private void reply(OutputStream out, int code, String msg) throws java.io.IOException {
        byte[] b = msg.getBytes("UTF-8");
        out.write(("HTTP/1.1 " + code + " X\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " + b.length + "\r\nConnection: close\r\n\r\n").getBytes());
        out.write(b); out.flush();
    }

    // ---- built-in file browser: internal storage, SD cards and USB drives, folders and .zip files, D-pad friendly ----
    private static final int REQ_PERM = 43;
    private ListView list;
    private TextView where;
    private File dir;                       // null = list of storage roots
    private boolean browsing = false, wantBrowser = false;
    private final List<File> entries = new ArrayList<>();

    private boolean hasFileAccess() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            try { return (Boolean) android.os.Environment.class.getMethod("isExternalStorageManager").invoke(null); }
            catch (Throwable t) { return false; }
        }
        return checkSelfPermission("android.permission.READ_EXTERNAL_STORAGE") == android.content.pm.PackageManager.PERMISSION_GRANTED;
    }

    /** Asks once for access to the files; the browser opens either way (readable folders still show). */
    private void openBrowser() {
        if (busy) return;
        if (!hasFileAccess() && !wantBrowser) {
            wantBrowser = true;
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                String[] acts = {"android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION", "android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION"};
                for (String a : acts) {
                    try {
                        Intent i = new Intent(a);
                        if (a.contains("APP_")) i.setData(Uri.parse("package:" + getPackageName()));
                        pausedSinceAsk = false;
                        startActivity(i);
                        if (status != null) status.setText("Consenti l'accesso a tutti i file per “" + APP_NAME + "”, poi torna indietro.");
                        return;                                  // continues in onResume
                    } catch (Exception ignored) { }
                }
            }
            requestPermissions(new String[]{"android.permission.READ_EXTERNAL_STORAGE"}, REQ_PERM);
            return;                                              // continues in onRequestPermissionsResult
        }
        wantBrowser = false;
        showBrowser(null);
    }

    @Override public void onRequestPermissionsResult(int req, String[] p, int[] r) {
        super.onRequestPermissionsResult(req, p, r);
        if (req == REQ_PERM && wantBrowser) { wantBrowser = false; showBrowser(null); }
    }

    private boolean pausedSinceAsk = false;
    @Override protected void onPause() { super.onPause(); if (wantBrowser) pausedSinceAsk = true; }
    @Override protected void onResume() {
        super.onResume();
        if (busy) return;
        if (wantBrowser) { if (pausedSinceAsk) { pausedSinceAsk = false; wantBrowser = false; showBrowser(null); } }   // back from the settings
        else if (browsing) showBrowser(dir);                    // refresh (a USB drive may have been plugged in)
    }

    private List<File> roots() {
        List<File> r = new ArrayList<>();
        File internal = android.os.Environment.getExternalStorageDirectory();
        if (internal != null) r.add(internal);
        File[] app = getExternalFilesDirs(null);            // one per volume: .../<volume>/Android/data/<pkg>/files
        if (app != null) for (File f : app) {
            if (f == null) continue;
            String s = f.getAbsolutePath(); int k = s.indexOf("/Android/data/");
            if (k > 0) { File v = new File(s.substring(0, k)); if (!r.contains(v)) r.add(v); }
        }
        File[] st = new File("/storage").listFiles();         // USB drives and SD cards
        if (st != null) for (File f : st) {
            String n = f.getName();
            if (n.equals("self") || n.equals("emulated") || !f.isDirectory()) continue;
            if (!r.contains(f)) r.add(f);
        }
        File mnt = new File("/mnt/media_rw"); File[] mm = mnt.listFiles();
        if (mm != null) for (File f : mm) if (f.isDirectory() && f.canRead() && !r.contains(f)) r.add(f);
        return r;
    }

    private String label(File f, boolean root) {
        if (root) {
            File internal = android.os.Environment.getExternalStorageDirectory();
            if (internal != null && f.equals(internal)) return "▸  Memoria interna";
            return "▸  Unità esterna / USB  (" + f.getName() + ")";
        }
        if (f == null) return "◂  ..  (cartella superiore)";
        if (f.isDirectory()) return "▸  " + f.getName();
        return "●  " + f.getName() + "   (" + (f.length() >> 20) + " MB)";
    }

    private void showBrowser(File d) {
        browsing = true; dir = d; entries.clear();
        List<String> labels = new ArrayList<>();
        if (d == null) {
            for (File f : roots()) { entries.add(f); labels.add(label(f, true)); }
        } else {
            entries.add(null); labels.add(label(null, false));
            File[] fs = d.listFiles();
            if (fs != null) {
                Arrays.sort(fs, new Comparator<File>() { @Override public int compare(File a, File b) {
                    if (a.isDirectory() != b.isDirectory()) return a.isDirectory() ? -1 : 1;
                    return a.getName().compareToIgnoreCase(b.getName());
                } });
                for (File f : fs) {
                    if (f.getName().startsWith(".")) continue;
                    if (f.isDirectory() || f.getName().toLowerCase().endsWith(".zip")) { entries.add(f); labels.add(label(f, false)); }
                }
            }
        }
        if (list == null) {
            LinearLayout l = new LinearLayout(this);
            l.setOrientation(LinearLayout.VERTICAL);
            l.setBackgroundColor(Color.BLACK);
            int pad = (int) (getResources().getDisplayMetrics().density * 32);
            l.setPadding(pad, pad, pad, pad);
            where = new TextView(this); where.setTextColor(Color.WHITE); where.setTextSize(20); where.setPadding(0, 0, 0, 16);
            list = new ListView(this);
            list.setOnItemClickListener(new AdapterView.OnItemClickListener() {
                @Override public void onItemClick(AdapterView<?> p, View v, int pos, long id) { choose(pos); }
            });
            l.addView(where, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
            l.addView(list, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
            browserView = l;
        }
        String head = d == null ? "Scegli dove cercare srally2.zip" : d.getAbsolutePath();
        if (d != null && d.listFiles() == null) head += "\n(cartella non leggibile: concedi l'accesso ai file nelle impostazioni dell'app)";
        else if (d == null && !hasFileAccess()) head += "\n(accesso ai file non concesso: alcune cartelle potrebbero risultare vuote)";
        where.setText(head);
        list.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, labels) {
            @Override public View getView(int pos, View cv, android.view.ViewGroup parent) {
                TextView t = (TextView) super.getView(pos, cv, parent);
                t.setTextColor(Color.WHITE); t.setTextSize(20);
                return t;
            }
        });
        setContentView(browserView);
        list.requestFocus();
        list.setSelection(0);
    }
    private View browserView;

    private void choose(int pos) {
        if (pos < 0 || pos >= entries.size()) return;
        File f = entries.get(pos);
        if (f == null) { File up = dir.getParentFile(); boolean root = false; for (File r : roots()) if (r.equals(dir)) root = true; showBrowser(root || up == null ? null : up); return; }
        if (f.isDirectory()) { showBrowser(f); return; }
        try { runBuild(new FileInputStream(f), null); } catch (Exception e) { showMain(); fail(e); }
    }

    private void showMain() {
        browsing = false;
        if (mainView != null) setContentView(mainView); else buildUi();
    }

    @Override public void onBackPressed() {
        if (browsing && !busy) {
            if (dir == null) { showMain(); (isTv ? browse : pick).requestFocus(); }
            else choose(0);
            return;
        }
        super.onBackPressed();
    }

    private void openPicker() {
        if (busy) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try { startActivityForResult(i, REQ_PICK); }
        catch (Exception e) { status.setText("Nessun selettore file disponibile.\n\n" + hint + "\n\nOppure copia srally2.zip in:\n" + base.getAbsolutePath()); }
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_PICK) return;
        if (res != RESULT_OK || data == null || data.getData() == null) { status.setText("Nessun file scelto (su alcune TV il selettore non esiste).\n\n" + hint + "\n\nOppure copia srally2.zip in:\n" + base.getAbsolutePath()); return; }
        Uri u = data.getData();
        try { runBuild(getContentResolver().openInputStream(u), null); } catch (Exception e) { fail(e); }
    }

    /** @param deleteOnBadRom a zip dropped into the app folder that turns out to be wrong is removed, so the picker comes back next time */
    private void runBuild(final InputStream in, final File deleteOnBadRom) {
        if (in == null) { fail(new java.io.IOException("file non leggibile")); return; }
        busy = true;
        showMain();
        pick.setEnabled(false); browse.setEnabled(false);
        status.setText("Preparazione dei dati di gioco...\n(una volta sola, può richiedere un minuto)");
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    RomBuilder.build(in, images, new RomBuilder.Progress() {
                        private long last = 0;
                        @Override public void onProgress(final String what, final int done, final int total) {
                            long now = System.currentTimeMillis();
                            if (now - last < 150) return;
                            last = now;
                            runOnUiThread(new Runnable() { @Override public void run() {
                                status.setText("Preparazione dei dati di gioco... " + (100 * done / Math.max(1, total)) + "%\n" + what);
                            } });
                        }
                    });
                    try { in.close(); } catch (Exception ignored) { }
                    runOnUiThread(new Runnable() { @Override public void run() { startGame(); } });
                } catch (final Exception e) {
                    try { in.close(); } catch (Exception ignored) { }
                    if (deleteOnBadRom != null && e instanceof RomBuilder.BadRomException) deleteOnBadRom.delete();
                    runOnUiThread(new Runnable() { @Override public void run() { fail(e); } });
                }
            }
        }, "rom-build").start();
    }

    private void fail(Exception e) {
        busy = false;
        if (pick != null) pick.setEnabled(true);
        if (browse != null) browse.setEnabled(true);
        String m = e instanceof java.util.zip.ZipException ? "Il file scelto non è uno zip valido." : String.valueOf(e.getMessage());
        if (status != null) status.setText("Errore: " + m + "\n\nCarica di nuovo srally2.zip.\n" + hint);
    }

    /** The render resolution is chosen in the game's settings menu ("Auto" adapts to the GPU). Older versions wrote
     *  "-ires 720" here on non-Snapdragon devices, which silently overrode that choice: such a generated file is reset. */
    private void writeDefaultArgs() {
        try {
            File f = new File(getExternalFilesDir(null), "args.txt");
            if (f.exists()) {
                byte[] b = new byte[(int) Math.min(f.length(), 4096)];
                FileInputStream in = new FileInputStream(f); int n = in.read(b); in.close();
                String t = n > 0 ? new String(b, 0, n, "UTF-8").trim() : "";
                if (!t.equals("-ires 720") && !t.startsWith("# -ires 720")) return;   // edited by the user: keep it
            }
            java.io.FileWriter w = new java.io.FileWriter(f);
            w.write("# extra options, one per line (the render resolution is in the in-game settings menu)\n");
            w.close();
        } catch (Exception ignored) { }
    }

    // ---- online leaderboard: finished races are queued by the game (leaderboard_pending.jsonl) and uploaded from here ----
    /** Server address. Empty = leaderboard off; a leaderboard_url.txt in the app folder overrides it. */
    static final String LB_URL = "";
    private static Thread uploader;

    static String leaderboardUrl(File dir) {
        String u = LB_URL;
        try {
            File f = new File(dir, "leaderboard_url.txt");
            if (f.isFile()) {
                java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f));
                String l = r.readLine(); r.close();
                if (l != null && l.trim().startsWith("http")) u = l.trim();
            }
        } catch (Exception ignored) { }
        while (u.endsWith("/")) u = u.substring(0, u.length() - 1);
        return u;
    }

    static String playerId(File dir) {
        File f = new File(dir, "player_id.txt");
        try {
            if (f.isFile()) {
                java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(f));
                String l = r.readLine(); r.close();
                if (l != null && l.trim().length() >= 16) return l.trim();
            }
            String id = java.util.UUID.randomUUID().toString();
            java.io.FileWriter w = new java.io.FileWriter(f); w.write(id + "\n"); w.close();
            return id;
        } catch (Exception e) { return "00000000-0000-0000-0000-000000000000"; }
    }

    private static void startUploader(final File dir) {
        playerId(dir);                       // the game reads it too (to mark your own rows on the leaderboard page)
        File uf = new File(dir, "leaderboard_url.txt");
        if (!uf.exists() && !LB_URL.isEmpty()) {   // the game reads the address from this file (in-game "Online leaderboard")
            try { java.io.FileWriter w = new java.io.FileWriter(uf); w.write(LB_URL + "\n"); w.close(); } catch (Exception ignored) { }
        }
        if (uploader != null) return;
        uploader = new Thread(new Runnable() { @Override public void run() {
            while (true) {
                try { uploadPending(dir); } catch (Throwable ignored) { }
                try { Thread.sleep(20000); } catch (InterruptedException e) { return; }
            }
        } }, "leaderboard-upload");
        uploader.setDaemon(true);
        uploader.start();
    }

    private static void uploadPending(File dir) throws Exception {
        String url = leaderboardUrl(dir);
        if (url.isEmpty()) return;
        File pending = new File(dir, "leaderboard_pending.jsonl"), sending = new File(dir, "leaderboard_sending.jsonl");
        if (!sending.exists()) {
            if (!pending.isFile() || pending.length() == 0) return;
            if (!pending.renameTo(sending)) return;
        }
        StringBuilder res = new StringBuilder(); int n = 0;
        java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader(sending));
        for (String l; (l = r.readLine()) != null && n < 50; ) { l = l.trim(); if (l.startsWith("{")) { if (n++ > 0) res.append(','); res.append(l); } }
        r.close();
        if (n == 0) { sending.delete(); return; }
        String body = "{\"player\":\"" + playerId(dir) + "\",\"app\":\"" + APP_VERSION + "\",\"results\":[" + res + "]}";
        java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(url + "/api/submit").openConnection();
        c.setConnectTimeout(10000); c.setReadTimeout(15000); c.setDoOutput(true); c.setRequestMethod("POST");
        c.setRequestProperty("Content-Type", "application/json");
        OutputStream o = c.getOutputStream(); o.write(body.getBytes("UTF-8")); o.close();
        int code = c.getResponseCode(); c.disconnect();
        if (code == 200 || code == 400) sending.delete();      // 400: malformed, do not retry forever
    }
    static final String APP_VERSION = "58";

    private void startGame() {
        try { if (server != null) server.close(); } catch (Exception ignored) { }
        startUploader(base);
        writeDefaultArgs();
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }
}
