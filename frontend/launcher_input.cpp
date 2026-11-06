// The launcher's actions, widgets and input (launcher_view.h): clicks and
// keys and the pad turned into what the screens do, and focus moved by
// direction as a pad needs it.

#include "launcher_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace xf::frontend {

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

constexpr int STICK_THRESHOLD = 17000;

std::string px(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1fpx", value);
    return text;
}

std::string pct(double fraction) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f%%", fraction * 100.0);
    return text;
}

using Clock = std::chrono::steady_clock;

} // namespace

Rml::Element *LauncherView::ancestor_with(Rml::Element *element, const char *attribute) const {
    while (element && !element->HasAttribute(attribute)) element = element->GetParentNode();
    return element;
}

Rml::Element *LauncherView::ancestor_with_class(Rml::Element *element, const char *name) const {
    while (element && !element->IsClassSet(name)) element = element->GetParentNode();
    return element;
}

// ---------------------------------------------------------------------------
// the document's events

void LauncherView::ProcessEvent(Rml::Event &event) {
    const Rml::String &type = event.GetType();
    Rml::Element *target = event.GetTargetElement();
    if (!target) return;
    if (type == "click") {
        if (target->GetId() == "popup-layer") return close_popup();
        if (Rml::Element *a = ancestor_with(target, "data-action")) {
            Action action{a->GetAttribute<Rml::String>("data-action", ""), a->GetAttribute<Rml::String>("data-arg", ""), a};
            do_action(action);
        } else if (!capturing_.empty() && !ancestor_with_class(target, "bind")) {
            end_capture(false);
        }
    } else if (type == "dblclick") {
        if (Rml::Element *tile = ancestor_with_class(target, "tile"))
            if (const GameEntry *g = selected_game(); g && tile->GetAttribute<int>("data-index", -1) == selected_) play(*g, false);
    } else if (type == "mousedown") {
        if (Rml::Element *slider = ancestor_with_class(target, "slider")) {
            drag_row_ = ancestor_with(slider, "data-kind");
            drag_slider(event.GetParameter("mouse_x", 0.f));
        }
    } else if (type == "mousemove") {
        if (drag_row_) drag_slider(event.GetParameter("mouse_x", 0.f));
    } else if (type == "mouseup") {
        drag_row_ = nullptr;
    } else if (type == "mouseover") {
        if (Rml::Element *bind = ancestor_with_class(target, "bind")) caption_for(bind);
    } else if (type == "focus") {
        if (target->IsClassSet("tile")) {
            const int index = target->GetAttribute<int>("data-index", -1);
            if (index >= 0 && listing_.games[size_t(index)].error.empty()) select_game(index, false);
        } else if (target->IsClassSet("bind")) {
            caption_for(target);
        }
    } else if (type == "blur") {
        if (target->IsClassSet("bind")) light_part("", false);
    } else if (type == "change") {
        if (target->GetId() == "folder-input" && event.GetParameter("linebreak", false))
            set_folder(target->GetAttribute<Rml::String>("value", ""));
    }
}

void LauncherView::caption_for(Rml::Element *bind) {
    const std::string name = bind->GetAttribute<Rml::String>("data-arg", "");
    const std::string part = bind->GetAttribute<Rml::String>("data-part", "");
    light_part(part, true);
    const auto it = edit_.keys.find(name);
    std::string keys;
    for (const KeyCap &c : parse_keys(it == edit_.keys.end() ? "" : it->second))
        keys += (keys.empty() ? "" : "  ") + std::string(c.shift ? "Shift+" : c.no_shift ? "no Shift " : "") + c.label;
    if (Rml::Element *caption = by_id("pad-caption"))
        caption->SetInnerRML("<b>" + html_escape(binding_info(name).label) + "</b>  \xC2\xB7  " + (keys.empty() ? "no key" : html_escape(keys)));
}

void LauncherView::light_part(const std::string &part, bool on) {
    Rml::ElementList lit;
    doc_->QuerySelectorAll(lit, ".pad-part.lit");
    for (Rml::Element *e : lit) e->SetClass("lit", false);
    if (on && !part.empty())
        if (Rml::Element *e = by_id(("part-" + part).c_str())) e->SetClass("lit", true);
}

