#pragma once

#include "eb/game_version.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace eb {
// Companion picture for a handheld's second panel, such as the AYN Thor's
// bottom screen. Every glyph, frame and backdrop is decoded at runtime from
// the user's imported pack; nothing from the game ships with the frontend.
// The game thread publishes state and drains commands between frames; the
// panel's UI thread renders and forwards touches. One mutex guards both.
class SecondScreen {
public:
    static constexpr int width = 310, height = 270;
    static constexpr int thumbnail_width = 128, thumbnail_height = 112;
    static constexpr int backdrop_count = 8;

    struct Window { int left{}, top{}, right{}, bottom{}; };
    struct Snapshot {
        std::string id;
        std::int64_t created_at_unix{};
        // thumbnail_width x thumbnail_height, 0xAARRGGBB; empty when missing.
        std::vector<std::uint32_t> thumbnail;
    };
    struct Settings {
        bool windows_below = true, widescreen = false, crt_filter = false, reduce_flashing = false,
             interpolate_frames = true;
        int frame_limit = 60, backdrop = 0;
        bool operator==(const Settings &) const = default;
    };
    struct Command {
        enum class Kind { Save, Load, Delete, Settings } kind;
        std::string id;
        Settings settings;
    };
    enum class Tab { Game, States, Settings };

    // Mother 2 has no font pointer table at the US address; it stays single-screen.
    static bool supported(GameVersion version) { return version == GameVersion::US; }
    SecondScreen(std::span<const std::uint8_t> image, GameVersion version);

    // Game thread.
    void publish_windows(std::span<const std::uint32_t, 256 * 224> pixels, std::span<const Window> windows);
    void publish_party(std::span<const std::uint8_t> work_ram);
    void publish_snapshots(std::vector<Snapshot> snapshots);
    void publish_settings(const Settings &settings);
    void show_message(std::string message);
    std::vector<Command> take_commands();
    // Whether the Game page is up to show text windows; while another page
    // is open the windows stay on the main screen.
    bool showing_windows() const;

    // UI thread. Returns false when nothing changed since the previous call,
    // letting the host skip an upload; the backdrop animates at 30 Hz.
    bool render(std::span<std::uint32_t, width * height> pixels);
    // action: 0 down, 1 move, 2 up, 3 cancel. Coordinates are logical pixels.
    void touch(int x, int y, int action);
    void select_tab(Tab tab);

private:
    struct Font {
        std::vector<std::uint8_t> widths, glyphs;
        int bytes_per_glyph{}, height{};
    };
    struct Member {
        std::string name;
        int level{}, hp{}, max_hp{}, pp{}, max_pp{};
        bool unconscious{};
    };
    struct Backdrop {
        std::vector<std::uint8_t> indices; // 256x256 palette indices
        std::array<std::uint32_t, 16> palette{};
        int cycle_type{}, cycle1_begin{}, cycle1_end{}, cycle2_begin{}, cycle2_end{}, cycle_speed{};
    };
    struct Button { int x{}, y{}, w{}, h{}; int id{}; };
    using Canvas = std::span<std::uint32_t, width * height>;

    // Decoding.
    void decode_font(std::span<const std::uint8_t> image, int index);
    void decode_backdrop(std::span<const std::uint8_t> image, int slot, int layer);

    // Drawing primitives.
    std::uint32_t frame_color(int index) const;
    void draw_tile(Canvas canvas, int x, int y, unsigned entry, float shade = 1.f) const;
    void draw_window(Canvas canvas, int x, int y, int w, int h, bool pressed = false, float shade = 1.f) const;
    int text_width(int font, std::string_view text) const;
    void draw_text(Canvas canvas, int font, int x, int y, std::string_view text, std::uint32_t color) const;
    void draw_digits(Canvas canvas, int x, int y, int value, int digits) const;
    void draw_cursor(Canvas canvas, int x, int y) const;
    void draw_backdrop(Canvas canvas, double seconds) const;

    // Pages.
    void layout();
    void draw_game(Canvas canvas) const;
    void draw_states(Canvas canvas) const;
    void draw_settings(Canvas canvas) const;
    void draw_tabs(Canvas canvas) const;
    void activate(int id);
    std::string setting_value(int row) const;

    mutable std::mutex mutex_;
    std::array<Font, 4> fonts_;
    std::vector<std::uint8_t> window_tiles_;
    std::vector<std::uint16_t> flavour_palettes_;
    std::array<Backdrop, backdrop_count> backdrops_;

    std::vector<std::uint32_t> windows_pixels_ = std::vector<std::uint32_t>(256 * 224);
    std::vector<Window> windows_;
    std::vector<Member> party_;
    std::int64_t money_{};
    int flavour_{};
    std::vector<Snapshot> snapshots_;
    Settings settings_;
    std::vector<Command> commands_;

    Tab tab_ = Tab::Game;
    int page_{}, selected_ = -1, pressed_ = -1, confirm_delete_ = -1;
    std::vector<Button> buttons_;
    std::string message_;
    std::chrono::steady_clock::time_point message_until_{}, confirm_until_{}, last_render_{};
    const std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    bool dirty_ = true;
};

// The Android panel reaches the running session through this hub. Rendering
// holds the hub lock, so unregistering waits for any in-flight UI call.
class SecondScreenRegistration {
public:
    explicit SecondScreenRegistration(SecondScreen *screen);
    ~SecondScreenRegistration();
    SecondScreenRegistration(const SecondScreenRegistration &) = delete;
    SecondScreenRegistration &operator=(const SecondScreenRegistration &) = delete;
};
bool with_second_screen_impl(void (*call)(SecondScreen &, void *), void *context);
// Runs function on the registered screen, if any; returns whether it ran.
template <class F> bool with_second_screen(F &&function) {
    using Function = std::remove_reference_t<F>;
    return with_second_screen_impl(
        [](SecondScreen &screen, void *context) { (*static_cast<Function *>(context))(screen); },
        const_cast<void *>(static_cast<const void *>(&function)));
}
// Set by the host while a second panel is showing the companion picture.
void set_second_screen_attached(bool attached);
bool second_screen_attached();
} // namespace eb
