/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "overlay.hpp"

#include "core/tween.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ghostcontrol
{
namespace
{

using hui::Action;
using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

/* A focused editing workspace, not a translucent layer over navigation.
 * Only the editing primitives use surfaces; the host is an opaque page. */
/* Spectral palette: moonlight text on a void page, mint focus, phantom violet
 * as the second voice. Every surface is a little translucent so the spirits
 * behind the workspace read through it. */
/* Ink ramp: a graphite bench under a cold night, one teal indicator. The
 * colour lives in the ghost field and the live readout, never in the chrome. */
const Color kInk = Color::rgb(0xeef0f5);
const Color kMuted = Color::rgb(0x7a8296);
const Color kSecondary = Color::rgb(0xa3a9b8);
const Color kAccent = Color::rgb(0x5fc8b8);
const Color kHairline = Color::rgb(0xeef0f5, 0.08f);
const Color kPage = Color::rgb(0x07080d);
const Color kWorkspace = Color::rgb(0x0b0d14);

/* Short instructions live beside, rather than above, the editing surface. */
const char *subtitle_for(OverlayKind kind)
{
    switch (kind)
    {
    case OverlayKind::range:
        return "Adjust with Left / Right.";
    case OverlayKind::color:
        return "Choose a colour with the D-pad.";
    case OverlayKind::text:
        return "Choose keys with the D-pad.";
    case OverlayKind::picker:
        return "Choose with Up / Down.";
    case OverlayKind::keybinder:
        return "Cross starts button capture.";
    default:
        return "";
    }
}

int channel(float value)
{
    return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

constexpr float kCanvasW = 1920.0f;
constexpr float kCanvasH = 1080.0f;

} // namespace

hui::ui::Theme ghost_theme()
{
    hui::ui::Theme theme = hui::ui::default_theme();
    theme.page = kWorkspace;
    theme.page_text = kInk;
    theme.page_text_muted = kSecondary;
    /* Flat surfaces on a hairline. Depth comes from the surface steps, not from
     * glow: the only bright thing on any screen is the ghost field itself. */
    theme.style = hui::ui::SurfaceStyle::flat;
    theme.corner = hui::ui::Corner::round;
    theme.radius = 8.0f;
    theme.radius_card = 14.0f;
    theme.border = 1.0f;
    theme.button_border = 1.0f;
    theme.shadow = hui::gfx::Color::rgb(0x000000, 0.0f);
    theme.shadow_blur = 0.0f;
    theme.focus_width = 2.0f;
    theme.focus_gap = 3.0f;
    theme.surface = hui::gfx::Color::rgb(0x11141c);
    theme.surface_high = hui::gfx::Color::rgb(0x1a1e28);
    theme.text = kInk;
    theme.text_muted = kMuted;
    /* On a dark bench the primary action is the off-white from the ramp; the
     * teal is the single indicator colour, used for the selection and the one
     * live reading that matters. */
    theme.primary = kInk;
    theme.on_primary = hui::gfx::Color::rgb(0x0b0d14);
    theme.secondary = hui::gfx::Color::rgb(0x1a1e28);
    theme.on_secondary = kInk;
    theme.accent = kAccent;
    theme.outline = kHairline;
    theme.focus = kAccent;
    theme.light = hui::gfx::Color::rgb(0xeef0f5, 0.10f);
    theme.danger = hui::gfx::Color::rgb(0xe5484d);
    theme.success = hui::gfx::Color::rgb(0x4cc38a);
    theme.warning = hui::gfx::Color::rgb(0xd8a657);
    theme.omega = 16.0f;
    theme.damping = 1.0f;
    theme.dark = true;
    return theme;
}

/* The core's button vocabulary, mapped to and from the kit's actions. Only the
 * buttons the key binder can offer appear here; the service names a button
 * without one as NONE, which an edit writes back as an unbound map.* value.
 * CROSS/CIRCLE are the physical buttons, not the confirm/back aliases, so a
 * profile's map.CIRCLE row shows CIRCLE even though the shell's back verb is
 * bound to it. */
const char *action_button_name(hui::Action action)
{
    switch (action)
    {
    case hui::Action::confirm: return "CROSS";
    case hui::Action::back: return "CIRCLE";
    case hui::Action::north: return "TRIANGLE";
    case hui::Action::west: return "SQUARE";
    case hui::Action::up: return "UP";
    case hui::Action::down: return "DOWN";
    case hui::Action::left: return "LEFT";
    case hui::Action::right: return "RIGHT";
    case hui::Action::l3: return "L3";
    case hui::Action::r3: return "R3";
    case hui::Action::page_prev: return "L1";
    case hui::Action::page_next: return "R1";
    case hui::Action::jump_prev: return "L2";
    case hui::Action::jump_next: return "R2";
    case hui::Action::menu: return "OPTIONS";
    case hui::Action::touch: return "TOUCHPAD";
    default: return "NONE";
    }
}

hui::Action button_action_from_name(std::string_view name)
{
    if (name == "CROSS") return hui::Action::confirm;
    if (name == "CIRCLE") return hui::Action::back;
    if (name == "TRIANGLE") return hui::Action::north;
    if (name == "SQUARE") return hui::Action::west;
    if (name == "UP") return hui::Action::up;
    if (name == "DOWN") return hui::Action::down;
    if (name == "LEFT") return hui::Action::left;
    if (name == "RIGHT") return hui::Action::right;
    if (name == "L3") return hui::Action::l3;
    if (name == "R3") return hui::Action::r3;
    if (name == "L1") return hui::Action::page_prev;
    if (name == "R1") return hui::Action::page_next;
    if (name == "L2") return hui::Action::jump_prev;
    if (name == "R2") return hui::Action::jump_next;
    if (name == "OPTIONS") return hui::Action::menu;
    if (name == "TOUCHPAD") return hui::Action::touch;
    return hui::Action::count;
}

Rect OverlayHost::panel_rect()
{
    return {0.0f, 0.0f, kCanvasW, kCanvasH};
}

Rect OverlayHost::content_rect()
{
    return {576.0f, 350.0f, 1248.0f, 586.0f};
}

void OverlayHost::open(const OverlayRequest &request, hui::ui::Feedback &feedback)
{
    request_ = request;
    kind_ = request.kind;
    have_result_ = false;
    result_ = OverlayResult{};
    age_ = 0.0f;
    clock_ = 0.0f;
    text_ = request.text;

    const hui::ui::Theme theme = ghost_theme();
    const std::string field_label = request.label.empty() ? request.title : request.label;

    switch (kind_)
    {
    case OverlayKind::range:
        slider_ = hui::ui::Slider{};
        slider_.style.theme = theme;
        slider_.style.on_page = true;
        slider_.style.label_size = 28.0f;
        slider_.style.value_size = 32.0f;
        slider_.style.end_label_size = 24.0f;
        slider_.style.bubble_height = 48.0f;
        slider_.style.bubble = hui::ui::BubbleMode::always;
        slider_.style.ticks = 9;
        slider_.style.end_labels = true;
        slider_.set_label(field_label);
        slider_.set_range(request.min, request.max, request.step);
        slider_.set_unit(request.unit);
        slider_.set_value(std::clamp(request.value, request.min, request.max));
        break;
    case OverlayKind::color:
        color_ = hui::ui::ColorPicker{};
        color_.style.theme = theme;
        color_.style.on_page = true;
        color_.style.title_size = 26.0f;
        color_.style.kind = hui::ui::ColorPickerKind::swatches;
        color_.style.columns = 6;
        color_.style.swatch_size = 128.0f;
        color_.style.swatch_gap = 20.0f;
        color_.style.swatch_radius = 3.0f;
        /* The ring picks as it moves, so the preview tracks the D-pad and
         * Cross is free to mean "accept" rather than "pick". */
        color_.style.select_on_move = true;
        // Draw the preview in the host: the kit's secondary preview caption
        // is fixed at 19px, below this workspace's readable type floor.
        color_.style.preview = false;
        color_.set_title(field_label);
        color_.set_color(Color{static_cast<float>(request.r) / 255.0f,
                               static_cast<float>(request.g) / 255.0f,
                               static_cast<float>(request.b) / 255.0f, 1.0f});
        break;
    case OverlayKind::text:
        keyboard_ = hui::ui::Keyboard{};
        keyboard_.style.theme = theme;
        keyboard_.style.panel = false;
        keyboard_.style.label_size = 32.0f;
        keyboard_.style.wide_label_size = 24.0f;
        keyboard_.style.gap = 12.0f;
        keyboard_.style.bindings = hui::ui::KeyboardBindings::standard();
        keyboard_.style.max_length = request.max_length;
        /* The keyboard owns no text: every character it emits goes into the
         * buffer here, which the field above the keys displays. */
        keyboard_.on_text = [this](std::string_view utf8) { text_.append(utf8); };
        keyboard_.on_backspace = [this]() {
            if (!text_.empty())
                text_.pop_back();
        };
        keyboard_.enter();
        field_ = hui::ui::TextField{};
        field_.style.theme = theme;
        // The kit renders its counter at 90% of the label size.
        field_.style.label_size = 28.0f;
        field_.style.helper_size = 24.0f;
        field_.style.field_height = 72.0f;
        field_.style.on_page = true;
        field_.style.max_length = request.max_length;
        field_.set_label(field_label);
        field_.set_placeholder("Nothing entered yet");
        field_.set_text(text_);
        break;
    case OverlayKind::picker:
    {
        wheel_ = hui::ui::WheelPicker{};
        wheel_.style.theme = theme;
        wheel_.style.visible = 5;
        wheel_.style.row_height = 88.0f;
        wheel_.style.title_size = 26.0f;
        wheel_.style.value_size = 34.0f;
        wheel_.style.shrink = 0.0f;
        wheel_.style.boxed = false;
        wheel_.style.on_page = true;
        wheel_.style.highlight.radius = 0.0f;
        wheel_.set_title(field_label);
        std::vector<hui::ui::WheelColumn> columns;
        columns.push_back(hui::ui::WheelColumn(request.items, 1.0f));
        wheel_.set_columns(std::move(columns));
        if (!request.items.empty())
        {
            const int last = static_cast<int>(request.items.size()) - 1;
            wheel_.set_index(0, std::clamp(request.index, 0, last));
        }
        break;
    }
    case OverlayKind::keybinder:
        binder_ = hui::ui::KeyBinder{};
        binder_.style.theme = theme;
        binder_.style.on_page = true;
        binder_.style.row_height = 76.0f;
        binder_.style.label_size = 28.0f;
        binder_.style.hint_size = 24.0f;
        binder_.style.highlight.radius = 0.0f;
        /* A row editing a saved profile may name a wider input set than the
         * kit's default; 0 keeps the default untouched. */
        if (request.allowed != 0)
            binder_.style.allowed = request.allowed;
        if (request.single_edit) {
            binder_.style.conflict = hui::ui::BindConflict::refuse;
            binder_.style.reset_row = false;
        }
        // Profile values need their original expressions, not the kit's
        // generic unbound label. Draw them below without modifying kit code,
        // binding keys/actions, focus, capture or persistence semantics.
        if (!request.binding_expressions.empty() &&
            request.binding_expressions.size() == request.bindings.size())
            binder_.style.unbound_text.clear();
        binder_.set_bindings(request.bindings);
        break;
    default:
        break;
    }

    layout();
    fill_hints();
    feedback.play(hui::audio::Cue::modal_open);
}

void OverlayHost::layout()
{
    const Rect content = content_rect();
    switch (kind_)
    {
    case OverlayKind::range:
        slider_.set_bounds({content.x + 24.0f, content.y + 200.0f,
                            content.w - 48.0f, slider_.preferred_height()});
        break;
    case OverlayKind::color:
        color_.set_bounds({content.x, content.y + 64.0f, 920.0f, 484.0f});
        break;
    case OverlayKind::text:
    {
        const float field_h = field_.preferred_height();
        field_.set_bounds({content.x, content.y, content.w, field_h});
        const float keys_y = content.y + field_h + 40.0f;
        keyboard_.set_bounds({content.x, keys_y, content.w, content.y + content.h - keys_y});
        break;
    }
    case OverlayKind::picker:
    {
        wheel_.set_bounds({content.x, content.y + 48.0f,
                           content.w, wheel_.preferred_height()});
        break;
    }
    case OverlayKind::keybinder:
        binder_.set_bounds({content.x, content.y, content.w,
                            std::min(binder_.preferred_height(), content.h)});
        break;
    default:
        break;
    }
}

void OverlayHost::fill_hints()
{
    using hui::ui::Button;
    switch (kind_)
    {
    case OverlayKind::range:
        hints_[0] = {Button::dpad, "Adjust"};
        hints_[1] = {Button::cross, "Confirm"};
        hints_[2] = {Button::circle, "Cancel"};
        hint_count_ = 3;
        break;
    case OverlayKind::color:
        hints_[0] = {Button::dpad, "Choose"};
        hints_[1] = {Button::cross, "Confirm"};
        hints_[2] = {Button::circle, "Cancel"};
        hint_count_ = 3;
        break;
    case OverlayKind::text:
        hints_[0] = {Button::dpad, "Move"};
        hints_[1] = {Button::cross, "Type"};
        hints_[2] = {Button::triangle, "Space"};
        hints_[3] = {Button::circle, "Cancel"};
        hint_count_ = 4;
        break;
    case OverlayKind::picker:
        hints_[0] = {Button::dpad, "Choose"};
        hints_[1] = {Button::cross, "Choose"};
        hints_[2] = {Button::circle, "Cancel"};
        hint_count_ = 3;
        break;
    case OverlayKind::keybinder:
        hints_[0] = {Button::dpad, "Move"};
        hints_[1] = {Button::cross, "Bind"};
        hints_[2] = {Button::circle, "Cancel"};
        hint_count_ = 3;
        break;
    default:
        hint_count_ = 0;
        break;
    }
}

void OverlayHost::close(hui::ui::Feedback &feedback)
{
    if (kind_ != OverlayKind::none)
        feedback.play(hui::audio::Cue::modal_close);
    kind_ = OverlayKind::none;
    hint_count_ = 0;
}

void OverlayHost::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    if (kind_ == OverlayKind::none)
        return;
    clock_ += dt;
    age_ += dt;

    switch (kind_)
    {
    case OverlayKind::range:
    {
        slider_.set_active(true);
        const hui::ui::Event event = slider_.handle(input, feedback);
        slider_.update(dt);
        if (input.is_pressed(Action::confirm))
        {
            result_.kind = OverlayKind::range;
            result_.value = slider_.value();
            have_result_ = true;
            close(feedback);
        }
        else if (event == hui::ui::Event::cancelled)
            close(feedback);
        break;
    }
    case OverlayKind::color:
    {
        color_.set_active(true);
        const hui::ui::Event event = color_.handle(input, feedback);
        color_.update(dt);
        if (input.is_pressed(Action::confirm))
        {
            const Color picked = color_.color();
            result_.kind = OverlayKind::color;
            result_.r = channel(picked.r);
            result_.g = channel(picked.g);
            result_.b = channel(picked.b);
            have_result_ = true;
            close(feedback);
        }
        else if (event == hui::ui::Event::cancelled)
            close(feedback);
        break;
    }
    case OverlayKind::text:
    {
        /* The field is a display: it shows the buffer the keyboard feeds, and
         * never takes input of its own while the keys are up. */
        field_.set_active(true);
        keyboard_.set_length(static_cast<int>(text_.size()));
        const hui::ui::Event event = keyboard_.handle(input, feedback);
        keyboard_.update(dt);
        field_.set_text(text_);
        field_.update(dt);
        if (event == hui::ui::Event::activated) // the Done key went in
        {
            result_.kind = OverlayKind::text;
            result_.text = text_;
            have_result_ = true;
            close(feedback);
        }
        else if (event == hui::ui::Event::cancelled)
            close(feedback);
        break;
    }
    case OverlayKind::picker:
    {
        wheel_.set_active(true);
        const hui::ui::Event event = wheel_.handle(input, feedback);
        wheel_.update(dt);
        if (event == hui::ui::Event::activated)
        {
            result_.kind = OverlayKind::picker;
            result_.index = wheel_.index(0);
            result_.label = wheel_.value(0);
            /* Carry what the choice stands for as well as its text, so a
             * command can take the address rather than the display label. */
            const int chosen = result_.index;
            if (chosen >= 0 && static_cast<std::size_t>(chosen) < request_.item_values.size())
                result_.item_value = request_.item_values[static_cast<std::size_t>(chosen)];
            else
                result_.item_value = result_.label;
            have_result_ = true;
            close(feedback);
        }
        else if (event == hui::ui::Event::cancelled)
            close(feedback);
        break;
    }
    case OverlayKind::keybinder:
    {
        // The kit has no focus/event when every profile source was rejected.
        // Back must still cancel the owned chain rather than strand the modal.
        if (binder_.bindings().empty() && input.is_pressed(Action::back)) {
            close(feedback);
            break;
        }
        binder_.set_active(true);
        const hui::ui::Event event = binder_.handle(input, feedback);
        binder_.update(dt);
        if (event == hui::ui::Event::changed && !binder_.listening())
        {
            /* Scaffold: report what changed. A wave-2 screen maps changed_id()
             * to its own profile command through the same row metadata. */
            const int id = binder_.changed_id();
            result_.kind = OverlayKind::keybinder;
            result_.index = id;
            if (id == hui::ui::KeyBinder::kResetId)
            {
                result_.text = "defaults restored";
                result_.action.clear();
            }
            else
            {
                for (const hui::ui::KeyBinding &row : binder_.bindings())
                    if (row.id == id)
                        result_.text = row.label;
                /* What the row is now bound to, as the service names it, so a
                 * {action} template posts the new binding rather than the old
                 * one. The shell carries this into the row's command. */
                result_.action = action_button_name(binder_.action_of(id));
            }
            have_result_ = true;
        }
        else if (event == hui::ui::Event::cancelled)
            close(feedback);
        break;
    }
    default:
        break;
    }
}

bool OverlayHost::take_result(OverlayResult *out)
{
    if (!have_result_)
        return false;
    if (out != nullptr)
        *out = result_;
    have_result_ = false;
    return true;
}

float OverlayHost::presence() const
{
    return hui::tween::clamp01(age_ / 0.18f);
}

void OverlayHost::draw(DrawList &list, const hui::ui::Fonts &fonts) const
{
    if (kind_ == OverlayKind::none)
        return;

    // Opaque from the first frame: no old rows, tabs or rail show through.
    // presence() remains the shell's timing API, not a fade over stale chrome.
    list.rounded_rect({0.0f, 190.0f, kCanvasW, kCanvasH-190.0f}, 0.0f, kPage);
    list.rounded_rect({528.0f, 232.0f, kCanvasW - 528.0f, 752.0f}, 0.0f, kWorkspace);
    hui::ui::text(list, fonts.semibold, "EDIT", 96.0f, 220.0f, 20.0f, kMuted);

    const auto heading_theme = ghost_theme();
    hui::ui::Painter heading(list, fonts, heading_theme);
    hui::ui::text(list, fonts.semibold,
                  hui::ui::fit_label(heading, request_.title, 46.0f, 1728.0f),
                  96.0f, 278.0f, 46.0f, kInk);
    list.rounded_rect({96.0f, 310.0f, 1728.0f, 1.0f}, 0.0f, kHairline);

    hui::ui::text(list, fonts.semibold, "EDITING", 96.0f, 364.0f, 20.0f, kAccent);
    // Fit only presentation strings; never shorten the buffer or result.
    hui::ui::text(list, fonts.regular,
                  hui::ui::fit_body(heading, subtitle_for(kind_), 24.0f, 404.0f),
                  96.0f, 416.0f, 24.0f, kInk);
    const char *accept = kind_ == OverlayKind::text ? "Select Done to confirm."
                       : kind_ == OverlayKind::keybinder ? "Then press the new button."
                       : "Cross confirms your choice.";
    hui::ui::text(list, fonts.regular, accept, 96.0f, 460.0f, 24.0f, kMuted);
    hui::ui::text(list, fonts.regular,
                  kind_ == OverlayKind::keybinder ? "Circle returns." : "Circle cancels.",
                  96.0f, 504.0f, 24.0f, kMuted);

    // Fixed footer belongs to the editor, not the underlying screen.
    list.rounded_rect({0.0f, 984.0f, kCanvasW, 96.0f}, 0.0f, kPage);
    list.rounded_rect({96.0f, 984.0f, 1728.0f, 1.0f}, 0.0f, kHairline);
    hui::ui::HintLayout footer;
    footer.cy = 1032.0f;
    footer.size = 36.0f;
    footer.text_size = 26.0f;
    hui::ui::draw_hints(list, fonts, hui::ui::GlyphStyle::mono(kInk, kWorkspace),
                        hints_.data(), hint_count_, 96.0f, false, footer);

    // Widget geometry is stationary and identical to layout(). This clip
    // leaves room for focus rings, while keeping the heading/footer fixed.
    hui::ui::Canvas canvas{list, fonts, 0, clock_};
    list.push_clip({544.0f, 326.0f, 1280.0f, 634.0f});
    switch (kind_)
    {
    case OverlayKind::range:
        slider_.draw(canvas);
        break;
    case OverlayKind::color:
        color_.draw(canvas);
        list.rounded_rect({1560.0f, 380.0f, 240.0f, 240.0f}, 3.0f, color_.color());
        hui::ui::text(list, fonts.regular, "Current colour", 1560.0f, 670.0f, 24.0f, kMuted);
        hui::ui::text(list, fonts.semibold, color_.hex(), 1560.0f, 712.0f, 30.0f, kInk);
        break;
    case OverlayKind::text:
        field_.draw(canvas);
        keyboard_.draw(canvas);
        break;
    case OverlayKind::picker:
        wheel_.draw(canvas);
        break;
    case OverlayKind::keybinder:
    {
        binder_.draw(canvas);
        const auto &bindings = binder_.bindings();
        if (!request_.binding_expressions.empty() &&
            request_.binding_expressions.size() == bindings.size()) {
            const auto &style = binder_.style;
            const Rect bounds = style.panel ? binder_.bounds().inset(style.panel_padding)
                                            : binder_.bounds();
            hui::ui::Painter paint(list, fonts, style.theme);
            // Use the real widget's scrolling row rectangles and clip. Long
            // expressions are fitted to the remaining value column, never
            // painted over a source key or outside the editing viewport.
            list.push_clip(bounds);
            for (std::size_t i = 0; i < bindings.size(); ++i) {
                const auto &binding = bindings[i];
                if (binding.action != Action::count ||
                    (binder_.listening() && binder_.focus() == static_cast<int>(i)))
                    continue;
                const Rect row = binder_.row_rect(static_cast<int>(i));
                if (row.y >= bounds.y + bounds.h || row.y + row.h <= bounds.y)
                    continue;
                const std::string_view expr = request_.binding_expressions[i];
                const std::string_view value = binding.locked
                    ? (expr.empty() ? std::string_view("(empty binding)") : expr)
                    : std::string_view("Not set");
                const float right = row.x + row.w - style.padding;
                const float room = row.w - 2.0f * style.padding - 18.0f -
                                   paint.label_width(binding.label, style.label_size);
                list.push_opacity(binding.locked ? 0.45f : 1.0f);
                paint.body(hui::ui::fit_body(paint, value, style.hint_size, std::max(0.0f, room)),
                           right, row.cy() + style.hint_size * 0.35f,
                           style.hint_size, style.theme.text_muted, hui::gfx::Align::right);
                list.pop_opacity();
            }
            list.pop_clip();
        }
        break;
    }
    default:
        break;
    }
    list.pop_clip();
}

} // namespace ghostcontrol
