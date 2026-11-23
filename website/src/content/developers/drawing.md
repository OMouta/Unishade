---
title: Drawing
description: Lines, rectangles, circles and text over the game, with no graphics code.
order: 7
---

An add-on with a `draw` function draws on top of the game. Unishade calls it for every frame it shows, and the add-on calls the functions of `UnishadeDraw` for what it wants in that frame.

```c
static void Draw(void* addon, const UnishadeDraw* draw)
{
    const float x = draw->width / 2, y = draw->height / 2, arm = 10 * draw->scale;
    const uint32_t white = UNISHADE_COLOR(255, 255, 255, 255);
    draw->line(draw, x - arm, y, x + arm, y, white, 2 * draw->scale);
    draw->line(draw, x, y - arm, x, y + arm, white, 2 * draw->scale);
}
```

That is a crosshair.

## Coordinates and sizes

Everything is in pixels of the game's picture. `0, 0` is its top left corner, and `width` and `height` are its size.

A game can be any size, on any display. Multiply sizes by `scale` and they come out as large as Unishade's own menu over that game. `16 * draw->scale` is a good height for text.

## Functions

| Function | Draws |
| --- | --- |
| `line(draw, x1, y1, x2, y2, color, thickness)` | A line |
| `rect(draw, x, y, width, height, color, rounding, thickness)` | A rectangle from its top left corner, with rounded corners. Filled with a thickness of 0, otherwise its outline. |
| `circle(draw, x, y, radius, color, thickness)` | A circle. Filled with a thickness of 0, otherwise its outline. |
| `text(draw, x, y, height, color, text)` | A line of text in Unishade's typeface, from its top left corner |
| `textWidth(draw, height, text)` | Nothing. It returns how wide the text is, to place it or put a box behind it. |

Colors are `UNISHADE_COLOR(red, green, blue, alpha)`, each from 0 to 255. Later calls draw over earlier ones.

## What else draw gets

- `settings`, the add-on's [settings](/developers/settings/), so a setting can turn the drawing on or change its color.
- `cursorX` and `cursorY`, where the mouse is in the picture.
- `time`, in seconds since Unishade started, for anything that moves.

## What to expect

- The drawing is under Unishade's menu and its messages.
- It hides while effects are off, and while the user holds compare.
- It is not in screenshots, which hold the game's picture alone.
- There are no pictures yet, only shapes and text.

To change the picture itself, such as its colors, an add-on has to [work on frames](/developers/frames/).
