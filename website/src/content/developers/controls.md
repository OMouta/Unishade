---
title: Controls
description: Controls an add-on draws under its settings, for text that changes, progress and values it keeps itself.
order: 6
---

Settings are a fixed list. For anything that changes while the add-on runs, add a `ui` function. Unishade calls it for every frame the controls show, and the add-on asks for the controls it wants right then. Unishade draws them the way it draws its own.

```c
static void Ui(void* addon, const UnishadeUi* ui)
{
    My* my = addon;
    ui->heading(ui, "Model");
    ui->text(ui, my->status);
    if (my->loading)
        ui->progress(ui, "Loading", my->loaded);
    ui->toggle(ui, "Fast mode", &my->fast);
    if (ui->button(ui, "Load again"))
        Reload(my);
}
```

The controls show under the add-on's settings, in **More > Add-ons** of the menu over the game.

## The controls

| Function | Shows |
| --- | --- |
| `heading(ui, text)` | A heading over the controls that follow |
| `text(ui, text)` | A paragraph |
| `progress(ui, label, fraction)` | A bar filled to `fraction`, from 0 to 1 |
| `button(ui, label)` | A button |
| `toggle(ui, label, &value)` | A switch for a `bool` |
| `slider(ui, label, &value, min, max)` | A bar for a `float` |
| `integer(ui, label, &value, min, max)` | A bar for an `int32_t` |
| `choice(ui, label, &value, choices)` | Choices for an `int32_t`, with `choices` as in settings |
| `color(ui, label, value, alpha)` | A color of three floats, or four with `alpha` |
| `tooltip(ui, text)` | What the control before it says when the mouse rests on it |

## How a control answers

This works as Dear ImGui does. `button` returns `true` once, in the first `ui` call after the press. The others show the value you point at and return `true` once after the user changed it, with the new value already written there.

The values belong to the add-on. Unishade does not save them. For a value that should be saved, use a [setting](/developers/settings/), or write it to a file in the add-on's folder.

## Labels

Unishade tells controls apart by their label, so each label is different within an add-on. To show the same words twice, add `##` and something of your own. What follows `##` is not shown:

```c
ui->button(ui, "Remove##first");
ui->button(ui, "Remove##second");
```

A label can change from frame to frame, as in a button that reads "Turn effects on" or "Turn effects off". A press only counts if the label is still the same in the next call.

## Keep it light

`ui` runs once per frame while the controls show. Read what you need from memory and leave slow work to `update` or a thread.
