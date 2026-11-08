// The launcher's settings page (settings_view.h).

#include "settings_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xf::frontend {

namespace {

constexpr int STICK_THRESHOLD = 17000;
const char *const EVENTS[] = {"click", "mousedown", "mouseup", "mousemove", "change"};

std::string px(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1fpx", value);
    return text;
}

std::string pct(double fraction) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f%%", std::clamp(fraction, 0.0, 1.0) * 100.0);
    return text;
}

} // namespace

SettingsView::SettingsView(LauncherCore &core, SettingsHost &host) : core_(core), host_(host) {}

SettingsView::~SettingsView() { detach(); }

void SettingsView::attach(Rml::Context *context, Rml::ElementDocument *document) {
    detach();
    ctx_ = context;
    doc_ = document;
    for (const char *type : EVENTS) doc_->AddEventListener(type, this);
    if (active_) {
        rebuild();
        defer([this] {
            if (Rml::Element *pages = by_id("pages")) {
                pages->SetScrollTop(scroll_);
                // a look change (the theme, the accent) makes the whole document again: the pad's selection stays where it was
                if (!focus_key_.empty() && host_.pad_mode())
                    if (Rml::Element *row = pages->QuerySelector("[data-key=\"" + focus_key_ + "\"]")) row->Focus(true);
            }
            focus_key_.clear();
        });
    }
}

void SettingsView::detach() {
    if (!doc_) return;
    if (Rml::Element *pages = by_id("pages")) scroll_ = pages->GetScrollTop();
    focus_key_.clear();
    if (active_ && ctx_)
        if (Rml::Element *focus = ctx_->GetFocusElement())
            if (Rml::Element *row = ancestor_with(focus, "data-key")) focus_key_ = row->GetAttribute<Rml::String>("data-key", "");
    for (const char *type : EVENTS) doc_->RemoveEventListener(type, this);
    doc_ = nullptr;
    popup_row_ = nullptr, drag_row_ = nullptr;
    popup_key_.clear();
}

void SettingsView::enter(SettingsTab tab) {
    active_ = true;
    if (!capturing_.empty()) capturing_.clear();
    close_popup();
    const bool changed = tab != tab_;
    tab_ = tab;
    edit_valid_ = false; // the player's overlay may have changed the settings meanwhile
    rebuild();
    if (changed)
        if (Rml::Element *pages = by_id("pages")) pages->SetScrollTop(0);
}

void SettingsView::leave() {
    if (!capturing_.empty()) end_capture();
    close_popup();
    drag_row_ = nullptr;
    active_ = false;
}

void SettingsView::set_game(const GameEntry *game) {
    const bool had = has_game_;
    const uint32_t before = game_.title_id;
    has_game_ = game != nullptr;
    if (game) game_ = *game;
    if (had == has_game_ && (!has_game_ || before == game_.title_id)) return;
    if (!capturing_.empty()) capturing_.clear();
    close_popup();
    edit_valid_ = false;
    if (active_) rebuild();
}

void SettingsView::use_own_settings() {
    if (!has_game_) return;
    reload_edit(); // as the file has them now
    if (!edit_.game.empty()) return;
    std::string error;
    core_.set_own_settings(game_, true, edit_, error);
    edit_valid_ = true;
    if (!error.empty()) std::fprintf(stderr, "launcher: settings: %s\n", error.c_str());
}

void SettingsView::folder_changed() {
    if (active_ && tab_ == SettingsTab::Interface) defer([this] { rebuild(); });
}

void SettingsView::show_tab(SettingsTab tab, int direction) {
    if (!capturing_.empty()) end_capture();
    close_popup();
    const bool changed = tab != tab_;
    if (direction == 0) direction = int(tab) >= int(tab_) ? 1 : -1;
    tab_ = tab;
    if (!active_) return; // made when the page is entered
    rebuild();
    if (Rml::Element *pages = by_id("pages")) pages->SetScrollTop(0);
    if (changed) {
        host_.play_sound(Cue::Tab);
        slide_page_in(direction);
    }
}

