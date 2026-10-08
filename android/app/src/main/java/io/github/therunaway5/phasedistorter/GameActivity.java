package io.github.therunaway5.phasedistorter;

import android.os.Bundle;
import android.view.Display;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

// Runs the native game (libmain.so) fullscreen. Any failure, including a bad
// ROM, ends SDL_main, which finishes this activity and closes the app.
public class GameActivity extends SDLActivity {
    static final String EXTRA_ROM = "rom";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        requestFastestRefresh();
    }

    @Override
    protected String[] getArguments() {
        // Handheld defaults: fill a 16:9 panel and present up to 120 Hz.
        List<String> arguments = new ArrayList<>(Arrays.asList(
            "--widescreen", "--aspect", "16:9", "--fps", "120"));
        String rom = getIntent().getStringExtra(EXTRA_ROM);
        if (rom != null) {
            arguments.add("--import-rom");
            arguments.add(rom);
        }
        return arguments.toArray(new String[0]);
    }

    @Override
    protected void onDestroy() {
        // SDL joins the game thread here, so the import is finished with the copy.
        super.onDestroy();
        String rom = getIntent().getStringExtra(EXTRA_ROM);
        if (rom != null)
            new File(rom).delete();
        // Native globals are not reinitialized within one process. Exit so
        // the next launch starts clean.
        System.exit(0);
    }

    // Prefer the panel's highest refresh rate at its current resolution, such
    // as 120 Hz on the AYN Thor, instead of the system's power-saving default.
    private void requestFastestRefresh() {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode best = current;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()
                    && mode.getRefreshRate() > best.getRefreshRate())
                best = mode;
        }
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(attributes);
    }
}
