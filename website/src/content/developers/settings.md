---
title: Settings
description: Settings an add-on lists, which Unishade shows in its menu and saves.
order: 5
---

List your settings and Unishade does the rest. It shows them in the menu over the game, under **More > Add-ons**, saves them, and hands their values to the add-on.

```c
enum { kStrength, kMode, kTint, kReset, kSettingCount };

static const UnishadeSetting kSettings[kSettingCount] = {
    { .key = "Strength", .label = "Strength", .type = UNISHADE_SETTING_SLIDER, .defaultValue = { 0.5f }, .max = 1 },
    { .key = "Mode", .label = "Mode", .type = UNISHADE_SETTING_CHOICE, .choices = "Soft\0Hard\0" },
    { .key = "Tint", .label = "Tint", .type = UNISHADE_SETTING_COLOR_RGB, .defaultValue = { 1, 1, 1 }, .group = "Color" },
    { .key = "Reset", .label = "Start over", .type = UNISHADE_SETTING_BUTTON },
};
```

Then in `UnishadeAddon`:

```c
    .settingCount = kSettingCount,
    .settings = kSettings,
    .settingSize = sizeof(UnishadeSetting),
```

## Types

| Type | Shows as | Value |
| --- | --- | --- |
| `UNISHADE_SETTING_TOGGLE` | A switch | 0 or 1 |
| `UNISHADE_SETTING_SLIDER` | A bar | A number from `min` to `max` |
| `UNISHADE_SETTING_INTEGER` | A bar | A whole number from `min` to `max` |
| `UNISHADE_SETTING_CHOICE` | Choices side by side, or a list | Which of `choices`, from 0 |
| `UNISHADE_SETTING_COLOR_RGB` | A color | Red, green and blue from 0 to 1 |
| `UNISHADE_SETTING_COLOR_RGBA` | A color with alpha | Four values from 0 to 1 |
| `UNISHADE_SETTING_BUTTON` | A button with the label on it | 1 once after a press |

`choices` is the labels one after another, each ending in `\0`. The end of the string closes the list.

## The other members

- `key` names the setting where it is saved. Keep it the same between versions.
- `defaultValue` is the value until the user changes it. A color uses all three or four.
- `tooltip` shows when the mouse rests on the setting.
- `group` puts the setting under a heading with the others that name the same group.
- `units` shows after a number, such as `ms`.

## Reading the values

`update`, `frame` and `draw` each get `settings`, four floats for every setting in the order you listed them:

```c
float strength = update->settings[kStrength][0];
const float* tint = update->settings[kTint]; // tint[0], tint[1], tint[2]
```

Unishade keeps values inside their range, so `settings[kMode][0]` is always a valid choice.

## Buttons

A button's value is 1 in the first `update` or `frame` call after the press, whichever comes first, and 0 otherwise. Nothing saves it.

## Where they are saved

It depends on what the add-on does.

An add-on that writes the picture or the result is part of a preset's look. It gets a tile next to the effects, each preset keeps whether it is on and its settings, and it starts off in every preset. A preset someone shares looks the same for anyone with the add-on.

Any other add-on's settings are about the computer, and go in `Unishade.ini`.

## Changing them from the add-on

`host->setSetting(host, kStrength, values)` sets a setting as the user would, with four floats. It is saved the same way.

When the user or a preset changes a setting, the add-on's `event` function gets `UNISHADE_EVENT_SETTING_CHANGED` with the setting's place in `value`.
