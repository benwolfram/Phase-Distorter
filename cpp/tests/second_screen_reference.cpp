// Drive the bottom-screen companion through touches and check the commands it
// issues. Every picture is decoded from the supplied pack, so this needs one.
// Optional second argument: a directory that receives a PPM of each page.
#include "eb/second_screen.hpp"
#include "generated_assets.hpp"
#include "generated_profile.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
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
    screen.render(std::span<std::uint32_t, eb::SecondScreen::width * eb::SecondScreen::height>(canvas));
    return canvas;
}

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

    screen.show_message("State saved");
    tap(screen, 50, 250);
    write(directory, "game_message", render(screen));
    require(!render(screen).empty(), "Render failed");
    std::cout << "PASS " << assets.title << ": second screen tabs, states, settings and messages\n";
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
