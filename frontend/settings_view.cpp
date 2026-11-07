// The launcher's settings window (settings_view.h).

#include "settings_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace xf::frontend {

namespace {

constexpr float WIDTH_DP = 780, HEIGHT_DP = 640;
constexpr int STICK_THRESHOLD = 17000;

std::string read_text(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

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

int contexts_made = 0;

} // namespace

SettingsView::SettingsView(LauncherCore &core, SettingsHost &host) : core_(core), host_(host) {}

SettingsView::~SettingsView() {
    if (ctx_) {
        if (doc_) ctx_->UnloadDocument(doc_), doc_ = nullptr;
        ctx_->Update();
        Rml::RemoveContext(name_);
        ctx_ = nullptr;
        // RmlUi keeps a render manager per render interface until told: its
        // textures and geometry go back to this window's renderer now, while
        // it lives (CreateContext's documented lifetime)
        Rml::ReleaseRenderManagers();
    }
    surface_.reset(); // the render interface after the context that used it
    if (window_) SDL_DestroyWindow(window_);
}

bool SettingsView::open(SDL_Window *parent, const std::filesystem::path &assets, SettingsTab tab, float scale, bool offscreen,
                        std::string &error) {
    assets_ = assets;
    tab_ = tab;
    scale_ = scale;
    offscreen_ = offscreen;
    rml_template_ = read_text(assets_ / "settings.rml");
    rcss_template_ = read_text(assets_ / "theme.rcss") + "\n" + read_text(assets_ / "settings.rcss");
    if (rml_template_.empty() || rcss_template_.size() < 2) return error = "no settings.rml or settings.rcss in " + assets_.string(), false;

    // the window: its size in dp at the parent's display scale, over the parent
    const float display = parent ? SDL_GetWindowDisplayScale(parent) : 1.f;
    const float s = scale_ > 0 ? scale_ : (display > 0 ? display : 1.f);
    Uint64 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (offscreen_) flags |= SDL_WINDOW_HIDDEN;
    window_ = SDL_CreateWindow("Settings", int(WIDTH_DP * s), int(HEIGHT_DP * s), flags);
    if (!window_) return error = std::string("SDL: ") + SDL_GetError(), false;
    off_w_ = uint32_t(WIDTH_DP * s), off_h_ = uint32_t(HEIGHT_DP * s);
    SDL_SetWindowMinimumSize(window_, int(560 * s), int(420 * s));
    if (parent && !offscreen_) {
        SDL_SetWindowParent(window_, parent);
        int px_ = 0, py = 0, pw = 0, ph = 0;
        SDL_GetWindowPosition(parent, &px_, &py);
        SDL_GetWindowSize(parent, &pw, &ph);
        SDL_SetWindowPosition(window_, px_ + std::max(0, (pw - int(WIDTH_DP * s)) / 2), py + std::max(0, (ph - int(HEIGHT_DP * s)) / 3));
    }
    surface_ = std::make_unique<ui::Window>();
    if (!surface_->open(window_, error)) return error = "Vulkan: " + error, false;
    name_ = "settings" + std::to_string(++contexts_made);
    ctx_ = Rml::CreateContext(name_, Rml::Vector2i(int(off_w_), int(off_h_)), &surface_->renderer());
    if (!ctx_) return error = "RmlUi has no context for the settings", false;
    return build_document(error);
}

bool SettingsView::build_document(std::string &error) {
    std::string rml = rml_template_;
    const size_t at = rml.find("<!--STYLE-->");
    if (at == std::string::npos) return error = "settings.rml has no <!--STYLE--> mark", false;
    rml.replace(at, 12, "<style>\n" + host_.styled(rcss_template_) + "\n</style>");
    doc_ = ctx_->LoadDocumentFromMemory(expand_icons(rml), "settings.rml");
    if (!doc_) return error = "settings.rml did not load", false;
    for (const char *type : {"click", "mousedown", "mouseup", "mousemove", "change"}) doc_->AddEventListener(type, this);
    doc_->Show();
    rebuild();
    settle_until_ = now_ + 0.5;
    return true;
}

void SettingsView::restyle() { restyle_ = true; }

bool SettingsView::has_focus() const { return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS); }

