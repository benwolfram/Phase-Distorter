package io.github.therunaway5.phasedistorter;

import android.graphics.Bitmap;

// Native companion picture for a second panel (see second_screen.hpp). All
// calls are safe before a session starts; they then do nothing.
final class SecondScreenBridge {
    static final int WIDTH = 310;
    static final int HEIGHT = 270;
    static final int TOUCH_DOWN = 0;
    static final int TOUCH_MOVE = 1;
    static final int TOUCH_UP = 2;
    static final int TOUCH_CANCEL = 3;

    private SecondScreenBridge() {}

    // Fills a WIDTH x HEIGHT ARGB_8888 bitmap; false when nothing changed.
    static native boolean render(Bitmap bitmap);

    // Logical canvas coordinates.
    static native void touch(int x, int y, int action);

    // While attached, the game moves its text windows to this panel.
    static native void setAttached(boolean attached);
}
