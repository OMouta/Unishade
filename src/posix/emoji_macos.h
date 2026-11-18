#pragma once

struct ImFontLoader;

// Draws emoji from Apple Color Emoji for Dear ImGui. That font holds pictures instead of outlines, which Dear ImGui's
// own loader cannot read, so Core Text draws them. Given to a font merged after the others, so it only draws what
// they lack.
const ImFontLoader* EmojiLoader();
