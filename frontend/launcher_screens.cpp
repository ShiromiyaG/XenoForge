// The launcher's main window (launcher_view.h): its menus, its dialogs, a game
// being prepared, and the once-a-frame update.

#include "launcher_view.h"

#include "xenoforge/game_build.h"

#include <algorithm>
#include <cstdio>

namespace xf::frontend {

using Clock = std::chrono::steady_clock;

namespace {

const char *const MENUS[] = {"file", "emulation", "options", "tools", "help"};

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

MenuItem item(const char *action, const std::string &label, const char *shortcut, const char *icon, bool enabled = true, const char *arg = "") {
    MenuItem m;
    m.action = action, m.label = label, m.shortcut = shortcut, m.icon = icon, m.enabled = enabled, m.arg = arg;
    return m;
}

} // namespace

// ---------------------------------------------------------------------------
// menus

std::vector<MenuItem> LauncherView::menu_items(const std::string &name) {
    const GameEntry *g = selected_game();
    const bool running = core_.run().running;
    const bool building = job_.active();
    const GameStatus status = g ? status_of(*g) : GameStatus::Ready;
    const bool playable = g && player_of(*g) && !running;
    const char *prepare = status == GameStatus::NeedsPreparing ? "Prepare" : status == GameStatus::Interpreted ? "Try preparing again" : "Rebuild the library";
    if (name == "file")
        return {item("browse", "Open games folder...", "Ctrl+O", "folder-open"), item("rescan", "Rescan the folder", "F5", "refresh-cw"), menu_separator(),
                item("exit", "Exit", "Alt+F4", "log-out")};
    if (name == "emulation") {
        std::vector<MenuItem> m = {item("play", "Play", "Enter", "play", playable), item("play-interpreted", "Play interpreted", "", "cpu", playable),
                                   item("prepare", prepare, "", "hammer", g && !running && !building), menu_separator(),
                                   item("stop", "Stop", "", "square", running)};
        if (building && dialog_ != Dialog::Prepare) m.push_back(item("show-prepare", "Show the build's progress", "", "loader-circle"));
        return m;
    }
    if (name == "options") {
        std::vector<MenuItem> m = {item("settings", "Settings...", "Ctrl+,", "settings"), item("controls", "Controls...", "", "gamepad-2"),
                                   item("game-settings", "Game settings...", "F2", "sliders-horizontal", g != nullptr),
                                   menu_separator()};
        const ThemeChoice theme = theme_from_text(core_.launcher().theme);
        const std::pair<ThemeChoice, const char *> themes[] = {
            {ThemeChoice::System, "The system's theme"}, {ThemeChoice::Light, "Light theme"}, {ThemeChoice::Dark, "Dark theme"}};
        for (const auto &[choice, label] : themes) {
            MenuItem t = item("theme", label, "", "", true, theme_text(choice));
            t.checked = choice == theme;
            m.push_back(t);
        }
        return m;
    }
    if (name == "tools")
        return {item("open-game-folder", "Open the disc's folder", "", "folder", g != nullptr), item("copy-id", "Copy the title ID", "", "copy", g != nullptr),
                menu_separator(), item("open-cache", "Open the game libraries' folder", "", "package"),
                item("open-config", "Open the settings file", "", "file-text"), item("open-logs", "Open the logs folder", "", "folder")};
    if (name == "help") return {item("about", "About XenoForge", "", "info")};
    // a row's own menu
    return {item("play", "Play", "Enter", "play", playable), item("play-interpreted", "Play interpreted", "", "cpu", playable),
            item("prepare", prepare, "", "hammer", g && !running && !building), menu_separator(),
            item("game-settings", "Game settings...", "F2", "sliders-horizontal", g != nullptr), menu_separator(),
            item("open-game-folder", "Open the disc's folder", "", "folder", g != nullptr), item("copy-id", "Copy the title ID", "", "copy", g != nullptr),
            item("copy-path", "Copy the disc's path", "", "copy", g != nullptr)};
}

void LauncherView::open_menu(const std::string &name, bool keyboard) {
    // the layer below the menu bar catches a click outside the menu; the menu
    // itself is in a host at the window's corner, so its place is the
    // window's coordinates (the layer has no place of its own until it has
    // been laid out open once)
    Rml::Element *layer = by_id("menu-layer");
    Rml::Element *host = by_id("menu-host");
    if (!layer || !host) return;
    if (!menu_.empty() && menu_ != "context")
        if (Rml::Element *t = by_id(("menu-" + menu_).c_str())) t->SetClass("open", false);
    menu_ = name;
    menu_keyboard_ = keyboard;
    menu_items_ = menu_items(name);
    host->SetInnerRML(menu_html(menu_items_));
    layer->SetClass("open", true);
    hot_ = -1;
    Rml::Element *menu = by_id("menu");
    Rml::Element *title = by_id(("menu-" + name).c_str());
    if (menu && title) {
        title->SetClass("open", true);
        const Rml::Vector2f at = title->GetAbsoluteOffset(Rml::BoxArea::Border);
        menu->SetProperty("left", px(at.x));
        menu->SetProperty("top", px(at.y + title->GetBox().GetSize(Rml::BoxArea::Border).y + 1 * dp()));
    }
    if (keyboard) menu_move(1);
    settle_until_ = std::max(settle_until_, now_ + 0.3);
}

void LauncherView::open_context_menu(float x, float y) {
    open_menu("context", false);
    Rml::Element *menu = by_id("menu");
    if (!menu) return;
    // at the pointer, in the window's coordinates
    menu->SetProperty("left", px(x));
    menu->SetProperty("top", px(y));
    // kept inside the window once it has its size (the frame after next)
    defer([this, x, y] {
        defer([this, x, y] {
            Rml::Element *menu = by_id("menu");
            if (!menu || menu_ != "context") return;
            const Rml::Vector2f size = menu->GetBox().GetSize(Rml::BoxArea::Border);
            const float room_w = float(ctx_->GetDimensions().x), room_h = float(ctx_->GetDimensions().y);
            if (x + size.x > room_w) menu->SetProperty("left", px(std::max(0.f, room_w - size.x - 4 * dp())));
            if (y + size.y > room_h) menu->SetProperty("top", px(std::max(0.f, y - size.y)));
        });
    });
}

void LauncherView::close_menu() {
    if (menu_.empty()) return;
    if (Rml::Element *t = by_id(("menu-" + menu_).c_str())) t->SetClass("open", false);
    if (Rml::Element *layer = by_id("menu-layer")) layer->SetClass("open", false);
    if (Rml::Element *host = by_id("menu-host")) host->SetInnerRML("");
    menu_.clear();
    menu_items_.clear();
    hot_ = -1;
}

void LauncherView::menu_move(int delta) {
    Rml::Element *menu = by_id("menu");
    if (!menu) return;
    Rml::ElementList items;
    menu->QuerySelectorAll(items, ".mi");
    if (items.empty()) return;
    int at = hot_;
    for (size_t tries = 0; tries < items.size(); tries++) {
        at = at < 0 ? (delta > 0 ? 0 : int(items.size()) - 1) : (at + delta + int(items.size())) % int(items.size());
        if (items[size_t(at)]->HasAttribute("data-action")) break;
    }
    hot_ = at;
    for (size_t i = 0; i < items.size(); i++) items[i]->SetClass("hot", int(i) == hot_);
}

void LauncherView::menu_switch(int delta) {
    int at = 0;
    for (int i = 0; i < 5; i++)
        if (menu_ == MENUS[i]) at = i;
    open_menu(MENUS[(at + delta + 5) % 5], true);
}

// ---------------------------------------------------------------------------
// dialogs

void LauncherView::show_dialog(Dialog dialog, const std::string &title, const std::string &body, const std::string &buttons, int width_dp) {
    close_menu();
    Rml::Element *layer = by_id("dialog-layer");
    if (!layer) return;
    layer->SetInnerRML(dialog_html(title, body, buttons, width_dp));
    layer->SetClass("open", true);
    dialog_ = dialog;
    defer([this] {
        if (Rml::Element *b = by_id("dlg-first")) b->Focus(pad_mode_);
    });
    settle_until_ = std::max(settle_until_, now_ + 0.4);
    update_status();
}

void LauncherView::close_dialog() {
    if (dialog_ == Dialog::Prepare && job_.active()) return hide_prepare();
    if (Rml::Element *layer = by_id("dialog-layer")) {
        layer->SetClass("open", false);
        layer->SetInnerRML("");
    }
    dialog_ = Dialog::None;
    update_status();
    update_toolbar();
    if (Rml::Element *row = by_id(("row-" + std::to_string(selected_)).c_str())) defer([row, this] {
            if (by_id(("row-" + std::to_string(selected_)).c_str()) == row) row->Focus(pad_mode_);
        });
}

void LauncherView::message_box(const std::string &title, const std::string &text) {
    show_dialog(Dialog::Message, title, "<div class=\"msg\">" + html_escape(text) + "</div>", button_html("dialog-close", "OK", true, "dlg-first"), 440);
}

void LauncherView::show_about() {
    show_dialog(Dialog::About, "About XenoForge", about_body_html(), button_html("dialog-close", "Close", true, "dlg-first"), 540);
}

// ---------------------------------------------------------------------------
// preparing a game

void LauncherView::begin_prepare(const GameEntry &game, bool demo) {
    PrepareJob::Runner runner = demo ? PrepareJob::demo_runner(opt_.demo_speed, opt_.demo_fail)
                                     : PrepareJob::build_runner(game.path, build::default_cache_dir(), opt_.build_jobs);
    prepare_title_ = game.title;
    prepare_title_id_ = game.title_id;
    prepare_listed_ = false;
    prepare_game_ = demo && !selected_game() ? -1 : selected_;
    job_.start(game.title, std::move(runner));
    prepare_stage_ = 0;
    log_open_ = opt_.show_log;
    show_prepare();
    update_toolbar();
}

void LauncherView::show_prepare() {
    if (!job_.state().started) return;
    prepare_hidden_ = false;
    prepare_kind_.clear();
    last_prep_count_.clear(), last_prep_time_.clear(), last_prep_file_.clear(), last_prep_state_.clear();
    prepare_log_lines_ = 0;
    std::string icon;
    if (prepare_game_ >= 0 && prepare_game_ < long(listing_.games.size())) icon = icon_name(listing_.games[size_t(prepare_game_)]);
    show_dialog(Dialog::Prepare, "Preparing " + prepare_title_, prepare_body_html(prepare_title_, icon, prepare_title_id_), "", 580);
    if (Rml::Element *log = by_id("log")) log->SetClass("open", log_open_);
    if (Rml::Element *t = by_id("log-toggle")) t->SetClass("open", log_open_);
    if (Rml::Element *t = by_id("log-toggle-text")) t->SetInnerRML(log_open_ ? "Hide log" : "Show log");
    update_prepare();
}

void LauncherView::hide_prepare() {
    prepare_hidden_ = true;
    if (Rml::Element *layer = by_id("dialog-layer")) {
        layer->SetClass("open", false);
        layer->SetInnerRML("");
    }
    dialog_ = Dialog::None;
    update_status();
}

void LauncherView::update_prepare() {
    const PrepareJob::State s = job_.state();
    if (!s.started) return;
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
    const bool failed = s.finished && !s.ok && !s.cancelled;
    const bool done = s.finished && s.ok;
    const std::string kind = s.cancelled ? "cancelled" : failed ? "failed" : done ? "done" : "running";

    // the end of a build: the game's status, and a word in the status bar when
    // its dialog is not showing
    if (s.finished && !prepare_listed_) {
        prepare_listed_ = true;
        cached_.clear(), rows_sig_.clear();
        if (failed) failed_.insert(prepare_title_id_);
        if (done) failed_.erase(prepare_title_id_);
        if (dialog_ != Dialog::Prepare || s.cancelled) {
            if (s.cancelled) set_message("Preparing " + prepare_title_ + " was cancelled", 0, 8);
            else if (failed) set_message("Preparing " + prepare_title_ + " failed: " + s.error, 2, 0);
            else set_message(prepare_title_ + " is prepared and ready to play", 1, 12);
        }
        if (s.cancelled && dialog_ == Dialog::Prepare) {
            dialog_ = Dialog::None;
            close_dialog();
        }
        update_toolbar();
        update_status();
    }
    if (dialog_ != Dialog::Prepare) return;

    for (int i = 0; i < 5; i++) {
        Rml::Element *stage = by_id(("stage-" + std::to_string(i)).c_str());
        Rml::Element *in = by_id(("stage-in-" + std::to_string(i)).c_str());
        if (!stage || !in) continue;
        const bool is_done = done || i < prepare_stage_;
        stage->SetClass("done", is_done);
        stage->SetClass("now", !done && !failed && i == prepare_stage_);
        stage->SetClass("bad", failed && i == prepare_stage_);
        double fraction = is_done ? 1.0 : (i == prepare_stage_ && s.total ? double(s.done) / double(s.total) : 0.0);
        if (failed && i == prepare_stage_) fraction = std::max(fraction, 0.4);
        in->SetProperty("width", pct(fraction));
    }
    if (Rml::Element *bar = by_id("pbar")) bar->SetClass("failed", failed), bar->SetClass("done", done);
    if (Rml::Element *fill = by_id("pfill")) fill->SetProperty("width", pct(done ? 1.0 : s.fraction));

    auto set_text = [&](const char *id, std::string &last, const std::string &text) {
        if (text == last) return;
        last = text;
        if (Rml::Element *e = by_id(id)) e->SetInnerRML(text);
    };
    std::string state_text;
    if (failed) {
        static const char *doing[] = {"reading the disc", "analyzing the game", "writing code", "compiling", "linking"};
        state_text = std::string("Stopped while ") + doing[std::clamp(prepare_stage_, 0, 4)];
    } else if (done) {
        state_text = "The game library is built: ready to play";
    } else if (job_.active() && s.detail == "cancelled") {
        state_text = "Cancelling";
    } else {
        state_text = PrepareJob::stage_name(s.stage);
    }
    set_text("prep-state", last_prep_state_, html_escape(state_text));
    if (Rml::Element *d = by_id("prep-dot")) {
        d->SetClass("bad", failed);
        d->SetClass("ready", done);
        d->SetClass("prep", !failed && !done);
    }
    const double fraction = done ? 1.0 : s.fraction;
    std::string count = "<b>" + std::to_string(int(fraction * 100 + 0.5)) + "%</b>";
    if (!done && !failed && s.total > 1)
        count += "<span class=\"dim\">   " + std::string(PrepareJob::stage_name(s.stage)) + "  \xC2\xB7  " + std::to_string(s.done) + " of " +
                 std::to_string(s.total) + " files</span>";
    else if (!done && !failed)
        count += "<span class=\"dim\">   " + std::string(PrepareJob::stage_name(s.stage)) + "</span>";
    else
        count += std::string("<span class=\"dim\">   ") + (failed ? "stopped" : "done") + "</span>";
    set_text("prep-count", last_prep_count_, count);
    std::string time = "Elapsed " + format_duration(s.elapsed);
    if (s.running && s.remaining >= 0) time += "  \xC2\xB7  about " + format_duration(s.remaining) + " left";
    set_text("prep-time", last_prep_time_, time);
    set_text("prep-file", last_prep_file_, html_escape(failed || done ? std::string() : s.detail));

    if (Rml::Element *err = by_id("prep-error")) {
        err->SetClass("on", failed);
        if (failed && err->GetInnerRML().empty()) {
            err->SetInnerRML("<b>The build stopped.</b> " + html_escape(s.error) +
                             "<div class=\"dim\" style=\"margin-top: 4dp;\">The game can still run interpreted, which is slow. Nothing was changed on "
                             "your disc.</div>");
            if (!log_open_) {
                log_open_ = true;
                if (Rml::Element *log = by_id("log")) log->SetClass("open", true);
                if (Rml::Element *t = by_id("log-toggle")) t->SetClass("open", true);
                if (Rml::Element *t = by_id("log-toggle-text")) t->SetInnerRML("Hide log");
            }
        }
    }
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
            log->SetScrollTop(log->GetScrollHeight());
        }
        prepare_log_lines_ = s.log_lines;
    }
    if (kind != prepare_kind_) {
        prepare_kind_ = kind;
        std::string html;
        if (kind == "running") {
            html = button_html("prep-hide", "Run in background", false, "dlg-first") + button_html("prep-cancel", "Cancel");
        } else if (kind == "failed") {
            const bool playable = prepare_game_ >= 0 && prepare_game_ < long(listing_.games.size()) && player_of(listing_.games[size_t(prepare_game_)]) != nullptr;
            html = button_html("prep-retry", "Try again", true, "dlg-first", "refresh-cw");
            if (playable) html += button_html("prep-interpreted", "Play interpreted", false, {}, "cpu");
            html += button_html("dialog-close", "Close");
        } else {
            html = button_html("prep-play", "Play", true, "dlg-first", "play") + button_html("dialog-close", "Close");
        }
        if (Rml::Element *foot = by_id("dlg-foot")) foot->SetInnerRML(html);
        defer([this] {
            if (Rml::Element *b = by_id("dlg-first")) b->Focus(pad_mode_);
        });
        update_toolbar();
    }
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

    // a game's end: how it went, in the status bar and the console
    if (core_.poll()) {
        const LauncherCore::Run &run = core_.run();
        const double seconds = std::chrono::duration<double>(Clock::now() - run.started).count();
        std::printf("launcher: %s ended after %.1f s\n", run.title.c_str(), seconds);
        for (const std::string &line : run.ended) std::printf("  %s\n", line.c_str());
        std::string summary = run.title + " ended after " + format_duration(seconds);
        if (!run.ended.empty()) {
            std::string last = run.ended.back();
            if (last.starts_with("player: ")) last.erase(0, 8);
            summary += " (" + last + ")";
        }
        set_message(summary, 0, 15);
        rows_sig_.clear();
        if (settings_) settings_->set_game(selected_game()); // the overlay may have changed the settings
    }
    poll_listing();
    rebuild_rows();

    // the checks' and pictures' requests, once there is something to show
    if (demo_pending_ && (!listing_.games.empty() || !listing_.busy)) {
        demo_pending_ = false;
        GameEntry fake;
        fake.title = "El Chavo Kart", fake.title_id = 0x475807D7;
        const GameEntry *g = selected_game();
        begin_prepare(g ? *g : fake, true);
    }
    if (start_pending_ >= 0) {
        if (!listing_.busy && start_pending_ >= long(listing_.games.size())) {
            std::fprintf(stderr, "launcher: no game %ld to start (%zu listed)\n", start_pending_, listing_.games.size());
            start_pending_ = -1;
        } else if (selected_ == start_pending_ && selected_game() && !core_.run().running && core_.run().ended.empty()) {
            start_pending_ = -1;
            play(*selected_game(), false); // the button's path: a disc not yet built is prepared first
        }
    }
    if (screen_pending_ && !listing_.busy) {
        screen_pending_ = false;
        if (opt_.screen == Screen::About) show_about();
        else {
            SettingsTab tab = opt_.screen == Screen::Controls ? SettingsTab::Keyboard : SettingsTab::Video;
            for (int i = 0; i < SETTINGS_TABS; i++)
                if (opt_.tab == settings_tab_name(SettingsTab(i))) tab = SettingsTab(i);
            open_settings(tab);
        }
        if (settings_ && !capture_pending_.empty()) {
            settings_->begin_capture(capture_pending_);
            capture_pending_.clear();
        }
    }
    if (!menu_pending_.empty() && !listing_.busy) {
        open_menu(menu_pending_, true);
        menu_pending_.clear();
    }
    if (context_pending_ && !listing_.busy && selected_game() && now_ > 0.8) { // the rows laid out by then
        context_pending_ = false;
        // the row's place once it has been laid out (the rows may just have been made again)
        defer([this] {
            defer([this] {
                if (Rml::Element *row = by_id(("row-" + std::to_string(selected_)).c_str())) {
                    const Rml::Vector2f at = row->GetAbsoluteOffset(Rml::BoxArea::Border);
                    open_context_menu(at.x + 180 * dp(), at.y + 20 * dp());
                }
            });
        });
    }

    if (job_.state().started) update_prepare();
    if (message_until_ > 0 && now_ >= message_until_) {
        message_until_ = 0;
        message_.clear();
    }
    update_toolbar();
    update_status();
    update_input();
}

} // namespace xf::frontend
