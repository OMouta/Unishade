---
title: Frames
description: Working on the game's picture on the graphics card, before effects run or after.
order: 9
---

An add-on can read and write the game's picture itself. This is how Depth gives effects a depth buffer and how DLSS5 changes the picture. It is also the hard part of an add-on: frames stay on the graphics card, and each system shares them in its own way.

If all you want is to put something on top of the game, [draw](/developers/drawing/) instead.

## Asking for frames

Flags in `UnishadeAddon.flags` say what the add-on wants. Unishade then calls its `frame` function for every frame the overlay draws with effects on.

| Flag | The add-on gets |
| --- | --- |
| `UNISHADE_READS_PICTURE` | The game's picture before effects, in `color` |
| `UNISHADE_WRITES_PICTURE` | That, and an `output` to write. Effects get it instead of the picture. |
| `UNISHADE_WRITES_DEPTH` | A `depth` to write before effects, which effects read as `DEPTH` |
| `UNISHADE_READS_DEPTH` | The depth another add-on wrote for this picture, where one did |
| `UNISHADE_READS_RESULT` | The picture after effects, in `color` |
| `UNISHADE_WRITES_RESULT` | That, and an `output` to write. It shows instead. |
| `UNISHADE_TAKES_HDR` | HDR pictures as they are. See [HDR](#hdr). |

An add-on that asks for both stages is called twice per picture. `frame->stage` says which call it is.

## Order

In each stage, add-ons that write depth go first, then those that only read, then those that write the picture or the result. Each add-on that writes gets the output of the one before it as its `color`.

## UnishadeFrame

| Member | What it is |
| --- | --- |
| `width`, `height` | The size of every image |
| `generation` | Changes when Unishade makes the images again, such as when the game's window changes size. Open them again when it does. |
| `picture` | Changes when the game's picture does. Before effects, an add-on can skip its work when this and `generation` are the same as last time. Unishade keeps `output` and `depth` as they were. |
| `color` | What to read. Only read it. |
| `output` | What to write, for an add-on that writes the picture or the result |
| `depth` | Depth to write or to read, in `R32_FLOAT`: 0 nearest and 1 at the far plane |
| `settings` | The add-on's [settings](/developers/settings/) |
| `sdrWhite` | SDR white in an HDR `color`, in its units. 1 otherwise. |

## Opening the images

Images are on the graphics card `host->adapter` names. The add-on makes a device of its own on that card and opens them there.

**Windows.** The images are Direct3D 11 textures shared through NT handles. Open them in Direct3D 12 with `ID3D12Device::OpenSharedHandle`, or in Direct3D 11 with `ID3D11Device1::OpenSharedResource1`. `adapter` is the card's LUID.

**macOS.** The images are IOSurfaces. Wrap them in Metal textures with `newTextureWithDescriptor:iosurface:plane:`, or in `CVPixelBuffer`s for Core ML or Vision. `adapter` is the `registryID` of the `MTLDevice`.

**Linux.** The images are dma-bufs, each described by a `UnishadeDmaBuf`. Import them in Vulkan with `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier`, or in EGL. `adapter` is the device number of the card's DRM render node.

## When the images are yours

**Windows.** Frames come with a Direct3D 11 fence. Wait for it to reach `frame->wait` before touching the images, and set it to `frame->signal` once done. Signal it even when there is nothing to do. On the graphics card it is enough to queue the signal before `frame` returns.

**macOS and Linux.** There is no fence. The images are ready when `frame` is called, and the add-on's work on the graphics card has to be finished when `frame` returns. Wait for it, such as with `waitUntilCompleted` on the last Metal command buffer.

Unishade does not write the images again before the add-on is done with them. Work that takes longer than a frame has to copy what it needs and write its result in a later frame. Depth does this: the model runs on a thread, and frames that arrive meanwhile keep the last estimate.

## HDR

A game's picture can be HDR, in `RGBA16_FLOAT`. What an add-on gets depends on `UNISHADE_TAKES_HDR`.

- With it, `color` and `output` are HDR, and what the add-on writes stays HDR.
- Without it, an add-on that only reads, or writes depth, gets an SDR copy in `BGRA8` and works as it does with an SDR game.
- Without it, an add-on that writes the picture or the result sits out while the picture is HDR, and its tile says so.

A game can switch at any time. That makes the images again, so look at `color.format` on every frame.

## Working code

The header's comment has every rule in full. Unishade's own add-ons are complete examples: [Depth](links:addonSource) for Windows and macOS, and DLSS5 for Direct3D 12.
