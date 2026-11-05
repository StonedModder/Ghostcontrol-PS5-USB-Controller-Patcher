# Overlay API

How a screen opens a modal and posts the value the player chooses.

The shell hosts one modal at a time over the list (`ghostcontrol::OverlayHost`,
`ui/src/overlay.hpp`). A screen does not draw or drive a modal itself: it
declares, on a row, *which* widget opens and *what command* receives the result.
The kit's own components back each kind, so a slider, colour picker, keyboard,
wheel and key binder all come for free.

## Declaring an overlay on a row

Each `Row` (see `ui/src/screens.hpp`) carries overlay metadata after its command
fields. The defaults mean "no overlay", so existing rows are unchanged.

| Field | Meaning |
| --- | --- |
| `overlay` | Which modal confirm opens: `none`, `range`, `color`, `text`, `picker`, `keybinder`. |
| `overlay_min` / `overlay_max` / `overlay_step` | The `range` bounds; step `0` is continuous. |
| `overlay_value` | The `range` starting thumb, when the row is not live. |
| `overlay_unit` | Suffix for a range value (`" ms"`). |
| `overlay_command` | The IPC command fired with the result, once per template. |
| `overlay_arguments` | One argument template per command. Placeholders are filled from the result. |

`overlay` takes precedence over `command` and `detail`: a settable reading can
stay `RowState::live` (it still shows its value) and open a modal on confirm.

Placeholders: `{value}` (range, formatted as an integer unless it has a
fraction), `{r}` `{g}` `{b}` (colour, 0..255), `{text}` (text entry), `{index}`
and `{label}` (picker), `{address}` (picker, the chosen item's underlying value
— see below), `{id}` (key binder), `{parent}` `{action}` (a chained modal's
first choice and a key binder's captured button -- see below). An unknown
placeholder is left in
place so a typo surfaces in the argument the service rejects.

### Range

```cpp
{"Effect period", "-", "How fast the effect cycles", RowState::live, {},
 LiveSource::fx_period_ms, nullptr, nullptr, LiveSource::none,
 OverlayKind::range, 100.0f, 60000.0f, 100.0f, 1000.0f, " ms", "RUNTIME_SET",
 {"dualsense_fx_period_ms={value}"}},
```

### Colour (three commands, one per channel)

```cpp
{"Lightbar colour", "-", "Red, green, blue", RowState::live, {},
 LiveSource::fx_color, nullptr, nullptr, LiveSource::none,
 OverlayKind::color, 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "RUNTIME_SET",
 {"dualsense_fx_r={r}", "dualsense_fx_g={g}", "dualsense_fx_b={b}"}},
```

### Text, picker, key binder

- **text**: a `Keyboard` under a `TextField` display; the Done key (or Cross on
  it) accepts, and the typed string arrives as `{text}`.
- **picker**: a `WheelPicker`. A row supplies its options as its `detail`
  lines, so no other data is needed; a row with `picker_source` feeds the wheel
  from live state or a fetched reply instead (below). The choice arrives as
  `{index}`, `{label}`, and `{address}` — the request's `item_values[index]`,
  or the label when the request named no values.
- **keybinder**: the profile editor seeds bindings from its own fresh
  `PROFILE_GET` reply. `OverlayRequest::binding_expressions` retains the exact
  expression beside each binding without changing its mapping key or action.
  Compound and unsupported values are locked and show their preserved expression;
  an empty unsupported expression shows `(empty expression)`. A genuine `NONE`
  mapping stays editable and shows `Not set`. Long expressions are fitted with an
  ellipsis inside the scrolling value column. Requests without expression metadata
  keep the generic widget's existing presentation. Locked rows cannot submit an edit.

### Picker over the paired Bluetooth list

A `picker` row can take its options from the polled view rather than the row
table. This is the Bluetooth screen's "Forget a device" row: the wheel shows a
human label while the command takes the device address the service expects.

```cpp
{"Forget a device", "", "Pick a paired device to drop", RowState::action,
 {}, LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::picker,
 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "BLUETOOTH_FORGET", {"{address}"},
 ReaderKind::none, PickerSource::bluetooth_paired},
```

`picker_source = bluetooth_paired` fills the wheel from
`ServiceSnapshot::View::bluetooth_paired`, the list the worker parsed out of the
last `BLUETOOTH_STATUS` reply: each entry carries the uppercase `address`
`BLUETOOTH_FORGET` accepts and a `<kind> - <ADDRESS>` label to show. The address
and the label travel together (`item_values` beside `items`), so `{address}`
resolves to the address and the argument never depends on how the label reads.
An empty list refuses the press with a "No paired devices" toast instead of
opening an empty wheel.

## How it works at run time

1. Confirm on a row whose `overlay != none` calls
   `ToolboxUi::open_overlay_for_row`, which builds an `OverlayRequest` (starting
   the range from the live reading where the row has one) and opens the host.
2. While the modal is up it owns the frame: the pad goes to the widget, and the
   hint row shows the widget's own verbs (Adjust/Set/Cancel, Keys/Press/Cancel,
   …).
