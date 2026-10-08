#include "eb/second_screen.hpp"
#include "eb/native/content_compression.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace eb {
namespace {
// US addresses; offsets index the headerless address image (addr & 0x3fffff).
constexpr std::size_t font_table = 0x03F054;
constexpr std::size_t window_graphics = 0x200000;
constexpr std::size_t flavour_palettes = 0x201FC8;
constexpr std::size_t battle_graphics = 0x0AD7A1, battle_arrangements = 0x0AD93D,
                      battle_palettes = 0x0ADAD9, battle_layers = 0x0ADCA1;
constexpr int flavour_count = 7, highlight_flavour = 3;
// Battle background layers chosen for calm, tileable patterns.
constexpr std::array<int, SecondScreen::backdrop_count> backdrop_layers{22, 43, 23, 40, 53, 45, 35, 57};

// WRAM, US.
constexpr std::size_t party_members = 39023, party_count = 39075, character_table = 39374,
                      character_size = 95, money = 0x9831, text_flavour = 0x99CD;

// Window tiles: 0x10 corner, 0x11 horizontal edge, 0x12 vertical edge,
// 0x03 solid interior, 0x20/0x30 rolling-digit tops/bottoms.
constexpr unsigned corner = 0x10, edge_h = 0x11, edge_v = 0x12, interior = 0x03, flip_h = 0x4000, flip_v = 0x8000;

constexpr int area_x = 27, area_y = 6;  // native 256x224 window area
constexpr int tab_y = 236, tab_w = 96, tab_h = 32;
constexpr std::array<int, 3> tab_x{7, 107, 207};
constexpr int card_w = 96, card_h = 96, cards_per_page = 6;
constexpr std::array<const char *, 7> setting_labels{
    "Text windows", "Widescreen", "Frame rate", "Smooth motion", "CRT filter", "Reduce flashing", "Backdrop"};

enum : int {
    tab_button = 100, save_button = 200, load_button = 201, delete_button = 202, page_button = 203,
    card_button = 300, setting_button = 400
};

std::uint32_t read_long(std::span<const std::uint8_t> image, std::size_t offset) {
    if (offset + 4 > image.size()) throw std::runtime_error("Second screen asset pointer out of range");
    return image[offset] | image[offset + 1] << 8 | image[offset + 2] << 16 | std::uint32_t(image[offset + 3]) << 24;
}

std::span<const std::uint8_t> slice(std::span<const std::uint8_t> image, std::size_t offset, std::size_t size) {
    if (offset > image.size() || size > image.size() - offset)
        throw std::runtime_error("Second screen asset out of range");
    return image.subspan(offset, size);
}

std::uint32_t argb(std::uint16_t bgr555) {
    const auto channel = [&](unsigned shift) { return ((bgr555 >> shift) & 31u) * 255 / 31; };
    return 0xff000000u | channel(0) << 16 | channel(5) << 8 | channel(10);
}

std::uint32_t shade(std::uint32_t color, float amount) {
    if (amount >= 1.f) return color;
    const auto channel = [&](unsigned shift) { return std::uint32_t(float((color >> shift) & 255) * amount) << shift; };
    return 0xff000000u | channel(16) | channel(8) | channel(0);
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, float t) {
    const auto channel = [&](unsigned shift) {
        const float x = float((a >> shift) & 255), y = float((b >> shift) & 255);
        return std::uint32_t(x + (y - x) * t) << shift;
    };
    return 0xff000000u | channel(16) | channel(8) | channel(0);
}

std::string format_time(std::int64_t unix, const char *format) {
    const std::time_t time = std::time_t(unix);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[32]{};
    std::strftime(text, sizeof text, format, &local);
    return text;
}

std::mutex hub_mutex;
SecondScreen *hub_screen{};
std::atomic<bool> hub_attached{};
} // namespace

