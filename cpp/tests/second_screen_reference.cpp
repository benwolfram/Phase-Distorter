// Drive the bottom-screen companion through touches and check the commands it
// issues. Every picture is decoded from the supplied pack, so this needs one.
// Optional second argument: a directory that receives a PPM of each page.
#include "eb/second_screen.hpp"
#include "generated_assets.hpp"
#include "generated_profile.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <utility>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}

void tap(eb::SecondScreen &screen, int x, int y) {
    screen.touch(x, y, 0);
    screen.touch(x, y, 2);
}

std::vector<std::uint32_t> render(eb::SecondScreen &screen) {
    std::vector<std::uint32_t> canvas(std::size_t(eb::SecondScreen::width) * eb::SecondScreen::height);
    // An unchanged page skips drawing until the backdrop's next 30 Hz step.
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (screen.render(std::span<std::uint32_t, eb::SecondScreen::width * eb::SecondScreen::height>(canvas)))
            return canvas;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("The page never redrew");
}

// A two-by-two menu in window 1, as WINDOW_STATS and the option pool hold it,
// whose cursor answers the joypad like the game's.
struct Menu {
    std::vector<std::uint8_t> ram = std::vector<std::uint8_t>(0x20000);
    int column{}, line{};
    std::vector<std::uint16_t> presses;
    void put(std::size_t at, unsigned value) { ram[at] = std::uint8_t(value), ram[at + 1] = std::uint8_t(value >> 8); }
    explicit Menu(bool options = true) {
        for (std::size_t id = 0; id < 53; ++id) put(0x88e4 + id * 2, 0xffff);
        put(0x88e4 + 2, 0); // window 1 in slot 0
        put(0x8958, 1);
        put(0x8650 + 43, options ? 0 : 0xffff);
        for (unsigned index = 0; index < 4; ++index) {
            const std::size_t at = 0x89d4 + index * 45;
            put(at, 1);
            put(at + 2, index < 3 ? index + 1 : 0xffff);
            put(at + 8, index % 2 * 8);
            put(at + 10, index / 2);
        }
        sync();
    }
    void sync() { put(0x8650 + 14, unsigned(column + 1)), put(0x8650 + 16, unsigned(line)); }
    // Runs until the tap queue drains, moving the cursor on each new press.
    void run(eb::SecondScreen &screen) {
        std::uint16_t previous = 0;
        for (int frame = 0; frame < 600; ++frame) {
            const auto buttons = screen.drive_input(ram);
            if (buttons && buttons != previous) {
                presses.push_back(buttons);
                if (buttons == 0x0800) line = 0;
                if (buttons == 0x0400) line = 1;
                if (buttons == 0x0200) column = 0;
                if (buttons == 0x0100) column = 8;
                sync();
            }
            previous = buttons;
        }
    }
};

void write(const std::filesystem::path &directory, const std::string &name, const std::vector<std::uint32_t> &canvas) {
    if (directory.empty()) return;
    std::ofstream output(directory / (name + ".ppm"), std::ios::binary);
    output << "P6\n" << eb::SecondScreen::width << ' ' << eb::SecondScreen::height << "\n255\n";
    for (const auto pixel : canvas) {
        const char rgb[] = {char(pixel >> 16), char(pixel >> 8), char(pixel)};
        output.write(rgb, sizeof rgb);
    }
}

