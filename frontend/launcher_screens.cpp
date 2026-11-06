// The launcher's screens put into the document (launcher_view.h): the parts
// rebuilt when what they show changes, the prepare screen followed through
// a build, the toasts, and the once-a-frame update.

#include "launcher_view.h"

#include "xenoforge/game_build.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xf::frontend {

using Clock = std::chrono::steady_clock;

namespace {

const char *SCREEN_IDS[] = {"screen-library", "screen-settings", "screen-controls", "screen-about"};
const char *TAB_IDS[] = {"tab-library", "tab-settings", "tab-controls", "tab-about"};

void set_hidden(Rml::Element *element, bool hidden) {
    if (hidden) element->SetProperty("display", "none");
    else element->RemoveProperty("display");
}

std::string px(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1fpx", value);
    return text;
}

std::string pct(double fraction) {
    char text[24];
    std::snprintf(text, sizeof(text), "%.1f%%", std::clamp(fraction, 0.0, 1.0) * 100.0);
    return text;
}

// a hint: a glyph (a key or a pad button) and what it does
std::string hint(const std::string &glyph, const std::string &label, bool pad) {
    // "LB RB": one glyph for each button
    std::string glyphs;
    size_t at = 0;
    while (at < glyph.size()) {
        const size_t end = pad ? glyph.find(' ', at) : std::string::npos;
        const std::string one = glyph.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? glyph.size() : end + 1;
        glyphs += std::string("<div class=\"glyph") + (pad ? " pad" : "") + (at >= glyph.size() ? " last" : "") + "\">" + html_escape(one) + "</div>";
    }
    return "<div class=\"hint\">" + glyphs + "<div class=\"lbl\">" + html_escape(label) + "</div></div>";
}

} // namespace

// ---------------------------------------------------------------------------
// the library

void LauncherView::rebuild_library() {
    Rml::Element *screen = by_id("screen-library");
    if (!screen) return;
    const bool no_folder = core_.games_folder().empty();
    const bool show_empty = no_folder || (listing_.games.empty() && !listing_.busy);
    std::string sig = std::to_string(listing_.games.size()) + "/" + std::to_string(listing_.total) + (listing_.busy ? "b" : "i") +
                      core_.games_folder() + (show_empty ? "E" : "-") + std::to_string(failed_.size());
    for (const GameEntry &g : listing_.games) sig += g.title_id_text() + std::to_string(g.error.size());
    if (sig == library_sig_ && by_id("library")) return;
    library_sig_ = sig;
    library_empty_ = show_empty;
    settle_until_ = now_ + 1.2;

    // the focused tile, to have focus again
    int focused_index = -1;
    if (Rml::Element *f = ctx_->GetFocusElement())
        if (f->IsClassSet("tile")) focused_index = f->GetAttribute<int>("data-index", -1);

    if (!by_id("library")) {
        screen->SetInnerRML(
            "<div id=\"library\" style=\"width: 100%; height: 100%;\"><div id=\"lib-left\"><div id=\"lib-head\"><h1 class=\"title\">Library</h1>"
            "<div id=\"lib-count\"></div><div id=\"lib-scan\"></div></div><div id=\"grid-wrap\"><div id=\"grid\"></div><div id=\"grid-fade\"></div></div></div>"
            "<div id=\"detail\"></div></div><div id=\"lib-empty\" class=\"empty\" style=\"display: none;\"></div>");
    }
    if (Rml::Element *library = by_id("library")) set_hidden(library, show_empty);
    if (Rml::Element *empty_box = by_id("lib-empty")) {
        set_hidden(empty_box, !show_empty);
        if (show_empty) {
            empty_box->SetInnerRML(empty_html(no_folder, core_.games_folder()));
            if (screen_ == Screen::Library)
                defer([this] {
                    if (Rml::Element *b = by_id("empty-primary")) b->Focus(pad_mode_);
                });
        }
    }
    if (Rml::Element *path = by_id("folder-path")) path->SetInnerRML(html_escape(no_folder ? "No folder chosen" : core_.games_folder()));

    std::vector<TileInfo> tiles;
    for (size_t i = 0; i < listing_.games.size(); i++) {
        const GameEntry &g = listing_.games[i];
        TileInfo t;
        t.index = int(i);
        if (g.error.empty()) {
            t.title = g.title, t.id_text = g.title_id_text(), t.title_id = g.title_id;
            t.icon = icon_name(g);
            t.status = status_of(g);
        } else {
            t.error = "unreadable";
        }
        tiles.push_back(std::move(t));
    }
    std::string html = tiles_html(tiles);
    if (listing_.busy) {
        const size_t left = std::max(listing_.total, listing_.games.size() + 1) - listing_.games.size();
        html += skeleton_html(int(std::min<size_t>(left, 4)));
    }
    if (Rml::Element *grid = by_id("grid")) grid->SetInnerRML(html);
    if (Rml::Element *count = by_id("lib-count")) {
        set_hidden(count, listing_.games.empty());
        count->SetInnerRML(std::to_string(listing_.games.size()) + (listing_.games.size() == 1 ? " game" : " games"));
    }
    if (Rml::Element *scan = by_id("lib-scan")) {
        set_hidden(scan, !listing_.busy);
        scan->SetInnerRML("Reading " + std::to_string(listing_.games.size()) + " of " + std::to_string(listing_.total) + "...");
    }
    detail_sig_.clear();
    rebuild_detail();
    if (focused_index >= 0)
        defer([this, focused_index] {
            Rml::Element *grid = by_id("grid");
            if (!grid) return;
            for (int i = 0; i < grid->GetNumChildren(); i++)
                if (grid->GetChild(i)->GetAttribute<int>("data-index", -1) == focused_index) grid->GetChild(i)->Focus(pad_mode_);
        });
    rebuild_hints();
}

