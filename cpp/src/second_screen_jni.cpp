// Java bridge for the bottom-screen companion. The panel's UI thread calls in
// here; every call goes through the registration hub, so it does nothing
// while no session (or a session without a second screen) is running.
#include "eb/second_screen.hpp"

#include <android/bitmap.h>
#include <jni.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

namespace {
using Canvas = std::array<std::uint32_t, eb::SecondScreen::width * eb::SecondScreen::height>;
} // namespace

extern "C" {
JNIEXPORT jboolean JNICALL Java_io_github_therunaway5_phasedistorter_SecondScreenBridge_render(JNIEnv *env, jclass,
                                                                                               jobject bitmap) {
    AndroidBitmapInfo info{};
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || int(info.width) != eb::SecondScreen::width ||
        int(info.height) != eb::SecondScreen::height)
        return JNI_FALSE;
    // Only the UI thread renders, so one canvas suffices.
    static const auto canvas = std::make_unique<Canvas>();
    bool changed = false;
    if (!eb::with_second_screen([&](eb::SecondScreen &screen) { changed = screen.render(*canvas); }) || !changed)
        return JNI_FALSE;
    void *pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS || !pixels) return JNI_FALSE;
    // 0xAARRGGBB words become RGBA bytes in memory.
    for (int y = 0; y < eb::SecondScreen::height; ++y) {
        auto *row = reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(pixels) + std::size_t(y) * info.stride);
        for (int x = 0; x < eb::SecondScreen::width; ++x) {
            const std::uint32_t pixel = (*canvas)[std::size_t(y) * eb::SecondScreen::width + std::size_t(x)];
            row[x] = (pixel & 0xff00ff00u) | (pixel >> 16 & 0xffu) | (pixel & 0xffu) << 16;
        }
    }
    AndroidBitmap_unlockPixels(env, bitmap);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_io_github_therunaway5_phasedistorter_SecondScreenBridge_touch(JNIEnv *, jclass, jint x,
                                                                                          jint y, jint action) {
    eb::with_second_screen([&](eb::SecondScreen &screen) { screen.touch(x, y, action); });
}

JNIEXPORT void JNICALL Java_io_github_therunaway5_phasedistorter_SecondScreenBridge_setAttached(JNIEnv *, jclass,
                                                                                                jboolean attached) {
    eb::set_second_screen_attached(attached == JNI_TRUE);
}
}
