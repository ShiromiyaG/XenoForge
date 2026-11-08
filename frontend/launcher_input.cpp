// The launcher's main window (launcher_view.h): the document's events, the
// actions they and the keys and the pad lead to.

#include "launcher_view.h"

#include "xenoforge/game_build.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xf::frontend {

namespace {

constexpr int STICK_THRESHOLD = 17000;

} // namespace

Rml::Element *LauncherView::ancestor_with(Rml::Element *element, const char *attribute) const {
    while (element && !element->HasAttribute(attribute)) element = element->GetParentNode();
    return element;
}

// ---------------------------------------------------------------------------
// the document's events

void LauncherView::ProcessEvent(Rml::Event &event) {
    const Rml::String &type = event.GetType();
    Rml::Element *target = event.GetTargetElement();
    if (!target) return;
    if (type == "click") {
        if (target->GetId() == "menu-layer") return close_menu();
        if (Rml::Element *title = ancestor_with(target, "data-menu")) {
            const std::string name = title->GetAttribute<Rml::String>("data-menu", "");
            if (menu_ == name) close_menu();
            else open_menu(name, false);
            return;
        }
        if (Rml::Element *mi = ancestor_with(target, "data-mi")) {
            if (mi->HasAttribute("data-action")) {
                const std::string action = mi->GetAttribute<Rml::String>("data-action", ""), arg = mi->GetAttribute<Rml::String>("data-arg", "");
                close_menu();
                do_action(action, arg);
            }
            return;
        }
        // a row's Options button: that row's menu, under the button
        if (Rml::Element *opt = ancestor_with(target, "data-row-options")) {
            if (Rml::Element *row = ancestor_with(opt, "data-index")) {
                select_game(row->GetAttribute<int>("data-index", -1), false);
                const Rml::Vector2f at = opt->GetAbsoluteOffset(Rml::BoxArea::Border);
                if (menu_ == "context") close_menu();
                else if (selected_game()) open_context_menu(at.x, at.y + opt->GetBox().GetSize(Rml::BoxArea::Border).y + 2 * dp());
            }
            return;
        }
        if (Rml::Element *a = ancestor_with(target, "data-action")) {
            close_menu();
            do_action(a->GetAttribute<Rml::String>("data-action", ""), a->GetAttribute<Rml::String>("data-arg", ""));
            return;
        }
        close_menu();
        if (Rml::Element *row = ancestor_with(target, "data-index")) select_game(row->GetAttribute<int>("data-index", -1), false);
    } else if (type == "dblclick") {
        if (Rml::Element *row = ancestor_with(target, "data-index")) {
            select_game(row->GetAttribute<int>("data-index", -1), false);
            if (const GameEntry *g = selected_game()) play(*g, false);
        }
    } else if (type == "mousedown") {
        const int button = event.GetParameter<int>("button", 0);
        if (button == 1) {
            if (Rml::Element *row = ancestor_with(target, "data-index")) {
                select_game(row->GetAttribute<int>("data-index", -1), false);
                row->Focus();
                if (selected_game()) open_context_menu(event.GetParameter<float>("mouse_x", 0), event.GetParameter<float>("mouse_y", 0));
            }
        }
    } else if (type == "mouseover") {
        // with a menu open, moving over another title opens that one
        if (!menu_.empty() && menu_ != "context")
            if (Rml::Element *title = ancestor_with(target, "data-menu")) {
                const std::string name = title->GetAttribute<Rml::String>("data-menu", "");
                if (name != menu_) open_menu(name, false);
            }
    } else if (type == "change") {
        if (target->GetId() == "search") {
            const std::string value = target->GetAttribute<Rml::String>("value", "");
            if (value != filter_) {
                filter_ = value;
                if (Rml::Element *clear = by_id("search-clear")) clear->SetClass("on", !filter_.empty());
                rebuild_rows();
            }
        }
    } else if (type == "blur") {
        if (target->GetId() == "search")
            if (Rml::Element *box = by_id("search-box")) box->SetClass("focus", false);
    } else if (type == "focus") {
        if (target->GetId() == "search")
            if (Rml::Element *box = by_id("search-box")) box->SetClass("focus", true);
        if (Rml::Element *row = ancestor_with(target, "data-index"); row && row == target) select_game(row->GetAttribute<int>("data-index", -1), false);
    }
}

