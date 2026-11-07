// The launcher's main window in RmlUi (launcher_view.h): the document, the
// theme, the state of the games and the table.

#include "launcher_view.h"

#include "stale_library.h"
#include "xenoforge/game_build.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>

namespace xf::frontend {

using Clock = std::chrono::steady_clock;

namespace {

std::string read_text(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string digest_text(uint64_t digest) {
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)digest);
    return text;
}

} // namespace

LauncherView::LauncherView(LauncherCore &core, ui::Renderer &renderer, SDL_Window *window, ViewOptions options)
    : core_(core), renderer_(renderer), window_(window), opt_(std::move(options)) {
    select_pending_ = opt_.select;
    start_pending_ = opt_.start_index;
    demo_pending_ = opt_.demo_progress;
    capture_pending_ = opt_.capture;
    menu_pending_ = opt_.menu;
    context_pending_ = opt_.context_menu;
    screen_pending_ = opt_.screen != Screen::Library;
    widths_ = parse_widths(core_.launcher().columns);
    parse_sort(core_.launcher().sort, sort_, ascending_);
    last_update_ = Clock::now();
}

LauncherView::~LauncherView() {
    settings_.reset(); // its context and window first
    if (doc_ && ctx_) {
        ctx_->UnloadDocument(doc_);
        ctx_->Update(); // the document goes while this listener is alive
    }
}

bool LauncherView::load(Rml::Context *context, const std::filesystem::path &assets_dir, std::string &error) {
    ctx_ = context;
    assets_ = assets_dir;
    rml_template_ = read_text(assets_ / "launcher.rml");
    rcss_template_ = read_text(assets_ / "theme.rcss") + "\n" + read_text(assets_ / "launcher.rcss");
    if (rml_template_.empty() || rcss_template_.size() < 2) return error = "no launcher.rml or its stylesheets in " + assets_.string(), false;
    return build_document(error);
}

bool LauncherView::resolve_dark() const {
    switch (theme_from_text(core_.launcher().theme)) {
    case ThemeChoice::Light: return false;
    case ThemeChoice::Dark: return true;
    default: return SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT; // unknown: dark
    }
}

std::string LauncherView::styled(const std::string &rcss) const {
    const Accent accent = accent_from_hex(core_.accent());
    return apply_tokens(rcss, accent, palette(dark_, accent));
}

bool LauncherView::build_document(std::string &error) {
    dark_ = resolve_dark();
    std::string rml = rml_template_;
    const size_t at = rml.find("<!--STYLE-->");
    if (at == std::string::npos) return error = "launcher.rml has no <!--STYLE--> mark", false;
    rml.replace(at, 12, "<style>\n" + styled(rcss_template_) + "\n</style>");
    doc_ = ctx_->LoadDocumentFromMemory(expand_icons(rml), "launcher.rml");
    if (!doc_) return error = "launcher.rml did not load", false;
    for (const char *type : {"click", "dblclick", "mousedown", "mouseup", "mousemove", "mouseover", "change"}) doc_->AddEventListener(type, this);
    doc_->AddEventListener("focus", this, true);
    doc_->Show();
    rows_sig_.clear(), status_sig_.clear();
    menu_.clear(), menu_items_.clear();
    if (Rml::Element *search = by_id("search")) search->SetAttribute("value", filter_);
    if (Rml::Element *clear = by_id("search-clear")) clear->SetClass("on", !filter_.empty());
    rebuild_head();
    rebuild_rows(true);
    update_toolbar();
    update_status();
    // a dialog open when the document was made again is shown again
    const Dialog was = dialog_;
    dialog_ = Dialog::None;
    if (was == Dialog::Prepare) show_prepare();
    else if (was == Dialog::About) show_about();
    settle_until_ = std::max(settle_until_, now_ + 0.6);
    return true;
}

void LauncherView::reload_document() {
    defer([this] {
        Rml::ElementDocument *old = doc_;
        std::string error;
        doc_ = nullptr;
        if (old) ctx_->UnloadDocument(old);
        if (!build_document(error)) std::fprintf(stderr, "launcher: %s\n", error.c_str());
        if (selected_ >= 0) select_game(selected_, true);
    });
}

void LauncherView::look_changed() {
    dark_ = resolve_dark();
    reload_document();
    if (settings_) settings_->restyle();
}

void LauncherView::system_theme_changed() {
    if (theme_from_text(core_.launcher().theme) == ThemeChoice::System && resolve_dark() != dark_) look_changed();
}

bool LauncherView::busy() const {
    return listing_.busy || start_pending_ >= 0 || core_.run().running || job_.active() || now_ < settle_until_ || !deferred_.empty() ||
           (settings_ && settings_->busy());
}