void run(const eb::GameAssets &assets, const std::filesystem::path &directory) {
    eb::SecondScreen screen(assets.image, assets.version);

    // Party and money come straight from work RAM.
    std::vector<std::uint8_t> ram(0x20000);
    const char *names[] = {"Ness", "Paula", "Jeff"};
    for (int member = 0; member < 3; ++member) {
        ram[39023 + member] = std::uint8_t(member + 1);
        const std::size_t base = 39374 + std::size_t(member) * 95;
        for (int index = 0; names[member][index]; ++index) ram[base + index] = std::uint8_t(names[member][index] + 0x30);
        ram[base + 5] = 12;
        ram[base + 10] = 98, ram[base + 12] = 40, ram[base + 69] = std::uint8_t(76 - member * 30), ram[base + 75] = 21;
    }
    ram[39075] = 3;
    ram[0x9831] = 0x39, ram[0x9832] = 0x05;
    ram[0x99cd] = 1;
    screen.publish_party(ram);
    write(directory, "game", render(screen));

    // Seven states span two pages, newest first.
    std::vector<eb::SecondScreen::Snapshot> snapshots;
    for (int index = 0; index < 7; ++index) {
        std::vector<std::uint32_t> thumbnail(std::size_t(eb::SecondScreen::thumbnail_width) *
                                             eb::SecondScreen::thumbnail_height);
        for (int y = 0; y < eb::SecondScreen::thumbnail_height; ++y)
            for (int x = 0; x < eb::SecondScreen::thumbnail_width; ++x)
                thumbnail[std::size_t(y) * eb::SecondScreen::thumbnail_width + std::size_t(x)] =
                    0xff000000u | std::uint32_t(x * 2) << 16 | std::uint32_t(y * 2) << 8 | std::uint32_t(index * 36);
        snapshots.push_back({"state-" + std::to_string(index), 1760000000 + index * 3600, index == 4 ? std::vector<std::uint32_t>{} : thumbnail});
    }
    screen.publish_snapshots(snapshots);

    tap(screen, 150, 250); // States tab
    tap(screen, 40, 20);   // Save
    auto commands = screen.take_commands();
    require(commands.size() == 1 && commands[0].kind == eb::SecondScreen::Command::Kind::Save, "Save was not issued");

    tap(screen, 120, 80); // second card
    tap(screen, 110, 20); // Load
    commands = screen.take_commands();
    require(commands.size() == 1 && commands[0].kind == eb::SecondScreen::Command::Kind::Load &&
                commands[0].id == "state-5",
            "Load did not target the selected card");
    write(directory, "states", render(screen));

    tap(screen, 190, 20); // Delete asks first
    require(screen.take_commands().empty(), "Delete skipped its confirmation");
    write(directory, "states_confirm", render(screen));
    tap(screen, 190, 20);
    commands = screen.take_commands();
    require(commands.size() == 1 && commands[0].kind == eb::SecondScreen::Command::Kind::Delete &&
                commands[0].id == "state-5",
            "Confirmed delete was not issued");

    tap(screen, 270, 20); // next page
    tap(screen, 20, 60);  // first card there is the oldest state
    tap(screen, 110, 20);
    commands = screen.take_commands();
    require(commands.size() == 1 && commands[0].id == "state-0", "Paging did not reach the oldest state");
    write(directory, "states_page2", render(screen));

    // A press that slides off its button does nothing.
    screen.touch(110, 20, 0);
    screen.touch(200, 200, 1);
    screen.touch(200, 200, 2);
    require(screen.take_commands().empty(), "A cancelled press still activated");

    tap(screen, 250, 250); // Settings tab
    tap(screen, 100, 20);  // Text windows
    tap(screen, 100, 80);  // Frame rate
    commands = screen.take_commands();
    require(commands.size() == 2 && commands[0].kind == eb::SecondScreen::Command::Kind::Settings &&
                !commands[0].settings.windows_below && commands[1].settings.frame_limit == 90 &&
                !commands[1].settings.windows_below,
            "Settings rows did not toggle cumulatively");
    write(directory, "settings", render(screen));

    // Each backdrop is a battle background decoded from the pack; a failed
    // decode would fall back to a flat fill.
    for (int backdrop = 1; backdrop <= eb::SecondScreen::backdrop_count; ++backdrop) {
        tap(screen, 100, 200);
        commands = screen.take_commands();
        require(commands.size() == 1 && commands[0].settings.backdrop == backdrop % eb::SecondScreen::backdrop_count,
                "Backdrop row did not cycle");
        tap(screen, 50, 250);
        const auto canvas = render(screen);
        std::set<std::uint32_t> colors(canvas.begin(), canvas.begin() + eb::SecondScreen::width * 100);
        require(colors.size() >= 4, "Backdrop " + std::to_string(backdrop) + " is flat");
        write(directory, "backdrop" + std::to_string(backdrop % eb::SecondScreen::backdrop_count), canvas);
        tap(screen, 250, 250);
    }

    // Game page taps. Window 1 sits at native (16, 16); its text starts one
    // tile in, on lines 16 pixels apart, and the page offsets it by (27, 6).
    tap(screen, 50, 250);
    const std::vector<std::uint32_t> pixels(256 * 224, 0xff000000u);
    const eb::SecondScreen::Window window{16, 16, 176, 64, 1};
    screen.publish_windows(std::span<const std::uint32_t, 256 * 224>(pixels.data(), pixels.size()),
                           std::span<const eb::SecondScreen::Window>(&window, 1));
    const auto cell = [](int column, int line) { return std::pair{27 + 16 + 8 + column * 8 + 3, 6 + 16 + 8 + line * 16 + 4}; };
    {
        Menu menu;
        const auto [x, y] = cell(10, 1); // inside the second option of the second line
        tap(screen, x, y);
        menu.run(screen);
        require(menu.column == 8 && menu.line == 1 && !menu.presses.empty() && menu.presses.back() == 0x0080,
                "A menu tap did not walk the cursor to its option and confirm");
        tap(screen, 5, 5); // outside every window
        menu.presses.clear();
        menu.run(screen);
        require(menu.presses == std::vector<std::uint16_t>{0x8000}, "A tap outside the windows did not press B");
    }
    {
        // Quick taps queue in order, like letters on the naming keyboard.
        Menu menu;
        const auto [x1, y1] = cell(9, 0);
        const auto [x2, y2] = cell(1, 1);
        tap(screen, x1, y1);
        tap(screen, x2, y2);
        menu.run(screen);
        std::vector<std::pair<int, int>> confirmed;
        menu.column = 0, menu.line = 0;
        for (const auto press : menu.presses) {
            if (press == 0x0800) menu.line = 0;
            if (press == 0x0400) menu.line = 1;
            if (press == 0x0200) menu.column = 0;
            if (press == 0x0100) menu.column = 8;
            if (press == 0x0080) confirmed.emplace_back(menu.column, menu.line);
        }
        require(confirmed == std::vector<std::pair<int, int>>{{8, 0}, {0, 1}}, "Queued taps did not confirm in order");
    }
    {
        // A text window without options just takes A.
        Menu menu(false);
        const auto [x, y] = cell(3, 0);
        tap(screen, x, y);
        menu.run(screen);
        require(menu.presses == std::vector<std::uint16_t>{0x0080}, "A text window tap did not press A");
    }
    {
        // Cancelling drops a tap mid-walk, as the pad or a loaded state does.
        Menu menu;
        const auto [x, y] = cell(10, 1);
        tap(screen, x, y);
        screen.cancel_input();
        menu.run(screen);
        require(menu.presses.empty(), "A cancelled tap still pressed buttons");
    }
    {
        // Boxes beside a battle target window (ID 49) step Left and Right.
        const eb::SecondScreen::Window target{100, 40, 220, 72, 49};
        screen.publish_windows(std::span<const std::uint32_t, 256 * 224>(pixels.data(), pixels.size()),
                               std::span<const eb::SecondScreen::Window>(&target, 1));
        write(directory, "game_target", render(screen));
        for (const auto [x, button] : {std::pair{27 + 100 - 9, std::uint16_t(0x0200)},
                                       std::pair{27 + 220 + 9, std::uint16_t(0x0100)}}) {
            Menu menu;
            tap(screen, x, 6 + 50);
            menu.run(screen);
            require(menu.presses == std::vector<std::uint16_t>{button}, "A target step box did not press its direction");
        }
        screen.publish_windows(std::span<const std::uint32_t, 256 * 224>(pixels.data(), pixels.size()),
                               std::span<const eb::SecondScreen::Window>(&window, 1));
    }
    write(directory, "game_windows", render(screen));

    // In a battle the backdrop becomes the fight's own background; the
    // Coil Snake's group 3 uses a different layer from every choice above.
    const auto top_colors = [&] {
        const auto canvas = render(screen);
        return std::set<std::uint32_t>(canvas.begin(), canvas.begin() + eb::SecondScreen::width * 8);
    };
    const auto calm = top_colors();
    ram[0x9643] = 1, ram[0x4a8c] = 3;
    screen.publish_party(ram);
    const auto battle = top_colors();
    require(battle.size() >= 4 && battle != calm, "The battle backdrop did not replace the chosen one");
    write(directory, "game_battle", render(screen));
    ram[0x9643] = 0;
    screen.publish_party(ram);

    // The title screen's attract mode runs with a blank party record.
    std::vector<std::uint8_t> blank(0x20000);
    blank[39023] = 1, blank[39075] = 1;
    screen.publish_party(blank);
    write(directory, "game_blank", render(screen));
    screen.publish_party(ram);

    // On a clear field a row offers the command window (A) and Talk to (L);
    // with three members and $1337 it starts at x 43.
    screen.publish_windows(std::span<const std::uint32_t, 256 * 224>(pixels.data(), pixels.size()), {});
    for (const auto [x, button] : {std::pair{79, std::uint16_t(0x0080)}, std::pair{155, std::uint16_t(0x0020)}}) {
        Menu menu;
        tap(screen, x, 154);
        menu.run(screen);
        require(menu.presses == std::vector<std::uint16_t>{button}, "A field shortcut did not press its button");
    }
    write(directory, "game_field", render(screen));
    ram[0x9643] = 1;
    screen.publish_party(ram);
    {
        Menu menu;
        tap(screen, 79, 154);
        menu.run(screen);
        require(menu.presses.empty(), "The field row answered during a battle");
    }
    ram[0x9643] = 0;
    screen.publish_party(ram);

    screen.show_message("State saved");
    tap(screen, 50, 250);
    write(directory, "game_message", render(screen));
    require(!render(screen).empty(), "Render failed");
    std::cout << "PASS " << assets.title << ": second screen tabs, states, settings, taps, battles and messages\n";
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc >= 2, "second_screen_reference pack.ebpak [capture-directory]");
        const auto assets = eb::load_game_assets(argv[1], eb::asset_profiles());
        require(eb::SecondScreen::supported(assets.version), "The second screen needs the US pack");
        run(assets, argc >= 3 ? std::filesystem::path(argv[2]) : std::filesystem::path());
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
