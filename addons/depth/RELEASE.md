# Depth estimation optional downloads

Unishade downloads these files when you install the Depth add-on on its Add-ons page. They are not part of Unishade or its installer. The libraries are assets of the `depth-assets` release, and the model is downloaded straight from Hugging Face at a pinned commit. `addons/depth/files.ini` lists each file's address and SHA-256.

- `onnxruntime.dll`: ONNX Runtime 1.24.4 with the DirectML execution provider, from the `Microsoft.ML.OnnxRuntime.DirectML` NuGet package (`runtimes/win-x64/native/onnxruntime.dll`). MIT license, Microsoft Corporation.
- `DirectML.dll`: DirectML 1.15.4, from the `Microsoft.AI.DirectML` NuGet package (`bin/x64-win/DirectML.dll`). Microsoft Software License Terms, which allow redistribution in applications; the terms are in `LICENSE.txt` of that package.
- `depth-anything-v2-small.onnx`: Depth Anything V2 Small (ViT-S), float16 ONNX export `onnx/model_fp16.onnx` from [onnx-community/depth-anything-v2-small](https://huggingface.co/onnx-community/depth-anything-v2-small), saved under this name. Apache-2.0 license. Model by Lihe Yang and others, [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2).

The host builds against the ONNX Runtime 1.24.4 headers, so replace `onnxruntime.dll` only with the same or a newer 1.x release of the DirectML build. The model must be a Depth Anything V2 export with `batch_size`, `height` and `width` as its free input dimensions and float32 or float16 tensors.

These third-party files are not covered by Unishade's MIT license. Their inclusion does not imply endorsement by their authors.

For removal requests, contact [tiago@mouta.me](mailto:tiago@mouta.me).

Unishade checks every file's SHA-256 before it puts any of them in place. If a download is unavailable or fails the check, the add-on stays as it was. The installer of Unishade 0.6.3 and earlier reads `downloads.ini` from the `depth-assets` release instead, which stays there for it.