// ---------------------------------------------------------------------------
// actions

void LauncherView::do_action(const Action &a) {
    if (!capturing_.empty() && a.name != "capture") end_capture(false);
    const GameEntry *game = selected_game();
    if (a.name == "tab") {
        static const char *const names[4] = {"library", "settings", "controls", "about"};
        for (int i = 0; i < 4; i++)
            if (a.arg == names[i]) show_screen(Screen(i));
    } else if (a.name == "browse") {
        browse();
    } else if (a.name == "rescan") {
        core_.rescan();
        selected_ = -1;
        library_sig_.clear();
        art_title_ = 0xFFFFFFFF;
    } else if (a.name == "play") {
        if (game) play(*game, false);
    } else if (a.name == "play-interpreted") {
        if (game) play(*game, true);
    } else if (a.name == "prepare") {
        if (game) begin_prepare(*game, false);
    } else if (a.name == "stop") {
        core_.stop();
    } else if (a.name == "game-settings") {
        show_screen(Screen::Settings);
        defer([this] { focus_first(); });
    } else if (a.name == "toggle") {
        toggle_setting(a.element);
    } else if (a.name == "select") {
        open_popup(a.element);
    } else if (a.name == "option") {
        choose_option(a.arg);
    } else if (a.name == "accent") {
        std::string error;
        core_.set_accent(a.arg, error);
        reload_document();
    } else if (a.name == "capture") {
        if (capturing_ == a.arg) end_capture(false);
        else begin_capture(a.arg);
    } else if (a.name == "default-keys") {
        LauncherCore::default_keys(edit_);
        save_edit();
        rebuild_controls();
    } else if (a.name == "toggle-log") {
        log_open_ = !log_open_;
        if (Rml::Element *log = by_id("log")) log->SetClass("open", log_open_);
        if (Rml::Element *t = by_id("log-toggle")) t->SetClass("open", log_open_);
        if (Rml::Element *t = by_id("log-toggle-text")) t->SetInnerRML(log_open_ ? "Hide log" : "Show log");
    } else if (a.name == "prep-cancel") {
        job_.cancel();
        last_prep_state_.clear();
        if (Rml::Element *e = by_id("prep-state")) e->SetInnerRML("Cancelling");
    } else if (a.name == "prep-close") {
        close_prepare();
    } else if (a.name == "prep-retry") {
        if (prepare_game_ >= 0 && prepare_game_ < long(listing_.games.size())) begin_prepare(listing_.games[size_t(prepare_game_)], false);
        else begin_prepare(GameEntry(), opt_.demo_progress);
    } else if (a.name == "prep-play") {
        const long index = prepare_game_;
        close_prepare();
        if (index >= 0 && index < long(listing_.games.size())) {
            start_game(listing_.games[size_t(index)], false);
        }
    } else if (a.name == "prep-interpreted") {
        const long index = prepare_game_;
        close_prepare();
        if (index >= 0 && index < long(listing_.games.size())) play(listing_.games[size_t(index)], true);
    }
}

