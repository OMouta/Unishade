---
title: The library
description: The one function an add-on exports, the functions Unishade calls and the rules they follow.
order: 4
---

The library exports one function. It returns a `UnishadeAddon` that says what the add-on does and what to call.

```c
#include "unishade_addon.h"

typedef struct My
{
    const UnishadeHost* host;
} My;

static void* Create(const UnishadeHost* host, char* error, size_t errorSize)
{
    My* my = calloc(1, sizeof(My));
    my->host = host;
    host->log(host, UNISHADE_LOG_INFO, "Started.");
    return my;
}

static void Destroy(void* addon)
{
    free(addon);
}

static const UnishadeAddon kAddon = {
    .size = sizeof(UnishadeAddon),
    .api = UNISHADE_API_VERSION,
    .create = Create,
    .destroy = Destroy,
};

UNISHADE_ADDON_EXPORT const UnishadeAddon* UnishadeGetAddon(void)
{
    return &kAddon;
}
```

In C++, put `UnishadeGetAddon` inside `extern "C"`.

## UnishadeAddon

| Member | What it is |
| --- | --- |
| `size` | `sizeof(UnishadeAddon)` |
| `api` | `UNISHADE_API_VERSION` |
| `flags` | What the add-on does with [frames](/developers/frames/) and keys, or 0 |
| `settings`, `settingCount`, `settingSize` | Its [settings](/developers/settings/), how many, and `sizeof(UnishadeSetting)` |
| `create`, `destroy` | Start and stop. Both are needed. |
| `update` | Called once per pass of Unishade's main loop |
| `frame` | Called for every frame, for an add-on that asked for the picture |
| `event` | Told [what happened](/developers/functions/#events) |
| `ui` | Draws its [own controls](/developers/controls/) |
| `draw` | Draws [over the game](/developers/drawing/) |

Leave out what you don't need. Members you don't set are zero, which means "none".

## Starting and stopping

`create` gets the host and returns a pointer of the add-on's own. Unishade hands that pointer to every other function. To refuse to start, return `NULL` and write the reason to `error`. The reason shows on the add-on's row.

The host stays valid until `destroy`. It has:

- `folder`, the add-on's own folder, ending in a separator. The add-on may write there.
- `dataFolder`, where `Unishade.log` is.
- `log`, which writes a line to `Unishade.log` after the add-on's name.
- [The functions](/developers/functions/) that ask Unishade things.

`destroy` is called when the user turns the add-on off, when its folder is removed and when Unishade quits. Unishade may unload the library afterwards.

## update

`update` runs once per pass of Unishade's main loop, whether a game is running or not. It gets the settings' values and the time. An add-on that takes no frames does its regular work here, and this is where a [button](/developers/settings/#buttons) press arrives.

## When something fails

`update` and `frame` return `false` with a reason in `error` to stop the add-on. Unishade shows the reason on the Add-ons page and under Messages. The add-on stays stopped until the user turns it on again, which starts it over.

## Threads

Unishade calls every function on its main thread. An add-on may run threads of its own. From those threads it may call `host->log` and nothing else of the host.

Call the host's other functions only from inside one of the add-on's own functions, while Unishade is calling it.