// The tab's page comes in from the side the tab lies on, over a fifth of a second: the console's blades
// shuffle one place, here the content does. The scroll box stays still (and clips what is outside it).
void SettingsView::slide_page_in(int direction) {
    Rml::Element *pages = by_id("pages");
    Rml::Element *page = pages ? pages->GetFirstChild() : nullptr;
    if (!page) return;
    const Rml::Tween tween(Rml::Tween::Cubic, Rml::Tween::Out);
    const Rml::Property from_left(float(40 * direction), Rml::Unit::DP), to_left(0.f, Rml::Unit::DP);
    page->Animate("left", to_left, 0.22f, tween, 1, false, 0.f, &from_left);
    const Rml::Property from_opacity(0.f, Rml::Unit::NUMBER), to_opacity(1.f, Rml::Unit::NUMBER);
    page->Animate("opacity", to_opacity, 0.2f, Rml::Tween(Rml::Tween::Quadratic, Rml::Tween::Out), 1, false, 0.f, &from_opacity);
}

void SettingsView::reload_edit() {
    std::string error;
    if (has_game_) {
        core_.load_settings(game_, edit_, error);
    } else {
        edit_ = Settings();
        frontend::load(core_.config_file(), edit_, error);
    }
    edit_valid_ = true;
}

void SettingsView::save_edit() {
    std::string error;
    if (!core_.save_settings(edit_, error)) std::fprintf(stderr, "launcher: settings: %s\n", error.c_str());
}

SettingsPage SettingsView::page() {
    if (!edit_valid_) reload_edit();
    SettingsPage p;
    p.settings = &edit_;
    p.game_title = has_game_ ? game_.title : "";
    p.title_id = has_game_ ? game_.title_id_text() : "";
    p.folder = core_.games_folder();
    p.accent = accent_from_hex(core_.accent()).base;
    p.theme = theme_text(theme_from_text(core_.launcher().theme));
    p.backdrop = core_.launcher().backdrop;
    p.sounds = core_.launcher().sounds;
    p.smooth_edges = core_.launcher().smooth_edges;
    if (tab_ == SettingsTab::Controllers) p.gamepads = LauncherCore::gamepads();
    p.capturing = capturing_;
    return p;
}

void SettingsView::rebuild() {
    if (!doc_) return;
    const SettingsPage p = page();
    if (Rml::Element *scope = by_id("scope")) scope->SetInnerRML(settings_scope_html(p));
    if (Rml::Element *tabs = by_id("tabs")) tabs->SetInnerRML(settings_tabs_html(tab_));
    if (Rml::Element *pages = by_id("pages")) {
        const float scroll = pages->GetScrollTop();
        pages->SetInnerRML(settings_page_html(tab_, p));
        pages->SetScrollTop(scroll);
    }
}

// ---------------------------------------------------------------------------
// the document's events

Rml::Element *SettingsView::ancestor_with(Rml::Element *element, const char *attribute) const {
    while (element && !element->HasAttribute(attribute)) element = element->GetParentNode();
    return element;
}

bool SettingsView::mine(Rml::Element *element) const {
    for (; element; element = element->GetParentNode())
        if (element->GetId() == "spage" || element->GetId() == "popup-layer") return true;
    return false;
}

void SettingsView::ProcessEvent(Rml::Event &event) {
    const Rml::String &type = event.GetType();
    Rml::Element *target = event.GetTargetElement();
    if (!target) return;
    // a drag goes on wherever the pointer is
    if (type == "mousemove") {
        if (drag_row_) drag_slider(event.GetParameter("mouse_x", 0.f));
        return;
    }
    if (type == "mouseup") {
        drag_row_ = nullptr;
        return;
    }
    if (!mine(target)) return;
    if (type == "click") {
        if (target->GetId() == "popup-layer") return close_popup();
        if (Rml::Element *a = ancestor_with(target, "data-action"))
            do_action(a->GetAttribute<Rml::String>("data-action", ""), a->GetAttribute<Rml::String>("data-arg", ""), a);
        else if (!capturing_.empty())
            end_capture();
    } else if (type == "mousedown") {
        Rml::Element *slider = target;
        while (slider && !slider->IsClassSet("slider")) slider = slider->GetParentNode();
        if (slider) {
            drag_row_ = ancestor_with(slider, "data-kind");
            drag_slider(event.GetParameter("mouse_x", 0.f));
        }
    } else if (type == "change") {
        if (target->GetId() == "folder-input" && event.GetParameter("linebreak", false))
            host_.set_folder(target->GetAttribute<Rml::String>("value", ""));
    }
}