void LauncherView::play(const GameEntry &game, bool force_interpreted) {
    if (core_.run().running) return;
    if (!player_of(game)) return toast("No player beside the launcher.", true, false, 6);
    if (force_interpreted) {
        failed_.insert(game.title_id);
        library_sig_.clear();
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
        return toast("No player beside the launcher.", true, false, 6);
    if (interpreted) {
        const PlayerBinary *generic = nullptr;
        for (const PlayerBinary &p : listing_.players)
            if (p.generic) generic = &p;
        if (!generic) return toast("No generic player beside the launcher.", true, false, 6);
        plan.program = generic->path, plan.generic = true, plan.from_library = false;
        plan.args.push_back("--interpret");
    }
    std::string message;
    if (!core_.start(game, plan, message)) return toast(message, true, false, 8);
    std::printf("launcher: started %s on %s%s, its output in %s\n", utf8(plan.program.filename()).c_str(), utf8(game.path).c_str(),
                plan.from_library ? " from its cached library" : "", utf8(core_.run().log).c_str());
    detail_sig_.clear();
    toast("Playing " + game.title, false, true, 0);
}

void LauncherView::browse() {
    SDL_ShowOpenFolderDialog(on_picked, nullptr, window_, core_.games_folder().empty() ? nullptr : core_.games_folder().c_str(), false);
}

void LauncherView::set_folder(const std::string &folder) {
    std::string message;
    if (!core_.set_games_folder(folder, true, message)) toast(message, true, false, 6);
    selected_ = -1;
    edit_valid_ = false;
    library_sig_.clear();
    art_title_ = 0xFFFFFFFF;
    last_listing_ = Clock::time_point();
    if (Rml::Element *input = by_id("folder-input")) input->SetAttribute("value", folder);
    if (screen_ == Screen::Settings) defer([this] { rebuild_settings(); });
}

// ---------------------------------------------------------------------------
// the settings' widgets

bool *LauncherView::bool_field(const std::string &key) {
    if (key == "video.fullscreen") return &edit_.video.fullscreen;
    if (key == "video.vsync") return &edit_.video.vsync;
    if (key == "video.smooth") return &edit_.video.smooth;
    if (key == "audio.mute") return &edit_.audio.mute;
    if (key == "input.vibration") return &edit_.input.vibration;
    if (key == "input.keyboard") return &edit_.input.keyboard;
    return nullptr;
}

bool LauncherView::get_slider(const std::string &key, double &fraction, std::string &text) {
    if (key == "audio.volume") fraction = edit_.audio.volume / 100.0;
    else if (key == "input.left_deadzone") fraction = edit_.input.left_deadzone;
    else if (key == "input.right_deadzone") fraction = edit_.input.right_deadzone;
    else return false;
    text = std::to_string(int(fraction * 100 + 0.5)) + "%";
    return true;
}

void LauncherView::set_slider(const std::string &key, double fraction) {
    fraction = std::clamp(std::round(fraction * 100.0) / 100.0, 0.0, 1.0);
    if (key == "audio.volume") edit_.audio.volume = int(fraction * 100 + 0.5);
    else if (key == "input.left_deadzone") edit_.input.left_deadzone = fraction;
    else if (key == "input.right_deadzone") edit_.input.right_deadzone = fraction;
}

void LauncherView::refresh_slider(Rml::Element *row) {
    double fraction = 0;
    std::string text;
    if (!get_slider(row->GetAttribute<Rml::String>("data-key", ""), fraction, text)) return;
    if (Rml::Element *fill = row->QuerySelector(".fill")) fill->SetProperty("width", pct(fraction));
    if (Rml::Element *thumb = row->QuerySelector(".thumb")) thumb->SetProperty("left", pct(fraction));
    if (Rml::Element *value = row->QuerySelector(".value")) value->SetInnerRML(text);
}

void LauncherView::drag_slider(float mouse_x) {
    if (!drag_row_) return;
    Rml::Element *slider = drag_row_->QuerySelector(".slider");
    if (!slider) return;
    const float left = slider->GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const float width = slider->GetBox().GetSize(Rml::BoxArea::Border).x;
    if (width <= 0) return;
    const std::string key = drag_row_->GetAttribute<Rml::String>("data-key", "");
    set_slider(key, (mouse_x - left) / width);
    refresh_slider(drag_row_);
    save_edit();
}

void LauncherView::toggle_setting(Rml::Element *row) {
    const std::string key = row->GetAttribute<Rml::String>("data-key", "");
    if (key == "own") {
        const GameEntry *g = selected_game();
        if (!g) return;
        std::string error;
        core_.set_own_settings(*g, edit_.game.empty(), edit_, error);
        edit_valid_ = true;
        if (!error.empty()) toast(error, true, false, 6);
        defer([this] { rebuild_settings(); });
        return;
    }
    bool *field = bool_field(key);
    if (!field) return;
    *field = !*field;
    if (Rml::Element *toggle = row->QuerySelector(".toggle")) toggle->SetClass("on", *field);
    save_edit();
}

std::vector<Choice> LauncherView::choices_for(const std::string &key) const {
    return key == "video.aspect" ? aspect_choices() : user_choices();
}

std::string LauncherView::choice_value(const std::string &key) const {
    return key == "video.aspect" ? edit_.video.aspect : std::to_string(edit_.input.keyboard_user);
}

void LauncherView::open_popup(Rml::Element *row) {
    Rml::Element *layer = by_id("popup-layer");
    Rml::Element *box = row ? row->QuerySelector(".select") : nullptr;
    if (!layer || !box) return;
    popup_row_ = row;
    popup_key_ = row->GetAttribute<Rml::String>("data-key", "");
    const std::string current = choice_value(popup_key_);
    std::string html = "<div class=\"popup\" id=\"popup\">";
    for (const Choice &c : choices_for(popup_key_))
        html += std::string("<div class=\"option f") + (c.value == current ? " current" : "") + "\" data-action=\"option\" data-arg=\"" + c.value + "\">" +
                html_escape(c.label) + "</div>";
    html += "</div>";
    layer->SetInnerRML(html);
    layer->SetClass("open", true);
    Rml::Element *popup = by_id("popup");
    const Rml::Vector2f at = box->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = box->GetBox().GetSize(Rml::BoxArea::Border);
    popup->SetProperty("left", px(at.x));
    popup->SetProperty("top", px(at.y + size.y + 6 * dp()));
    popup->SetProperty("width", px(size.x));
    box->SetClass("open", true);
    defer([this] {
        if (Rml::Element *p = by_id("popup")) {
            p->SetClass("on", true);
            Rml::ElementList options;
            p->QuerySelectorAll(options, ".option.current");
            if (options.empty()) p->QuerySelectorAll(options, ".option");
            if (!options.empty()) options[0]->Focus(pad_mode_);
        }
    });
    rebuild_hints();
}

void LauncherView::close_popup() {
    Rml::Element *layer = by_id("popup-layer");
    if (!layer || !popup_row_) return;
    if (Rml::Element *box = popup_row_->QuerySelector(".select")) box->SetClass("open", false);
    layer->SetClass("open", false);
    layer->SetInnerRML("");
    Rml::Element *row = popup_row_;
    popup_row_ = nullptr;
    popup_key_.clear();
    defer([row, this] {
        if (doc_ && row->GetOwnerDocument() == doc_) row->Focus(pad_mode_);
    });
}

void LauncherView::choose_option(const std::string &value) {
    Rml::Element *row = popup_row_;
    const std::string key = popup_key_;
    if (!row) return;
    if (key == "video.aspect") edit_.video.aspect = value;
    else if (key == "input.keyboard_user") edit_.input.keyboard_user = std::atoi(value.c_str());
    for (const Choice &c : choices_for(key))
        if (c.value == value)
            if (Rml::Element *text = row->QuerySelector(".value-text")) text->SetInnerRML(html_escape(c.label));
    save_edit();
    close_popup();
}

// ---------------------------------------------------------------------------
// the bindings' capture

void LauncherView::begin_capture(const std::string &binding) {
    const std::string before = capturing_;
    capturing_ = binding;
    Rml::ElementList rows;
    doc_->QuerySelectorAll(rows, ".bind");
    for (Rml::Element *row : rows) {
        const std::string name = row->GetAttribute<Rml::String>("data-arg", "");
        if (name != binding && name != before) continue;
        row->SetClass("capturing", name == binding);
        if (Rml::Element *keys = row->QuerySelector(".keys")) {
            const auto it = edit_.keys.find(name);
            keys->SetInnerRML(keycaps_html(it == edit_.keys.end() ? "" : it->second, name == binding));
        }
        if (name == binding) row->Focus(pad_mode_);
    }
}

void LauncherView::end_capture(bool) {
    const std::string name = capturing_;
    capturing_.clear();
    if (name.empty() || !doc_) return;
    Rml::ElementList rows;
    doc_->QuerySelectorAll(rows, ".bind");
    for (Rml::Element *row : rows) {
        if (row->GetAttribute<Rml::String>("data-arg", "") != name) continue;
        row->SetClass("capturing", false);
        if (Rml::Element *keys = row->QuerySelector(".keys")) {
            const auto it = edit_.keys.find(name);
            keys->SetInnerRML(keycaps_html(it == edit_.keys.end() ? "" : it->second, false));
        }
        caption_for(row);
    }
}

// ---------------------------------------------------------------------------
// input

bool LauncherView::on_event(const SDL_Event &e) {
    auto to_pad = [&](bool pad) {
        if (pad == pad_mode_) return;
        pad_mode_ = pad;
        if (pad) SDL_HideCursor();
        else SDL_ShowCursor();
        rebuild_hints();
    };
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        to_pad(false);
        return on_key(e.key.key, e.key.scancode, true, e.key.repeat);
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: to_pad(false); return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        to_pad(true);
        return on_pad_button(SDL_GamepadButton(e.gbutton.button), e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (std::abs(int(e.gaxis.value)) > STICK_THRESHOLD) to_pad(true);
        on_pad_axis(SDL_GamepadAxis(e.gaxis.axis), e.gaxis.value);
        return true;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (screen_ == Screen::Controls) defer([this] { rebuild_controls(); });
        return false;
    default: return false;
    }
}

bool LauncherView::on_key(SDL_Keycode key, SDL_Scancode scancode, bool, bool repeat) {
    if (!capturing_.empty()) {
        if (!repeat) {
            if (scancode != SDL_SCANCODE_ESCAPE) {
                edit_.keys[capturing_] = SDL_GetScancodeName(scancode);
                save_edit();
            }
            end_capture(true);
        }
        return true;
    }
    Rml::Element *focus = ctx_->GetFocusElement();
    if (focus && focus->GetTagName() == "input") {
        if (key == SDLK_ESCAPE) {
            focus->Blur();
            return true;
        }
        return false;
    }
    switch (key) {
    case SDLK_UP: navigate(NavDir::Up); return true;
    case SDLK_DOWN: navigate(NavDir::Down); return true;
    case SDLK_LEFT: navigate(NavDir::Left); return true;
    case SDLK_RIGHT: navigate(NavDir::Right); return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        if (!repeat) activate();
        return true;
    case SDLK_ESCAPE:
        if (!repeat) back();
        return true;
    case SDLK_TAB:
        if (!repeat) next_page((SDL_GetModState() & SDL_KMOD_SHIFT) ? -1 : 1);
        return true;
    case SDLK_F5:
        if (!repeat) do_action({"rescan", "", nullptr});
        return true;
    case SDLK_F2:
        if (!repeat && screen_ == Screen::Library) do_action({"game-settings", "", nullptr});
        return true;
    default: return false;
    }
}

bool LauncherView::on_pad_button(SDL_GamepadButton button, bool down) {
    auto direction = [&](NavDir dir) {
        if (down) {
            navigate(dir);
            dpad_repeat_ = {true, dir, now_ + 0.38};
        } else if (dpad_repeat_.active && dpad_repeat_.dir == dir) {
            dpad_repeat_.active = false;
        }
    };
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: direction(NavDir::Up); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: direction(NavDir::Down); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: direction(NavDir::Left); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: direction(NavDir::Right); break;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        if (down) {
            if (!capturing_.empty()) end_capture(false);
            else activate();
        }
        break;
    case SDL_GAMEPAD_BUTTON_EAST:
        if (down) back();
        break;
    case SDL_GAMEPAD_BUTTON_WEST:
        if (down && screen_ == Screen::Library && !prepare_open_) do_action({"game-settings", "", nullptr});
        break;
    case SDL_GAMEPAD_BUTTON_NORTH:
        if (down && screen_ == Screen::Library && !prepare_open_) do_action({"rescan", "", nullptr});
        break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        if (down && !prepare_open_) next_page(-1);
        break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        if (down && !prepare_open_) next_page(1);
        break;
    case SDL_GAMEPAD_BUTTON_START:
        if (down && screen_ == Screen::Library && !prepare_open_)
            if (const GameEntry *g = selected_game()) play(*g, false);
        break;
    default: break;
    }
    return true;
}

