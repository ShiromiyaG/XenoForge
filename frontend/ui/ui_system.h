// RmlUi's system interface on SDL3 (time, log, cursor, clipboard, text
// input) and SDL's events turned into RmlUi's input (mouse, wheel, keys,
// text), after RmlUi's SDL platform backend (MIT, Backends/
// RmlUi_Platform_SDL.cpp). The pad is not here: it moves focus, which the
// launcher's view does (launcher_view.h).

#pragma once

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>

#include <SDL3/SDL.h>

#include <string>

namespace xf::ui {

class SystemInterface final : public Rml::SystemInterface {
  public:
    explicit SystemInterface(SDL_Window *window);
    ~SystemInterface() override;

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override;
    // "mem:" names (the renderer's pictures) are whole paths already
    void JoinPath(Rml::String &translated_path, const Rml::String &document_path, const Rml::String &path) override;
    void SetMouseCursor(const Rml::String &cursor_name) override;
    void SetClipboardText(const Rml::String &text) override;
    void GetClipboardText(Rml::String &text) override;
    void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override;
    void DeactivateKeyboard() override;

    // whether the log has seen an error (the screens' files not found, say)
    int errors() const { return errors_; }

  private:
    SDL_Window *window_;
    SDL_Cursor *cursors_[4] = {};
    int errors_ = 0;
};

// An SDL event applied to the context: the mouse, wheel, keys and text. The
// window's pixel density scales the mouse to the context's pixels. True when
// the event went on unused by the context.
bool process_event(Rml::Context *context, SDL_Window *window, const SDL_Event &event);

Rml::Input::KeyIdentifier convert_key(SDL_Keycode key);
int key_modifiers();

} // namespace xf::ui