SecondScreen::SecondScreen(std::span<const std::uint8_t> image, GameVersion version) {
    if (!supported(version)) throw std::runtime_error("The second screen supports EarthBound (US) only");
    for (int index = 0; index < int(fonts_.size()); ++index) decode_font(image, index);
    window_tiles_ = native::decompress_content(image, window_graphics, 0x4000);
    const auto palettes = slice(image, flavour_palettes, flavour_count * 64);
    for (std::size_t index = 0; index < palettes.size(); index += 2)
        flavour_palettes_.push_back(std::uint16_t(palettes[index] | palettes[index + 1] << 8));
    for (int slot = 0; slot < backdrop_count; ++slot) {
        try {
            decode_backdrop(image, slot, backdrop_layers[slot]);
        } catch (const std::exception &) {
            backdrops_[slot] = {}; // A plain flavour fill stands in.
        }
    }
    layout();
}

void SecondScreen::decode_font(std::span<const std::uint8_t> image, int index) {
    const auto entry = slice(image, font_table + std::size_t(index) * 12, 12);
    auto &font = fonts_[index];
    font.bytes_per_glyph = entry[8] | entry[9] << 8;
    font.height = entry[10] | entry[11] << 8;
    if (font.height <= 0 || font.bytes_per_glyph < font.height || font.bytes_per_glyph % font.height)
        throw std::runtime_error("Unexpected font layout");
    const auto widths = slice(image, read_long(entry, 0) & 0x3fffff, 96);
    const auto glyphs = slice(image, read_long(entry, 4) & 0x3fffff, std::size_t(96) * font.bytes_per_glyph);
    font.widths.assign(widths.begin(), widths.end());
    font.glyphs.assign(glyphs.begin(), glyphs.end());
}

void SecondScreen::decode_backdrop(std::span<const std::uint8_t> image, int slot, int layer) {
    const auto entry = slice(image, battle_layers + std::size_t(layer) * 17, 17);
    const int bpp = entry[2];
    if (bpp != 2 && bpp != 4) throw std::runtime_error("Unexpected battle background depth");
    const auto graphics = native::decompress_content(
        image, read_long(image, battle_graphics + std::size_t(entry[0]) * 4) & 0x3fffff, 0x10000);
    const auto arrangement = native::decompress_content(
        image, read_long(image, battle_arrangements + std::size_t(entry[0]) * 4) & 0x3fffff, 0x1000);
    if (arrangement.size() < 32 * 32 * 2) throw std::runtime_error("Short battle background arrangement");
    const auto palette = slice(image, read_long(image, battle_palettes + std::size_t(entry[1]) * 4) & 0x3fffff, 32);
    auto &backdrop = backdrops_[slot];
    for (int index = 0; index < 16; ++index)
        backdrop.palette[index] = argb(std::uint16_t(palette[index * 2] | palette[index * 2 + 1] << 8));
    backdrop.cycle_type = entry[3];
    backdrop.cycle1_begin = entry[4], backdrop.cycle1_end = entry[5];
    backdrop.cycle2_begin = entry[6], backdrop.cycle2_end = entry[7];
    backdrop.cycle_speed = entry[8];
    backdrop.indices.assign(256 * 256, 0);
    const std::size_t tile_bytes = std::size_t(bpp) * 8;
    for (int ty = 0; ty < 32; ++ty)
        for (int tx = 0; tx < 32; ++tx) {
            const std::size_t at = std::size_t(ty * 32 + tx) * 2;
            const unsigned map = arrangement[at] | arrangement[at + 1] << 8;
            const std::size_t base = (map & 0x3ff) * tile_bytes;
            for (int row = 0; row < 8; ++row)
                for (int column = 0; column < 8; ++column) {
                    const int r = map & flip_v ? 7 - row : row, c = map & flip_h ? 7 - column : column;
                    unsigned value = 0;
                    for (int plane = 0; plane < bpp; ++plane) {
                        const std::size_t byte = base + std::size_t(plane / 2) * 16 + std::size_t(r) * 2 + (plane & 1);
                        if (byte < graphics.size()) value |= ((graphics[byte] >> (7 - c)) & 1u) << plane;
                    }
                    backdrop.indices[std::size_t(ty * 8 + row) * 256 + std::size_t(tx * 8 + column)] =
                        std::uint8_t(value);
                }
        }
}

