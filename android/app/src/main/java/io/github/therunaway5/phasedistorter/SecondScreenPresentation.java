package io.github.therunaway5.phasedistorter;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.Choreographer;
import android.view.Display;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

// Shows the native companion picture on a second panel, such as the AYN
// Thor's bottom screen, scaled by a whole number so pixels stay square.
final class SecondScreenPresentation extends Presentation {
    SecondScreenPresentation(Context context, Display display) {
        super(context, display);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Controllers keep driving the game on the main panel; this one only
        // takes touches.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setCancelable(false);
        setContentView(new CanvasView(getContext()));
        hideSystemBars();
    }

    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        View decor = getWindow().getDecorView();
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = decor.getWindowInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    private static final class CanvasView extends View implements Choreographer.FrameCallback {
        private final Bitmap bitmap = Bitmap.createBitmap(
            SecondScreenBridge.WIDTH, SecondScreenBridge.HEIGHT, Bitmap.Config.ARGB_8888);
        private final Paint paint = new Paint();
        private final Rect target = new Rect();
        private int scale = 1;
        private boolean running;

        CanvasView(Context context) {
            super(context);
            paint.setFilterBitmap(false);
            paint.setAntiAlias(false);
        }

        @Override
        protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            running = true;
            Choreographer.getInstance().postFrameCallback(this);
        }

        @Override
        protected void onDetachedFromWindow() {
            running = false;
            Choreographer.getInstance().removeFrameCallback(this);
            super.onDetachedFromWindow();
        }

        @Override
        public void doFrame(long frameTimeNanos) {
            if (!running) return;
            if (SecondScreenBridge.render(bitmap)) invalidate();
            Choreographer.getInstance().postFrameCallback(this);
        }

        @Override
        protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
            super.onSizeChanged(width, height, oldWidth, oldHeight);
            scale = Math.max(1, Math.min(width / SecondScreenBridge.WIDTH,
                                         height / SecondScreenBridge.HEIGHT));
            int drawnWidth = SecondScreenBridge.WIDTH * scale;
            int drawnHeight = SecondScreenBridge.HEIGHT * scale;
            int left = (width - drawnWidth) / 2;
            int top = (height - drawnHeight) / 2;
            target.set(left, top, left + drawnWidth, top + drawnHeight);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            canvas.drawColor(Color.BLACK);
            canvas.drawBitmap(bitmap, null, target, paint);
        }

        @Override
        public boolean onTouchEvent(MotionEvent event) {
            int action;
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: action = SecondScreenBridge.TOUCH_DOWN; break;
                case MotionEvent.ACTION_MOVE: action = SecondScreenBridge.TOUCH_MOVE; break;
                case MotionEvent.ACTION_UP: action = SecondScreenBridge.TOUCH_UP; break;
                case MotionEvent.ACTION_CANCEL: action = SecondScreenBridge.TOUCH_CANCEL; break;
                default: return true;
            }
            int x = (int) Math.floor((event.getX() - target.left) / scale);
            int y = (int) Math.floor((event.getY() - target.top) / scale);
            SecondScreenBridge.touch(x, y, action);
            if (action == SecondScreenBridge.TOUCH_UP) performClick();
            return true;
        }

        @Override
        public boolean performClick() {
            return super.performClick();
        }
    }
}
