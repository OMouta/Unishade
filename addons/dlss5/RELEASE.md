# DLSS5 optional downloads

Unishade downloads `nvngx_dlssnr.dll` when you install the DLSS5 add-on on its Add-ons page. It is not part of Unishade or its installer. It is an asset of the `dlss5-assets` release, and `addons/official.ini` lists its address and SHA-256. `renodx-dlss.addon64` is only used by Unishade 0.6.3 and earlier.

- `renodx-dlss.addon64`: RenoDX / [clshortfuse](https://github.com/clshortfuse). [RenoDX Discord](https://discord.com/invite/renodx).
- `nvngx_dlssnr.dll`: NVIDIA DLSSNR. Copyright 2026 NVIDIA Corporation.

These third-party files are not covered by Unishade's MIT license. Their inclusion does not imply endorsement by their authors.

For removal requests, contact [tiago@mouta.me](mailto:tiago@mouta.me).

Unishade checks the file's SHA-256 before it puts it in place. If the download is unavailable or fails the check, the add-on stays as it was. The installer of Unishade 0.6.3 and earlier reads `downloads.ini` from the `dlss5-assets` release instead, which stays there for it.
