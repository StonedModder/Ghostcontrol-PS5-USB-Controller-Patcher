/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The modal overlays the shell can host over its list.
 *
 * The kit ships every widget a settings screen needs -- a slider, a colour
 * picker, a keyboard, a wheel and a key binder -- but each is designed to be
 * hosted by a page that owns the layout, the focus and the verbs. The toolbox
 * shell is that host: it owns one modal at a time, forwards the pad to the
 * widget that is up, shows that widget's own verbs in the hint row, and turns
 * the value the player leaves behind into a queued command.
 *
 * Keeping the host in its own translation unit keeps the shell's frame code
 * about the list it draws, and lets a wave-2 screen add a modal by declaring
 * it on a row (see screens.hpp) instead of touching this file.
 */
#pragma once

#include "screens.hpp"

#include "core/input.hpp"
#include "gfx/draw_list.hpp"
#include "ui/components/color_picker.hpp"
#include "ui/components/key_binder.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/components/range_slider.hpp"
#include "ui/components/text_field.hpp"
#include "ui/components/wheel_picker.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ghostcontrol
{

/** Everything a screen needs to say to open a modal.
 *
 * A row that declares an OverlayKind is turned into one of these by the shell,
 * but the host itself does not know about rows: a host-snapshot harness, a
 * setup wizard or a wave-2 screen can open one directly. */
struct OverlayRequest
{
    OverlayKind kind = OverlayKind::none;
    std::string title; // the heading, usually the row's label
    std::string label; // the widget's own label
    std::string unit;  // appended to a range value (" ms")

    // range
    float min = 0.0f;
    float max = 100.0f;
    float step = 0.0f; // 0: continuous
    float value = 0.0f;

    // colour, 0..255
    int r = 255;
    int g = 255;
    int b = 255;

    // text entry
    std::string text;
    int max_length = 0;

    // item picker
    std::vector<std::string> items;
    /* Optional per-item underlying value, parallel to `items`: what the choice
     * stands for once it is made (a device address, a file path). Empty means
     * the item's own text is the value, so every existing picker is unchanged.
     * The chosen one arrives in OverlayResult::item_value. */
    std::vector<std::string> item_values;
    int index = 0;

    // key binder
    std::vector<hui::ui::KeyBinding> bindings;
    /** The action bits a key binder row may be bound to. 0 keeps the kit's
     *  default (the face buttons, the D-pad and the stick clicks); a screen
     *  editing a saved profile widens it to the shoulders, triggers, Options
     *  and touchpad, which the profile's map.* keys also name. */
    std::uint32_t allowed = 0;
    // Profile persistence accepts one mapping per command, never an implicit
    // multi-row swap/reset. Other widget clients keep the kit defaults.
    bool single_edit = false;
    /** Exact profile expressions, parallel to bindings. Presentation only:
     * locked/unsupported values are retained; action and source key stay intact.
     * Empty metadata keeps the generic key-binder presentation unchanged. */
    std::vector<std::string> binding_expressions;
};

/** What a modal hands back when the player accepts it. */
struct OverlayResult
{
    OverlayKind kind = OverlayKind::none;
    float value = 0.0f; // range
    int r = 0;          // colour
    int g = 0;
    int b = 0;
    std::string text;   // text entry, or a key binder's summary
    int index = 0;      // picker
    std::string label;  // picker's chosen label
    /* The picker choice's underlying value: the request's item_values entry at
     * the chosen index, or the label when the request named none. This is what
     * a {address} argument template is filled from. */
    std::string item_value;
    /* For a chained row (Row::overlay_next): the first modal's chosen label,
     * carried into the second modal's result so the row's single
     * overlay_command can name both. Filled by the shell, never by the host. */
    std::string parent_label;
    /* A key binder's newly bound button as the service names it ("CROSS",
     * "L3", "NONE"): what a {action} template is filled from. */
    std::string action;
};

/** The toolbox palette every kit widget is styled with (Dialog, overlays). */
hui::ui::Theme ghost_theme();

/** The service's name for the button a bound action maps to ("CROSS", "L1",
 *  "NONE"), so a key binder's result can fill a command argument. Unknown
 *  actions read "NONE", the service's own word for an unbound map.* value. */
const char *action_button_name(hui::Action action);
/** The action a service button name maps to ("CIRCLE" -> Action::back), or
 *  Action::count for a name the key binder cannot offer. */
hui::Action button_action_from_name(std::string_view name);

class OverlayHost
{
  public:
    bool is_open() const
    {
        return kind_ != OverlayKind::none;
    }

    const OverlayRequest *request() const { return is_open() ? &request_ : nullptr; }

    /** Opens the modal the request names, replacing anything already up. */
    void open(const OverlayRequest &request, hui::ui::Feedback &feedback);
    /** Closes it with no value: a cancel, or the screen is leaving. */
    void close(hui::ui::Feedback &feedback);
    /** Forwards one frame; on an accept it closes and arms the result. */
    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    /** True once after an accept, handing the value back and clearing it. */
    bool take_result(OverlayResult *out);

    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts) const;

    /** The verbs of the widget that is up, for the shell's hint row. */
    std::span<const hui::ui::Hint> hints() const
    {
        return {hints_.data(), static_cast<std::size_t>(hint_count_)};
    }
    /** 0..1 entrance ease, for the shell to time its own chrome against. */
    float presence() const;

  private:
    // Places the widget inside the panel. Kept in one place so what is drawn
    // and what is hit are the same rectangle.
    void layout();
    void fill_hints();

    static hui::gfx::Rect panel_rect();
    static hui::gfx::Rect content_rect();

    OverlayKind kind_ = OverlayKind::none;
    OverlayRequest request_;
    OverlayResult result_;
    bool have_result_ = false;
    float age_ = 0.0f;
    float clock_ = 0.0f;

    hui::ui::Slider slider_;
    hui::ui::ColorPicker color_;
    hui::ui::Keyboard keyboard_;
    hui::ui::TextField field_;
    hui::ui::WheelPicker wheel_;
    hui::ui::KeyBinder binder_;
    std::string text_; // the text being entered

    std::array<hui::ui::Hint, 4> hints_{};
    int hint_count_ = 0;
};

} // namespace ghostcontrol
