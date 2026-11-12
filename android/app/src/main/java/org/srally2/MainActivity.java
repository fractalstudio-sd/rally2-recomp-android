package org.srally2;

import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import org.libsdl.app.SDLActivity;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

/**
 * SDL hosts the native game (libmain.so). The first argument is the folder with the game images built on the computer
 * (<code>push_images.sh</code> copies them to <code>Android/data/org.srally2/files/images</code>). Extra command line
 * options (for example <code>-ires 720</code> or <code>-43</code>) can be put in <code>files/args.txt</code>.
 */
public class MainActivity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override protected String[] getArguments() {
        File base = getExternalFilesDir(null);
        List<String> a = new ArrayList<>();
        a.add(new File(base, "images").getAbsolutePath());
        File extra = new File(base, "args.txt");
        if (extra.exists()) {
            try (BufferedReader r = new BufferedReader(new FileReader(extra))) {
                String line;
                while ((line = r.readLine()) != null)
                    for (String t : line.trim().split("\\s+")) if (!t.isEmpty() && !t.startsWith("#")) a.add(t);
            } catch (Exception ignored) { }
        }
        return a.toArray(new String[0]);
    }

    /** Called from native code after the settings menu saved new options: relaunch the whole app. */
    public void restartApp() {
        Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (i != null) {
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(i);
        }
        new Handler(Looper.getMainLooper()).postDelayed(() -> Process.killProcess(Process.myPid()), 300);
    }

    @Override protected void onCreate(Bundle savedInstanceState) {
        File base = getExternalFilesDir(null);   // make sure the folder exists so it can be filled over USB / adb
        if (base != null) new File(base, "images").mkdirs();
        super.onCreate(savedInstanceState);
    }
}