// ---------------------------------------------------------------------------
// actions

void LauncherView::do_action(const std::string &name, const std::string &arg) {
    const GameEntry *game = selected_game();
    if (name == "play") {
        if (game) play(*game, false);
    } else if (name == "play-interpreted") {
        if (game) play(*game, true);
    } else if (name == "prepare") {
        if (game && !core_.run().running && !job_.active()) begin_prepare(*game, false);
    } else if (name == "stop") {
        core_.stop();
    } else if (name == "browse") {
        browse_folder(window_);
    } else if (name == "rescan") {
        rescan();
    } else if (name == "settings" || name == "game-settings") {
        open_settings(SettingsTab::Video);
    } else if (name == "controls") {
        open_settings(SettingsTab::Controllers);
    } else if (name == "demo-games") {
        core_.show_demo_games(!core_.demo_games());
        selected_ = -1;
        rows_sig_.clear();
        cached_.clear();
        last_listing_ = Clock::time_point();
        if (settings_) settings_->set_game(nullptr);
        set_message(core_.demo_games() ? "Listing the demo games" : "Listing the games folder", 0, 4);
    } else if (name == "about") {
        show_about();
    } else if (name == "exit") {
        quit_ = true;
    } else if (name == "sort") {
        Column column = Column::Title;
        bool ascending = true;
        parse_sort(arg, column, ascending);
        if (column == sort_) ascending_ = !ascending_;
        else sort_ = column, ascending_ = true;
        rebuild_head();
        rebuild_rows(true);
        save_layout();
    } else if (name == "clear-search") {
        filter_.clear();
        if (Rml::Element *search = by_id("search")) search->SetAttribute("value", "");
        if (Rml::Element *clear = by_id("search-clear")) clear->SetClass("on", false);
        rebuild_rows();
    } else if (name == "dialog-close") {
        close_dialog();
    } else if (name == "show-prepare") {
        show_prepare();
    } else if (name == "prep-hide") {
        hide_prepare();
    } else if (name == "prep-cancel") {
        job_.cancel();
        last_prep_state_.clear();
        if (Rml::Element *e = by_id("prep-state")) e->SetInnerRML("Cancelling");
    } else if (name == "prep-retry") {
        if (prepare_game_ >= 0 && prepare_game_ < long(listing_.games.size())) {
            selected_ = prepare_game_;
            begin_prepare(listing_.games[size_t(prepare_game_)], false);
        } else {
            GameEntry fake;
            fake.title = prepare_title_, fake.title_id = prepare_title_id_;
            begin_prepare(fake, opt_.demo_progress);
        }
    } else if (name == "prep-play" || name == "prep-interpreted") {
        const long index = prepare_game_;
        close_dialog();
        if (index >= 0 && index < long(listing_.games.size())) {
            if (name == "prep-play") start_game(listing_.games[size_t(index)], false);
            else play(listing_.games[size_t(index)], true);
        }
    } else if (name == "toggle-log") {
        log_open_ = !log_open_;
        if (Rml::Element *log = by_id("log")) log->SetClass("open", log_open_);
        if (Rml::Element *t = by_id("log-toggle")) t->SetClass("open", log_open_);
        if (Rml::Element *t = by_id("log-toggle-text")) t->SetInnerRML(log_open_ ? "Hide log" : "Show log");
    } else if (name == "open-game-folder") {
        if (game) open_path(game->path.parent_path());
    } else if (name == "copy-id") {
        if (game) SDL_SetClipboardText(game->title_id_text().c_str()), set_message("Copied " + game->title_id_text(), 0, 4);
    } else if (name == "copy-path") {
        if (game) SDL_SetClipboardText(utf8(game->path).c_str()), set_message("Copied the disc's path", 0, 4);
    } else if (name == "open-cache") {
        std::error_code ec;
        std::filesystem::create_directories(build::default_cache_dir(), ec);
        open_path(build::default_cache_dir());
    } else if (name == "open-config") {
        open_path(core_.config_file());
    } else if (name == "open-logs") {
        const std::filesystem::path logs = core_.config_file().parent_path() / "logs";
        std::error_code ec;
        std::filesystem::create_directories(logs, ec);
        open_path(logs);
    }
}