// ---- Game thread --------------------------------------------------------

void SecondScreen::publish_windows(std::span<const std::uint32_t, 256 * 224> pixels,
                                   std::span<const Window> windows) {
    std::lock_guard lock(mutex_);
    std::copy(pixels.begin(), pixels.end(), windows_pixels_.begin());
    windows_.assign(windows.begin(), windows.end());
}

void SecondScreen::publish_party(std::span<const std::uint8_t> ram) {
    if (ram.size() < character_table + character_size * 4) return;
    const auto word = [&](std::size_t at) { return int(ram[at] | ram[at + 1] << 8); };
    std::vector<Member> party;
    const int count = std::min<int>(ram[party_count], 6);
    for (int index = 0; index < count && party.size() < 4; ++index) {
        const int id = ram[party_members + std::size_t(index)];
        if (id < 1 || id > 4) continue; // guests have no character record
        const std::size_t base = character_table + std::size_t(id - 1) * character_size;
        Member member;
        for (std::size_t letter = 0; letter < 5 && ram[base + letter]; ++letter) {
            const int ascii = ram[base + letter] - 0x30;
            member.name.push_back(ascii >= 0x20 && ascii < 0x80 ? char(ascii) : '?');
        }
        member.level = ram[base + 5];
        member.max_hp = word(base + 10), member.max_pp = word(base + 12);
        member.hp = word(base + 69), member.pp = word(base + 75);
        member.unconscious = ram[base + 14] == 1;
        party.push_back(std::move(member));
    }
    const std::int64_t cash = ram[money] | ram[money + 1] << 8 | ram[money + 2] << 16 | std::int64_t(ram[money + 3]) << 24;
    std::lock_guard lock(mutex_);
    party_ = std::move(party);
    money_ = cash;
    flavour_ = std::clamp(int(ram[text_flavour]) - 1, 0, flavour_count - 1);
}

bool SecondScreen::showing_windows() const {
    std::lock_guard lock(mutex_);
    return tab_ == Tab::Game;
}

void SecondScreen::publish_snapshots(std::vector<Snapshot> snapshots) {
    std::ranges::sort(snapshots, [](const Snapshot &a, const Snapshot &b) { return a.created_at_unix > b.created_at_unix; });
    std::lock_guard lock(mutex_);
    std::string selected_id = selected_ >= 0 && selected_ < int(snapshots_.size()) ? snapshots_[selected_].id : "";
    snapshots_ = std::move(snapshots);
    selected_ = -1;
    for (int index = 0; index < int(snapshots_.size()); ++index)
        if (snapshots_[index].id == selected_id) selected_ = index;
    const int pages = std::max(1, (int(snapshots_.size()) + cards_per_page - 1) / cards_per_page);
    page_ = std::min(page_, pages - 1);
    confirm_delete_ = -1;
    layout();
    dirty_ = true;
}

void SecondScreen::publish_settings(const Settings &settings) {
    std::lock_guard lock(mutex_);
    if (settings_ == settings) return;
    settings_ = settings;
    dirty_ = true;
}

void SecondScreen::show_message(std::string message) {
    std::lock_guard lock(mutex_);
    message_ = std::move(message);
    message_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    dirty_ = true;
}

std::vector<SecondScreen::Command> SecondScreen::take_commands() {
    std::lock_guard lock(mutex_);
    return std::exchange(commands_, {});
}

// ---- Input ---------------------------------------------------------------

void SecondScreen::select_tab(Tab tab) {
    std::lock_guard lock(mutex_);
    tab_ = tab;
    pressed_ = -1;
    layout();
    dirty_ = true;
}

