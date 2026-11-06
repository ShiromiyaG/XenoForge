// The launcher's screens in RmlUi (launcher_view.h).

#include "launcher_view.h"

#include "xenoforge/game_build.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

namespace xf::frontend {

using Clock = std::chrono::steady_clock;

namespace {

const char *SCREEN_IDS[] = {"screen-library", "screen-settings", "screen-controls", "screen-about"};
const char *TAB_IDS[] = {"tab-library", "tab-settings", "tab-controls", "tab-about"};

std::string read_text(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string px(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1fpx", value);
    return text;
}

std::string digest_text(uint64_t digest) {
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)digest);
    return text;
}

} // namespace

LauncherView::LauncherView(LauncherCore &core, ui::Renderer &renderer, SDL_Window *window, ViewOptions options)
    : core_(core), renderer_(renderer), window_(window), opt_(std::move(options)) {
    screen_ = opt_.screen;
    select_pending_ = opt_.select;
    start_pending_ = opt_.start_index;
    demo_pending_ = opt_.demo_progress;
    capture_pending_ = opt_.capture;
    last_update_ = Clock::now();
}

LauncherView::~LauncherView() {
    if (doc_ && ctx_) {
        ctx_->UnloadDocument(doc_);
        ctx_->Update(); // the document goes while this listener is alive
    }
}

bool LauncherView::load(Rml::Context *context, const std::filesystem::path &assets_dir, std::string &error) {
    ctx_ = context;
    assets_ = assets_dir;
    rml_template_ = read_text(assets_ / "launcher.rml");
    rcss_template_ = read_text(assets_ / "launcher.rcss");
    if (rml_template_.empty() || rcss_template_.empty()) return error = "no launcher.rml or launcher.rcss in " + assets_.string(), false;
    return build_document(error);
}

bool LauncherView::build_document(std::string &error) {
    const Accent accent = accent_from_hex(core_.accent());
    std::string rml = rml_template_;
    const size_t at = rml.find("<!--STYLE-->");
    if (at == std::string::npos) return error = "launcher.rml has no <!--STYLE--> mark", false;
    rml.replace(at, 12, "<style>\n" + apply_tokens(rcss_template_, accent) + "\n</style>");
    doc_ = ctx_->LoadDocumentFromMemory(rml, "launcher.rml");
    if (!doc_) return error = "launcher.rml did not load", false;
    for (const char *type : {"click", "dblclick", "mousedown", "mouseup", "mousemove", "mouseover", "mouseout", "change", "keydown"})
        doc_->AddEventListener(type, this);
    doc_->AddEventListener("focus", this, true);
    doc_->AddEventListener("blur", this, true);
    doc_->Show();
    if (Rml::Element *about = by_id("screen-about")) about->SetInnerRML(about_html());
    if (Rml::Element *path = by_id("folder-path")) path->SetInnerRML(html_escape(core_.games_folder().empty() ? "No folder chosen" : core_.games_folder()));
    library_sig_.clear(), detail_sig_.clear();
    art_title_ = 0xFFFFFFFF;
    prepare_open_ = false;
    rebuild_library();
    rebuild_settings();
    rebuild_controls();
    show_screen(screen_, true);
    return true;
}

void LauncherView::reload_document() {
    defer([this] {
        Rml::ElementDocument *old = doc_;
        const bool was_open = prepare_open_;
        std::string error;
        doc_ = nullptr;
        if (old) ctx_->UnloadDocument(old);
        if (!build_document(error)) std::fprintf(stderr, "launcher: %s\n", error.c_str());
        if (was_open) close_prepare();
        pill_frames_ = 3;
    });
}

bool LauncherView::busy() const {
    return listing_.busy || start_pending_ >= 0 || core_.run().running || job_.active() || now_ < settle_until_ || !deferred_.empty();
}

// ---------------------------------------------------------------------------
// state

const GameEntry *LauncherView::selected_game() const {
    if (selected_ < 0 || selected_ >= long(listing_.games.size())) return nullptr;
    const GameEntry &g = listing_.games[size_t(selected_)];
    return g.error.empty() ? &g : nullptr;
}