void LauncherView::rebuild_detail() {
    Rml::Element *detail = by_id("detail");
    if (!detail) return;
    const GameEntry *g = selected_game();
    const LauncherCore::Run &run = core_.run();
    const bool running = g && run.running && run.title == g->title;
    std::string sig = g ? g->title_id_text() + std::to_string(int(status_of(*g))) + (running ? "R" : "-") + std::to_string(run.ended.size()) : "none";
    if (!g) sig += listing_.busy ? "scan" : "idle";
    if (sig == detail_sig_) return;
    detail_sig_ = sig;
    if (!g) {
        detail->SetInnerRML(detail_empty_html(listing_.busy));
        return;
    }
    const PlayerBinary *player = player_of(*g);
    DetailInfo info;
    info.title = g->title, info.id_text = g->title_id_text(), info.version = version_text(g->version), info.title_id = g->title_id;
    info.file = g->path.filename().string();
    info.icon = icon_name(*g);
    info.status = status_of(*g);
    info.has_player = player != nullptr;
    info.running = running;
    info.runs_in = !player ? "no player beside the launcher" : player->generic ? "generic player" : player->path.stem().string();
    if (info.status == GameStatus::Ready && player && player->generic && cached_of(*g)) info.runs_in = "generic player, library";
    if (!run.running && run.title == g->title) info.last_run = run.ended;
    detail->SetInnerRML(detail_html(info));
}

void LauncherView::rebuild_settings() {
    Rml::Element *screen = by_id("screen-settings");
    if (!screen) return;
    if (!edit_valid_) reload_edit();
    const GameEntry *g = selected_game();
    SettingsPage page;
    page.settings = &edit_;
    page.game_title = g ? g->title : "";
    page.title_id = g ? g->title_id_text() : "";
    page.folder = core_.games_folder();
    page.accent = accent_from_hex(core_.accent()).base;
    screen->SetInnerRML(settings_html(page));
}

void LauncherView::rebuild_controls() {
    Rml::Element *screen = by_id("screen-controls");
    if (!screen) return;
    if (!edit_valid_) reload_edit();
    const GameEntry *g = selected_game();
    ControlsPage page;
    page.settings = &edit_;
    page.gamepads = LauncherCore::gamepads();
    page.capturing = capturing_;
    page.game_title = g ? g->title : "";
    screen->SetInnerRML(controls_html(page));
}