void LauncherView::on_pad_axis(SDL_GamepadAxis axis, int value) {
    if (axis == SDL_GAMEPAD_AXIS_LEFTX) stick_x_ = value;
    else if (axis == SDL_GAMEPAD_AXIS_LEFTY) stick_y_ = value;
    else return;
    // the stick as a d-pad: the stronger axis, once it passes the threshold
    NavDir dir = NavDir::Down;
    bool held = false;
    if (std::abs(stick_x_) > STICK_THRESHOLD || std::abs(stick_y_) > STICK_THRESHOLD) {
        held = true;
        if (std::abs(stick_x_) > std::abs(stick_y_)) dir = stick_x_ > 0 ? NavDir::Right : NavDir::Left;
        else dir = stick_y_ > 0 ? NavDir::Down : NavDir::Up;
    }
    if (!held) {
        stick_repeat_.active = false;
    } else if (!stick_repeat_.active || stick_repeat_.dir != dir) {
        navigate(dir);
        stick_repeat_ = {true, dir, now_ + 0.38};
    }
}

void LauncherView::update_input(double) {
    {
        std::lock_guard guard(picked.lock);
        if (picked.ready) {
            picked.ready = false;
            const std::string folder = picked.path;
            defer([this, folder] { set_folder(folder); });
        }
    }
    for (Repeat *r : {&dpad_repeat_, &stick_repeat_})
        if (r->active && now_ >= r->next) {
            navigate(r->dir);
            r->next = now_ + 0.1;
        }
}