void LauncherView::play(const GameEntry &game, bool force_interpreted) {
    if (core_.run().running) return;
    if (!player_of(game)) return message_box("The game cannot start", "There is no player beside the launcher to run it.");
    if (force_interpreted) {
        failed_.insert(game.title_id);
        rows_sig_.clear();
        return start_game(game, true);
    }
    if (status_of(game) == GameStatus::NeedsPreparing) return begin_prepare(game, false);
    start_game(game, false);
}

// Start the game from its library: what to start is decided by plan_launch
// (a cached library: the generic player; else the player carrying the game's
// code), how by LauncherCore::start. Interpreted: the generic player told not
// to load a library.
void LauncherView::start_game(const GameEntry &game, bool interpreted) {
    LaunchPlan plan;
    if (!plan_launch(listing_.players, game, cached_of(game) && !interpreted, opt_.player_args, plan))
        return message_box("The game cannot start", "There is no player beside the launcher to run it.");
    if (interpreted) {
        const PlayerBinary *generic = nullptr;
        for (const PlayerBinary &p : listing_.players)
            if (p.generic) generic = &p;
        if (!generic) return message_box("The game cannot start", "There is no generic player beside the launcher to interpret it.");
        plan.program = generic->path, plan.generic = true, plan.from_library = false;
        plan.args.push_back("--interpret");
    }
    std::string message;
    if (!core_.start(game, plan, message)) return message_box("The game did not start", message);
    std::printf("launcher: started %s on %s%s, its output in %s\n", utf8(plan.program.filename()).c_str(), utf8(game.path).c_str(),
                plan.from_library ? " from its cached library" : "", utf8(core_.run().log).c_str());
    message_.clear();
    rows_sig_.clear();
    update_toolbar();
    update_status();
}

// ---------------------------------------------------------------------------
// input

bool LauncherView::on_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (pad_mode_) pad_mode_ = false, SDL_ShowCursor();
        return on_key(e.key.key, e.key.repeat);
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (pad_mode_) pad_mode_ = false, SDL_ShowCursor();
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        pad_mode_ = true;
        return on_pad_button(SDL_GamepadButton(e.gbutton.button), e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        on_pad_axis(SDL_GamepadAxis(e.gaxis.axis), e.gaxis.value);
        return true;
    default: return false;
    }
}