3. On accept the host hands back an `OverlayResult`; the shell substitutes it
   into each template and queues one command per template through
   `ServiceSnapshot::queue_command`, then shows the result as a toast — the same
   path a plain command row uses. Nothing touches the service from the frame.

## Opening a modal without a row

For a screen that is not built from the row tables (and for the snapshot
harness), the shell exposes:

```cpp
ui.open_overlay(request, feedback);                      // a request you build
ui.open_row_overlay("Effect period", view, feedback);    // the modal a named row declares
```

`open_overlay` records no owning row, so an accepted result posts nothing; the
caller reads `OverlayHost::take_result` if it wants the value. Both hooks are
for the host harness; the app drives modals through rows.

## Adding a new kind

Add the enum value in `screens.hpp`, a case in `OverlayHost::open` / `layout` /
`update` / `draw` / `fill_hints` in `overlay.cpp`, and the placeholders in
`format_overlay_argument` (`toolbox_ui.cpp`). No shell or build changes: the
build collects every source under `src/`.

## Runtime picker options (wave 2b)

A picker no longer has to carry its options in the row table. A row names
where the items come from with `picker_source` (`ui/src/screens.hpp`):

| `picker_source` | Options come from |
| --- | --- |
| `detail` (default) | the row's own `detail` lines, as before |
| `bluetooth_paired` | the polled `View::bluetooth_paired`: one option per paired device, labelled `<kind> - <ADDRESS>` and valued by its address (no fetch) |
| `fetch` | a command's reply, read by `picker_command` / `picker_key` |

For `fetch`, `picker_command` is the command whose reply lists the options and
`picker_key` is the array field in that reply (`"imports"`, `"exports"`, …).
When the picker opens the shell queues `picker_command` through
`ServiceSnapshot::queue_fetch` and reads whatever is in the fetch store, so the
first open can be empty; a screen that wants the list ready queues the command
itself when its section opens. Options are plain strings, used as the item
label; the chosen one arrives as `{label}` (its position as `{index}`, and, when
the request gave per-item values, the chosen value as `{address}`).

### Fetching a reply into UI state

`ServiceSnapshot` gained a reply-capture path beside `queue_command`:

```cpp
bool queue_fetch(const char *command, const char *arg = nullptr, unsigned timeout_ms = 0);
ServiceSnapshot::Fetch last_reply(std::string_view command) const;

struct Fetch { std::string body; bool reached; unsigned long long generation; };
```

`queue_fetch` joins the same worker FIFO with the same validation as
`queue_command`, but keeps the reply in a per-command store instead of folding
it into a toast. `last_reply` returns the latest `body`, whether the service
`reached` at all, and a `generation` bumped on every fresh reply -- so a screen
compares the generation it last drew against (`0` means "no reply yet") and
rebuilds when it changes. Nothing here touches the service from a frame.