// ---------------------------------------------------------------------------
// state

const GameEntry *LauncherView::selected_game() const {
    if (selected_ < 0 || selected_ >= long(listing_.games.size())) return nullptr;
    const GameEntry &g = listing_.games[size_t(selected_)];
    return g.error.empty() ? &g : nullptr;
}

bool LauncherView::cached_of(const GameEntry &game) const {
    auto it = cached_.find(game.digest);
    if (it == cached_.end())
        it = cached_.emplace(game.digest, build::find_cached(build::default_cache_dir(), digest_text(game.digest)).has_value()).first;
    return it->second;
}

const PlayerBinary *LauncherView::player_of(const GameEntry &game) const { return player_for(listing_.players, game, cached_of(game)); }

bool LauncherView::stale_of(const GameEntry &game) const {
    if (!cached_of(game)) return false;
    auto it = stale_.find(game.digest);
    if (it == stale_.end())
        it = stale_.emplace(game.digest, library_has_new_entries(build::default_cache_dir(), digest_text(game.digest))).first;
    return it->second;
}

GameStatus LauncherView::status_of(const GameEntry &game) const {
    const bool cached = cached_of(game);
    const PlayerBinary *player = player_for(listing_.players, game, cached);
    return game_status(cached, player && !player->generic, failed_.count(game.title_id) > 0, stale_of(game));
}

std::string LauncherView::runs_in(const GameEntry &game) const {
    const PlayerBinary *player = player_of(game);
    if (!player) return "no player";
    if (!player->generic) return player->path.stem().string();
    return cached_of(game) ? "generic player, library" : "generic player";
}

std::string LauncherView::icon_name(const GameEntry &game) {
    if (game.icon.empty()) return {};
    const std::string name = "icon_" + game.title_id_text();
    if (!icons_.count(game.title_id)) {
        if (!renderer_.add_image(name, game.icon)) return {};
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
        rebuild_rows();
        select_game(select_pending_, true);
        select_pending_ = -1;
    }
    if (!listing_.busy && select_pending_ >= long(listing_.games.size())) select_pending_ = -1;
    // the first game the table shows is the selected one
    if (selected_ < 0 && select_pending_ < 0 && !listing_.games.empty()) {
        rebuild_rows();
        for (size_t i : order_)
            if (listing_.games[i].error.empty()) {
                select_game(long(i), true);
                break;
            }
    }
}

void LauncherView::select_game(long index, bool scroll) {
    if (index < 0 || index >= long(listing_.games.size())) return;
    const long before = selected_;
    selected_ = index;
    if (before != index) {
        if (Rml::Element *old = by_id(("row-" + std::to_string(before)).c_str())) old->SetClass("sel", false);
        if (settings_) settings_->set_game(selected_game());
    }
    if (Rml::Element *row = by_id(("row-" + std::to_string(index)).c_str())) {
        row->SetClass("sel", true);
        if (scroll) row->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
    }
    update_toolbar();
    update_status();
}

// The selection moved by rows as the table shows them (delta), or to its
// first or last (to_end, delta's sign saying which); the row keeps focus.
void LauncherView::move_selection(int delta, bool to_end) {
    if (order_.empty()) return;
    long at = -1;
    for (size_t i = 0; i < order_.size(); i++)
        if (long(order_[i]) == selected_) at = long(i);
    long next = to_end ? (delta < 0 ? 0 : long(order_.size()) - 1) : at < 0 ? 0 : std::clamp(at + delta, 0L, long(order_.size()) - 1);
    select_game(long(order_[size_t(next)]), true);
    if (Rml::Element *row = by_id(("row-" + std::to_string(selected_)).c_str())) row->Focus(pad_mode_);
}

int LauncherView::visible_rows() const {
    Rml::Element *body = by_id("tbody");
    if (!body) return 10;
    return std::max(1, int(body->GetClientHeight() / (30 * dp())) - 1);
}

void LauncherView::save_layout() {
    Settings::Launcher l = core_.launcher();
    l.columns = widths_text(widths_);
    l.sort = sort_text(sort_, ascending_);
    std::string error;
    if (!core_.set_launcher(l, error)) std::fprintf(stderr, "launcher: %s\n", error.c_str());
}

// ---------------------------------------------------------------------------
// the table

void LauncherView::rebuild_head() {
    if (Rml::Element *head = by_id("thead")) head->SetInnerRML(table_head_html(widths_, sort_, ascending_));
}

