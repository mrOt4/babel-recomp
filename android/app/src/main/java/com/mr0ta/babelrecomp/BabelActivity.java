package com.mr0ta.babelrecomp;

import android.content.res.AssetManager;
import android.os.Bundle;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import org.libsdl.app.SDLActivity;

/*
 * The game reads its data "beside the executable": on Android that is the
 * app's external files folder (Android/data/com.mr0ta.babelrecomp/files/),
 * where the player also puts rom.sfc. The files bundled in the APK (launcher
 * fonts and images, i18n/, the mod catalog) are copied there once per
 * version. Nothing the player added is touched.
 */
public class BabelActivity extends SDLActivity {
    private static final String TAG = "babel-recomp";
    private static final String[] BUNDLED = { "assets", "i18n", "mods" };

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };   /* SDL3 is linked into libmain.so */
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        installBundledFiles();
        super.onCreate(savedInstanceState);
    }

    private void installBundledFiles() {
        File root = getExternalFilesDir(null);
        if (root == null) return;
        String version;
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName
                    + "-" + getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
        } catch (Exception e) {
            version = "unknown";
        }
        File marker = new File(root, ".bundled-version");
        if (marker.exists() && version.equals(readFile(marker))) return;
        AssetManager am = getAssets();
        for (String dir : BUNDLED) copyTree(am, dir, new File(root, dir));
        writeFile(marker, version);
    }

    private static void copyTree(AssetManager am, String path, File dst) {
        try {
            String[] children = am.list(path);
            if (children == null || children.length == 0) {
                copyFile(am, path, dst);
                return;
            }
            dst.mkdirs();
            for (String child : children)
                copyTree(am, path + "/" + child, new File(dst, child));
        } catch (IOException e) {
            Log.w(TAG, "copy " + path + ": " + e);
        }
    }

    private static void copyFile(AssetManager am, String path, File dst) throws IOException {
        /* The player's own mod selection is kept across updates. */
        if (dst.getName().equals("state.toml") && dst.exists()) return;
        File parent = dst.getParentFile();
        if (parent != null) parent.mkdirs();
        try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(dst)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    private static String readFile(File f) {
        try (InputStream in = new java.io.FileInputStream(f)) {
            byte[] b = new byte[256];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n) : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeFile(File f, String s) {
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(s.getBytes());
        } catch (IOException e) {
            Log.w(TAG, "write " + f + ": " + e);
        }
    }
}