void SettingsView::set_game(const GameEntry *game) {
    const bool had = has_game_;
    const uint32_t before = game_.title_id;
    has_game_ = game != nullptr;
    if (game) game_ = *game;
    if (had == has_game_ && (!has_game_ || before == game_.title_id)) return;
    if (!capturing_.empty()) capturing_.clear();
    close_popup();
    edit_valid_ = false;
    rebuild();
}

void SettingsView::folder_changed() {
    if (tab_ == SettingsTab::Interface) defer([this] { rebuild(); });
}

void SettingsView::show_tab(SettingsTab tab) {
    if (!capturing_.empty()) end_capture();
    close_popup();
    tab_ = tab;
    rebuild();
    if (Rml::Element *pages = by_id("pages")) pages->SetScrollTop(0);
    settle_until_ = std::max(settle_until_, now_ + 0.3);
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
    std::string title = "Settings";
    if (has_game_) title += std::string(" \xE2\x80\x94 ") + game_.title + (edit_.game.empty() ? " (shared)" : " (its own)");
    if (window_) SDL_SetWindowTitle(window_, title.c_str());
}

// ---------------------------------------------------------------------------
// the document's events

Rml::Element *SettingsView::ancestor_with(Rml::Element *element, const char *attribute) const {
    while (element && !element->HasAttribute(attribute)) element = element->GetParentNode();
    return element;
}

void SettingsView::ProcessEvent(Rml::Event &event) {
    const Rml::String &type = event.GetType();
    Rml::Element *target = event.GetTargetElement();
    if (!target) return;
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
    } else if (type == "mousemove") {
        if (drag_row_) drag_slider(event.GetParameter("mouse_x", 0.f));
    } else if (type == "mouseup") {
        drag_row_ = nullptr;
    } else if (type == "change") {
        if (target->GetId() == "folder-input" && event.GetParameter("linebreak", false))
            host_.set_folder(target->GetAttribute<Rml::String>("value", ""));
    }
}

void SettingsView::do_action(const std::string &name, const std::string &arg, Rml::Element *element) {
    if (!capturing_.empty() && name != "capture") end_capture();
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
        host_.browse_folder(window_);
    } else if (name == "rescan") {
        host_.rescan();
    } else if (name == "close") {
        close_ = true;
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
    const Rml::Vector2f at = box->GetAbsoluteOffset(Rml::BoxArea::Border);
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
            if (!options.empty()) options[0]->Focus(pad_mode_);
        }
    });
}

void SettingsView::close_popup() {
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

void SettingsView::choose_option(const std::string &key, const std::string &value, Rml::Element *row) {
    if (key == "theme") {
        Settings::Launcher l = core_.launcher();
        l.theme = value;
        std::string error;
        core_.set_launcher(l, error);
        host_.look_changed();
    } else if (key == "backdrop") {
        // read by the launcher's window every frame: nothing to rebuild
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
            row->Focus(pad_mode_);
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

bool SettingsView::on_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        pad_mode_ = false;
        return on_key(e.key.key, e.key.scancode, e.key.repeat);
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: pad_mode_ = false; return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        pad_mode_ = true;
        return on_pad_button(SDL_GamepadButton(e.gbutton.button), e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY && std::abs(int(e.gaxis.value)) > STICK_THRESHOLD) {
            pad_mode_ = true;
            navigate(e.gaxis.value > 0 ? NavDir::Down : NavDir::Up);
        }
        return true;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (tab_ == SettingsTab::Controllers) defer([this] { rebuild(); });
        return false;
    default: return false;
    }
}

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
    const bool ctrl = SDL_GetModState() & SDL_KMOD_CTRL;
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
        if (!ctrl) return false; // focus moves as RmlUi moves it
        if (!repeat) show_tab(SettingsTab((int(tab_) + ((SDL_GetModState() & SDL_KMOD_SHIFT) ? SETTINGS_TABS - 1 : 1)) % SETTINGS_TABS));
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
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: show_tab(SettingsTab((int(tab_) + SETTINGS_TABS - 1) % SETTINGS_TABS)); break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: show_tab(SettingsTab((int(tab_) + 1) % SETTINGS_TABS)); break;
    default: break;
    }
    return true;
}