void LauncherView::rebuild_rows(bool force) {
    Rml::Element *body = by_id("tbody");
    if (!body) return;
    const bool no_folder = core_.games_folder().empty();
    const LauncherCore::Run &run = core_.run();

    std::vector<RowKey> keys(listing_.games.size());
    for (size_t i = 0; i < listing_.games.size(); i++) {
        const GameEntry &g = listing_.games[i];
        RowKey &k = keys[i];
        k.file = g.path.filename().string();
        if (!g.error.empty()) {
            k.title = k.file, k.status = 3;
            continue;
        }
        k.title = g.title, k.id_text = g.title_id_text(), k.version = g.version;
        k.status = int(status_of(g)), k.runs_in = runs_in(g);
    }
    std::string sig = core_.games_folder() + "|" + std::to_string(listing_.total) + (listing_.busy ? "b" : "i") + "|" + filter_ + "|" +
                      sort_text(sort_, ascending_) + "|" + widths_text(widths_) + "|" + (run.running ? run.title : "") + "|";
    for (const RowKey &k : keys) sig += k.title + "/" + std::to_string(k.status) + "/" + k.runs_in + ";";
    if (!force && sig == rows_sig_) return;
    rows_sig_ = sig;
    settle_until_ = std::max(settle_until_, now_ + 0.5);

    order_.clear();
    for (size_t i : sorted_rows(keys, sort_, ascending_))
        if (row_matches(keys[i], filter_)) order_.push_back(i);

    std::string html;
    for (size_t pos = 0; pos < order_.size(); pos++) {
        const size_t i = order_[pos];
        const GameEntry &g = listing_.games[i];
        RowInfo r;
        r.index = int(i);
        r.alternate = pos % 2 == 1;
        r.file = keys[i].file;
        if (!g.error.empty()) {
            r.unreadable = true, r.title = keys[i].title, r.file = g.error;
        } else {
            r.title = g.title, r.id_text = g.title_id_text(), r.version = version_text(g.version), r.title_id = g.title_id;
            r.icon = icon_name(g), r.status = GameStatus(keys[i].status), r.runs_in = keys[i].runs_in;
            r.running = run.running && run.title == g.title;
        }
        html += table_row_html(r, widths_, long(i) == selected_);
    }
    body->SetInnerRML(html);

    const bool filtered = !filter_.empty() && order_.empty() && !listing_.games.empty();
    const bool show_empty = no_folder || filtered || (listing_.games.empty() && !listing_.busy);
    if (Rml::Element *empty = by_id("tempty")) {
        empty->SetClass("on", show_empty);
        empty->SetInnerRML(show_empty ? empty_html(no_folder, filtered, core_.games_folder()) : std::string());
    }
    update_toolbar();
    update_status();
}

// ---------------------------------------------------------------------------
// the toolbar and the status bar

void LauncherView::update_toolbar() {
    const GameEntry *g = selected_game();
    const LauncherCore::Run &run = core_.run();
    const bool running = run.running;
    const GameStatus status = g ? status_of(*g) : GameStatus::Ready;
    if (Rml::Element *play = by_id("tb-play")) {
        const std::string want = running ? "stop" : "play";
        if (play->GetAttribute<Rml::String>("data-action", "") != want || !play->HasAttribute("data-built")) {
            play->SetAttribute("data-action", want);
            play->SetAttribute("data-built", "1");
            play->SetInnerRML(running ? icon_html("square") + "<div>Stop</div>" : icon_html("play") + "<div>Play</div>");
            play->SetClass("stop", running);
        }
        play->SetClass("off", !running && (!g || !player_of(*g)));
    }
    if (Rml::Element *prepare = by_id("tb-prepare")) {
        const char *label = !g || status == GameStatus::NeedsPreparing ? "Prepare" : status == GameStatus::Interpreted ? "Try preparing again" : status == GameStatus::Stale ? "Rebuild (new code)" : "Rebuild";
        if (Rml::Element *text = by_id("tb-prepare-label"))
            if (text->GetInnerRML() != label) text->SetInnerRML(label);
        prepare->SetClass("off", !g || running || job_.active());
    }
}

void LauncherView::set_message(const std::string &text, int kind, double seconds) {
    message_ = text, message_kind_ = kind;
    message_until_ = seconds > 0 ? now_ + seconds : 0;
    update_status();
}