void SecondScreen::touch(int x, int y, int action) {
    std::lock_guard lock(mutex_);
    int hit = -1;
    for (const auto &button : buttons_)
        if (x >= button.x && x < button.x + button.w && y >= button.y && y < button.y + button.h) hit = button.id;
    const int before = pressed_;
    if (action == 0)
        pressed_ = hit;
    else if (action == 1) {
        if (hit != pressed_) pressed_ = -1;
    } else {
        const int target = pressed_;
        pressed_ = -1;
        if (action == 2 && target >= 0 && target == hit) activate(target);
    }
    if (pressed_ != before) dirty_ = true;
}

void SecondScreen::activate(int id) {
    dirty_ = true;
    const auto now = std::chrono::steady_clock::now();
    if (id >= tab_button && id < tab_button + 3) {
        tab_ = Tab(id - tab_button);
    } else if (id == save_button) {
        commands_.push_back({Command::Kind::Save, {}, {}});
    } else if (id == load_button) {
        if (selected_ >= 0 && selected_ < int(snapshots_.size()))
            commands_.push_back({Command::Kind::Load, snapshots_[selected_].id, {}});
    } else if (id == delete_button) {
        if (selected_ < 0 || selected_ >= int(snapshots_.size())) return;
        if (confirm_delete_ == selected_ && now < confirm_until_) {
            commands_.push_back({Command::Kind::Delete, snapshots_[selected_].id, {}});
            confirm_delete_ = -1;
        } else {
            confirm_delete_ = selected_;
            confirm_until_ = now + std::chrono::seconds(3);
        }
    } else if (id == page_button) {
        const int pages = std::max(1, (int(snapshots_.size()) + cards_per_page - 1) / cards_per_page);
        page_ = (page_ + 1) % pages;
    } else if (id >= card_button && id < card_button + cards_per_page) {
        const int index = page_ * cards_per_page + (id - card_button);
        if (index < int(snapshots_.size())) selected_ = index;
        confirm_delete_ = -1;
    } else if (id >= setting_button && id < setting_button + int(setting_labels.size())) {
        auto &s = settings_;
        switch (id - setting_button) {
        case 0: s.windows_below = !s.windows_below; break;
        case 1: s.widescreen = !s.widescreen; break;
        case 2: s.frame_limit = s.frame_limit == 60 ? 90 : s.frame_limit == 90 ? 120 : 60; break;
        case 3: s.interpolate_frames = !s.interpolate_frames; break;
        case 4: s.crt_filter = !s.crt_filter; break;
        case 5: s.reduce_flashing = !s.reduce_flashing; break;
        case 6: s.backdrop = (s.backdrop + 1) % backdrop_count; break;
        }
        commands_.push_back({Command::Kind::Settings, {}, s});
    }
    layout();
}

void SecondScreen::layout() {
    buttons_.clear();
    for (int index = 0; index < 3; ++index) buttons_.push_back({tab_x[index], tab_y, tab_w, tab_h, tab_button + index});
    if (tab_ == Tab::States) {
        buttons_.push_back({6, 4, 72, 32, save_button});
        buttons_.push_back({82, 4, 72, 32, load_button});
        buttons_.push_back({158, 4, 80, 32, delete_button});
        buttons_.push_back({242, 4, 64, 32, page_button});
        for (int slot = 0; slot < cards_per_page; ++slot)
            buttons_.push_back({7 + (slot % 3) * 101, 40 + (slot / 3) * 98, card_w, card_h, card_button + slot});
    } else if (tab_ == Tab::Settings) {
        for (int row = 0; row < int(setting_labels.size()); ++row)
            buttons_.push_back({14, 13 + row * 30, 282, 28, setting_button + row});
    }
}

// ---- Drawing -------------------------------------------------------------

std::uint32_t SecondScreen::frame_color(int index) const {
    return argb(flavour_palettes_[std::size_t(flavour_) * 32 + 28 + std::size_t(index)]);
}

