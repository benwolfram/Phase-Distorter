package io.github.therunaway5.phasedistorter;

import android.content.Context;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

// Runs the native game (libmain.so) fullscreen. Any failure, including a bad
// ROM, ends SDL_main, which finishes this activity and closes the app.
// A second panel, such as the AYN Thor's bottom screen, shows text windows,
// save states and settings while the activity is visible.
public class GameActivity extends SDLActivity {
    static final String EXTRA_ROM = "rom";
    private static final String TAG = "PhaseDistorter";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private DisplayManager displayManager;
    private SecondScreenPresentation secondScreen;
    private boolean visible;

    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override
        public void onDisplayAdded(int displayId) { showSecondScreenIfPresent(); }

        @Override
        public void onDisplayRemoved(int displayId) {
            if (secondScreen != null && secondScreen.getDisplay().getDisplayId() == displayId)
                dismissSecondScreen();
        }

        @Override
        public void onDisplayChanged(int displayId) {}
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        requestFastestRefresh();
        displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        displayManager.registerDisplayListener(displayListener, handler);
    }

    @Override
    protected void onStart() {
        super.onStart();
        visible = true;
        showSecondScreenIfPresent();
    }

    @Override
    protected void onStop() {
        visible = false;
        dismissSecondScreen();
        super.onStop();
    }

    @Override
    protected String[] getArguments() {
        // Widescreen and 120 Hz are native Android defaults, so changes made
        // from the second screen persist across launches.
        List<String> arguments = new ArrayList<>();
        String rom = getIntent().getStringExtra(EXTRA_ROM);
        if (rom != null) {
            arguments.add("--import-rom");
            arguments.add(rom);
        }
        return arguments.toArray(new String[0]);
    }

    private void showSecondScreenIfPresent() {
        if (!visible || secondScreen != null || displayManager == null) return;
        int gameDisplay = getWindowManager().getDefaultDisplay().getDisplayId();
        Display target = null;
        for (Display display : displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (companionPanel(display, gameDisplay)) {
                target = display;
                break;
            }
        }
        if (target == null) {
            for (Display display : displayManager.getDisplays()) {
                if (companionPanel(display, gameDisplay)) {
                    target = display;
                    break;
                }
            }
        }
        if (target == null) return;
        try {
            SecondScreenPresentation presentation = new SecondScreenPresentation(this, target);
            presentation.setOnDismissListener(dialog -> {
                if (secondScreen != presentation) return;
                secondScreen = null;
                detachSecondScreen();
                // The system can dismiss a presentation on its own, for
                // example when the panel reconfigures; bring it back.
                handler.post(this::showSecondScreenIfPresent);
            });
            presentation.show();
            secondScreen = presentation;
            SecondScreenBridge.setAttached(true);
        } catch (Throwable error) {
            // Without the panel the game keeps its windows on the main screen.
            Log.w(TAG, "Second screen unavailable", error);
            secondScreen = null;
            detachSecondScreen();
        }
    }

    // Only a panel built into the handheld takes the windows. A TV over HDMI,
    // a cast or a screen recorder's display is out of reach of touch, so the
    // game stays single-screen with its windows on top.
    private static boolean companionPanel(Display display, int gameDisplay) {
        if (display.getDisplayId() == gameDisplay || display.getDisplayId() == Display.DEFAULT_DISPLAY) return false;
        if ((display.getFlags() & Display.FLAG_PRIVATE) != 0) return false;
        try {
            // Display.getType() is hidden; 1 is Display.TYPE_INTERNAL.
            Object type = Display.class.getMethod("getType").invoke(display);
            if (type instanceof Integer) return (Integer) type == 1;
        } catch (Throwable error) {
            Log.w(TAG, "Display type unavailable", error);
        }
        // Without the type, a presentation-only display is an external one.
        return (display.getFlags() & Display.FLAG_PRESENTATION) == 0;
    }

    private void dismissSecondScreen() {
        SecondScreenPresentation presentation = secondScreen;
        secondScreen = null;
        detachSecondScreen();
        if (presentation != null) {
            try {
                presentation.dismiss();
            } catch (Throwable error) {
                Log.w(TAG, "Second screen dismiss failed", error);
            }
        }
    }

    private static void detachSecondScreen() {
        try {
            SecondScreenBridge.setAttached(false);
        } catch (Throwable ignored) {
            // libmain failed to load; SDL reports that separately.
        }
    }

    @Override
    protected void onDestroy() {
        if (displayManager != null) displayManager.unregisterDisplayListener(displayListener);
        dismissSecondScreen();
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