void LauncherView::rebuild_hints() {
    Rml::Element *bar = by_id("hints");
    if (!bar) return;
    std::string html = "<div id=\"hint-left\">";
    html += html_escape(message_.empty() ? std::string("XenoForge  \xC2\xB7  pre-release") : message_);
    html += "</div>";
    const bool pad = pad_mode_;
    if (prepare_open_) {
        html += hint(pad ? "B" : "Esc", job_.active() ? "Cancel" : "Close", pad);
        html += hint(pad ? "A" : "Enter", "Select", pad);
    } else {
        if (screen_ == Screen::Library && library_empty_) {
            html += hint(pad ? "A" : "Enter", "Choose folder", pad);
        } else if (screen_ == Screen::Library) {
            html += hint(pad ? "A" : "Enter", "Play", pad);
            html += hint(pad ? "X" : "F2", "Game settings", pad);
            html += hint(pad ? "Y" : "F5", "Rescan", pad);
        } else if (screen_ == Screen::Controls) {
            html += hint(pad ? "A" : "Enter", "Set a key", pad);
            html += hint(pad ? "B" : "Esc", "Back", pad);
        } else if (screen_ == Screen::Settings) {
            html += hint(pad ? "A" : "Enter", "Change", pad);
            html += hint(pad ? "\xE2\x86\x90\xE2\x86\x92" : "\xE2\x86\x90 \xE2\x86\x92", "Adjust", pad);
            html += hint(pad ? "B" : "Esc", "Back", pad);
        } else {
            html += hint(pad ? "B" : "Esc", "Back", pad);
        }
        html += hint(pad ? "LB RB" : "Tab", "Page", pad);
    }
    bar->SetInnerRML(html);
}

void LauncherView::show_screen(Screen screen, bool instant) {
    if (capturing_.size()) end_capture(false);
    close_popup();
    const Screen before = screen_;
    screen_ = screen;
    settle_until_ = std::max(settle_until_, now_ + 1.0);
    for (int i = 0; i < 4; i++) {
        if (Rml::Element *e = by_id(SCREEN_IDS[i])) {
            e->SetClass("active", i == int(screen));
            e->SetClass("left", i < int(screen));
            // a page that is gone is not drawn at all, once it has faded
            if (i == int(screen)) e->RemoveProperty("visibility");
            else if (instant) e->SetProperty("visibility", "hidden");
            else
                defer_until(now_ + 0.4, [this, i] {
                    if (int(screen_) != i)
                        if (Rml::Element *page = by_id(SCREEN_IDS[i])) page->SetProperty("visibility", "hidden");
                });
        }
        if (Rml::Element *t = by_id(TAB_IDS[i])) t->SetClass("active", i == int(screen));
    }
    if (screen == Screen::Settings && before != screen) edit_valid_ = false, rebuild_settings();
    if (screen == Screen::Controls && before != screen) edit_valid_ = false, rebuild_controls();
    if (instant) pill_frames_ = 3;
    else pill_frames_ = 1;
    rebuild_hints();
    if (before != screen && screen == Screen::Library)
        select_game(int(selected_), true);
}

void LauncherView::place_pill() {
    Rml::Element *pill = by_id("tab-pill"), *tab = by_id(TAB_IDS[int(screen_)]);
    if (!pill || !tab) return;
    pill->SetProperty("left", px(tab->GetOffsetLeft()));
    pill->SetProperty("width", px(tab->GetOffsetWidth() + 4 * dp()));
}

// ---------------------------------------------------------------------------
// preparing a game

void LauncherView::begin_prepare(const GameEntry &game, bool demo) {
    const std::string title = game.title;
    PrepareJob::Runner runner = demo ? PrepareJob::demo_runner(opt_.demo_speed, opt_.demo_fail)
                                     : PrepareJob::build_runner(game.path, build::default_cache_dir(), opt_.build_jobs);
    prepare_title_id_ = game.title_id;
    prepare_listed_ = false;
    prepare_game_ = selected_;
    job_.start(title, std::move(runner));
    prepare_stage_ = 0;
    prepare_log_lines_ = 0;
    prepare_kind_.clear();
    last_prep_count_.clear(), last_prep_time_.clear(), last_prep_file_.clear(), last_prep_state_.clear();
    log_open_ = opt_.show_log;
    Rml::Element *card = by_id("prepare-card");
    if (!card) return;
    card->SetInnerRML(prepare_html(title, icon_name(game), game.title_id));
    if (Rml::Element *p = by_id("prepare")) p->SetClass("open", true), p->SetClass("failed", false);
    prepare_open_ = true;
    if (Rml::Element *log = by_id("log")) log->SetClass("open", log_open_);
    if (Rml::Element *t = by_id("log-toggle")) t->SetClass("open", log_open_);
    if (Rml::Element *t = by_id("log-toggle-text")) t->SetInnerRML(log_open_ ? "Hide log" : "Show log");
    update_prepare();
    rebuild_hints();
}