void SettingsView::do_action(const std::string &name, const std::string &arg, Rml::Element *element) {
    if (!capturing_.empty() && name != "capture") end_capture();
    // "tab" sounds in show_tab, a dropdown's own list when it opens; the rest are choices
    if (name == "toggle" || name == "option" || name == "accent") host_.play_sound(Cue::Toggle);
    else if (name == "select") host_.play_sound(Cue::MenuOpen);
    else if (name != "tab") host_.play_sound(Cue::Select);
    if (name == "tab") {
        for (int i = 0; i < SETTINGS_TABS; i++)
            if (arg == settings_tab_name(SettingsTab(i))) show_tab(SettingsTab(i));
    } else if (name == "toggle") {
        toggle_setting(element);
    } else if (name == "select") {
        open_popup(element);
    } else if (name == "option") {
        choose_option(popup_key_, arg, popup_row_);
        close_popup();
    } else if (name == "accent") {
        std::string error;
        core_.set_accent(arg, error);
        host_.look_changed();
    } else if (name == "capture") {
        if (capturing_ == arg) end_capture();
        else begin_capture(arg);
    } else if (name == "default-keys") {
        LauncherCore::default_keys(edit_);
        save_edit();
        rebuild();
    } else if (name == "browse") {
        host_.browse_folder();
    } else if (name == "rescan") {
        host_.rescan();
    }
}

// ---------------------------------------------------------------------------
// the widgets

bool *SettingsView::bool_field(const std::string &key) {
    if (key == "video.fullscreen") return &edit_.video.fullscreen;
    if (key == "video.vsync") return &edit_.video.vsync;
    if (key == "video.smooth") return &edit_.video.smooth;
    if (key == "audio.mute") return &edit_.audio.mute;
    if (key == "input.vibration") return &edit_.input.vibration;
    if (key == "input.keyboard") return &edit_.input.keyboard;
    return nullptr;
}

bool SettingsView::get_slider(const std::string &key, double &fraction, std::string &text) {
    if (key == "audio.volume") fraction = edit_.audio.volume / 100.0;
    else if (key == "input.left_deadzone") fraction = edit_.input.left_deadzone;
    else if (key == "input.right_deadzone") fraction = edit_.input.right_deadzone;
    else return false;
    text = std::to_string(int(fraction * 100 + 0.5)) + "%";
    return true;
}

void SettingsView::set_slider(const std::string &key, double fraction) {
    fraction = std::clamp(std::round(fraction * 100.0) / 100.0, 0.0, 1.0);
    if (key == "audio.volume") edit_.audio.volume = int(fraction * 100 + 0.5);
    else if (key == "input.left_deadzone") edit_.input.left_deadzone = fraction;
    else if (key == "input.right_deadzone") edit_.input.right_deadzone = fraction;
}

void SettingsView::refresh_slider(Rml::Element *row) {
    double fraction = 0;
    std::string text;
    if (!get_slider(row->GetAttribute<Rml::String>("data-key", ""), fraction, text)) return;
    if (Rml::Element *fill = row->QuerySelector(".fill")) fill->SetProperty("width", pct(fraction));
    if (Rml::Element *thumb = row->QuerySelector(".thumb")) thumb->SetProperty("left", pct(fraction));
    if (Rml::Element *value = row->QuerySelector(".value")) value->SetInnerRML(text);
}

void SettingsView::drag_slider(float mouse_x) {
    if (!drag_row_) return;
    Rml::Element *slider = drag_row_->QuerySelector(".slider");
    if (!slider) return;
    const float left = slider->GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const float width = slider->GetBox().GetSize(Rml::BoxArea::Border).x;
    if (width <= 0) return;
    set_slider(drag_row_->GetAttribute<Rml::String>("data-key", ""), (mouse_x - left) / width);
    refresh_slider(drag_row_);
    save_edit();
}

