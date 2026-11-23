---
title: Functions and events
description: What an add-on can ask of Unishade through its host, and what Unishade tells it.
order: 8
---

`create` gets a `UnishadeHost`. Its functions are how an add-on reads what Unishade is doing and acts on it. Every function takes the host itself as its first argument.

Call them on the main thread, from inside one of the add-on's own functions. Only `log` is safe from other threads.

## State

`getState` returns what Unishade is doing right now:

```c
const UnishadeState* state = host->getState(host);
if (state->playing)
    printf("%s at %ux%u\n", state->gameName, state->width, state->height);
```

| Member | What it is |
| --- | --- |
| `playing` | Unishade draws over a game |
| `gameName`, `executable` | The game's name in Unishade, or its window's title, and its program |
| `width`, `height` | The size of the game's picture, or 0 |
| `hdr` | The picture is HDR |
| `frameRate` | The frames a second Unishade showed lately |
| `effectsOn` | The user has effects on |
| `menuOpen` | Unishade's menu shows over the game |
| `presetName`, `presetPath` | The preset in use |
| `time` | Seconds since Unishade started |

Strings are empty, never `NULL`, when there is nothing to say.

## Actions

| Function | Does |
| --- | --- |
| `showMessage(host, text)` | Shows a short message at the bottom of the game for a few seconds |
| `setEffectsOn(host, on)` | Turns every effect on or off, as the power button does |
| `takeScreenshot(host)` | Saves a screenshot of the next frame, as the user's screenshot settings say |
| `setSetting(host, setting, values)` | Sets one of the add-on's own [settings](/developers/settings/) |
| `log(host, level, text)` | Writes a line to `Unishade.log`. Warnings and errors also show under Messages. |

## Presets

```c
uint32_t count = host->presetCount(host);
for (uint32_t i = 0; i < count; ++i)
{
    const UnishadePreset* preset = host->getPreset(host, i);
    if (!preset->current && !strcmp(preset->name, "Night"))
        host->usePreset(host, i);
}
```

Presets are listed as the menu lists them, the game's own first. `usePreset` returns `false` when the preset in use has changes the user has to save or discard first.

## Effects

An add-on can turn techniques on and off and change effects' values, as the user does in the menu. The preset then has changes to save.

```c
uint32_t effect = host->findEffect(host, "Bloom.fx");
uint32_t uniform = host->findUniform(host, effect, "fBloomIntensity");
if (uniform != UNISHADE_NONE)
{
    float value = 1.5f;
    host->setValue(host, effect, uniform, &value, 1);
}
```

| Function | Does |
| --- | --- |
| `techniqueCount`, `getTechnique` | Lists the techniques in the order they run, on or off |
| `findTechnique(host, name, file)` | Finds a technique by name, in an effect file when `file` is not `NULL` |
| `setTechnique(host, technique, enabled)` | Turns a technique on or off |
| `findEffect(host, file)` | Finds the effect an effect file made |
| `uniformCount`, `getUniform`, `findUniform` | Lists and finds the values of an effect that presets keep |
| `getValue`, `setValue` | Reads and sets a value as floats |
| `getIntValue`, `setIntValue` | The same as ints, for whole numbers too large for a float |

The find functions return `UNISHADE_NONE` when there is no match.

Effects, techniques and uniforms are numbered from 0. The numbers change when a preset loads, effects compile or the user moves a technique. `effectsGeneration` changes whenever they do, and so does the `UNISHADE_EVENT_EFFECTS_CHANGED` event. Look things up again after either.

## Keys

An add-on with `UNISHADE_READS_KEYS` in its flags can ask whether a key or mouse button is held:

```c
if (host->keyDown(host, 0x20)) // Space
```

Keys are Windows virtual-key codes on every system. `0x01` is the left mouse button. `keyDown` is false while neither the game nor the menu has the keyboard. Reading keys costs time on every frame, so only add-ons that ask get them.

## What a function returns

`getState`, `getPreset`, `getTechnique` and `getUniform` return a struct that belongs to Unishade. It stays as it is, with its strings, until the add-on calls the same function again or returns to Unishade. Copy what you want to keep.

## Events

An add-on with an `event` function is told what happens, between its other calls:

```c
static void Event(void* addon, const UnishadeEvent* event)
{
    if (event->type == UNISHADE_EVENT_SCREENSHOT_SAVED)
        Upload(event->text);
}
```

| Event | When | `value` and `text` |
| --- | --- | --- |
| `UNISHADE_EVENT_GAME_STARTED` | Unishade began drawing over a game | `text`: the game's name |
| `UNISHADE_EVENT_GAME_STOPPED` | It stopped | |
| `UNISHADE_EVENT_PRESET_LOADED` | A preset loaded, the same one again included | `text`: its path |
| `UNISHADE_EVENT_EFFECTS_CHANGED` | Effects loaded, or techniques moved | |
| `UNISHADE_EVENT_EFFECTS_TOGGLED` | Every effect was turned on or off | `value`: 1 for on |
| `UNISHADE_EVENT_MENU_TOGGLED` | The menu over the game opened or closed | `value`: 1 for open |
| `UNISHADE_EVENT_SETTING_CHANGED` | One of the add-on's settings changed | `value`: its place in the list |
| `UNISHADE_EVENT_SCREENSHOT_SAVED` | A screenshot was written | `text`: its file |

An add-on that starts while a game is running is told of the game and the preset right away.