void LauncherView::close_prepare() {
    if (Rml::Element *p = by_id("prepare")) p->SetClass("open", false);
    prepare_open_ = false;
    rebuild_hints();
    detail_sig_.clear();
    rebuild_library();
    // the card is emptied once it has faded
    prepare_clear_at_ = now_ + 0.4;
    if (screen_ == Screen::Library) select_game(int(selected_), true);
}

void LauncherView::update_prepare() {
    if (!prepare_open_ || !doc_) return;
    const PrepareJob::State s = job_.state();
    using build::Stage;
    int stage_index = -1;
    switch (s.stage) {
    case Stage::Reading: stage_index = 0; break;
    case Stage::Discovering: stage_index = 1; break;
    case Stage::Emitting: stage_index = 2; break;
    case Stage::Compiling: stage_index = 3; break;
    case Stage::Linking: stage_index = 4; break;
    default: break;
    }
    if (stage_index >= 0) prepare_stage_ = stage_index;
    const bool failed = s.finished && !s.ok;
    const bool done = s.finished && s.ok;
    const bool cancelled = s.finished && s.cancelled;
    if (cancelled) return close_prepare(); // asked for: back to the library
    if (s.finished && !prepare_listed_) prepare_listed_ = true, library_sig_.clear(), detail_sig_.clear(); // its card is ready now

    if (Rml::Element *p = by_id("prepare")) p->SetClass("failed", failed);
    // the stages
    for (int i = 0; i < 5; i++) {
        Rml::Element *stage = by_id(("stage-" + std::to_string(i)).c_str());
        Rml::Element *in = by_id(("stage-in-" + std::to_string(i)).c_str());
        if (!stage || !in) continue;
        const bool is_done = done || i < prepare_stage_;
        const bool is_now = !done && i == prepare_stage_ && !failed;
        stage->SetClass("done", is_done);
        stage->SetClass("now", is_now);
        stage->SetClass("bad", failed && i == prepare_stage_);
        double fraction = is_done ? 1.0 : (i == prepare_stage_ && s.total ? double(s.done) / double(s.total) : 0.0);
        if (failed && i == prepare_stage_) fraction = std::max(fraction, 0.4);
        in->SetProperty("width", pct(fraction));
    }
    if (Rml::Element *bar = by_id("pbar")) bar->SetClass("failed", failed), bar->SetClass("done", done);
    if (Rml::Element *fill = by_id("pfill")) fill->SetProperty("width", pct(done ? 1.0 : s.fraction));

    // the words
    auto set_text = [&](const char *id, std::string &last, const std::string &text) {
        if (text == last) return;
        last = text;
        if (Rml::Element *e = by_id(id)) e->SetInnerRML(text);
    };
    std::string state_text, eyebrow;
    if (failed) eyebrow = "Preparation failed";
    else if (done) state_text = "The library is built", eyebrow = "Ready to play";
    else if (job_.active() && s.detail == "cancelled") state_text = "Cancelling", eyebrow = "Preparing";
    else state_text = PrepareJob::stage_name(s.stage), eyebrow = "Preparing";
    if (failed) {
        // "Compiling" reads badly after "while": say what it was doing
        static const char *doing[] = {"reading the disc", "analyzing the game", "writing code", "compiling", "linking"};
        state_text = std::string("Stopped while ") + doing[std::clamp(prepare_stage_, 0, 4)];
    }
    set_text("prep-state", last_prep_state_, state_text);
    if (Rml::Element *e = by_id("prep-eyebrow")) e->SetInnerRML(eyebrow);
    if (Rml::Element *d = by_id("prep-dot")) {
        d->SetClass("bad", failed);
        d->SetClass("ready", done);
        d->SetClass("prep", !failed && !done);
    }
    const double fraction = done ? 1.0 : s.fraction;
    std::string count = "<b style=\"color: #ffffff;\">" + std::to_string(int(fraction * 100 + 0.5)) + "%</b>";
    if (!done && !failed && s.total > 1)
        count += "<span class=\"muted\" style=\"font-weight: 400; font-size: 14dp;\">   " + std::string(PrepareJob::stage_name(s.stage)) + "  \xC2\xB7  " +
                 std::to_string(s.done) + " of " + std::to_string(s.total) + " files</span>";
    else if (!done && !failed)
        count += "<span class=\"muted\" style=\"font-weight: 400; font-size: 14dp;\">   " + std::string(PrepareJob::stage_name(s.stage)) + "</span>";
    else if (failed)
        count += "<span class=\"muted\" style=\"font-weight: 400; font-size: 14dp;\">   stopped</span>";
    else
        count += "<span class=\"muted\" style=\"font-weight: 400; font-size: 14dp;\">   done</span>";
    set_text("prep-count", last_prep_count_, count);
    std::string time = "Elapsed " + format_duration(s.elapsed);
    if (s.running && s.remaining >= 0) time += "  \xC2\xB7  about " + format_duration(s.remaining) + " left";
    set_text("prep-time", last_prep_time_, time);
    set_text("prep-file", last_prep_file_, html_escape(failed || done ? std::string() : s.detail));

    // the failure
    if (Rml::Element *err = by_id("prep-error")) {
        set_hidden(err, !failed);
        if (failed && err->GetInnerRML().empty()) {
            err->SetInnerRML("<b>The build stopped.</b><br/>" + html_escape(s.error) +
                             "<br/><span class=\"dim\">The game can still run interpreted, which is slow. Nothing was changed on your disc.</span>");
            if (!log_open_) {
                log_open_ = true;
                if (Rml::Element *log = by_id("log")) log->SetClass("open", true);
                if (Rml::Element *t = by_id("log-toggle")) t->SetClass("open", true);
                if (Rml::Element *t = by_id("log-toggle-text")) t->SetInnerRML("Hide log");
            }
        }
    }
    // the log
    if (s.log_lines > prepare_log_lines_) {
        if (Rml::Element *log = by_id("log")) {
            for (const std::string &line : job_.log_from(prepare_log_lines_)) {
                Rml::ElementPtr row = doc_->CreateElement("div");
                row->SetClass("l", true);
                const bool detail = line.starts_with("  ");
                if (!detail) row->SetClass("head", true);
                if (line.starts_with("Failed")) row->SetClass("err", true);
                row->SetInnerRML(html_escape(detail ? line.substr(2) : line));
                log->AppendChild(std::move(row));
            }
            // the newest line in view
            log->SetScrollTop(log->GetScrollHeight());
        }
        prepare_log_lines_ = s.log_lines;
    }
    // the buttons
    const std::string kind = failed ? "failed" : done ? "done" : "running";
    if (kind != prepare_kind_) {
        prepare_kind_ = kind;
        Rml::Element *actions = by_id("prep-actions");
        if (actions) {
            std::string html;
            if (kind == "running") {
                html = "<div class=\"btn f\" data-action=\"prep-cancel\" id=\"prep-first\">Cancel</div>";
            } else if (kind == "failed") {
                html = "<div class=\"btn f\" data-action=\"prep-close\">Close</div>";
                const bool playable = prepare_game_ >= 0 && prepare_game_ < long(listing_.games.size()) &&
                                      player_of(listing_.games[size_t(prepare_game_)]) != nullptr;
                if (playable)
                    html += "<div class=\"btn f\" data-action=\"prep-interpreted\">Play interpreted</div>";
                html += "<div class=\"btn primary f\" data-action=\"prep-retry\" id=\"prep-first\">Try again</div>";
                failed_.insert(prepare_title_id_);
            } else {
                html = "<div class=\"btn f\" data-action=\"prep-close\">Close</div>";
                html += "<div class=\"btn primary row f\" data-action=\"prep-play\" id=\"prep-first\"><div class=\"play-icon\" style=\"border-width: 8dp 0 8dp 13dp;\"></div><div class=\"lbl\">Play</div></div>";
                failed_.erase(prepare_title_id_);
            }
            actions->SetInnerRML(html);
            defer([this] {
                if (Rml::Element *b = by_id("prep-first")) b->Focus(pad_mode_);
            });
        }
        rebuild_hints();
    }
}

