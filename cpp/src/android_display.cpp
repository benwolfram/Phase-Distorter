#include "eb/desktop_display.hpp"
#include "eb/asset_store.hpp"
#include "eb/controller_preferences.hpp"
#include "eb/display_settings.hpp"
#include "eb/frame_pacer.hpp"
#include "eb/frame_presenter.hpp"
#include "eb/game_session.hpp"
#include "eb/launch_options.hpp"
#include "eb/session_storage.hpp"
#include "desktop_input.hpp"
#include "gl_api.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>

// Android counterpart of desktop_display.cpp. The Java launcher obtains the
// ROM through the system file picker, so this display has no importer or
// settings panel: one fullscreen GLES surface, the game picture and gamepad.

namespace eb {
namespace {
constexpr int height = 224;
} // namespace
struct DesktopDisplay::Impl {
  public:
    double frame_rate() const {
        if (settings_.high_frame_rate() || SDL_GL_GetSwapInterval() == 0)
            return eb::FramePacer::frame_rate;
        return eb::FramePacer::rate_for_refresh(refresh_rate(), settings_.variable_refresh);
    }
    double presentation_rate() const {
        // Android always composites with vsync, so presenting faster than the
        // panel refresh only blocks in eglSwapBuffers.
        const double refresh = refresh_rate();
        if (!vsync_requested_ || refresh <= 0)
            return settings_.frame_limit;
        return settings_.frame_limit > 0 ? std::min<double>(settings_.frame_limit, refresh) : refresh;
    }
    void update_swap_interval() {
        const int wanted = vsync_requested_ ? 1 : 0;
        if (wanted == requested_swap_interval_)
            return;
        requested_swap_interval_ = wanted;
        if (SDL_GL_SetSwapInterval(wanted) != 0)
            std::cerr << "Swap interval unavailable: " << SDL_GetError() << '\n';
    }
    explicit Impl(const LaunchOptions &options, eb::DisplaySettings &settings)
        : vsync_requested_(options.vsync), settings_(settings),
          controller_settings_(
              load_controller_settings(options.config.empty() ? "" : options.config + ".controllers")) {
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
            throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
        initialized_ = true;
        try {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
            SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
            SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
            SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
            SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
            SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
            // Android sizes the surface to the display; the requested size is ignored.
            window_ = SDL_CreateWindow("Phase Distorter", 0, 0, 0, 0,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!window_)
                throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
            context_ = SDL_GL_CreateContext(window_);
            if (!context_)
                throw std::runtime_error(std::string("SDL_GL_CreateContext: ") + SDL_GetError());
            update_swap_interval();
            presenter_ = std::make_unique<eb::FramePresenter>();
            presenter_->prepare_scene_effects();
            if (settings_.crt_filter) presenter_->prepare_crt();
            input_ = std::make_unique<DesktopInput>();
            input_->configure(controller_settings_);
            std::cout << "OpenGL ES: " << glGetString(GL_VERSION) << " / " << glGetString(GL_RENDERER) << '\n';
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Impl() { cleanup(); }
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    eb::GameAssets import_assets(std::string &path, const std::string &error) {
        // Reached only when no usable pack exists. Remove a pack that failed
        // verification so the next launch asks for a ROM again, then stop.
        if (!error.empty()) {
            std::error_code ignored;
            std::filesystem::remove(native_path(path), ignored);
        }
        throw std::runtime_error(error.empty() ? "No ROM was supplied" : error);
    }

    unsigned render_width() const {
        int drawable_width{}, drawable_height{};
        SDL_GL_GetDrawableSize(window_, &drawable_width, &drawable_height);
        return settings_.render_width(drawable_width, std::max(1, drawable_height));
    }

    bool events(std::uint16_t &buttons) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                return false;
            input_->process_device_event(event);
        }
        buttons |= input_->buttons();
        return true;
    }

    void present(PresentationPicture picture, const std::string &capture) {
        int drawable_width = 0, drawable_height = 0;
        SDL_GL_GetDrawableSize(window_, &drawable_width, &drawable_height);
        const double aspect = picture.fixed_aspect > 0
            ? picture.fixed_aspect
            : settings_.target_aspect(drawable_width, std::max(1, drawable_height));
        if (!picture.scene || !presenter_->draw_scene(*picture.scene, drawable_width, drawable_height, aspect, 0))
            presenter_->draw(picture.pixels, int(picture.width), height, drawable_width, drawable_height, aspect, 0);
        presenter_->apply_crt(settings_.crt_filter);
        if (!capture.empty())
            presenter_->capture().write_ppm(capture);
        SDL_GL_SwapWindow(window_);
    }

  private:
    double refresh_rate() const {
        SDL_DisplayMode mode{};
        const int display = SDL_GetWindowDisplayIndex(window_);
        if (display < 0 || SDL_GetCurrentDisplayMode(display, &mode) != 0)
            return 0;
        return mode.refresh_rate;
    }
    void cleanup() {
        input_.reset();
        presenter_.reset();
        if (context_)
            SDL_GL_DeleteContext(context_);
        if (window_)
            SDL_DestroyWindow(window_);
        if (initialized_)
            SDL_Quit();
        context_ = nullptr;
        window_ = nullptr;
        initialized_ = false;
    }
    bool vsync_requested_ = true;
    int requested_swap_interval_ = -2;
    SDL_Window *window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    std::unique_ptr<DesktopInput> input_;
    std::unique_ptr<eb::FramePresenter> presenter_;
    eb::DisplaySettings &settings_;
    ControllerSettings controller_settings_;
    bool initialized_ = false;
};

DesktopDisplay::DesktopDisplay(const LaunchOptions &options, DisplaySettings &settings)
    : impl_(std::make_unique<Impl>(options, settings)) {}
DesktopDisplay::~DesktopDisplay() = default;
GameAssets DesktopDisplay::import_assets(std::string &path, const std::string &error, const std::string &,
                                         bool, GameVersion) {
    return impl_->import_assets(path, error);
}
void DesktopDisplay::start_panel(bool, const std::string &, const std::string &, GameVersion,
                                 const std::string &) {}
bool DesktopDisplay::poll_events(std::uint16_t &physical_buttons) { return impl_->events(physical_buttons); }
void DesktopDisplay::update_debug(GameDebug &) {}
void DesktopDisplay::adopt_debug(const GameDebug &) {}
std::optional<GameVersion> DesktopDisplay::take_game_request() { return std::nullopt; }
std::optional<SaveStateSnapshotRequest> DesktopDisplay::take_snapshot_action() { return std::nullopt; }
void DesktopDisplay::set_snapshot_state(std::vector<SaveStateSnapshotInfo>, std::string, bool) {}
unsigned DesktopDisplay::render_width() const { return impl_->render_width(); }
bool DesktopDisplay::fullscreen() const { return true; }
bool DesktopDisplay::wants_register_diagnostics() const { return false; }
double DesktopDisplay::frame_rate() const { return impl_->frame_rate(); }
double DesktopDisplay::presentation_rate() const { return impl_->presentation_rate(); }
void DesktopDisplay::update_swap_interval() { impl_->update_swap_interval(); }
void DesktopDisplay::present(const SessionDiagnostics &, PresentationPicture picture, const std::string &capture) {
    impl_->present(picture, capture);
}
} // namespace eb
