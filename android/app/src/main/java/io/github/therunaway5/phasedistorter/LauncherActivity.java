package io.github.therunaway5.phasedistorter;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

// Starts the game directly when an imported pack exists. Otherwise the system
// file picker supplies a ROM, and cancelling it closes the app.
public class LauncherActivity extends Activity {
    private static final String TAG = "PhaseDistorter";
    private static final int PICK_ROM = 1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null)
            return; // The picker is already open.
        // The native game reads and writes packs in this same directory.
        File files = getExternalFilesDir(null);
        if (files != null && (new File(files, "earthbound.ebpak").exists()
                || new File(files, "mother2.ebpak").exists())) {
            startGame(null);
            return;
        }
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        // ROMs have no registered MIME type, so offer every file.
        pick.setType("*/*");
        startActivityForResult(pick, PICK_ROM);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_ROM)
            return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            finish();
            return;
        }
        // Content URIs are not file paths. Copy the ROM where native code can
        // read it; the game imports it into a pack and deletes this copy.
        // A zip archive supplies its first .smc or .sfc entry.
        File rom = new File(getCacheDir(), "import.rom");
        try {
            copy(data.getData(), rom);
        } catch (IOException | SecurityException error) {
            Log.e(TAG, "Cannot read the selected ROM", error);
            rom.delete();
            finish();
            return;
        }
        startGame(rom.getAbsolutePath());
    }

    private void copy(Uri source, File destination) throws IOException {
        InputStream opened = getContentResolver().openInputStream(source);
        if (opened == null)
            throw new IOException("No data for " + source);
        try (InputStream input = new BufferedInputStream(opened)) {
            if (isZip(input)) {
                ZipInputStream zip = new ZipInputStream(input);
                for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                    if (!entry.isDirectory() && isRomName(entry.getName())) {
                        write(zip, destination);
                        return;
                    }
                }
                throw new IOException("No .smc or .sfc file in " + source);
            }
            write(input, destination);
        }
    }

    private static boolean isZip(InputStream input) throws IOException {
        input.mark(4);
        byte[] magic = new byte[4];
        int count = input.read(magic);
        input.reset();
        return count == 4 && magic[0] == 'P' && magic[1] == 'K'
            && magic[2] == 3 && magic[3] == 4;
    }

    private static boolean isRomName(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".smc") || lower.endsWith(".sfc");
    }

    private static void write(InputStream input, File destination) throws IOException {
        try (OutputStream output = new FileOutputStream(destination)) {
            byte[] buffer = new byte[64 * 1024];
            for (int count; (count = input.read(buffer)) > 0;)
                output.write(buffer, 0, count);
        }
    }

    private void startGame(String rom) {
        Intent game = new Intent(this, GameActivity.class);
        if (rom != null)
            game.putExtra(GameActivity.EXTRA_ROM, rom);
        startActivity(game);
        finish();
        overridePendingTransition(0, 0);
    }
}