// ---------------------------------------------------------------------------
// toasts

void LauncherView::toast(const std::string &text, bool error, bool with_stop, double seconds) {
    Rml::Element *box = by_id("running");
    if (!box) return;
    toast_text_ = text, toast_error_ = error, toast_stop_ = with_stop;
    toast_until_ = seconds > 0 ? now_ + seconds : 0;
    std::string html = std::string("<div class=\"toast") + (error ? " err" : "") + "\" id=\"toast\"><div class=\"dot " + (error ? "bad" : "ready") +
                       "\"></div><div class=\"msg nowrap\">" + html_escape(text) + "</div>";
    if (with_stop) html += "<div class=\"btn f\" data-action=\"stop\">Stop</div>";
    html += "</div>";
    box->SetInnerRML(html);
    defer([this] {
        if (Rml::Element *t = by_id("toast")) t->SetClass("on", true);
    });
}

void LauncherView::clear_toast() {
    toast_until_ = 0;
    toast_text_.clear();
    if (Rml::Element *t = by_id("toast")) t->SetClass("on", false);
    defer_until(now_ + 0.4, [this] {
        if (toast_text_.empty())
            if (Rml::Element *box = by_id("running")) box->SetInnerRML("");
    });
}

// ---------------------------------------------------------------------------
// once a frame