bool LauncherView::on_key(SDL_Keycode key, bool repeat) {
    const SDL_Keymod mods = SDL_GetModState();
    const bool ctrl = mods & SDL_KMOD_CTRL, alt = mods & SDL_KMOD_ALT, shift = mods & SDL_KMOD_SHIFT;
    // a menu open: the keys move in it
    if (!menu_.empty()) {
        switch (key) {
        case SDLK_UP: menu_move(-1); return true;
        case SDLK_DOWN: menu_move(1); return true;
        case SDLK_LEFT:
            if (menu_ != "context") menu_switch(-1);
            return true;
        case SDLK_RIGHT:
            if (menu_ != "context") menu_switch(1);
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: {
            Rml::ElementList items;
            if (Rml::Element *menu = by_id("menu")) menu->QuerySelectorAll(items, ".mi");
            if (hot_ >= 0 && hot_ < int(items.size()) && items[size_t(hot_)]->HasAttribute("data-action")) {
                const std::string action = items[size_t(hot_)]->GetAttribute<Rml::String>("data-action", "");
                const std::string arg = items[size_t(hot_)]->GetAttribute<Rml::String>("data-arg", "");
                close_menu();
                do_action(action, arg);
            }
            return true;
        }
        case SDLK_ESCAPE:
        case SDLK_LALT:
        case SDLK_RALT:
        case SDLK_F10: close_menu(); return true;
        default: break;
        }
    }
    // a dialog: Escape closes it, the arrows move among its buttons
    if (dialog_ != Dialog::None) {
        switch (key) {
        case SDLK_ESCAPE:
            if (!repeat) close_dialog();
            return true;
        case SDLK_LEFT: dialog_navigate(NavDir::Left); return true;
        case SDLK_RIGHT: dialog_navigate(NavDir::Right); return true;
        case SDLK_UP: dialog_navigate(NavDir::Up); return true;
        case SDLK_DOWN: dialog_navigate(NavDir::Down); return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!repeat) {
                Rml::Element *focus = ctx_->GetFocusElement();
                Rml::Element *a = focus ? ancestor_with(focus, "data-action") : nullptr;
                if (!a || !a->Closest("#dialog-layer")) a = by_id("dlg-first"); // what has focus, if it is the dialog's
                if (a) do_action(a->GetAttribute<Rml::String>("data-action", ""), a->GetAttribute<Rml::String>("data-arg", ""));
            }
            return true;
        default: return false;
        }
    }
    // the search box keeps its keys, but for these
    Rml::Element *focus = ctx_->GetFocusElement();
    if (focus && focus->GetTagName() == "input") {
        if (key == SDLK_ESCAPE) {
            if (!filter_.empty()) do_action("clear-search", "");
            else focus->Blur();
            return true;
        }
        if (key == SDLK_DOWN || key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            focus->Blur();
            move_selection(0);
            return true;
        }
        return false;
    }
    if (repeat && key != SDLK_UP && key != SDLK_DOWN && key != SDLK_PAGEUP && key != SDLK_PAGEDOWN) return false;
    if (ctrl && key == SDLK_O) return do_action("browse", ""), true;
    if (ctrl && key == SDLK_F) {
        if (Rml::Element *search = by_id("search")) search->Focus();
        return true;
    }
    if (ctrl && key == SDLK_COMMA) return do_action("settings", ""), true;
    if (alt) {
        const std::pair<SDL_Keycode, const char *> keys[] = {{SDLK_E, "emulation"}, {SDLK_H, "help"}};
        for (const auto &[k, name] : keys)
            if (key == k) return open_menu(name, true), true;
    }
    switch (key) {
    case SDLK_F10:
        if (shift) {
            if (Rml::Element *row = by_id(("row-" + std::to_string(selected_)).c_str())) {
                const Rml::Vector2f at = row->GetAbsoluteOffset(Rml::BoxArea::Border);
                open_context_menu(at.x + 180 * dp(), at.y + 20 * dp());
            }
        } else {
            open_menu("emulation", true);
        }
        return true;
    case SDLK_APPLICATION:
        if (Rml::Element *row = by_id(("row-" + std::to_string(selected_)).c_str())) {
            const Rml::Vector2f at = row->GetAbsoluteOffset(Rml::BoxArea::Border);
            open_context_menu(at.x + 180 * dp(), at.y + 20 * dp());
        }
        return true;
    case SDLK_UP: move_selection(-1); return true;
    case SDLK_DOWN: move_selection(1); return true;
    case SDLK_PAGEUP: move_selection(-visible_rows()); return true;
    case SDLK_PAGEDOWN: move_selection(visible_rows()); return true;
    case SDLK_HOME: move_selection(-1, true); return true;
    case SDLK_END: move_selection(1, true); return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        do_action("play", "");
        return true;
    case SDLK_F2: do_action("game-settings", ""); return true;
    case SDLK_F5: do_action("rescan", ""); return true;
    case SDLK_ESCAPE:
        if (!filter_.empty()) do_action("clear-search", "");
        return true;
    default: return false;
    }
}