void SecondScreen::draw_tile(Canvas canvas, int x, int y, unsigned entry, float amount) const {
    const std::size_t base = (entry & 0x3ff) * 16;
    if (base + 16 > window_tiles_.size()) return;
    // Bit 13 selects the highlight flavour; the game's own entries never set it here.
    const std::size_t flavour = entry & 0x2000 ? std::size_t(flavour_ == highlight_flavour ? 1 : highlight_flavour)
                                                : std::size_t(flavour_);
    for (int row = 0; row < 8; ++row) {
        const int py = y + row;
        if (py < 0 || py >= height) continue;
        const int r = entry & flip_v ? 7 - row : row;
        const unsigned low = window_tiles_[base + std::size_t(r) * 2], high = window_tiles_[base + std::size_t(r) * 2 + 1];
        for (int column = 0; column < 8; ++column) {
            const int px = x + column;
            if (px < 0 || px >= width) continue;
            const int bit = entry & flip_h ? column : 7 - column;
            const unsigned value = ((low >> bit) & 1) | ((high >> bit) & 1) << 1;
            if (!value) continue;
            canvas[std::size_t(py) * width + std::size_t(px)] =
                shade(argb(flavour_palettes_[flavour * 32 + 28 + value]), amount);
        }
    }
}

void SecondScreen::draw_window(Canvas canvas, int x, int y, int w, int h, bool highlighted, float amount) const {
    const unsigned extra = highlighted ? 0x2000 : 0;
    const int columns = w / 8, rows = h / 8;
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column) {
            const bool left = column == 0, right = column == columns - 1, top = row == 0, bottom = row == rows - 1;
            unsigned tile = interior;
            if ((left || right) && (top || bottom)) tile = corner;
            else if (top || bottom) tile = edge_h;
            else if (left || right) tile = edge_v;
            if (tile != interior) tile |= (right ? flip_h : 0) | (bottom ? flip_v : 0);
            draw_tile(canvas, x + column * 8, y + row * 8, tile | extra, amount);
        }
}

int SecondScreen::text_width(int font_index, std::string_view text) const {
    const auto &font = fonts_[font_index];
    int total = 0;
    for (const char character : text) {
        const int glyph = int(static_cast<unsigned char>(character)) - 0x20;
        if (glyph >= 0 && glyph < 96) total += font.widths[glyph] + 1;
    }
    return total;
}

void SecondScreen::draw_text(Canvas canvas, int font_index, int x, int y, std::string_view text,
                             std::uint32_t color) const {
    const auto &font = fonts_[font_index];
    const int columns = font.bytes_per_glyph / font.height;
    for (const char character : text) {
        const int glyph = int(static_cast<unsigned char>(character)) - 0x20;
        if (glyph < 0 || glyph >= 96) continue;
        const std::size_t base = std::size_t(glyph) * font.bytes_per_glyph;
        for (int column = 0; column < columns; ++column)
            for (int row = 0; row < font.height; ++row) {
                const unsigned byte = font.glyphs[base + std::size_t(column * font.height + row)];
                for (int bit = 0; bit < 8; ++bit) {
                    if (byte & (0x80u >> bit)) continue; // set bits are paper
                    const int px = x + column * 8 + bit, py = y + row;
                    if (px >= 0 && px < width && py >= 0 && py < height)
                        canvas[std::size_t(py) * width + std::size_t(px)] = color;
                }
            }
        x += font.widths[glyph] + 1;
    }
}

void SecondScreen::draw_digits(Canvas canvas, int x, int y, int value, int digits) const {
    // The rolling HP/PP meter digits: a top tile over an underlined bottom tile.
    value = std::clamp(value, 0, 999);
    for (int place = digits - 1; place >= 0; --place) {
        const int digit = value % 10;
        const int px = x + place * 8;
        if (value || place == digits - 1) {
            draw_tile(canvas, px, y, 0x20u + unsigned(digit));
            draw_tile(canvas, px, y + 8, 0x30u + unsigned(digit));
        }
        value /= 10;
    }
}