void LauncherView::update() {
    const Clock::time_point t = Clock::now();
    const double dt = std::chrono::duration<double>(t - last_update_).count();
    last_update_ = t;
    now_ += dt;

    auto work = std::move(deferred_);
    deferred_.clear();
    for (auto &fn : work) fn();
    for (size_t i = 0; i < timers_.size();) {
        if (timers_[i].first <= now_) {
            auto fn = std::move(timers_[i].second);
            timers_.erase(timers_.begin() + long(i));
            fn();
        } else {
            i++;
        }
    }

    // a game's end: how it went, and the settings again (the overlay may
    // have changed them)
    if (core_.poll()) {
        const LauncherCore::Run &run = core_.run();
        const double seconds = std::chrono::duration<double>(Clock::now() - run.started).count();
        std::printf("launcher: %s ended after %.1f s\n", run.title.c_str(), seconds);
        for (const std::string &line : run.ended) std::printf("  %s\n", line.c_str());
        edit_valid_ = false;
        detail_sig_.clear();
        std::string summary = run.title + " ended after " + format_duration(seconds);
        toast(summary, false, false, 7);
    }
    poll_listing();
    rebuild_library();
    rebuild_detail();

    // the demo build starts once there is a game to name
    if (demo_pending_ && (!listing_.games.empty() || !listing_.busy)) {
        demo_pending_ = false;
        GameEntry fake;
        fake.title = "El Chavo Kart", fake.title_id = 0x475807D7;
        const GameEntry *g = selected_game();
        begin_prepare(g ? *g : fake, true);
    }
    // --start N
    if (start_pending_ >= 0) {
        if (!listing_.busy && start_pending_ >= long(listing_.games.size())) {
            std::fprintf(stderr, "launcher: no game %ld to start (%zu listed)\n", start_pending_, listing_.games.size());
            start_pending_ = -1;
        } else if (selected_ == start_pending_ && selected_game() && !core_.run().running && core_.run().ended.empty()) {
            start_pending_ = -1;
            play(*selected_game(), false); // the button's path: a disc not yet built is prepared first
        }
    }
    if (!capture_pending_.empty() && screen_ == Screen::Controls && by_id("controls-scroll")) {
        begin_capture(capture_pending_);
        capture_pending_.clear();
    }

    if (prepare_open_) update_prepare();
    if (prepare_clear_at_ > 0 && now_ >= prepare_clear_at_ && !prepare_open_) {
        prepare_clear_at_ = 0;
        if (Rml::Element *card = by_id("prepare-card")) card->SetInnerRML("");
    }
    if (toast_until_ > 0 && now_ >= toast_until_) clear_toast();
    // a running game's toast, and its end
    const LauncherCore::Run &run = core_.run();
    if (run.running && !toast_stop_ && !prepare_open_) toast("Playing " + run.title, false, true, 0);
    if (!run.running && toast_stop_) clear_toast(), toast_stop_ = false;

    if (pill_frames_ > 0) {
        place_pill();
        pill_frames_--;
    }
    update_input(dt);
    // smooth scrolling toward what focus asked for
    for (auto it = scroll_targets_.begin(); it != scroll_targets_.end();) {
        Rml::Element *e = it->first;
        const float current = e->GetScrollTop();
        const float next = current + (it->second - current) * float(std::min(1.0, dt * 14.0));
        if (std::fabs(it->second - next) < 0.6f) {
            e->SetScrollTop(it->second);
            it = scroll_targets_.erase(it);
        } else {
            e->SetScrollTop(next);
            ++it;
        }
    }
}

} // namespace xf::frontend