void LauncherView::update_status() {
    const LauncherCore::Run &run = core_.run();
    const PrepareJob::State s = job_.state();
    const bool progress = job_.active() && dialog_ != Dialog::Prepare;
    std::string count;
    if (listing_.busy)
        count = "Reading " + std::to_string(listing_.games.size()) + " of " + std::to_string(std::max(listing_.total, listing_.games.size())) + "...";
    else if (!filter_.empty())
        count = std::to_string(order_.size()) + " of " + std::to_string(listing_.games.size()) + " games";
    else
        count = std::to_string(listing_.games.size()) + (listing_.games.size() == 1 ? " game" : " games");
    const std::string folder = core_.games_folder().empty() ? "No games folder: click to choose one" : core_.games_folder();
    std::string message, message_class;
    if (run.running) {
        message = icon_html("play", "ic st-ic") + "<div>Playing " + html_escape(run.title) + "</div><div class=\"st-link\" data-action=\"stop\">Stop</div>";
        message_class = "ok";
    } else if (!message_.empty()) {
        message = "<div class=\"nowrap\">" + html_escape(message_) + "</div>";
        message_class = message_kind_ == 2 ? "err" : message_kind_ == 1 ? "ok" : "";
    }
    std::string bar;
    if (progress) {
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%d%%", int(s.fraction * 100 + 0.5));
        char width[24];
        std::snprintf(width, sizeof(width), "%.1f%%", std::clamp(s.fraction, 0.0, 1.0) * 100);
        bar = "<div>Preparing " + html_escape(prepare_title_) + "</div><div class=\"minibar\"><div class=\"minibar-fill\" style=\"width: " +
              std::string(width) + ";\"></div></div><div style=\"margin-left: 6dp;\">" + percent + "</div>";
    }
    const std::string sig = count + "|" + folder + "|" + message + message_class + "|" + bar;
    if (sig == status_sig_) return;
    status_sig_ = sig;
    if (Rml::Element *e = by_id("st-count")) e->SetInnerRML(html_escape(count));
    if (Rml::Element *e = by_id("st-folder")) e->SetInnerRML(html_escape(folder));
    if (Rml::Element *e = by_id("st-message")) {
        e->SetInnerRML(message);
        e->SetClass("err", message_class == "err");
        e->SetClass("ok", message_class == "ok");
    }
    if (Rml::Element *e = by_id("st-progress")) {
        e->SetInnerRML(bar);
        e->SetClass("on", progress);
    }
}

// ---------------------------------------------------------------------------
// the settings window and the games folder

void LauncherView::open_settings(SettingsTab tab) {
    if (settings_) {
        settings_->show_tab(tab);
        if (!opt_.offscreen) SDL_RaiseWindow(settings_->window());
        return;
    }
    settings_ = std::make_unique<SettingsView>(core_, *this);
    std::string error;
    if (!settings_->open(window_, assets_, tab, opt_.scale, opt_.offscreen, error)) {
        settings_.reset();
        message_box("The settings cannot be shown", error);
        return;
    }
    settings_->set_game(selected_game());
}

void LauncherView::close_settings_window() { settings_.reset(); }

namespace {

// a folder the system's dialog picked, handed over from SDL's callback
struct Picked {
    std::mutex lock;
    std::string path;
    bool ready = false;
};
Picked picked;

void SDLCALL on_picked(void *, const char *const *files, int) {
    std::lock_guard guard(picked.lock);
    if (files && files[0]) picked.path = files[0], picked.ready = true;
}

} // namespace

void LauncherView::browse_folder(SDL_Window *parent) {
    SDL_ShowOpenFolderDialog(on_picked, nullptr, parent ? parent : window_, core_.games_folder().empty() ? nullptr : core_.games_folder().c_str(), false);
}

void LauncherView::update_input() {
    {
        std::lock_guard guard(picked.lock);
        if (picked.ready) {
            picked.ready = false;
            const std::string folder = picked.path;
            defer([this, folder] { set_folder(folder); });
        }
    }
    for (Repeat *r : {&pad_repeat_, &stick_repeat_})
        if (r->active && now_ >= r->next) {
            move_selection(r->delta);
            r->next = now_ + 0.09;
        }
}

void LauncherView::set_folder(const std::string &folder) {
    std::string message;
    if (!core_.set_games_folder(folder, true, message)) message_box("The folder was not saved", message);
    selected_ = -1;
    rows_sig_.clear();
    cached_.clear(), stale_.clear();
    last_listing_ = Clock::time_point();
    if (settings_) settings_->set_game(nullptr), settings_->folder_changed();
}

void LauncherView::rescan() {
    core_.rescan();
    selected_ = -1;
    rows_sig_.clear();
    cached_.clear(), stale_.clear();
    last_listing_ = Clock::time_point();
    if (settings_) settings_->set_game(nullptr);
}

void LauncherView::open_path(const std::filesystem::path &path) {
    std::string url = "file:///";
    for (char c : utf8(path)) {
        if (c == '\\') url += '/';
        else if (c == ' ') url += "%20";
        else if (c == '#') url += "%23";
        else url += c;
    }
    if (!SDL_OpenURL(url.c_str())) set_message(std::string("Could not open ") + utf8(path) + ": " + SDL_GetError(), 2, 8);
}

} // namespace xf::frontend