std::vector<Rml::Element *> LauncherView::focusables() {
    std::vector<Rml::Element *> roots;
    if (popup_row_) roots.push_back(by_id("popup-layer"));
    else if (prepare_open_) roots.push_back(by_id("prepare"));
    else {
        roots.push_back(by_id("topbar"));
        roots.push_back(by_id(screen_ == Screen::Library ? "screen-library" : screen_ == Screen::Settings ? "screen-settings"
                                                          : screen_ == Screen::Controls ? "screen-controls" : "screen-about"));
    }
    std::vector<Rml::Element *> found;
    for (Rml::Element *root : roots) {
        if (!root) continue;
        Rml::ElementList list;
        root->QuerySelectorAll(list, ".f");
        for (Rml::Element *e : list)
            if (e->IsVisible(true) && e->GetOffsetWidth() > 0 && e->GetOffsetHeight() > 0) found.push_back(e);
    }
    return found;
}

void LauncherView::ensure_visible(Rml::Element *element) {
    Rml::Element *container = element ? element->GetClosestScrollableContainer() : nullptr;
    if (!container) return;
    const float margin = 26 * dp();
    const float container_top = container->GetAbsoluteOffset(Rml::BoxArea::Padding).y;
    const float element_top = element->GetAbsoluteOffset(Rml::BoxArea::Border).y;
    const float height = element->GetBox().GetSize(Rml::BoxArea::Border).y;
    const float current = scroll_targets_.count(container) ? scroll_targets_[container] : container->GetScrollTop();
    // where the element is in the content, from the scroll the layout shows now
    const float position = element_top - container_top + container->GetScrollTop();
    const float client = container->GetClientHeight();
    float target = current;
    if (position - margin < current) target = position - margin;
    else if (position + height + margin > current + client) target = position + height + margin - client;
    target = std::max(target, 0.f);
    if (std::fabs(target - container->GetScrollTop()) > 0.5f) scroll_targets_[container] = target;
}