void SettingsView::toggle_setting(Rml::Element *row) {
    if (!row) return;
    const std::string key = row->GetAttribute<Rml::String>("data-key", "");
    if (key == "own") {
        if (!has_game_) return;
        std::string error;
        core_.set_own_settings(game_, edit_.game.empty(), edit_, error);
        edit_valid_ = true;
        if (!error.empty()) std::fprintf(stderr, "launcher: settings: %s\n", error.c_str());
        defer([this] { rebuild(); });
        return;
    }
    if (key == "ui.sounds") {
        // the launcher's own, shared like the theme: not a game's setting
        Settings::Launcher l = core_.launcher();
        l.sounds = !l.sounds;
        std::string error;
        core_.set_launcher(l, error);
        if (Rml::Element *box = row->QuerySelector(".check")) box->SetClass("on", l.sounds);
        if (l.sounds) host_.play_sound(Cue::Toggle); // turned on: this is the first one heard
        return;
    }
    if (key == "ui.smooth_edges") {
        // also the launcher's own; the window reads it every frame and makes its targets again
        Settings::Launcher l = core_.launcher();
        l.smooth_edges = !l.smooth_edges;
        std::string error;
        core_.set_launcher(l, error);
        if (Rml::Element *box = row->QuerySelector(".check")) box->SetClass("on", l.smooth_edges);
        return;
    }
    bool *field = bool_field(key);
    if (!field) return;
    *field = !*field;
    if (Rml::Element *box = row->QuerySelector(".check")) box->SetClass("on", *field);
    save_edit();
}

std::vector<Choice> SettingsView::choices_for(const std::string &key) const {
    if (key == "video.aspect") return aspect_choices();
    if (key == "theme") return theme_choices();
    if (key == "backdrop") return backdrop_choices();
    return user_choices();
}

std::string SettingsView::choice_value(const std::string &key) const {
    if (key == "video.aspect") return edit_.video.aspect;
    if (key == "theme") return theme_text(theme_from_text(core_.launcher().theme));
    if (key == "backdrop") return core_.launcher().backdrop;
    return std::to_string(edit_.input.keyboard_user);
}

void SettingsView::open_popup(Rml::Element *row) {
    Rml::Element *layer = by_id("popup-layer");
    Rml::Element *box = row ? row->QuerySelector(".select") : nullptr;
    if (!layer || !box) return;
    popup_row_ = row;
    popup_key_ = row->GetAttribute<Rml::String>("data-key", "");
    const std::string current = choice_value(popup_key_);
    std::string html = "<div class=\"menu\" id=\"popup\">";
    for (const Choice &c : choices_for(popup_key_))
        html += std::string("<div class=\"mi f") + (c.value == current ? " current" : "") + "\" data-action=\"option\" data-arg=\"" + c.value + "\">" +
                "<span class=\"mi-ic\">" + (c.value == current ? icon_glyph("check") : std::string()) + "</span><div class=\"mi-l\">" + html_escape(c.label) +
                "</div></div>";
    html += "</div>";
    layer->SetInnerRML(html);
    layer->SetClass("open", true);
    Rml::Element *popup = by_id("popup");
    // the layer is in the window's own coordinates, the box on the page strip, which slid left by the page's
    // own offset to show the settings (a transform: the layout offsets do not know of it)
    Rml::Vector2f at = box->GetAbsoluteOffset(Rml::BoxArea::Border);
    if (Rml::Element *page = by_id("spage")) at.x -= page->GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const Rml::Vector2f size = box->GetBox().GetSize(Rml::BoxArea::Border);
    popup->SetProperty("left", px(at.x));
    popup->SetProperty("top", px(at.y + size.y + 2 * ctx_->GetDensityIndependentPixelRatio()));
    popup->SetProperty("min-width", px(size.x));
    box->SetClass("open", true);
    defer([this] {
        if (Rml::Element *p = by_id("popup")) {
            Rml::ElementList options;
            p->QuerySelectorAll(options, ".mi.current");
            if (options.empty()) p->QuerySelectorAll(options, ".mi");
            if (!options.empty()) options[0]->Focus(host_.pad_mode());
        }
    });
}

void SettingsView::close_popup() {
    Rml::Element *layer = by_id("popup-layer");
    if (!layer || !popup_row_) return;
    if (Rml::Element *box = popup_row_->QuerySelector(".select")) box->SetClass("open", false);
    layer->SetClass("open", false);
    layer->SetInnerRML("");
    const std::string key = popup_row_->GetAttribute<Rml::String>("data-key", "");
    popup_row_ = nullptr;
    popup_key_.clear();
    // the row that opened it takes focus back (found again by its key: the page may have been made again)
    defer([key, this] {
        if (Rml::Element *pages = by_id("pages"))
            if (Rml::Element *row = pages->QuerySelector("[data-key=\"" + key + "\"]")) row->Focus(host_.pad_mode());
    });
}

