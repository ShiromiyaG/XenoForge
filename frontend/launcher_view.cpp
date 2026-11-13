// The launcher's main window in RmlUi (launcher_view.h): the document, the
// theme, the state of the games and the table.

#include "launcher_view.h"

#include "stale_library.h"
#include "xenoforge/game_build.h"

#include <algorithm>
#include <cctype>
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
    parse_sort(core_.launcher().sort, sort_, ascending_);
    last_update_ = Clock::now();
    settings_ = std::make_unique<SettingsView>(core_, *this);
    if (!opt_.offscreen) { // a picture of the window needs no sound
        std::string why;
        if (!sound_.open(why)) std::fprintf(stderr, "launcher: %s\n", why.c_str());
    }
}

void LauncherView::play_sound(Cue cue) {
    if (core_.launcher().sounds) sound_.play(cue);
}

LauncherView::~LauncherView() {
    if (settings_) settings_->detach(); // its listeners off the document before it goes
    if (doc_ && ctx_) {
        ctx_->UnloadDocument(doc_);
        ctx_->Update(); // the document goes while this listener is alive
    }
}

bool LauncherView::load(Rml::Context *context, const std::filesystem::path &assets_dir, std::string &error) {
    ctx_ = context;
    assets_ = assets_dir;
    rml_template_ = read_text(assets_ / "launcher.rml");
    rcss_template_ = read_text(assets_ / "theme.rcss") + "\n" + read_text(assets_ / "launcher.rcss") + "\n" +
                    read_text(assets_ / "settings.rcss");
    // the controller button pictures the hint bars name as mem:pad-<button> (ui/assets/glyphs, Xbox 360 set)
    for (const char *button : {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start"}) {
        const std::string file = std::string("360_") + button + ".png";
        std::string name = std::string("pad-") + button;
        for (char &c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        const std::string png = read_text(assets_ / "glyphs" / file);
        if (!renderer_.add_image(name, std::vector<uint8_t>(png.begin(), png.end())))
            std::fprintf(stderr, "launcher: no button picture %s\n", file.c_str());
    }
    // XenoForge's mark, from the one logo (assets/branding, copied beside the pages by the build)
    {
        const std::string png = read_text(assets_ / "xenoforge.png");
        if (!renderer_.add_image("xenoforge", std::vector<uint8_t>(png.begin(), png.end())))
            std::fprintf(stderr, "launcher: no mark picture xenoforge.png\n");
    }
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
    for (const char *type : {"click", "dblclick", "mousedown", "mouseover", "change"}) doc_->AddEventListener(type, this);
    doc_->AddEventListener("focus", this, true);
    doc_->AddEventListener("blur", this, true);
    doc_->Show();
    settings_->attach(ctx_, doc_);
    // the page of the strip that was shown stays shown (no slide: the document is new)
    const bool in_settings = section_ == Section::Settings;
    if (Rml::Element *slide = by_id("slide")) slide->SetClass("to-settings", in_settings);
    if (Rml::Element *e = by_id("library")) e->SetClass("hidden", in_settings);
    if (Rml::Element *e = by_id("spage")) e->SetClass("hidden", !in_settings);
    if (Rml::Element *e = by_id("nav-games")) e->SetClass("on", !in_settings);
    if (Rml::Element *e = by_id("nav-settings")) e->SetClass("on", in_settings);
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
        settings_->detach();
        doc_ = nullptr;
        if (old) ctx_->UnloadDocument(old);
        if (!build_document(error)) std::fprintf(stderr, "launcher: %s\n", error.c_str());
        if (selected_ >= 0) select_game(selected_, true);
    });
}

void LauncherView::look_changed() {
    dark_ = resolve_dark();
    reload_document();
}

void LauncherView::system_theme_changed() {
    if (theme_from_text(core_.launcher().theme) == ThemeChoice::System && resolve_dark() != dark_) look_changed();
}