void SecondScreen::draw_cursor(Canvas canvas, int x, int y) const {
    // A small pointing triangle in the frame's highlight colour.
    const auto color = frame_color(1);
    for (int row = 0; row < 7; ++row)
        for (int column = 0; column <= 3 - std::abs(row - 3); ++column) {
            const int px = x + column, py = y + row;
            if (px >= 0 && px < width && py >= 0 && py < height) canvas[std::size_t(py) * width + std::size_t(px)] = color;
        }
}

void SecondScreen::draw_backdrop(Canvas canvas, double seconds) const {
    const auto &backdrop = backdrops_[std::size_t(std::clamp(settings_.backdrop, 0, backdrop_count - 1))];
    if (backdrop.indices.empty()) {
        std::fill(canvas.begin(), canvas.end(), shade(frame_color(3), 0.8f));
        return;
    }
    // Palette cycling as the battle background table describes it, at 60 Hz ticks.
    auto palette = backdrop.palette;
    if (backdrop.cycle_speed && backdrop.cycle_type) {
        const long step = long(seconds * 60.0) / backdrop.cycle_speed;
        const auto rotate = [&](int begin, int end) {
            if (begin < 0 || end > 15 || begin >= end) return;
            const int span = end - begin + 1;
            for (int index = begin; index <= end; ++index)
                palette[index] = backdrop.palette[begin + int((index - begin + step) % span)];
        };
        if (backdrop.cycle_type == 1) rotate(backdrop.cycle1_begin, backdrop.cycle1_end);
        else {
            rotate(backdrop.cycle1_begin, backdrop.cycle1_end);
            rotate(backdrop.cycle2_begin, backdrop.cycle2_end);
        }
    }
    for (auto &color : palette) color = shade(color, 0.8f);
    const int scroll_x = int(seconds * 6.0), scroll_y = int(seconds * 4.0);
    for (int y = 0; y < height; ++y) {
        const auto *row = &backdrop.indices[std::size_t((y + scroll_y) & 255) * 256];
        auto *out = &canvas[std::size_t(y) * width];
        for (int x = 0; x < width; ++x) out[x] = palette[row[(x + scroll_x) & 255] & 15];
    }
}

bool SecondScreen::render(std::span<std::uint32_t, width * height> canvas) {
    std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!dirty_ && now - last_render_ < std::chrono::milliseconds(33)) return false;
    dirty_ = false;
    last_render_ = now;
    if (confirm_delete_ >= 0 && now >= confirm_until_) confirm_delete_ = -1;
    draw_backdrop(canvas, std::chrono::duration<double>(now - start_).count());
    switch (tab_) {
    case Tab::Game: draw_game(canvas); break;
    case Tab::States: draw_states(canvas); break;
    case Tab::Settings: draw_settings(canvas); break;
    }
    draw_tabs(canvas);
    if (now < message_until_ && !message_.empty()) {
        const int w = std::min(296, (text_width(0, message_) + 31) / 8 * 8);
        const int x = (width - w) / 2, y = 104;
        draw_window(canvas, x, y, w, 32, true);
        draw_text(canvas, 0, x + (w - text_width(0, message_)) / 2, y + 8, message_, frame_color(1));
    }
    return true;
}

