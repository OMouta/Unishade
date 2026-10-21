---
layout: ../layouts/Legal.astro
title: Privacy
description: Unishade doesn't collect anything about you.
updated: October 3, 2026
---

Unishade doesn't collect anything about you. There are no accounts, analytics or telemetry.

Everything Unishade captures or saves, like screenshots, presets and logs, stays on your computer.

## What Unishade downloads

Unishade and Setup only go online to check for a new version and to download what they need. Like any website you visit, those sites see your IP address.

- Each time it starts, Unishade for Windows asks GitHub (api.github.com) for the newest version. The request sends your IP address and a user agent with Unishade's version, like `Unishade/0.6.0`. **Check for updates** in the menu's **Settings** turns this off.
- While **Show on Discord** is on, Unishade for Windows downloads Discord's list of games from discord.com to find your game's icon, at most once a week.
- Setup downloads ReShade from reshade.me, and ReShade's license, the effects and the presets from GitHub. If you pick an add-on, it comes from GitHub, and depth estimation's model from Hugging Face.
- On macOS and Linux, Unishade downloads the effects and presets from GitHub when you ask it to, with **Download effects and presets** or `unishade --install-effects`.

What those sites do with a request is up to their own privacy policies.

## Discord

While Unishade for Windows runs on a game and Discord is open, it tells the Discord app on your computer the game's name and your preset's name. Discord shows them on your profile, with the game's icon, to the people who can see your activity. What Discord does with them is up to [Discord's privacy policy](https://discord.com/privacy).

This is on until you turn off **Show on Discord** under **Discord** in the Unishade window. Discord clears it when you do, or when Unishade closes.

## This website

unishade.me is hosted on Cloudflare, so Cloudflare sees your IP address when you visit and handles it as [Cloudflare's privacy policy](https://www.cloudflare.com/privacypolicy/) says. The site itself has no analytics or trackers, and sets no cookies of its own. The download buttons link to files on GitHub.

Questions go to [tiago@mouta.me](mailto:tiago@mouta.me).
