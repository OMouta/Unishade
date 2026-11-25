# What dmgbuild puts in Unishade's disk image for the Mac, and how Finder shows it: the app and the Applications
# folder on assets/dmg/background.png, which draws the arrow between them. scripts/dmg.sh runs dmgbuild with it,
# and names the app and the picture.

import os.path

app = defines["app"]  # noqa: F821, dmgbuild gives the settings their defines.

# zlib, which every macOS reads.
format = "UDZO"
files = [app]
symlinks = {"Applications": "/Applications"}
# Nothing here hides the app's extension, which Finder leaves out by itself: dmgbuild would mark the app's folder for
# it, and an app with such a mark no longer passes the strict check of its signature.

# The mounted disk has the app's icon on the desktop and in Finder's sidebar.
icon = os.path.join(app, "Contents", "Resources", "Unishade.icns")

# The picture is 660 by 400 points, and scripts/dmg_background.swift has the same places for the icons. The window is
# 28 points taller for its title bar.
background = defines["background"]  # noqa: F821
window_rect = ((200, 160), (660, 428))
icon_size = 128
text_size = 13
icon_locations = {"Unishade.app": (170, 180), "Applications": (490, 180)}