`Row` also carries `command_timeout_ms`: the per-command wire deadline for a
command row, `0` for the client default. A command that reaches the network
(`UPDATE_CHECK`, `UPDATE_DOWNLOAD`) names its own so a legitimately slow reply
is not reported as a timeout.

### Reader rows with fetched content

A row can open the reader on content that is not its `detail` lines. `Row`
carries `reader` (`ReaderKind`): `log_tail` decodes a fetched `LOG_TAIL`, and
`raw_input` filters the service's `"setup raw:"` lines out of the same tail.
Both open an empty panel, queue `LOG_TAIL 120` through `queue_fetch`, and
rebuild when the reply's generation changes. While such a reader is up, Cross
refreshes it (it re-queues the fetch) and Circle closes it; a plain `detail`
reader keeps Cross = close. The shell exposes `ToolboxUi::show_reader(title,
lines, feedback)` for the snapshot harness to photograph these without a live
service.

## Chained modals (wave 2c)

Some rows need two answers in order: rename a profile (pick it, then type its
new name) or edit one (pick it, then capture a binding). A row names the second
modal after its own fields:

| Field | Meaning |
| --- | --- |
| `overlay_next` | The modal opened once this row's modal is accepted. Default `none` (post straight away). |
| `overlay_next_command` | A fetch queued between the two modals, e.g. `PROFILE_GET`, whose reply seeds the second modal. |
| `overlay_next_argument` | That fetch's argument, templated from the *first* modal's result (`{label}` of the chosen option). |
| `overlay_text` / `overlay_max_length` | A text modal's seed and character cap. A chained text modal is seeded from the first result instead. |
| `overlay_hex` | Hex-encode each produced argument before queueing. `PROFILE_RENAME` and `SETUP_BEGIN` decode their argument from hex, so a row whose command expects that says so. |

At run time the shell opens the first modal as usual. On accept it starts a
chain: it queues `overlay_next_command` (if any), waits on the fetch store off
the frame thread, then opens `overlay_next`, seeded from the first result (a
key binder's rows come from a decoded `PROFILE_GET` reply; see
`seed_profile_bindings`). When the second modal is accepted, the row's single
`overlay_command` fires with a *merged* result: the second modal's live value
(the new name, the captured button) plus the first modal's choice.

Two placeholders carry the pair, so one command can name both:

| Placeholder | Filled from |
| --- | --- |
| `{parent}` | the first modal's chosen label (`OverlayResult::parent_label`) |
| `{action}` | a key binder's newly bound button, as the service names it (`OverlayResult::action`) |

Example -- rename a profile (`PROFILE_RENAME <old>|<new>`, hex on the wire):

```cpp
{"Rename profile", "", "...", RowState::action, {},
 LiveSource::none, nullptr, nullptr, LiveSource::none, OverlayKind::picker,
 0.0f, 0.0f, 0.0f, 0.0f, nullptr, "PROFILE_RENAME", {"{parent}|{text}"},
 ReaderKind::none, PickerSource::fetch, "PROFILES", "profiles", 0u,
 OverlayKind::text, nullptr, nullptr, nullptr, 63u, /*overlay_hex=*/true},
```

`OverlayResult` gained `parent_label` and `action`; `format_overlay_argument`
understands `{parent}` and `{action}`. The action names come from
`action_button_name` / `button_action_from_name` (`overlay.hpp`), the mapping
between the kit's `Action` and the service's button words (`CROSS`, `L1`,
`NONE`). A key binder for a profile widens its accepted inputs with
`OverlayRequest::allowed` (0 keeps the kit default) so the shoulders, triggers,
Options and touchpad can be bound too.

### Populating a picker on section entry

A fetch-backed picker reads whatever is already in the fetch store when it
opens. So the *first* open is not empty, `ToolboxUi::open_section` queues the
`picker_command` of every fetch-backed row in the section it is entering
(`PROFILES`, `PROFILE_FILES`), once each. The outstanding reply lands on the
worker before the player reaches the row.

