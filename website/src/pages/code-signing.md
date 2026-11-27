---
layout: ../layouts/Legal.astro
title: Code signing policy
description: How Unishade for Windows is signed, and who approves it.
updated: November 25, 2026
---

Free code signing provided by [SignPath.io](links:signpath), certificate by [SignPath Foundation](links:signpathFoundation).

## What's signed

`Unishade.exe`, the depth and DLSS5 add-ons `Depth.dll` and `Unishade_nvngx.dll`, and Unishade Setup, as published on [GitHub Releases](links:releases). GitHub Actions builds them from [Unishade's repository](links:github), and each release is signed only after an approver approves it.

The effects, and the files the add-ons download from their own authors, aren't signed by Unishade. The Linux build isn't signed. Betas for Windows, which GitHub Releases marks as pre-releases, aren't signed either.

## macOS

The Mac app and its disk image aren't signed through SignPath. GitHub Actions signs them with the Apple Developer ID certificate of Unishade's maintainer, and Apple notarizes them. That goes for betas too. The depth add-on's `Depth.dylib` is signed with the same certificate.

## Team

- Committers and reviewers: [Tiago Mouta](https://github.com/OMouta)
- Approvers: [Tiago Mouta](https://github.com/OMouta)

## Privacy

The [privacy policy](/privacy/) lists everything Unishade and Setup send, and where.