void LauncherView::focus_first() {
    if (!doc_) return;
    if (screen_ == Screen::Library && !prepare_open_) {
        if (Rml::Element *grid = by_id("grid"))
            for (int i = 0; i < grid->GetNumChildren(); i++)
                if (grid->GetChild(i)->GetAttribute<int>("data-index", -1) == selected_) {
                    grid->GetChild(i)->Focus(pad_mode_);
                    ensure_visible(grid->GetChild(i));
                    return;
                }
        if (Rml::Element *b = by_id("empty-primary")) return void(b->Focus(pad_mode_));
    }
    Rml::Element *root = by_id(screen_ == Screen::Library ? "screen-library" : screen_ == Screen::Settings ? "screen-settings"
                                                           : screen_ == Screen::Controls ? "screen-controls" : "screen-about");
    if (!root) return;
    Rml::ElementList list;
    root->QuerySelectorAll(list, ".f");
    for (Rml::Element *e : list)
        if (e->IsVisible(true) && e->GetOffsetWidth() > 0) {
            e->Focus(pad_mode_);
            ensure_visible(e);
            return;
        }
}

void LauncherView::navigate(NavDir dir) {
    if (!doc_) return;
    Rml::Element *focus = ctx_->GetFocusElement();
    if (focus && focus->GetTagName() == "input") return;
    // Left and Right change the value of a slider or a dropdown that has focus
    if (focus && (dir == NavDir::Left || dir == NavDir::Right) && !popup_row_) {
        Rml::Element *row = ancestor_with(focus, "data-kind");
        const std::string kind = row ? row->GetAttribute<Rml::String>("data-kind", "") : "";
        const double step = dir == NavDir::Right ? 0.05 : -0.05;
        if (kind == "slider") {
            double fraction = 0;
            std::string text;
            const std::string key = row->GetAttribute<Rml::String>("data-key", "");
            if (get_slider(key, fraction, text)) {
                set_slider(key, fraction + step);
                refresh_slider(row);
                save_edit();
            }
            return;
        }
        if (kind == "select") {
            const std::string key = row->GetAttribute<Rml::String>("data-key", "");
            const std::vector<Choice> choices = choices_for(key);
            const std::string current = choice_value(key);
            int at = 0;
            for (size_t i = 0; i < choices.size(); i++)
                if (choices[i].value == current) at = int(i);
            at = std::clamp(at + (dir == NavDir::Right ? 1 : -1), 0, int(choices.size()) - 1);
            popup_row_ = row;
            popup_key_ = key;
            choose_option(choices[size_t(at)].value);
            return;
        }
        if (kind == "toggle" && row->GetAttribute<Rml::String>("data-key", "") != "own") {
            bool *field = bool_field(row->GetAttribute<Rml::String>("data-key", ""));
            if (field && *field != (dir == NavDir::Right)) toggle_setting(row);
            return;
        }
    }
    std::vector<Rml::Element *> items = focusables();
    if (items.empty()) return;
    int from = -1;
    for (size_t i = 0; i < items.size(); i++)
        if (items[i] == focus) from = int(i);
    if (from < 0) return focus_first();
    std::vector<NavRect> rects;
    for (Rml::Element *e : items) {
        const Rml::Vector2f at = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
        rects.push_back({at.x, at.y, size.x, size.y});
    }
    const int to = nav_pick(rects, from, dir);
    if (to < 0) return;
    items[size_t(to)]->Focus(true);
    ensure_visible(items[size_t(to)]);
}

void LauncherView::activate() {
    Rml::Element *focus = ctx_->GetFocusElement();
    if (!focus) return focus_first();
    if (focus->IsClassSet("tile")) {
        if (const GameEntry *g = selected_game()) play(*g, false);
        return;
    }
    if (Rml::Element *a = ancestor_with(focus, "data-action"))
        do_action({a->GetAttribute<Rml::String>("data-action", ""), a->GetAttribute<Rml::String>("data-arg", ""), a});
}

void LauncherView::back() {
    if (popup_row_) return close_popup();
    if (prepare_open_) {
        if (job_.active()) do_action({"prep-cancel", "", nullptr});
        else close_prepare();
        return;
    }
    if (!capturing_.empty()) return end_capture(false);
    if (screen_ != Screen::Library) {
        show_screen(Screen::Library);
        defer([this] { focus_first(); });
    }
}

void LauncherView::next_page(int delta) {
    if (prepare_open_ || popup_row_) return;
    const int next = (int(screen_) + delta + 4) % 4;
    show_screen(Screen(next));
    if (pad_mode_) defer([this] { focus_first(); });
}

} // namespace xf::frontend
