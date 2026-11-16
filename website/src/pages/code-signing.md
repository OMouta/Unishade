---
layout: ../layouts/Legal.astro
title: Code signing policy
description: How Unishade for Windows is signed, and who approves it.
updated: October 9, 2026
---

Free code signing provided by [SignPath.io](links:signpath), certificate by [SignPath Foundation](links:signpathFoundation).

## What's signed

`Unishade.exe`, the depth and DLSS5 add-ons `Depth.dll` and `Unishade_nvngx.dll`, and Unishade Setup, as published on [GitHub Releases](links:releases). GitHub Actions builds them from [Unishade's repository](links:github), and each release is signed only after an approver approves it.

The effects, and the files the add-ons download from their own authors, aren't signed by Unishade. The macOS and Linux builds aren't signed.

## Team

- Committers and reviewers: [Tiago Mouta](https://github.com/OMouta)
- Approvers: [Tiago Mouta](https://github.com/OMouta)

## Privacy

The [privacy policy](/privacy/) lists everything Unishade and Setup send, and where.
