// RmlUi's system interface and input on SDL3 (ui_system.h).

#include "ui_system.h"

#include <RmlUi/Core/Log.h>

#include <cstdio>

namespace xf::ui {

namespace {
enum { CURSOR_DEFAULT, CURSOR_POINTER, CURSOR_TEXT, CURSOR_UNAVAILABLE };
} // namespace

SystemInterface::SystemInterface(SDL_Window *window) : window_(window) {
    cursors_[CURSOR_DEFAULT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    cursors_[CURSOR_POINTER] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    cursors_[CURSOR_TEXT] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    cursors_[CURSOR_UNAVAILABLE] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NOT_ALLOWED);
}

SystemInterface::~SystemInterface() {
    SDL_SetCursor(nullptr);
    for (SDL_Cursor *c : cursors_)
        if (c) SDL_DestroyCursor(c);
}

double SystemInterface::GetElapsedTime() { return double(SDL_GetTicksNS()) * 1e-9; }

bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String &message) {
    if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT) errors_++;
    if (type <= Rml::Log::LT_WARNING) std::fprintf(stderr, "launcher: ui: %s\n", message.c_str());
    return true;
}

void SystemInterface::JoinPath(Rml::String &translated_path, const Rml::String &document_path, const Rml::String &path) {
    if (path.starts_with("mem:")) {
        translated_path = path;
        return;
    }
    Rml::SystemInterface::JoinPath(translated_path, document_path, path);
}

void SystemInterface::SetMouseCursor(const Rml::String &cursor_name) {
    SDL_Cursor *cursor = cursors_[CURSOR_DEFAULT];
    if (cursor_name == "pointer")
        cursor = cursors_[CURSOR_POINTER];
    else if (cursor_name == "text")
        cursor = cursors_[CURSOR_TEXT];
    else if (cursor_name == "unavailable")
        cursor = cursors_[CURSOR_UNAVAILABLE];
    if (cursor) SDL_SetCursor(cursor);
}

void SystemInterface::SetClipboardText(const Rml::String &text) { SDL_SetClipboardText(text.c_str()); }

void SystemInterface::GetClipboardText(Rml::String &text) {
    if (char *clip = SDL_GetClipboardText()) {
        text = clip;
        SDL_free(clip);
    }
}

void SystemInterface::ActivateKeyboard(Rml::Vector2f, float) { SDL_StartTextInput(window_); }
void SystemInterface::DeactivateKeyboard() { SDL_StopTextInput(window_); }

int key_modifiers() {
    const SDL_Keymod mods = SDL_GetModState();
    int state = 0;
    if (mods & SDL_KMOD_CTRL) state |= Rml::Input::KM_CTRL;
    if (mods & SDL_KMOD_SHIFT) state |= Rml::Input::KM_SHIFT;
    if (mods & SDL_KMOD_ALT) state |= Rml::Input::KM_ALT;
    if (mods & SDL_KMOD_GUI) state |= Rml::Input::KM_META;
    if (mods & SDL_KMOD_CAPS) state |= Rml::Input::KM_CAPSLOCK;
    if (mods & SDL_KMOD_NUM) state |= Rml::Input::KM_NUMLOCK;
    return state;
}

Rml::Input::KeyIdentifier convert_key(SDL_Keycode key) {
    using namespace Rml::Input;
    if (key >= SDLK_A && key <= SDLK_Z) return KeyIdentifier(KI_A + (key - SDLK_A));
    if (key >= SDLK_0 && key <= SDLK_9) return KeyIdentifier(KI_0 + (key - SDLK_0));
    if (key >= SDLK_F1 && key <= SDLK_F12) return KeyIdentifier(KI_F1 + (key - SDLK_F1));
    switch (key) {
    case SDLK_SPACE: return KI_SPACE;
    case SDLK_RETURN: return KI_RETURN;
    case SDLK_KP_ENTER: return KI_NUMPADENTER;
    case SDLK_ESCAPE: return KI_ESCAPE;
    case SDLK_TAB: return KI_TAB;
    case SDLK_BACKSPACE: return KI_BACK;
    case SDLK_DELETE: return KI_DELETE;
    case SDLK_INSERT: return KI_INSERT;
    case SDLK_HOME: return KI_HOME;
    case SDLK_END: return KI_END;
    case SDLK_PAGEUP: return KI_PRIOR;
    case SDLK_PAGEDOWN: return KI_NEXT;
    case SDLK_LEFT: return KI_LEFT;
    case SDLK_RIGHT: return KI_RIGHT;
    case SDLK_UP: return KI_UP;
    case SDLK_DOWN: return KI_DOWN;
    case SDLK_LSHIFT: return KI_LSHIFT;
    case SDLK_RSHIFT: return KI_RSHIFT;
    case SDLK_LCTRL: return KI_LCONTROL;
    case SDLK_RCTRL: return KI_RCONTROL;
    case SDLK_LALT: return KI_LMENU;
    case SDLK_RALT: return KI_RMENU;
    case SDLK_MINUS: return KI_OEM_MINUS;
    case SDLK_EQUALS: return KI_OEM_PLUS;
    case SDLK_COMMA: return KI_OEM_COMMA;
    case SDLK_PERIOD: return KI_OEM_PERIOD;
    case SDLK_SLASH: return KI_OEM_2;
    case SDLK_SEMICOLON: return KI_OEM_1;
    case SDLK_BACKSLASH: return KI_OEM_5;
    case SDLK_LEFTBRACKET: return KI_OEM_4;
    case SDLK_RIGHTBRACKET: return KI_OEM_6;
    case SDLK_APOSTROPHE: return KI_OEM_7;
    case SDLK_GRAVE: return KI_OEM_3;
    default: return KI_UNKNOWN;
    }
}

bool process_event(Rml::Context *context, SDL_Window *window, const SDL_Event &event) {
    const float density = SDL_GetWindowPixelDensity(window);
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        return context->ProcessMouseMove(int(event.motion.x * density), int(event.motion.y * density),
                                         key_modifiers());
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        SDL_CaptureMouse(true);
        return context->ProcessMouseButtonDown(event.button.button - 1, key_modifiers());
    case SDL_EVENT_MOUSE_BUTTON_UP:
        SDL_CaptureMouse(false);
        return context->ProcessMouseButtonUp(event.button.button - 1, key_modifiers());
    case SDL_EVENT_MOUSE_WHEEL:
        return context->ProcessMouseWheel(Rml::Vector2f(-event.wheel.x, -event.wheel.y), key_modifiers());
    case SDL_EVENT_KEY_DOWN: return context->ProcessKeyDown(convert_key(event.key.key), key_modifiers());
    case SDL_EVENT_KEY_UP: return context->ProcessKeyUp(convert_key(event.key.key), key_modifiers());
    case SDL_EVENT_TEXT_INPUT: return context->ProcessTextInput(Rml::String(event.text.text));
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: return context->ProcessMouseLeave();
    default: return true;
    }
}

} // namespace xf::ui