void SettingsView::choose_option(const std::string &key, const std::string &value, Rml::Element *row) {
    if (key == "theme") {
        Settings::Launcher l = core_.launcher();
        l.theme = value;
        std::string error;
        core_.set_launcher(l, error);
        host_.look_changed();
    } else if (key == "backdrop") {
        // read by the window every frame: nothing to rebuild
        Settings::Launcher l = core_.launcher();
        l.backdrop = value;
        std::string error;
        core_.set_launcher(l, error);
    } else {
        if (key == "video.aspect") edit_.video.aspect = value;
        else if (key == "input.keyboard_user") edit_.input.keyboard_user = std::atoi(value.c_str());
        else return;
        save_edit();
    }
    if (row)
        for (const Choice &c : choices_for(key))
            if (c.value == value)
                if (Rml::Element *text = row->QuerySelector(".value-text")) text->SetInnerRML(html_escape(c.label));
}

void SettingsView::begin_capture(const std::string &binding) {
    if (tab_ != SettingsTab::Keyboard) show_tab(SettingsTab::Keyboard);
    if (!doc_) return;
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
        if (name == binding) {
            row->Focus(host_.pad_mode());
            ensure_visible(row);
        }
    }
}

void SettingsView::end_capture() {
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
    }
}

// ---------------------------------------------------------------------------
// input

bool SettingsView::on_key(SDL_Keycode key, SDL_Scancode scancode, bool repeat) {
    if (!capturing_.empty()) {
        if (!repeat) {
            if (scancode != SDL_SCANCODE_ESCAPE) {
                edit_.keys[capturing_] = SDL_GetScancodeName(scancode);
                save_edit();
            }
            end_capture();
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
    const SDL_Keymod mods = SDL_GetModState();
    const bool ctrl = mods & SDL_KMOD_CTRL, shift = mods & SDL_KMOD_SHIFT;
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
        // Tab goes to the next page, Ctrl+Tab (or Shift+Tab) to the previous one
        if (!repeat) show_tab(SettingsTab((int(tab_) + ((ctrl || shift) ? SETTINGS_TABS - 1 : 1)) % SETTINGS_TABS), (ctrl || shift) ? -1 : 1);
        return true;
    default: return false;
    }
}

bool SettingsView::on_pad_button(SDL_GamepadButton button, bool down) {
    if (!down) return true;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: navigate(NavDir::Up); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: navigate(NavDir::Down); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: navigate(NavDir::Left); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: navigate(NavDir::Right); break;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        if (!capturing_.empty()) end_capture();
        else activate();
        break;
    case SDL_GAMEPAD_BUTTON_EAST: back(); break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: show_tab(SettingsTab((int(tab_) + SETTINGS_TABS - 1) % SETTINGS_TABS), -1); break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: show_tab(SettingsTab((int(tab_) + 1) % SETTINGS_TABS), 1); break;
    default: return false;
    }
    return true;
}

void SettingsView::on_pad_axis(SDL_GamepadAxis axis, int value) {
    if (axis != SDL_GAMEPAD_AXIS_LEFTY) return;
    const int dir = std::abs(value) > STICK_THRESHOLD ? (value > 0 ? 1 : -1) : 0;
    if (dir == stick_dir_) return;
    stick_dir_ = dir;
    if (dir) navigate(dir > 0 ? NavDir::Down : NavDir::Up);
}