bool LauncherView::cached_of(const GameEntry &game) const {
    return build::find_cached(build::default_cache_dir(), digest_text(game.digest)).has_value();
}

const PlayerBinary *LauncherView::player_of(const GameEntry &game) const {
    return player_for(listing_.players, game, cached_of(game));
}

GameStatus LauncherView::status_of(const GameEntry &game) const {
    const bool cached = cached_of(game);
    const PlayerBinary *player = player_for(listing_.players, game, cached);
    return game_status(cached, player && !player->generic, failed_.count(game.title_id) > 0);
}

std::string LauncherView::icon_name(const GameEntry &game) {
    if (game.icon.empty()) return {};
    const std::string name = "icon_" + game.title_id_text();
    if (!icons_.count(game.title_id)) {
        if (!renderer_.add_image(name, game.icon)) return {};
        renderer_.add_soft_image(name, game.icon);
        icons_.insert(game.title_id);
    }
    return name;
}

void LauncherView::poll_listing() {
    const Clock::time_point t = Clock::now();
    if (last_listing_ != Clock::time_point() && t - last_listing_ < std::chrono::milliseconds(120)) return;
    last_listing_ = t;
    listing_ = core_.listing();
    if (selected_ >= long(listing_.games.size())) selected_ = -1;
    if (select_pending_ >= 0 && select_pending_ < long(listing_.games.size())) {
        select_game(int(select_pending_), screen_ == Screen::Library);
        select_pending_ = -1;
    }
    if (!listing_.busy && select_pending_ >= long(listing_.games.size())) select_pending_ = -1;
    // the first game listed is the selected one
    if (selected_ < 0 && select_pending_ < 0)
        for (size_t i = 0; i < listing_.games.size(); i++)
            if (listing_.games[i].error.empty()) {
                select_game(int(i), screen_ == Screen::Library);
                break;
            }
}

void LauncherView::select_game(int index, bool focus_tile) {
    if (index < 0 || index >= int(listing_.games.size())) return;
    const bool changed = selected_ != index;
    selected_ = index;
    if (changed) {
        edit_valid_ = false;
        detail_sig_.clear();
        const GameEntry *g = selected_game();
        set_backdrop(g);
        if (screen_ == Screen::Settings) rebuild_settings();
        if (screen_ == Screen::Controls) rebuild_controls();
    }
    if (focus_tile) {
        defer([this, index] {
            Rml::Element *grid = by_id("grid");
            if (!grid) return;
            for (int i = 0; i < grid->GetNumChildren(); i++) {
                Rml::Element *tile = grid->GetChild(i);
                if (tile->GetAttribute<int>("data-index", -1) == index) {
                    tile->Focus(pad_mode_);
                    ensure_visible(tile);
                    break;
                }
            }
        });
    }
}

void LauncherView::reload_edit() {
    std::string error;
    if (const GameEntry *g = selected_game()) {
        core_.load_settings(*g, edit_, error);
        edit_title_ = g->title_id;
    } else {
        edit_ = Settings();
        frontend::load(core_.config_file(), edit_, error);
        edit_title_ = 0;
    }
    edit_valid_ = true;
}

void LauncherView::save_edit() {
    std::string error;
    if (!core_.save_settings(edit_, error)) toast(error, true, false, 6);
}

// ---------------------------------------------------------------------------
// building

void LauncherView::set_backdrop(const GameEntry *game) {
    const uint32_t id = game ? game->title_id : 0;
    if (id == art_title_) return;
    art_title_ = id;
    Rml::Element *front = by_id(art_front_ == 0 ? "art-a" : "art-b");
    Rml::Element *back = by_id(art_front_ == 0 ? "art-b" : "art-a");
    if (!back) return;
    if (!game) {
        back->SetClass("on", false);
        if (front) front->SetClass("on", false);
        return;
    }
    const std::string icon = icon_name(*game);
    back->SetAttribute("style", art_style(icon.empty() ? icon : "soft_" + icon, game->title_id));
    back->SetClass("on", true);
    if (front) front->SetClass("on", false);
    art_front_ ^= 1;
}


} // namespace xf::frontend