bool LauncherView::on_pad_button(SDL_GamepadButton button, bool down) {
    auto repeat = [&](int delta) {
        if (down) {
            move_selection(delta);
            pad_repeat_ = {true, delta, now_ + 0.36};
        } else if (pad_repeat_.active && pad_repeat_.delta == delta) {
            pad_repeat_.active = false;
        }
    };
    if (!menu_.empty()) {
        if (!down) return true;
        switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: menu_move(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: menu_move(1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            if (menu_ != "context") menu_switch(-1);
            break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            if (menu_ != "context") menu_switch(1);
            break;
        case SDL_GAMEPAD_BUTTON_SOUTH: on_key(SDLK_RETURN, false); break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_BACK: close_menu(); break;
        default: break;
        }
        return true;
    }
    if (dialog_ != Dialog::None) {
        if (!down) return true;
        switch (button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: dialog_navigate(NavDir::Up); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: dialog_navigate(NavDir::Down); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: dialog_navigate(NavDir::Left); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: dialog_navigate(NavDir::Right); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: on_key(SDLK_RETURN, false); break;
        case SDL_GAMEPAD_BUTTON_EAST: close_dialog(); break;
        default: break;
        }
        return true;
    }
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: repeat(-1); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: repeat(1); break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        if (down) move_selection(-visible_rows());
        break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        if (down) move_selection(visible_rows());
        break;
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_START:
        if (down) do_action("play", "");
        break;
    case SDL_GAMEPAD_BUTTON_WEST:
        if (down) do_action("game-settings", "");
        break;
    case SDL_GAMEPAD_BUTTON_NORTH:
        if (down) do_action("rescan", "");
        break;
    case SDL_GAMEPAD_BUTTON_EAST:
        if (down && !filter_.empty()) do_action("clear-search", "");
        break;
    case SDL_GAMEPAD_BUTTON_BACK:
        if (down) open_menu("emulation", true);
        break;
    default: break;
    }
    return true;
}

void LauncherView::on_pad_axis(SDL_GamepadAxis axis, int value) {
    if (axis != SDL_GAMEPAD_AXIS_LEFTY) return;
    if (std::abs(value) > STICK_THRESHOLD) {
        pad_mode_ = true;
        const int delta = value > 0 ? 1 : -1;
        if (!stick_repeat_.active || stick_repeat_.delta != delta) {
            if (!menu_.empty()) menu_move(delta);
            else if (dialog_ == Dialog::None) move_selection(delta);
            stick_repeat_ = {menu_.empty() && dialog_ == Dialog::None, delta, now_ + 0.36};
        }
    } else {
        stick_repeat_.active = false;
    }
}

void LauncherView::dialog_navigate(NavDir dir) {
    Rml::Element *layer = by_id("dialog-layer");
    if (!layer) return;
    Rml::ElementList list;
    layer->QuerySelectorAll(list, ".f");
    std::vector<Rml::Element *> items;
    for (Rml::Element *e : list)
        if (e->IsVisible(true) && e->GetOffsetWidth() > 0) items.push_back(e);
    if (items.empty()) return;
    Rml::Element *focus = ctx_->GetFocusElement();
    int from = -1;
    for (size_t i = 0; i < items.size(); i++)
        if (items[i] == focus) from = int(i);
    if (from < 0) {
        if (Rml::Element *first = by_id("dlg-first")) first->Focus(true);
        else items[0]->Focus(true);
        return;
    }
    std::vector<NavRect> rects;
    for (Rml::Element *e : items) {
        const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
        rects.push_back({at.x, at.y, size.x, size.y});
    }
    const int to = nav_pick(rects, from, dir);
    if (to >= 0) items[size_t(to)]->Focus(true);
}

} // namespace xf::frontend
