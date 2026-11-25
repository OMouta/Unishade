---
title: Versions
description: How an add-on built today keeps working in a later Unishade.
order: 10
---

The interface has a version, `UNISHADE_API_VERSION` in the header. It is 1. An add-on states the version it was built for twice: as `Api` in `addon.ini`, and as `api` in its `UnishadeAddon`.

## What Unishade loads

Unishade loads add-ons built for its own version or an older one. An add-on built for a newer version shows on the Add-ons page as needing a newer Unishade, and its library is never loaded.

## How the interface grows

Every struct that can get more members starts with its `size`. New members only go at the end, and nothing is removed or reordered.

- Structs the add-on fills in, such as `UnishadeAddon`, carry the size its header had. A later Unishade reads that far and treats the rest as empty.
- Structs Unishade fills in, such as `UnishadeHost` and `UnishadeFrame`, carry the size Unishade's header has.
- `settingSize` does the same for the settings array, so `UnishadeSetting` can grow too.

So an add-on built for version 1 keeps working when version 2 adds a function to the host or a member to a frame.

## Using something newer

An add-on built with a newer header can still run in an older Unishade if it checks before it uses what is new:

```c
if (UNISHADE_HAS(UnishadeHost, host, takeScreenshot))
    host->takeScreenshot(host);
```

`UNISHADE_HAS` says whether the struct Unishade handed over is long enough to have the member. Set `Api` in `addon.ini` to the oldest version the add-on works with.

## Official add-ons

Depth and DLSS5 are released on their own, each with a version of its own, as a release on GitHub with the add-on's `addon.ini` in it. Unishade offers the newest release whose `Api` it has, so a release built for a newer version of the interface leaves older Unishades on the one before it. When a newer release is out, an installed one keeps working, and the Add-ons page shows **Update** to get the new one.