std::vector<Rml::Element *> SettingsView::focusables() {
    std::vector<Rml::Element *> roots;
    if (popup_row_) roots.push_back(by_id("popup-layer"));
    else roots = {by_id("tabs"), by_id("scope"), by_id("pages")};
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

void SettingsView::ensure_visible(Rml::Element *element) {
    Rml::Element *container = element ? element->GetClosestScrollableContainer() : nullptr;
    if (!container) return;
    const float margin = 16 * ctx_->GetDensityIndependentPixelRatio();
    const float top = element->GetAbsoluteOffset(Rml::BoxArea::Border).y - container->GetAbsoluteOffset(Rml::BoxArea::Padding).y + container->GetScrollTop();
    const float height = element->GetBox().GetSize(Rml::BoxArea::Border).y;
    const float client = container->GetClientHeight();
    float target = container->GetScrollTop();
    if (top - margin < target) target = top - margin;
    else if (top + height + margin > target + client) target = top + height + margin - client;
    container->SetScrollTop(std::max(target, 0.f));
}

void SettingsView::focus_first() {
    std::vector<Rml::Element *> items = focusables();
    for (Rml::Element *e : items)
        if (ancestor_with(e, "data-kind") || e->IsClassSet("bind")) {
            e->Focus(true);
            ensure_visible(e);
            return;
        }
    if (!items.empty()) items[0]->Focus(true);
}

void SettingsView::navigate(NavDir dir) {
    if (!doc_) return;
    Rml::Element *focus = ctx_->GetFocusElement();
    // in a text box Left and Right are the caret's; Up and Down (here only from the pad) leave it
    if (focus && focus->GetTagName() == "input" && (dir == NavDir::Left || dir == NavDir::Right)) return;
    // Left and Right change the value of a slider, a dropdown or a check box that has focus
    if (focus && (dir == NavDir::Left || dir == NavDir::Right) && !popup_row_) {
        Rml::Element *row = ancestor_with(focus, "data-kind");
        const std::string kind = row ? row->GetAttribute<Rml::String>("data-kind", "") : "";
        const std::string key = row ? row->GetAttribute<Rml::String>("data-key", "") : "";
        if (kind == "swatches") {
            step_accent(dir == NavDir::Right ? 1 : -1, false);
            return;
        }
        if (kind == "slider") {
            double fraction = 0;
            std::string text;
            if (get_slider(key, fraction, text)) {
                set_slider(key, fraction + (dir == NavDir::Right ? 0.05 : -0.05));
                refresh_slider(row);
                save_edit();
                host_.play_sound(Cue::Focus);
            }
            return;
        }
        if (kind == "select") {
            const std::vector<Choice> choices = choices_for(key);
            const std::string current = choice_value(key);
            int at = 0;
            for (size_t i = 0; i < choices.size(); i++)
                if (choices[i].value == current) at = int(i);
            const int next = std::clamp(at + (dir == NavDir::Right ? 1 : -1), 0, int(choices.size()) - 1);
            if (next != at) host_.play_sound(Cue::Toggle);
            choose_option(key, choices[size_t(next)].value, row);
            return;
        }
        if (kind == "toggle" && key == "ui.sounds") {
            if (core_.launcher().sounds != (dir == NavDir::Right)) toggle_setting(row);
            return;
        }
        if (kind == "toggle" && key == "ui.smooth_edges") {
            if (core_.launcher().smooth_edges != (dir == NavDir::Right)) host_.play_sound(Cue::Toggle), toggle_setting(row);
            return;
        }
        if (kind == "toggle" && key != "own") {
            bool *field = bool_field(key);
            if (field && *field != (dir == NavDir::Right)) host_.play_sound(Cue::Toggle), toggle_setting(row);
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
    host_.play_sound(Cue::Focus);
}

void SettingsView::activate() {
    Rml::Element *focus = ctx_->GetFocusElement();
    if (!focus || focus == doc_ || !mine(focus)) return focus_first();
    if (Rml::Element *row = ancestor_with(focus, "data-kind"); row && row->GetAttribute<Rml::String>("data-kind", "") == "swatches")
        return step_accent(1, true);
    if (Rml::Element *a = ancestor_with(focus, "data-action"))
        do_action(a->GetAttribute<Rml::String>("data-action", ""), a->GetAttribute<Rml::String>("data-arg", ""), a);
}

// The swatches' row with the pad: Left and Right choose the neighbour (not past the ends), A the next, round again.
void SettingsView::step_accent(int delta, bool wrap) {
    const std::vector<std::string> &choices = accent_choices();
    const int count = int(choices.size());
    if (count == 0) return;
    const std::string current = accent_from_hex(core_.accent()).base;
    int at = 0;
    for (int i = 0; i < count; i++)
        if (choices[size_t(i)] == current) at = i;
    const int next = wrap ? (at + delta + count) % count : std::clamp(at + delta, 0, count - 1);
    if (next == at) return;
    host_.play_sound(Cue::Toggle);
    std::string error;
    core_.set_accent(choices[size_t(next)], error);
    host_.look_changed();
}

void SettingsView::back() {
    host_.play_sound(Cue::Back);
    if (popup_row_) return close_popup();
    if (!capturing_.empty()) return end_capture();
    host_.leave_settings();
}

// ---------------------------------------------------------------------------
// once a frame

void SettingsView::update() {
    auto work = std::move(deferred_);
    deferred_.clear();
    for (auto &fn : work) fn();
}

} // namespace xf::frontend