bool LauncherView::busy() const {
    return listing_.busy || start_pending_ >= 0 || core_.run().running || job_.active() || now_ < settle_until_ || !deferred_.empty() ||
           settings_->busy();
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

// The ribbons behind the list (ui.frag, MODE_BACKDROP), once a frame: the clock
// runs only while the window can be seen (not minimized, hidden or covered);
// the greens are tones of the accent, so the colours change only when the
// accent does.
void LauncherView::update_backdrop() {
    const Settings::Launcher &l = core_.launcher();
    const double dt = std::clamp(now_ - backdrop_seen_, 0.0, 0.1);
    backdrop_seen_ = now_;
    const SDL_WindowFlags flags = window_ ? SDL_GetWindowFlags(window_) : 0;
    // it runs while the window can be seen (focus or not: one beside another is still looked at)
    const bool seen = opt_.offscreen || !(flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED));
    if (l.backdrop == "animated" && seen) backdrop_clock_ += dt;

    int r = 0x4c, g = 0xa9, b = 0x28;
    const std::string accent = accent_from_hex(core_.accent()).base;
    if (accent.size() == 7) std::sscanf(accent.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
    ui::Renderer::Backdrop backdrop;
    backdrop.on = true;
    backdrop.time = float(backdrop_clock_);
    backdrop.strength = l.backdrop == "off" ? 0.f : 1.f;
    backdrop.tint[0] = float(r) / 255.f, backdrop.tint[1] = float(g) / 255.f, backdrop.tint[2] = float(b) / 255.f;
    backdrop.light = !dark_; // the light theme's gradient (#3e4241 to #919191)
    renderer_.set_backdrop(backdrop);
    renderer_.set_msaa(l.smooth_edges ? 4 : 1);
}

// The selected row's glow comes up as the row is selected and goes down as it is left (a box-shadow cannot
// be animated, its opacity can). Only the rows that are, or have just been, selected carry one.
void LauncherView::fade_glow(Rml::Element *row, bool on) {
    Rml::Element *glow = row->QuerySelector(".glow");
    if (!glow && on) {
        Rml::ElementPtr made = doc_->CreateElement("div");
        made->SetClass("glow", true);
        glow = row->InsertBefore(std::move(made), row->GetFirstChild());
    }
    if (!glow) return;
    const Rml::Property zero(0.f, Rml::Unit::NUMBER), one(1.f, Rml::Unit::NUMBER);
    glow->Animate("opacity", on ? one : zero, 0.2f, Rml::Tween(Rml::Tween::Cubic, Rml::Tween::Out), 1, false, 0.f, on ? &zero : &one);
    if (!on) {
        const std::string id = row->GetId();
        defer_until(now_ + 0.3, [this, id] {
            Rml::Element *left = by_id(id.c_str());
            Rml::Element *g = left ? left->QuerySelector(".glow") : nullptr;
            if (g && !left->IsClassSet("sel")) left->RemoveChild(g);
        });
    }
}

void LauncherView::select_game(long index, bool scroll) {
    if (index < 0 || index >= long(listing_.games.size())) return;
    const long before = selected_;
    selected_ = index;
    if (before != index) {
        if (Rml::Element *old = by_id(("row-" + std::to_string(before)).c_str())) old->SetClass("sel", false), fade_glow(old, false);
        if (settings_) settings_->set_game(selected_game());
        if (before >= 0) play_sound(Cue::Focus); // not the first selection of a list: nobody moved
    }
    if (Rml::Element *row = by_id(("row-" + std::to_string(index)).c_str())) {
        row->SetClass("sel", true);
        if (before != index) fade_glow(row, true);
        if (scroll) row->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
    }
    settle_until_ = std::max(settle_until_, now_ + 0.3); // the rows' transition
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
    return std::max(1, int(body->GetClientHeight() / (74 * dp())) - 1); // a row and its gap
}

void LauncherView::save_layout() {
    Settings::Launcher l = core_.launcher();
    l.sort = sort_text(sort_, ascending_);
    std::string error;
    if (!core_.set_launcher(l, error)) std::fprintf(stderr, "launcher: %s\n", error.c_str());
}

// ---------------------------------------------------------------------------
// the table

void LauncherView::rebuild_head() {
    if (Rml::Element *head = by_id("thead")) head->SetInnerRML(table_head_html(sort_, ascending_));
}

void LauncherView::rebuild_rows(bool force) {
    Rml::Element *body = by_id("tbody");
    if (!body) return;
    const bool no_folder = core_.games_folder().empty() && !core_.demo_games(); // the demo games need no folder
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
        k.status = int(status_of(g));
    }
    std::string sig = core_.games_folder() + "|" + std::to_string(listing_.total) + (listing_.busy ? "b" : "i") + "|" + filter_ + "|" +
                      sort_text(sort_, ascending_) + "|" + (run.running ? run.title : "") + "|";
    for (const RowKey &k : keys) sig += k.title + "/" + std::to_string(k.status) + ";";
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
        r.file = keys[i].file;
        if (!g.error.empty()) {
            r.unreadable = true, r.title = keys[i].title, r.file = g.error;
        } else {
            r.title = g.title, r.id_text = g.title_id_text(), r.version = version_text(g.version), r.title_id = g.title_id;
            r.icon = icon_name(g), r.status = GameStatus(keys[i].status);
            r.running = run.running && run.title == g.title;
        }
        html += table_row_html(r, long(i) == selected_);
    }
    body->SetInnerRML(html);
    // The rows shown one by one as the folder is read stay as they are once it is read: brought in again
    // whole, from nothing, they would vanish and come back. A list that appears whole (read before it was
    // first drawn, sorted again) comes in.
    if (listing_.busy && !order_.empty()) rows_shown_reading_ = true;
    if (animate_rows_ && !listing_.busy && !order_.empty()) { // the whole list is in: it comes in
        animate_rows_ = false;
        if (!rows_shown_reading_) animate_rows_in();
    }
    if (!listing_.busy) rows_shown_reading_ = false; // (a new sort after the reading comes in)

    const bool filtered = !filter_.empty() && order_.empty() && !listing_.games.empty();
    const bool loading = listing_.busy && listing_.games.empty() && !no_folder; // nothing in yet: a spinner, not a blank list
    const bool show_empty = no_folder || filtered || loading || (listing_.games.empty() && !listing_.busy);
    if (Rml::Element *table = by_id("table")) {
        table->SetClass("no-rows", show_empty);
        table->SetClass("has-games", !listing_.games.empty()); // the search box is for a list that has games
    }
    if (Rml::Element *empty = by_id("tempty")) {
        empty->SetClass("on", show_empty);
        empty->SetInnerRML(show_empty ? empty_html(no_folder, filtered, loading, core_.games_folder()) : std::string());
    }
    update_toolbar();
    update_status();
}