void SecondScreen::draw_game(Canvas canvas) const {
    const auto overlaps = [&](int x, int y, int w, int h) {
        for (const auto &window : windows_)
            if (window.left + area_x < x + w && window.right + area_x > x && window.top + area_y < y + h &&
                window.bottom + area_y > y)
                return true;
        return false;
    };
    // Party status in the style of the battle HP/PP boxes, under any windows.
    const int count = int(party_.size());
    const int cards_w = count * 72 + std::max(0, count - 1) * 4, card_y = 230 - 56;
    if (count && !overlaps((width - cards_w) / 2, card_y, cards_w, 56)) {
        for (int index = 0; index < count; ++index) {
            const auto &member = party_[index];
            const int x = (width - cards_w) / 2 + index * 76;
            const float amount = member.unconscious ? 0.55f : 1.f;
            draw_window(canvas, x, card_y, 72, 56, false, amount);
            draw_text(canvas, 0, x + (72 - text_width(0, member.name)) / 2, card_y + 4, member.name,
                      shade(frame_color(1), amount));
            draw_text(canvas, 3, x + 9, card_y + 25, "HP", shade(frame_color(2), amount));
            draw_digits(canvas, x + 32, card_y + 20, member.hp, 3);
            if (member.max_pp) {
                draw_text(canvas, 3, x + 9, card_y + 41, "PP", shade(frame_color(2), amount));
                draw_digits(canvas, x + 32, card_y + 36, member.pp, 3);
            }
        }
        // Money sits above the party while the field is clear; open menus
        // bring the game's own money window.
        const std::string cash = "$" + std::to_string(money_);
        const int w = std::max(64, (text_width(0, cash) + 31) / 8 * 8);
        const int money_x = (width + cards_w) / 2 - w;
        if (windows_.empty()) {
            draw_window(canvas, money_x, card_y - 36, w, 32);
            draw_text(canvas, 0, money_x + (w - text_width(0, cash)) / 2, card_y - 28, cash, frame_color(1));
        }
    }
    for (const auto &window : windows_)
        for (int y = window.top; y < window.bottom; ++y)
            for (int x = window.left; x < window.right; ++x) {
                const auto pixel = windows_pixels_[std::size_t(y) * 256 + std::size_t(x)];
                if (pixel >> 24) canvas[std::size_t(y + area_y) * width + std::size_t(x + area_x)] = pixel;
            }
}

void SecondScreen::draw_states(Canvas canvas) const {
    const bool chosen = selected_ >= 0 && selected_ < int(snapshots_.size());
    const auto button = [&](int id, int x, int w, std::string_view label, bool enabled) {
        draw_window(canvas, x, 4, w, 32, pressed_ == id, enabled ? 1.f : 0.6f);
        draw_text(canvas, 0, x + (w - text_width(0, label)) / 2, 12, label,
                  enabled ? frame_color(1) : frame_color(2));
    };
    const int pages = std::max(1, (int(snapshots_.size()) + cards_per_page - 1) / cards_per_page);
    button(save_button, 6, 72, "Save", true);
    button(load_button, 82, 72, "Load", chosen);
    button(delete_button, 158, 80, confirm_delete_ >= 0 ? "Sure?" : "Delete", chosen);
    button(page_button, 242, 64, std::to_string(page_ + 1) + "/" + std::to_string(pages), pages > 1);
    if (snapshots_.empty()) {
        draw_window(canvas, 39, 96, 232, 64);
        const std::string_view lines[] = {"No save states yet.", "Tap Save to make one."};
        for (int line = 0; line < 2; ++line)
            draw_text(canvas, 0, 39 + (232 - text_width(0, lines[line])) / 2, 106 + line * 18, lines[line],
                      frame_color(line ? 2 : 1));
        return;
    }
    for (int slot = 0; slot < cards_per_page; ++slot) {
        const int index = page_ * cards_per_page + slot;
        const int x = 7 + (slot % 3) * 101, y = 40 + (slot / 3) * 98;
        if (index >= int(snapshots_.size())) {
            draw_window(canvas, x, y, card_w, card_h, false, 0.45f);
            continue;
        }
        const auto &snapshot = snapshots_[index];
        const bool selected = index == selected_;
        draw_window(canvas, x, y, card_w, card_h, selected || pressed_ == card_button + slot);
        const int tx = x + 16, ty = y + 8;
        if (snapshot.thumbnail.size() == std::size_t(thumbnail_width) * thumbnail_height) {
            // Average 2x2 blocks of the half-size capture.
            for (int row = 0; row < 56; ++row)
                for (int column = 0; column < 64; ++column) {
                    unsigned r = 0, g = 0, b = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const auto pixel = snapshot.thumbnail[std::size_t(row * 2 + dy) * thumbnail_width +
                                                                  std::size_t(column * 2 + dx)];
                            r += (pixel >> 16) & 255, g += (pixel >> 8) & 255, b += pixel & 255;
                        }
                    canvas[std::size_t(ty + row) * width + std::size_t(tx + column)] =
                        0xff000000u | (r / 4) << 16 | (g / 4) << 8 | b / 4;
                }
        } else {
            for (int row = 0; row < 56; ++row)
                std::fill_n(&canvas[std::size_t(ty + row) * width + std::size_t(tx)], 64, mix(frame_color(3), frame_color(2), 0.25f));
        }
        // The tiny font has no digits, so the date shares the main line in grey.
        auto date = format_time(snapshot.created_at_unix, "%b %d ");
        if (date.size() > 4 && date[4] == '0') date.erase(4, 1);
        const auto time = format_time(snapshot.created_at_unix, "%H:%M");
        const int line_x = x + (card_w - text_width(0, date) - text_width(0, time)) / 2;
        draw_text(canvas, 0, line_x, y + 68, date, frame_color(2));
        draw_text(canvas, 0, line_x + text_width(0, date), y + 68, time, frame_color(1));
        if (selected) draw_cursor(canvas, x + 4, y + 72);
    }
}