std::vector<Rml::Element *> SettingsView::focusables() {
    std::vector<Rml::Element *> roots;
    if (popup_row_) roots.push_back(by_id("popup-layer"));
    else roots = {by_id("scope"), by_id("tabs"), by_id("pages"), by_id("sfoot")};
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
    if (focus && focus->GetTagName() == "input") return;
    // Left and Right change the value of a slider, a dropdown or a check box that has focus
    if (focus && (dir == NavDir::Left || dir == NavDir::Right) && !popup_row_) {
        Rml::Element *row = ancestor_with(focus, "data-kind");
        const std::string kind = row ? row->GetAttribute<Rml::String>("data-kind", "") : "";
        const std::string key = row ? row->GetAttribute<Rml::String>("data-key", "") : "";
        if (kind == "slider") {
            double fraction = 0;
            std::string text;
            if (get_slider(key, fraction, text)) {
                set_slider(key, fraction + (dir == NavDir::Right ? 0.05 : -0.05));
                refresh_slider(row);
                save_edit();
            }
            return;
        }
        if (kind == "select") {
            const std::vector<Choice> choices = choices_for(key);
            const std::string current = choice_value(key);
            int at = 0;
            for (size_t i = 0; i < choices.size(); i++)
                if (choices[i].value == current) at = int(i);
            at = std::clamp(at + (dir == NavDir::Right ? 1 : -1), 0, int(choices.size()) - 1);
            choose_option(key, choices[size_t(at)].value, row);
            return;
        }
        if (kind == "toggle" && key != "own") {
            bool *field = bool_field(key);
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

void SettingsView::activate() {
    Rml::Element *focus = ctx_->GetFocusElement();
    if (!focus || focus == doc_) return focus_first();
    if (Rml::Element *a = ancestor_with(focus, "data-action"))
        do_action(a->GetAttribute<Rml::String>("data-action", ""), a->GetAttribute<Rml::String>("data-arg", ""), a);
}

void SettingsView::back() {
    if (popup_row_) return close_popup();
    if (!capturing_.empty()) return end_capture();
    close_ = true;
}

// ---------------------------------------------------------------------------
// once a frame

void SettingsView::frame() {
    const uint64_t ticks = SDL_GetTicksNS();
    now_ += last_ticks_ ? double(ticks - last_ticks_) * 1e-9 : 0;
    last_ticks_ = ticks;
    if (restyle_) {
        restyle_ = false;
        const float scroll = by_id("pages") ? by_id("pages")->GetScrollTop() : 0;
        popup_row_ = nullptr, drag_row_ = nullptr;
        if (doc_) ctx_->UnloadDocument(doc_), doc_ = nullptr;
        std::string error;
        if (!build_document(error)) std::fprintf(stderr, "launcher: %s\n", error.c_str());
        defer([this, scroll] {
            if (Rml::Element *p = by_id("pages")) p->SetScrollTop(scroll);
        });
    }
    auto work = std::move(deferred_);
    deferred_.clear();
    for (auto &fn : work) fn();

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    const uint32_t width = offscreen_ ? off_w_ : uint32_t(std::max(w, 0)), height = offscreen_ ? off_h_ : uint32_t(std::max(h, 0));
    if (width && height) {
        ctx_->SetDimensions(Rml::Vector2i(int(width), int(height)));
        const float display = SDL_GetWindowDisplayScale(window_);
        ctx_->SetDensityIndependentPixelRatio(scale_ > 0 ? scale_ : offscreen_ ? 1.f : (display > 0 ? display : 1.f));
    }
    ctx_->Update();
    surface_->frame([this] { ctx_->Render(); }, offscreen_ ? off_w_ : 0, offscreen_ ? off_h_ : 0);
}

} // namespace xf::frontend