// The rows slide in from the left and fade in, one after another (the first ten; the rest with the tenth),
// as the console's list does when a page opens. A hold at nothing for the row's turn comes first: an
// animation's delay would leave the row shown at rest until its turn.
void LauncherView::animate_rows_in() {
    Rml::Element *body = by_id("tbody");
    if (!body) return;
    const Rml::Tween out(Rml::Tween::Cubic, Rml::Tween::Out);
    const Rml::Property zero(0.f, Rml::Unit::NUMBER), one(1.f, Rml::Unit::NUMBER);
    const Rml::Property away(-28.f, Rml::Unit::DP), home(0.f, Rml::Unit::DP);
    for (int i = 0, n = body->GetNumChildren(); i < n; i++) {
        Rml::Element *row = body->GetChild(i);
        const float wait = 0.001f + 0.04f * float(std::min(i, 9));
        row->Animate("opacity", zero, wait, Rml::Tween{}, 1, false, 0.f, &zero);
        row->AddAnimationKey("opacity", one, 0.3f, out);
        row->Animate("left", away, wait, Rml::Tween{}, 1, false, 0.f, &away);
        row->AddAnimationKey("left", home, 0.3f, out);
    }
    settle_until_ = std::max(settle_until_, now_ + 0.9);
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
    if (Rml::Element *interpreted = by_id("tb-play-interpreted")) interpreted->SetClass("off", running || !g || !player_of(*g));
    if (Rml::Element *demo = by_id("tb-demo")) demo->SetClass("on", core_.demo_games());
    // the scan under way: the Rescan button's arrows and the status bar's icon turn
    if (Rml::Element *rescan = by_id("tb-rescan"))
        if (Rml::Element *icon = rescan->GetFirstChild()) icon->SetClass("spin", listing_.busy);
    if (Rml::Element *busy = by_id("st-busy")) {
        busy->SetClass("on", listing_.busy);
        if (Rml::Element *icon = busy->GetFirstChild()) icon->SetClass("spin", listing_.busy);
    }
    // B only clears the search. (Set only when it changes: a property set every frame, the same or not, dirties the
    // layout and the whole document is laid out again each frame.)
    if (Rml::Element *back = by_id("hint-b")) {
        const bool shown = !filter_.empty();
        if (back->HasAttribute("data-shown") != shown || !back->HasAttribute("data-built")) {
            if (shown) back->SetAttribute("data-shown", "1");
            else back->RemoveAttribute("data-shown");
            back->SetAttribute("data-built", "1");
            back->SetProperty("display", shown ? "flex" : "none");
        }
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
// the settings page and the games folder

// The strip slides to a page; the one left behind is hidden once it is out of
// sight, so nothing focuses it or reads its keys.
void LauncherView::show_section(Section section) {
    if (section == section_) return;
    section_ = section;
    const bool in_settings = section == Section::Settings;
    close_menu();
    play_sound(Cue::Page);
    if (Rml::Element *focus = ctx_->GetFocusElement()) focus->Blur(); // a search box in the page that goes
    if (Rml::Element *slide = by_id("slide")) slide->SetClass("to-settings", in_settings);
    if (Rml::Element *e = by_id("nav-games")) e->SetClass("on", !in_settings);
    if (Rml::Element *e = by_id("nav-settings")) e->SetClass("on", in_settings);
    // the page that arrives is shown at once, the one that leaves when the slide is done
    if (Rml::Element *e = by_id(in_settings ? "spage" : "library")) e->SetClass("hidden", false);
    const double slid = now_ + 0.5;
    defer_until(slid, [this, in_settings] {
        if (section_ == (in_settings ? Section::Settings : Section::Library))
            if (Rml::Element *e = by_id(in_settings ? "library" : "spage")) e->SetClass("hidden", true);
    });
    settle_until_ = std::max(settle_until_, slid + 0.1);
    if (!in_settings) {
        settings_->leave();
        if (Rml::Element *row = by_id(("row-" + std::to_string(selected_)).c_str())) row->Focus(pad_mode_);
    }
}

void LauncherView::open_settings(SettingsTab tab, bool own) {
    settings_->set_game(selected_game());
    if (own) settings_->use_own_settings(); // "Game settings": the game's own, already checked
    settings_->enter(tab);
    show_section(Section::Settings);
}

void LauncherView::leave_settings() { show_section(Section::Library); }

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

void LauncherView::browse_folder() {
    SDL_ShowOpenFolderDialog(on_picked, nullptr, window_, core_.games_folder().empty() ? nullptr : core_.games_folder().c_str(), false);
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
    animate_rows_ = true;
    cached_.clear(), stale_.clear();
    last_listing_ = Clock::time_point();
    settings_->set_game(nullptr), settings_->folder_changed();
}

void LauncherView::rescan() {
    core_.rescan();
    selected_ = -1;
    rows_sig_.clear();
    animate_rows_ = true;
    cached_.clear(), stale_.clear();
    last_listing_ = Clock::time_point();
    settings_->set_game(nullptr);
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