std::string SecondScreen::setting_value(int row) const {
    const auto on = [](bool value) { return std::string(value ? "On" : "Off"); };
    switch (row) {
    case 0: return settings_.windows_below ? "Bottom screen" : "Top screen";
    case 1: return on(settings_.widescreen);
    case 2: return settings_.frame_limit ? std::to_string(settings_.frame_limit) + " fps" : "Unlimited";
    case 3: return on(settings_.interpolate_frames);
    case 4: return on(settings_.crt_filter);
    case 5: return on(settings_.reduce_flashing);
    case 6: return std::to_string(settings_.backdrop + 1) + " of " + std::to_string(backdrop_count);
    }
    return {};
}

void SecondScreen::draw_settings(Canvas canvas) const {
    draw_window(canvas, 6, 4, 296, 224);
    for (int row = 0; row < int(setting_labels.size()); ++row) {
        const int y = 13 + row * 30;
        if (pressed_ == setting_button + row) {
            const auto fill = mix(frame_color(3), frame_color(2), 0.35f);
            for (int py = y; py < y + 28; ++py) std::fill_n(&canvas[std::size_t(py) * width + 14], 282, fill);
            draw_cursor(canvas, 18, y + 10);
        }
        draw_text(canvas, 0, 28, y + 6, setting_labels[row], frame_color(1));
        const auto value = setting_value(row);
        draw_text(canvas, 0, 288 - text_width(0, value), y + 6, value, frame_color(2));
    }
}

void SecondScreen::draw_tabs(Canvas canvas) const {
    static constexpr std::array<const char *, 3> labels{"Game", "States", "Settings"};
    for (int index = 0; index < 3; ++index) {
        const bool active = int(tab_) == index;
        draw_window(canvas, tab_x[index], tab_y, tab_w, tab_h, active || pressed_ == tab_button + index,
                    active ? 1.f : 0.7f);
        draw_text(canvas, 0, tab_x[index] + (tab_w - text_width(0, labels[index])) / 2, tab_y + 8, labels[index],
                  active ? frame_color(1) : frame_color(2));
    }
}

// ---- Hub -----------------------------------------------------------------

SecondScreenRegistration::SecondScreenRegistration(SecondScreen *screen) {
    std::lock_guard lock(hub_mutex);
    hub_screen = screen;
}

SecondScreenRegistration::~SecondScreenRegistration() {
    std::lock_guard lock(hub_mutex);
    hub_screen = nullptr;
}

bool with_second_screen_impl(void (*call)(SecondScreen &, void *), void *context) {
    std::lock_guard lock(hub_mutex);
    if (!hub_screen) return false;
    call(*hub_screen, context);
    return true;
}

void set_second_screen_attached(bool attached) { hub_attached = attached; }
bool second_screen_attached() { return hub_attached; }
} // namespace eb
