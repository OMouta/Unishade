# Inference runtimes and optimisation for a small ViT depth network on Windows GPUs shared with a game, state in November 2026

Reading guide. "Read in source" means the number comes from the linked page. "Measured by me" means I read it from a package registry on 2026-11-24 with an HTTP range request, no download of the user's data involved. "My estimate" means arithmetic or judgement on top of sourced numbers. I could not reach github.com/microsoft/onnxruntime pages or its API in this session, they returned 404, so ONNX Runtime release facts come from mirrors, the docs site and registries.

## 1. Status of DirectML and its successors in 2026, and which runtimes can share a D3D12 device

### Takeaway
DirectML is frozen at 1.15.4 from October 2024 and Microsoft calls it "sustained engineering" and "legacy". The last DirectML build of ONNX Runtime on NuGet and PyPI is 1.24.4 while mainline ONNX Runtime is at 1.32.0. The one successor that offers documented D3D12 zero-copy is NVIDIA's TensorRT RTX EP, through ONNX Runtime's new interop API with D3D12 resource import and fences since 1.24 and command queue hand-off since 1.25. AMD, Intel and WebGPU providers have no documented D3D12 resource sharing, so DirectML stays the zero-copy path for them.

### Cited findings

DirectML
- Microsoft Learn states "DirectML is in sustained engineering. DirectML continues to be supported, but new feature development has moved to Windows ML". Page updated 2026-01-09. [Microsoft Learn, Introduction to DirectML](https://learn.microsoft.com/en-us/windows/ai/directml/dml)
- The Windows ML provider page, dated 2026-11-20, lists the included providers as CPU, "DirectML (legacy)" and "Windows ML CG EP (experimental)". [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)
- Measured by me. The newest Microsoft.AI.DirectML on NuGet is 1.15.4, published 2024-10-28. Nothing newer exists two years later. [NuGet, Microsoft.AI.DirectML](https://www.nuget.org/packages/Microsoft.AI.DirectML/)
- Measured by me. The newest Microsoft.ML.OnnxRuntime.DirectML is 1.24.4, published 2026-03-17. Microsoft.ML.OnnxRuntime is at 1.32.0, published 2026-11-13. The PyPI wheel onnxruntime-directml also stops at 1.24.4. [NuGet, Microsoft.ML.OnnxRuntime.DirectML](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML); [PyPI, onnxruntime-directml](https://pypi.org/project/onnxruntime-directml/)
- The ONNX Runtime 1.25.0 notes still carry two DirectML EP bug fixes, #27815 and #27823, so the EP source received fixes after the last DirectML package. [newreleases.io mirror of ORT 1.25.0 notes](https://newreleases.io/project/pypi/onnxruntime/release/1.25.0)
- The DirectML docs still describe what Unishade does today: tensors are ordinary D3D12 resources, work is recorded into D3D12 command lists on the app's queue, and layer-by-layer execution lets an app "interleave Direct3D 12 rendering workloads with your DirectML compute dispatches". [Microsoft Learn, Introduction to DirectML](https://learn.microsoft.com/en-us/windows/ai/directml/dml)
- TensorRT-RTX 1.7 lists a known issue: a DirectML-related inference failure on Windows with NVIDIA driver R615 or earlier. [NVIDIA, TensorRT-RTX 1.7 release notes](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/getting-started/release-notes-1/1.7.html)

Windows ML, the 2025 one
- Windows ML reached general availability on 2025-09-23. [Windows Developer Blog](https://blogs.windows.com/windowsdeveloper/2025/09/23/windows-ml-is-generally-available)
- Vendor providers are not shipped with the app. Windows downloads them on demand through the `ExecutionProviderCatalog` APIs, only on Windows 11 24H2, build 26100, or later. [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)
- Windows ML 2.x provider list: AMD GPU (`AMDGPUExecutionProvider`), NvTensorRtRtx, OpenVINO, QNN, VitisAI, WebGPU (experimental), MIGraphX (deprecated). [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)
- Requirements read in source. NvTensorRtRtx needs GeForce RTX 30xx or newer, driver 32.0.15.5585 or newer. AMD GPU EP needs Adrenalin 24.12.1 or later and Windows ML 2.5.96 or later. MIGraphX needed RDNA 3 or later and "will be retired from the Windows ML API interface with a new Windows ML major release". QNN and VitisAI target NPUs. WebGPU EP runs on "any DirectX 12-capable GPU". [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)
- Current provider builds, read in source. NvTensorRtRtx 2.30.51.0 on 2026-10-28, AMD GPU and MIGraphX 1.8.65.0 on 2026-10-28, WebGPU 0.6.0.0 on 2026-11-16. New builds of all vendor providers reach general availability on 2026-12-01, the Intel one on OpenVINO 2026.4.1. [WindowsML wiki, EP releases](https://github.com/microsoft/WindowsML/wiki/Windows-ML-Execution-Provider-Releases)
- Measured by me. The wheel onnxruntime-windowsml 1.30.1 for win_amd64 is 26.7 MB, uploaded 2026-11-13. [PyPI, onnxruntime-windowsml](https://pypi.org/project/onnxruntime-windowsml/)

Windows ML CGC backend, new in Windows ML 2.9
- CGC "compiles a model into programs for a Direct3D 12 device and runs them on the GPU target your app picked". It uses compiler backends from AMD, Intel, NVIDIA and Qualcomm with "shared DirectX fallbacks". Diagnostics report subgraphs by technology, `D3D12_DRIVER` or `DIRECTML`. [Microsoft Learn, The CGC backend](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/runtime/model-compilation-execution)
- Status read in source: "experimental, not supported for use in production environments". Models outside the validated list "are likely to fail with ERROR_NOT_SUPPORTED, run slowly, or give different results". The validated non-LLM list is YOLO11, YOLOv8, CLIP text, CLIP vision, ResNet-50 and Whisper parts. No DINOv2 or DPT model is on it. [Microsoft Learn, The CGC backend](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/runtime/model-compilation-execution)
- Microsoft recommends testing on NVIDIA GPUs with driver 621.24 during the experimental release. The native payload is "about 110 MB per architecture". It deploys D3D12Core.dll from Agility SDK 1.620.1 and DirectML.dll beside the app. The first session compiles programs, "which can take several seconds", and a program cache keeps them. [Microsoft Learn, The CGC backend](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/runtime/model-compilation-execution)
- An ONNX Runtime app can reach CGC through `WinMLCGExecutionProvider` in `WinMLCGEp.dll`, registered with `RegisterExecutionProviderLibrary`. It needs a physical D3D12 GPU and a file-backed ONNX model. [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)
- AMD describes the same pipeline as "Windows ML to DxCGC" in its Build 2026 post. [AMD blog, Build 2026](https://amd.com/en/blogs/2026/advancing-windows-ml-acceleration-with-amd-at-microsoft-build-2026.html)

NVIDIA TensorRT for RTX and the NvTensorRtRtx EP
- Release cadence read in source: 1.2 on 2025-11-12, 1.3 on 2025-12-19, 1.4 on 2026-03-18, 1.5 on 2026-05-28 with an ABI break, 1.6 on 2026-07-28. [GitHub, NVIDIA/TensorRT-RTX releases](https://github.com/NVIDIA/TensorRT-RTX/releases). PyPI has tensorrt-rtx 1.7.1.116 uploaded 2026-10-14. [PyPI, tensorrt-rtx](https://pypi.org/project/tensorrt-rtx/)
- GPU coverage. The EP page says "RTX GPUs based on Ampere and later architectures, NVIDIA GeForce RTX 30xx and above". [ONNX Runtime, TensorRT RTX EP](https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html). The 1.7 notes say Turing works but default engines target Ampere and later, so Turing needs its own engine build. [NVIDIA, TensorRT-RTX 1.7 release notes](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/getting-started/release-notes-1/1.7.html)
- Size claim read in source: "just under 200 MB". [ONNX Runtime, TensorRT RTX EP](https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html)
- Size measured by me. The wheel onnxruntime-ep-nv-tensorrt-rtx-cu13 0.5.0 for win_amd64, uploaded 2026-10-22, is 110.9 MB compressed and 286.0 MB unpacked. Inside: tensorrt_rtx_1_7.dll 229.3 MB, tensorrt_plugins.dll 51.3 MB, tensorrt_onnxparser_rtx_1_7.dll 2.3 MB, onnxruntime_providers_nv_tensorrt_rtx.dll 2.4 MB. [PyPI, onnxruntime-ep-nv-tensorrt-rtx-cu13](https://pypi.org/project/onnxruntime-ep-nv-tensorrt-rtx-cu13/)
- DLLs an app must ship, read in source: the EP DLL, `onnxruntime.dll`, `onnxruntime_providers_shared.dll`, `tensorrt_rtx_<major>_<minor>.dll`, `tensorrt_onnxparser_rtx_<major>_<minor>.dll` and `cudart64_<version>.dll`. No cuDNN or cuBLAS. [NVIDIA, TensorRT-RTX EP ABI integration guide](https://nvidia.github.io/TensorRT-RTX-EP-ABI/integration-guide.html)
- The EP is a plugin library on ONNX Runtime's device-based API "introduced in ORT 1.23.0". An app calls `RegisterExecutionProviderLibrary`, `GetEpDevices`, `AppendExecutionProvider_V2`. [ONNX Runtime, TensorRT RTX EP](https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html); [NVIDIA, integration guide](https://nvidia.github.io/TensorRT-RTX-EP-ABI/integration-guide.html)
- Build model. An ahead-of-time step produces an EP context model, a just-in-time step compiles kernels for the user's GPU, and `nv_runtime_cache_path` stores them. "The cache only contains compiled kernels. No information about the model's graph structure or weights is stored." [ONNX Runtime, TensorRT RTX EP](https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html)
- The cache is only valid for the same GPU SKU, TensorRT-RTX version and CiG state. Engines are not forward compatible and must be rebuilt after a runtime upgrade. [NVIDIA, TensorRT-RTX 1.4 release notes](https://docs.nvidia.com/deeplearning/tensorrt-rtx/1.4/getting-started/release-notes-1/1.4.html); [NVIDIA, Simultaneous compute and graphics](https://docs.nvidia.com/deeplearning/tensorrt-rtx/1.4/inference-library/compute-graphics.html)
- Provider options that matter here: `enable_cuda_graph` "to reduce kernel-launch overhead on repeated inference runs with fixed input shapes", `nv_runtime_cache_path`, `user_compute_stream`, `user_aux_stream_array` "required for correct CIG graphics interop", `nv_max_shared_mem_size`, `nv_target_sm`, and `VkExternalComputeQueueDataParamsNV_data` "used for CIG context creation" on Vulkan. [NVIDIA, EP provider options](https://nvidia.github.io/TensorRT-RTX-EP-ABI/_generated/provider-options.html)
- Vendor speed claim: "50% faster throughput compared to prior DirectML implementations", chart measured on an RTX 5090, per-model values only in the image. [NVIDIA developer blog, Windows ML on RTX PCs](https://developer.nvidia.com/blog/deploy-ai-models-faster-with-windows-ml-on-rtx-pcs). A second NVIDIA post says "up to 2x". [NVIDIA blog, RTX AI Garage](https://blogs.nvidia.com/blog/rtx-ai-garage-computex-microsoft-build/)

D3D12 interop in ONNX Runtime
- `OrtInteropApi` exists since ONNX Runtime 1.24 with `CreateExternalResourceImporterForDevice`, `CanImportMemory`, `ImportMemory`, `CreateTensorFromMemory` which creates a tensor backed by imported memory without copying, `ImportSemaphore`, `WaitSemaphore` and `SignalSemaphore`. The example uses `ORT_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE`, and semaphores are described as a "D3D12 timeline fence". The docs say "External resource import is optional for EPs to implement". [ONNX Runtime C API, OrtInteropApi](https://onnxruntime.ai/docs/api/c/struct_ort_interop_api.html)
- Since 1.25 there is `InitGraphicsInteropForEpDevice` with `OrtGraphicsInteropConfig`. For D3D12 the app passes its `ID3D12CommandQueue*`. The field is "Optional. When provided, the factory may use it for efficient GPU-side synchronization with inference streams." Without it "streams use the default context". [ONNX Runtime C API, OrtGraphicsInteropConfig](https://onnxruntime.ai/docs/api/c/struct_ort_graphics_interop_config.html)
- The ONNX Runtime 1.25.0 notes list "D3D12 external resource import support (#26948)" under the NVIDIA TensorRT RTX EP. It is the only EP named for this feature in any source I found. [newreleases.io mirror of ORT 1.25.0 notes](https://newreleases.io/project/pypi/onnxruntime/release/1.25.0)
- The design issue describes D3D12 resource and heap shared handles plus D3D12 fence shared handles with 64-bit wait and signal values, Windows only. I saw this through search snippets, the page itself returned 404. [microsoft/onnxruntime issue #26821](https://github.com/microsoft/onnxruntime/issues/26821)
- NVIDIA's EP sample list has device tensors, async copies and CUDA sync streams, but no D3D12 or Vulkan sample. [NVIDIA, EP C++ samples](https://nvidia.github.io/TensorRT-RTX-EP-ABI/cpp-samples.html)

CUDA EP and classic TensorRT EP
- Measured by me. The onnxruntime-gpu 1.32.0 wheel for win_amd64 is 157.8 MB, with onnxruntime_providers_cuda.dll at 186.6 MB unpacked. The cuDNN wheel for Windows is 696.3 MB. [PyPI, onnxruntime-gpu](https://pypi.org/project/onnxruntime-gpu/); [PyPI, nvidia-cudnn-cu12](https://pypi.org/project/nvidia-cudnn-cu12/)
- ONNX Runtime 1.25.0 introduced a CUDA plugin EP and CUDA graph work. [newreleases.io mirror of ORT 1.25.0 notes](https://newreleases.io/project/pypi/onnxruntime/release/1.25.0)
- Classic TensorRT is alive, tensorrt-cu13-libs 11.5.0.63 was uploaded 2026-11-17 under a proprietary licence. [PyPI, tensorrt-cu13-libs](https://pypi.org/project/tensorrt-cu13-libs/). NVIDIA describes TensorRT for RTX as an "8x smaller package size" than the classic one. [NVIDIA blog, TensorRT for RTX and SD 3.5](https://blogs.nvidia.com/blog/rtx-ai-garage-gtc-paris-tensorrt-rtx-nim-microservices)

WebGPU EP in native ONNX Runtime
- On native platforms the EP uses Dawn, which runs on D3D12, Vulkan or Metal. It ships as a plugin library. [ONNX Runtime, WebGPU EP](https://onnxruntime.ai/docs/execution-providers/WebGPU-ExecutionProvider.html)
- Measured by me. onnxruntime-ep-webgpu 0.6.0 for win_amd64, uploaded 2026-11-16, is 18.1 MB compressed and 43.9 MB unpacked: onnxruntime_providers_webgpu.dll 14.0 MB, dxcompiler.dll 18.0 MB, D3D12Core.dll 5.8 MB, d3d12SDKLayers.dll 5.7 MB. [PyPI, onnxruntime-ep-webgpu](https://pypi.org/project/onnxruntime-ep-webgpu/)
- Windows ML still labels it experimental. [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)
- ONNX Runtime 1.25.0 added WebGPU work relevant to ViTs: Gemm and MatMul with subgroups, Flash Attention changes, an optimised 4D Transpose, and EP API compatibility. [newreleases.io mirror of ORT 1.25.0 notes](https://newreleases.io/project/pypi/onnxruntime/release/1.25.0)
- Dawn itself has a shared buffer memory feature that exports shared fences on `EndAccess`. [Dawn docs, shared buffer memory](https://dawn.googlesource.com/dawn.git/+/13dde6201e5abb2f59765a1c6703933d7e1eb909/docs/dawn/features/shared_buffer_memory.md)

Intel and AMD providers
- Measured by me. The openvino 2026.4.1 wheel for win_amd64 is 85.0 MB compressed and 255.7 MB unpacked. openvino.dll is 16.5 MB, the GPU plugin 34.7 MB, the CPU plugin 46.3 MB, the NPU compiler 100.3 MB. The last onnxruntime-openvino wheel is 1.24.1 from 2026-02-26, 13.7 MB. [PyPI, openvino](https://pypi.org/project/openvino/); [PyPI, onnxruntime-openvino](https://pypi.org/project/onnxruntime-openvino/)
- AMD's GPU EP "automatically selects the most appropriate AMD acceleration backend" and is licensed under AMD's Ryzen AI terms. AMD says it "introduced a new plugin interface" for it. [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers); [AMD blog, Build 2026](https://amd.com/en/blogs/2026/advancing-windows-ml-acceleration-with-amd-at-microsoft-build-2026.html)
- AMD's earlier GPU EP preview was "built on ONNX Runtime and powered by ROCm technologies". [AMD blog, GPU EP with Amuse](https://www.amd.com/en/blogs/2025/introducing-amd-gpu-ep-for-generative-ai-with-amuse.html)

### Inferences
- DirectML will not get faster. No DirectML.dll after 1.15.4 means no new fused attention kernels or driver metacommand work arrives through that DLL, only driver-side changes.
- For Unishade's architecture, the NvTensorRtRtx plugin EP is the only non-DirectML provider with a documented way to bind app-owned D3D12 resources and fences. The minimum ONNX Runtime is 1.24 for resource import and 1.25 for the command queue hand-off.
- The last DirectML build of ONNX Runtime is 1.24.4, so combining DirectML and the 1.25 graphics interop call in one onnxruntime.dll needs either a self-built ONNX Runtime with DirectML enabled or the Windows ML redistribution, which still includes DirectML. I did not verify that a self-build of 1.32 with `--use_dml` works.
- CGC is the Microsoft-blessed long-term replacement for the "any D3D12 GPU" role and it runs on a D3D12 device the app chooses. It is not usable for Depth Anything today: experimental, no DPT-class model validated, and no documented API for binding app-owned D3D12 resources.
- OpenVINO's GPU plugin, the AMD GPU EP and QNN have no documented D3D12 resource import. Assume a CPU round trip or a shared-handle copy with them until proven otherwise.

### Gaps
- I found no statement of an end-of-support date for DirectML.
- I could not confirm from ONNX Runtime release notes why DirectML packages stop at 1.24.4, the repository pages returned 404 in this session.
- No source says whether the WebGPU EP exposes Dawn's shared buffer memory to import an app's D3D12 buffer, or whether it accepts an app-provided D3D12 device.
- No public sample shows NvTensorRtRtx with D3D12 resource import end to end. The API reference and one release note line are the only evidence.
- The AMD GPU EP repository `onnxruntime/onnxruntime-ep-amdgpu` is named in Microsoft's docs, but the PyPI package of that name is an empty 0.0.0 placeholder and I found no size, backend or interop documentation.
- An NVIDIA search snippet gave "AOT step typically takes 20-30 seconds" and "runtime cache cut compile time from 31.92 s to 1.95 s" for some model. I did not open the page that contains it, so treat both as unverified. The likely home is the [TensorRT-RTX docs](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/index.html).

## 2. Measured speed for Depth Anything and other ViTs per runtime

### Takeaway
Public numbers for Depth Anything V2 Small at 518 px cluster around 3 ms on an RTX 4090 with TensorRT FP16 and around 7 ms on an RTX 3070 with tuned PyTorch FP16. I found no published like-for-like DirectML number for V2 Small. The only head-to-head with DirectML is Desktop2Stereo's end-to-end table, where the capture and warp pipeline hides most of the difference.

### Cited findings

Depth Anything on NVIDIA
- TensorRT FP16, RTX 4090, input 518x518 from a 1280x720 image: Small 3 ms, Base 6 ms, Large 12 ms. The time includes pre and post processing, 10 warm-up frames, last inference reported. The table does not say V1 or V2, the repository covers both. [spacewalk01/depth-anything-tensorrt](https://www.github.com/spacewalk01/depth-anything-tensorrt)
- PyTorch on RTX 3070 8 GB, Depth Anything V2 Small, 518x518, batch 1, 1,369 patches, model only, 50 warm-up and 200 timed runs. Read in source, median ms: FP32 eager 19.13, FP16 10.92, BF16 11.05, FP16 with `torch.compile` 14.63, FP16 with channels_last 10.40, FP16 with CUDA graph 7.07, FP16 with channels_last and CUDA graph 6.86, which is 146 FPS. [karadjordjepetrovic/realtime-depth-optimization](https://github.com/karadjordjepetrovic/realtime-depth-optimization)
- Same source, kernel shares in the FP32 baseline of 18.1 ms GPU time: attention about 7.1 ms or 40 percent, linear layers 6.1 ms or 34 percent, depth head convolutions 2.8 ms or 15 percent, element-wise and the rest 2.0 ms. In FP16, attention drops to 1.4 ms by using a FlashAttention kernel, linear layers to 2.6 ms, convolutions to about 1.6 ms. Before CUDA graphs the GPU sat idle about 3.7 ms of each 10.9 ms frame. [karadjordjepetrovic/realtime-depth-optimization](https://github.com/karadjordjepetrovic/realtime-depth-optimization)
- ONNX Runtime, RTX 4080, ViT-S at 518x518: 13.3 ms average latency, quoted from the Depth-Anything-ONNX readme. A user on data-center GPUs reported about 98 ms for the same model. The snippet does not name the provider or precision. [fabio-sim/Depth-Anything-ONNX issue #26](https://github.com/fabio-sim/Depth-Anything-ONNX/issues/26)
- TensorRT FP16 inside ComfyUI, RTX 4090, 518x518: Small 35 FPS, Base 33 FPS, Large 24 FPS. Small on an H100: 213 FPS. These are node-level numbers with ComfyUI overhead. [yuvraj108c/ComfyUI-Depth-Anything-Tensorrt](https://www.github.com/yuvraj108c/ComfyUI-Depth-Anything-Tensorrt)
- A C++ TensorRT project for V2 lists only a target for ViT-L on an RTX 5070 of about 22 ms in FP16. It claims 392x392 is about 45 percent faster than 518x518 and CUDA graphs cut launch overhead by 10 to 15 percent. None of it is presented as measured. [alicaank/DepthAnythingV2-TensorRT](https://github.com/alicaank/DepthAnythingV2-TensorRT)
- iw3 uses `Any_V2_S` by default. Its author says "120 FPS is the performance of the depth estimation model itself", while desktop streaming went from 14 to 24 FPS after fixing JPEG encoding. The depth model was not the bottleneck. GPU not stated in what I read. [nagadomi/nunif issue #319](https://github.com/nagadomi/nunif/issues/319)
- Jetson AGX Orin, TensorRT FP16: Depth Anything 3 Metric-L 33 ms, Depth Pro 803 ms. [arXiv 2610.27199](https://arxiv.org/abs/2610.27199)
- A ROS 2 wrapper for Depth Anything 3 on a Jetson went from 6.35 FPS in PyTorch to 43 FPS or more with TensorRT 10.3. [Open Robotics Discourse](https://discourse.openrobotics.org/t/depth-anything-3-ros-2-wrapper-development/51030)

DirectML and cross-vendor numbers
- Desktop2Stereo end to end, R9 9900X, 1080p input and output, depth resolution 384. RTX 5070 Ti default backend: 31 FPS, 0.07 s latency, 23 percent GPU. RTX 5070 Ti "(dml)": 28 FPS, 0.12 s latency, 35 percent GPU. RX 9060 XT: 40 FPS, 0.06 s latency, 71 percent GPU. Apple M1: 15 FPS, 0.21 s. At depth resolution 192 the RTX rows are 31 and 30 FPS. The table names neither the model nor the precision. [lc700x/desktop2stereo Benchmark.md](https://github.com/lc700x/desktop2stereo/blob/main/Benchmark.md)
- Desktop2Stereo's runtime choice per vendor: TensorRT or `torch.compile` on NVIDIA, `torch.compile` on AMD through ROCm 7, Core ML on Apple Silicon, OpenVINO on Intel GPUs, torch-directml for other Windows devices. It says the optimisers "typically increase the output FPS by 30%~50%" and its default depth resolution for Depth Anything V2 is 336. [lc700x/desktop2stereo](https://github.com/lc700x/desktop2stereo)
- Depth Anything 3 Mono Large, ONNX Runtime 1.23 with DirectML on a Radeon RX 7900 XTX, batch 1, fp16: 1036x1036 6.9 fps, 728x728 16.4 fps, 518x518 31.7 fps, 364x364 58.9 fps, 252x252 88.6 fps. VRAM 1.7 GB down to 0.7 GB. The 4-bit weight variant reaches 33.7 fps at 518 and 101.3 fps at 252. [Jens-Duttke/Depth-Anything-3-MONO-ONNX](https://huggingface.co/Jens-Duttke/Depth-Anything-3-MONO-ONNX)

Other runtimes
- Core ML, Depth Anything V2 Small F16 at 518x396, read in source: MacBook Pro M1 Max 32.80 ms, MacBook Pro M3 Max 24.58 ms, iPhone 15 Pro Max 33.90 ms, iPhone 12 Pro Max 31.10 ms. The dominant compute unit is the Neural Engine in all four. The F16 model is 49.8 MB. [apple/coreml-depth-anything-v2-small](https://huggingface.co/apple/coreml-depth-anything-v2-small)
- WebGPU against CUDA: a 2026 paper measured per-dispatch overhead of 24 to 58 µs for CNN, ViT and U-Net workloads, and CUDA at 1.4 times WebGPU's throughput at matched float32 on a mobile RTX PRO 2000. It is not an ONNX Runtime test. [arXiv 2604.02344](https://arxiv.org/html/2604.02344v1)
- A 2026 edge benchmark found TensorRT best for CNNs, but for a transformer vision-language model "PyTorch tends to achieve the lowest inference times on the desktop GPUs". [arXiv 2607.11356](https://arxiv.org/pdf/2607.11356)
- On CPU, a study found ONNX Runtime matched PyTorch depth accuracy for Depth Anything V2 Small while NCNN lost accuracy on the ViT model. [Atlantis Press](https://www.atlantis-press.com/article/126028687.pdf)

### Inferences
- My estimate. Scaling the 3070 result of 6.86 ms and the 4090 result of 3 ms, a mid-range RTX 4060 to 4070 class card should run V2 Small at 518 px in roughly 5 to 9 ms with a TensorRT-class runtime when the GPU is otherwise idle. Under a game the number will be several times worse and variable.
- My estimate. NVIDIA's "50 percent faster than DirectML" claim plus the FlashAttention finding suggests DirectML sits at 1.5 to 2 times TensorRT-RTX latency for this model on RTX. DirectML 1.15.4 predates most fused attention work, and the 3070 profile shows attention is where FP16 fused kernels gain 5 times.
- The DA3 Large DirectML table gives a usable resolution curve for DirectML on AMD: from 518 to 252 px the pixel count falls 4.2 times and throughput rises 2.8 times, so fixed per-inference overhead matters at small sizes.
- Desktop2Stereo's DirectML row shows a higher GPU load, 35 against 23 percent, and 70 percent more latency for 10 percent fewer frames on the same RTX 5070 Ti. The backend there is torch-directml, not ONNX Runtime, so read it as direction only.
- The Apple numbers put the Neural Engine at 25 to 33 ms for V2 Small. That is slower than any discrete RTX path but uses no GPU time, which matters when a game owns the GPU.

### Gaps
- No published ms per inference for Depth Anything V2 Small or Base on ONNX Runtime DirectML with a named GPU, resolution and precision. Unishade's own measurements would be the first I know of.
- No published TensorRT for RTX number for any Depth Anything model. NVIDIA's per-model charts exist only as images.
- No Depth Anything number for the native WebGPU EP, OpenVINO on Arc, the AMD GPU EP, MIGraphX, ncnn Vulkan or MNN. Searches for each returned nothing usable.
- No Depth Anything V2 number at 266, 392 or 924 px on any Windows runtime.
- No measurement anywhere of depth inference latency while a game loads the same GPU.

## 3. CUDA or TensorRT with D3D12: sharing resources and competing with the game

### Takeaway
NVIDIA's answer is CUDA-in-Graphics mode: create the CUDA context from the app's D3D12 device and a compute command queue so TensorRT-RTX work is scheduled with the app's graphics work. NVIDIA built it for inference inside games, but its own docs say the kernels lose performance under the mode's shared-memory limits, and the mode has open bugs.

### Cited findings
- CiG setup read in source: check `CU_DEVICE_ATTRIBUTE_D3D12_CIG_SUPPORTED`, create a D3D12 device, a DIRECT queue and a COMPUTE queue, fill `CUctxCigParam` with `sharedDataType = CIG_DATA_TYPE_D3D12_COMMAND_QUEUE` and the compute queue, call `cuCtxCreate_v4`, then verify with `cuCtxGetLimit` and `CU_LIMIT_CIG_ENABLED`. It needs CUDA 12.6 or later and driver 555 or later. [NVIDIA, Simultaneous compute and graphics](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/inference-library/compute-graphics.html)
- The stated use case is video games running generative AI next to 3D rendering. [NVIDIA, Simultaneous compute and graphics](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/inference-library/compute-graphics.html)
- Limits read in source. Kernels running concurrently with graphics get 48 KiB of shared memory on Ampere and Ada. Blackwell gets 48 KiB on a normal priority queue, or 84 KiB on a `D3D12_COMMAND_QUEUE_PRIORITY_HIGH` queue "at the cost of L1 cache", which suits "relatively light graphics workloads". The limits "will generally result in reduced performance compared to running standalone inference". Engines must be built with `tacticSharedMem:48K` or `84K`. [NVIDIA, Simultaneous compute and graphics](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/inference-library/compute-graphics.html)
- "Simultaneous graphics and compute workloads are not supported for the NVIDIA Turing architecture." Only DirectX interop is supported in the documented release, Vulkan is planned. [NVIDIA, Simultaneous compute and graphics](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/inference-library/compute-graphics.html)
- Known issue in 1.7: allocations can fail in CiG mode even with free GPU memory. The workaround is a custom `IGpuAllocator` on `cudaMalloc`, which "can slightly reduce performance and disables CUDA Graph support". [NVIDIA, TensorRT-RTX 1.7 release notes](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/getting-started/release-notes-1/1.7.html)
- History: 1.3 added CUDA graphics mode contexts on Blackwell, 1.4 fixed CiG performance issues and segmentation faults on Blackwell. [GitHub, NVIDIA/TensorRT-RTX releases](https://github.com/NVIDIA/TensorRT-RTX/releases); [NVIDIA, TensorRT-RTX 1.4 release notes](https://docs.nvidia.com/deeplearning/tensorrt-rtx/1.4/getting-started/release-notes-1/1.4.html)
- Plain CUDA interop with D3D12 works "by importing and exporting through OS-level handles for both memory and synchronization objects". The driver API has `CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP` and resource types. [NVIDIA, CUDA programming guide, graphics interop](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html); [NVIDIA, CUDA driver API, external resource interop](https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__EXTRES__INTEROP.html)
- Pitfalls from forum reports. `CreateSharedHandle` fails unless the heap or resource was created with `D3D12_HEAP_FLAG_SHARED`. [NVIDIA forum, shared heap in UE4](https://forums.developer.nvidia.com/t/driver-crash-when-creating-root-signature-for-d3d12-resource-with-shared-heap-in-unreal-engine-4/79582). `cudaImportExternalMemory` returned `cudaErrorIllegalAddress` for textures of 900x900 and 500x500 while 800x800 and 1024x1024 worked. [NVIDIA forum, cudaErrorIllegalAddress](https://forums.developer.nvidia.com/t/cudaimportexternalmemory-return-cudaerrorillegaladdress/322607)
- CUDA docs warn that touching a resource from another API while CUDA has it mapped "produces undefined results", and that all outstanding signals and waits must finish before a semaphore is destroyed. [NVIDIA, CUDA programming guide, graphics interop](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html)
- Copy cost with I/O binding in NVIDIA's Windows ML test on an RTX 5090: device copy 4.2 ms, one-time host copy 1.3 ms, and a "~75x reduction in copy time for a 30 iteration loop" when tensors stay on the device. That is Stable Diffusion 3.5 Medium, with larger tensors than a depth map. [NVIDIA developer blog, Windows ML on RTX PCs](https://developer.nvidia.com/blog/deploy-ai-models-faster-with-windows-ml-on-rtx-pcs)
- ONNX Runtime wraps both mechanisms: `ImportMemory` and `ImportSemaphore` for D3D12 resources and fences, `InitGraphicsInteropForEpDevice` for the queue. A draft pull request, #27267, adds CiG context creation and a reviewer called the command queue "more of a performance optimization". I saw that only as a search snippet. [ONNX Runtime C API, OrtInteropApi](https://onnxruntime.ai/docs/api/c/struct_ort_interop_api.html); [ONNX Runtime C API, OrtGraphicsInteropConfig](https://onnxruntime.ai/docs/api/c/struct_ort_graphics_interop_config.html)

### Inferences
- Imported D3D12 buffers are zero-copy on the same GPU. Unishade already binds input and output as D3D12 buffers, so the change is to create them shareable, import them once, and pass fence values per frame.
- The game is a different process, so CiG cannot merge depth inference into the game's context. CiG only ties the CUDA work to Unishade's own queue. What it buys is one context for Unishade's overlay rendering and its inference, and D3D12 queue priority applied to the inference. Whether that hurts the game less than a separate CUDA context is not answered by any source.
- A plain CUDA context in Unishade's process is a third GPU context next to the game and Unishade's D3D12 device. The Windows scheduler time-slices contexts, so every inference pays context switches on top of kernel time. That is my reasoning, not a sourced fact.
- Buffers have fewer import pitfalls than textures. The size-dependent failure above was with textures. Keeping the tensor interface as buffers, as Unishade does now, avoids format and mip questions.
- RTX 20 series cards cannot use CiG and need their own engine build. Treat them as DirectML users.

### Gaps
- No measurement compares game frame time with inference in CiG mode, in a separate CUDA context, and in D3D12 through DirectML.
- No source documents how D3D12 fence values map onto CUDA external semaphore wait and signal calls beyond the API reference.
- I found no number for the latency of a CiG kernel at 48 KiB shared memory against the unrestricted kernel for a ViT.

## 4. Alternatives to ONNX Runtime

### Takeaway
None of the general alternatives has evidence of beating TensorRT-RTX or even DirectML for a ViT on desktop GPUs. The only approach with shipping proof in games is what AMD did for FSR 4: generate HLSL compute shaders from the ONNX model. That works for a 100K-parameter CNN, and nobody has published it for a 25M-parameter ViT.

### Cited findings
- FSR 4's network was converted from ONNX to HLSL by AMD's ML2Code tool, and AMD fused preprocessing and filter logic into the ML kernels by hand. [woti, Understanding FSR 4](https://woti.substack.com/p/understanding-fsr-4); [LavX, FSR 4 leak](https://news.lavx.hu/article/inside-amds-accidental-fsr-4-leak-how-a-100k-parameter-network-rewrites-real-time-upscaling)
- The same article describes the FSR 4 model as a kernel prediction network of about 100K parameters. FSR 4 requires HLSL Shader Model 6.4 and ships as a signed binary. [LavX, FSR 4 leak](https://news.lavx.hu/article/inside-amds-accidental-fsr-4-leak-how-a-100k-parameter-network-rewrites-real-time-upscaling); [GPUOpen, FSR 4 manual](https://gpuopen.com/manuals/fidelityfx_sdk2/techniques/super-resolution-ml/)
- Cooperative vectors previewed in June 2025. Shader Model 6.9 shipped at retail with Agility SDK 1.619 in February 2026, and the same post says "Cooperative Vector has been deprecated in favor of a future design unifying matrix-matrix and vector-matrix operations, coming in Shader Model 6.10". [DirectX blog, Cooperative vector preview](https://devblogs.microsoft.com/directx/cooperative-vector/); [DirectX blog, SM 6.9 retail](https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/)
- Microsoft describes cooperative vectors and WaveMMA as having "supported narrow matrix operation use cases" and replaces both with the LinAlg feature set in the Shader Model 6.10 preview. Latest Agility SDK builds are 1.620.2 retail and 1.722.1 preview, both dated 2026-11-09. [DirectX blog](https://devblogs.microsoft.com/directx/?p=12769); [DirectX blog, Agility SDK downloads](https://devblogs.microsoft.com/directx/directx12agility)
- DirectML can be called directly without ONNX Runtime. The developer builds operators or a graph, records them into their own command lists, and can interleave them with rendering. [Microsoft Learn, Introduction to DirectML](https://learn.microsoft.com/en-us/windows/ai/directml/dml)
- CGC compiles a model to programs for a D3D12 device using IHV compiler backends, see section 1. [Microsoft Learn, The CGC backend](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/runtime/model-compilation-execution)
- ncnn is maintained, version 1.0.20261020 was uploaded 2026-10-20 under BSD-3. Measured by me, the win_amd64 wheel is 7.1 MB. [PyPI, ncnn](https://pypi.org/project/ncnn/)
- ncnn lost accuracy on Depth Anything V2 Small in a CPU study where ONNX Runtime matched PyTorch. [Atlantis Press](https://www.atlantis-press.com/article/126028687.pdf)
- ExecuTorch's 2026 paper reports GPU results mainly on phones, with its Vulkan delegate fully delegating MobileNet V3 and ResNet50. [arXiv 2605.08195](https://arxiv.org/pdf/2605.08195)
- Depth Anything V2 Small INT8 through ExecuTorch and XNNPACK on an Arm phone ran at about 1.37 fps at 518x686. That is a CPU path. [Arm Developer](https://developer.arm.com/ai/models/hugging-face/Arm/depth-anything-v2-small-int8-xnnpack-executorch-vivo-x300/depth-anything-v2-small-int8-pte)
- Core ML on Apple Silicon runs V2 Small mainly on the Neural Engine at 25 to 33 ms, see section 2. The DA3 ONNX author needed a separate Core ML build that expands Attention, SkipLayerNormalization and FastGelu. [apple/coreml-depth-anything-v2-small](https://huggingface.co/apple/coreml-depth-anything-v2-small); [Jens-Duttke/Depth-Anything-3-MONO-ONNX](https://huggingface.co/Jens-Duttke/Depth-Anything-3-MONO-ONNX)

### Inferences
- Cooperative vectors and SM 6.10 LinAlg target small per-pixel MLPs evaluated inside shaders. A ViT needs batched matrix multiplies over 361 to 4,356 tokens with 384-wide embeddings, which is ordinary compute-shader GEMM work. I see no evidence that these HLSL features help it, and the API is still in preview after one deprecation.
- A hand-written or generated HLSL ViT is feasible because the op set is small: patch-embed convolution, LayerNorm, MatMul, softmax, GELU, and the DPT head's convolutions and bilinear resizes. It would give full control over scheduling in slices per frame and zero extra download. The cost is writing a tensor-core-competitive GEMM in HLSL for three vendors. The 3070 profile shows FP16 tensor-core kernels are where most of the speed comes from, and plain HLSL cannot reach those kernels. My judgement is that it would land near DirectML speed, not near TensorRT speed.
- Calling DirectML directly, without ONNX Runtime, removes 17 MB of onnxruntime.dll and allows layer-by-layer submission spread over several frames. It does not make the kernels faster.
- ncnn is the smallest download by far and runs on Vulkan, which fits Unishade's Linux backend. The reported ViT accuracy loss and the absence of any desktop ViT benchmark make it a research item, not a recommendation.

### Gaps
- No published desktop GPU benchmark of ncnn Vulkan, MNN, TVM or ExecuTorch Vulkan on a ViT, against TensorRT or DirectML.
- I found nothing on how DLSS or XeSS implement their kernels in D3D12, and nothing confirming whether FSR 4 avoids DirectML entirely.
- I did not research Vulkan ML extensions such as VK_ARM_tensors and data graphs, or MPSGraph, within the tool budget. No finding to report.
- No source describes a generated-HLSL implementation of a ViT of this size.

## 5. Model-level optimisations with measured effect

### Takeaway
The measured wins are FP16 with fused attention at 1.75 times, removal of per-kernel launch overhead at a further 1.5 times, and lower input resolution, which is the largest lever available on every vendor. INT8 and token merging both have negative evidence for DPT-style depth models.

### Cited findings
- FP16 against FP32 on RTX 3070, V2 Small, 518 px: 19.13 to 10.92 ms, 1.75 times. Mean relative error against FP32 on one game frame: 0.071 percent for FP16, 0.680 percent for BF16. [karadjordjepetrovic/realtime-depth-optimization](https://github.com/karadjordjepetrovic/realtime-depth-optimization)
- Core ML F16 against the original model: abs-rel error 0.0089, against 0.0072 for the F32 conversion. [apple/coreml-depth-anything-v2-small](https://huggingface.co/apple/coreml-depth-anything-v2-small)
- FP16 can break. On Jetson Orin with TensorRT 10.3, Base and Large FP16 engines matched FP32 but the Small engine at full resolution deviated by 17.5 percent, which the authors tie to an attention export problem. [arXiv 2610.27199](https://arxiv.org/pdf/2610.27199)
- A TensorRT project claims correlation above 0.999 and MAE below 0.1 percent for FP16 against PyTorch. [alicaank/DepthAnythingV2-TensorRT](https://github.com/alicaank/DepthAnythingV2-TensorRT)
- Fused attention. In FP16 PyTorch selected a FlashAttention kernel and attention time fell from 7.1 to 1.4 ms, 5 times. [karadjordjepetrovic/realtime-depth-optimization](https://github.com/karadjordjepetrovic/realtime-depth-optimization)
- Launch overhead. CUDA graphs took FP16 from 10.92 to 7.07 ms and cut the p95 minus median spread from about 0.6 ms to under 0.1 ms, with bit-identical output. [karadjordjepetrovic/realtime-depth-optimization](https://github.com/karadjordjepetrovic/realtime-depth-optimization)
- TensorRT-RTX has built-in CUDA graphs since 1.2, enabled in the EP with `enable_cuda_graph`, and NVIDIA reports "around 30% performance gain with LLMs" from them. The 1.7 notes say some models see microsecond-scale launch overhead with CUDA graphs. [GitHub, NVIDIA/TensorRT-RTX releases](https://github.com/NVIDIA/TensorRT-RTX/releases); [NVIDIA developer blog, Windows ML on RTX PCs](https://developer.nvidia.com/blog/deploy-ai-models-faster-with-windows-ml-on-rtx-pcs); [NVIDIA, TensorRT-RTX 1.7 release notes](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/getting-started/release-notes-1/1.7.html)
- channels_last saved 0.66 ms without CUDA graphs and 0.22 ms with them. Before it, a layout conversion kernel ran 45 times per frame for about 0.34 ms. [karadjordjepetrovic/realtime-depth-optimization](https://github.com/karadjordjepetrovic/realtime-depth-optimization)
- `torch.compile` made FP16 slower on that setup, 14.63 against 10.92 ms. [karadjordjepetrovic/realtime-depth-optimization](https://github.com/karadjordjepetrovic/realtime-depth-optimization)
- Static shapes on DirectML. For the DA3 ONNX build, pinning height and width with free-dimension overrides gains 1 to 2 percent. The author warns that pinning on ONNX Runtime 1.24 through 1.27 "can crash or produce wrong depth maps" for that model, while 1.23 and 1.28.0 were correct. [Jens-Duttke/Depth-Anything-3-MONO-ONNX](https://huggingface.co/Jens-Duttke/Depth-Anything-3-MONO-ONNX)
- Resolution on DirectML, DA3 Large on RX 7900 XTX: 31.7 fps at 518, 58.9 fps at 364, 88.6 fps at 252. [Jens-Duttke/Depth-Anything-3-MONO-ONNX](https://huggingface.co/Jens-Duttke/Depth-Anything-3-MONO-ONNX)
- Weight-only 4-bit quantisation with FP16 activations through `MatMulNBits`: 33.7 against 31.7 fps at 518 and 101.3 against 88.6 fps at 252, with about 0.3 GB less VRAM. It needs ONNX Runtime 1.22 or later. [Jens-Duttke/Depth-Anything-3-MONO-ONNX](https://huggingface.co/Jens-Duttke/Depth-Anything-3-MONO-ONNX)
- INT8 on TensorRT. An NVIDIA forum thread reports INT8 engines slower than FP16 for MiDaS DPT Hybrid on AGX Thor, "the same INT8 regression with Depth-Anything-V2", and a drop with FP8. NVIDIA staff called it unexpected and the thread shows no fix. [NVIDIA forum](https://forums.developer.nvidia.com/t/int8-throughput-and-latency-worse-than-fp16-for-midas-dpt-hybrid-model-on-thor/354915)
- INT8 elsewhere. A mobile assistant paper reports Depth Anything V2 INT8 weight quantisation at about 2.4 times lower latency with 2 percent or less accuracy loss, runtime not stated. [arXiv 2607.02371](https://arxiv.org/pdf/2607.02371)
- TensorRT-RTX 1.7 adds FP8 and INT8 weight-only-quantised convolutions for smaller engines and lower peak memory. [NVIDIA, TensorRT-RTX 1.7 release notes](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/getting-started/release-notes-1/1.7.html)
- Token merging. ToMe doubles throughput of ViT-L at 512 px and ViT-H at 518 px on classification with a 0.2 to 0.3 percent accuracy drop, without retraining. [arXiv 2210.09461](https://arxiv.org/abs/2210.09461)
- Token reduction on DPT depth. A 2026 paper notes that dense decoders such as DPT reassemble tokens into a 2D map, which merging corrupts. Its pruning removed 12.1 percent of backbone FLOPs at ratio 0.20, but "a wall-clock speedup appears only at 448×448 (1.12×)". [arXiv 2609.27988](https://arxiv.org/pdf/2609.27988)
- CubistMerge keeps the spatial grid after merging and reports a 1.25 times speedup on SAM-H for a 0.7 percent mIoU drop, off the shelf. [arXiv 2509.21764](https://arxiv.org/html/2509.21764v1)
- Graph changes that helped a DirectML build: fold multiple outputs into one disparity tensor, apply operator fusion and constant folding offline, use float16 input and output tensors, opset 21. [Jens-Duttke/Depth-Anything-3-MONO-ONNX](https://huggingface.co/Jens-Duttke/Depth-Anything-3-MONO-ONNX)

### Inferences
- My arithmetic. With 14 px patches, Unishade's four sizes give at most 19x19 = 361 tokens at 266, 28x28 = 784 at 392, 37x37 = 1,369 at 518 and 66x66 = 4,356 at 924 for a square input. Linear layers scale with token count and attention with its square, so 392 costs somewhat more than half of 518 in the encoder and 924 costs more than 3 times as much as 518 in linear layers and about 10 times in attention.
- The depth head is only about 15 percent of GPU time at 518 px in the FP32 profile and a larger share in FP16, 1.6 of roughly 7 ms. A lighter head than DPT is worth at most that share. No source measured a replacement head.
- The 3.7 ms of idle GPU per frame in eager PyTorch is launch overhead from hundreds of small kernels. DirectML and WebGPU also issue one dispatch per operator, so runtimes that fuse or capture the whole graph should win most at small resolutions, where kernels are short. This fits the DA3 DirectML curve.
- The ONNX Runtime 1.24 to 1.27 static-shape warning concerns a different model, but Unishade runs ONNX Runtime 1.24.4 on DirectML with fixed shapes. A correctness check of pinned against symbolic dimensions is cheap.
- INT8 with QDQ is a poor bet for this model on desktop GPUs. The evidence is slower-than-FP16 engines on NVIDIA and no DirectML or OpenVINO GPU data for ViT depth.
- Token merging needs an unmerge step before the DPT head and has shown 1.1 to 1.25 times on dense tasks. Reducing input resolution gets more for no engineering.

### Gaps
- No source measures ONNX Runtime graph optimisation levels, opset choice, or removal of Resize ops on DirectML for this model.
- No accuracy numbers for INT8 Depth Anything V2 on a desktop GPU runtime.
- No published ToMe result on Depth Anything itself.
- No measurement of a distilled or convolutional decoder replacing DPT on V2 Small.

## 6. Scheduling under GPU contention from a game

### Takeaway
Windows offers two documented controls, D3D12 command queue priority and the process GPU scheduling class, and neither has published measurements for inference next to a game. The cleanest way to stop competing with the game is a second adapter, which Lossless Scaling users do today, at the cost of a cross-adapter copy that nobody has measured well.

### Cited findings
- `D3D12_COMMAND_QUEUE_PRIORITY` has NORMAL at 0, HIGH at 100 and GLOBAL_REALTIME at 10000. Global realtime needs a privileged application and fails outright if the adapter or driver cannot preempt. [Microsoft Learn, D3D12_COMMAND_QUEUE_PRIORITY](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_command_queue_priority)
- A DirectX spec adds `ID3D12CommandQueue1` with `SetProcessPriority` and `SetGlobalPriority`, on queues created with `D3D12_COMMAND_QUEUE_FLAG_ALLOW_DYNAMIC_PRIORITY`. Process priority is "a non-yielding boost relative to other contexts in the same process". Global priority is relative to other apps and runs from IDLE through SOFT_REALTIME levels to HARD_REALTIME. Some values need elevation and return `E_ACCESSDENIED`. [DirectX-Specs, command queue dynamic priority](https://microsoft.github.io/DirectX-Specs/d3d/D3D12_CommandQueue_Dynamic_Priority.html)
- The same spec says priorities apply to scheduling groups of one DIRECT queue plus a number of COMPUTE queues, and that creation priority is ignored for DIRECT and COMPUTE queues in scheduling groups unless the new flag is used. It states no shipping version. [DirectX-Specs, command queue dynamic priority](https://microsoft.github.io/DirectX-Specs/d3d/D3D12_CommandQueue_Dynamic_Priority.html)
- `D3DKMTSetProcessSchedulingPriorityClass` sets a process's GPU scheduling class. The classes are Idle, Below Normal, Normal, Above Normal, High and Realtime. [Microsoft Learn, D3DKMTSetProcessSchedulingPriorityClass](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsetprocessschedulingpriorityclass); [Bitsum, GPU priorities](https://bitsum.com/?p=986585)
- Looking Glass used that call to give its capture host "a higher GPU thread priority than the foreground application". At "100% GPU load" the host was "not missing any frames" and the game lost less performance than with a frame limiter. No numbers. The host had to run as SYSTEM through PsExec. [Level1Techs forum](https://forum.level1techs.com/t/news-dxgi-capture-performance/160459)
- The Realtime class caused a D3D11 NVENC encoder hang in one report. [NVIDIA forum](https://forums.developer.nvidia.com/t/nvenc-encoder-hangs-on-windows-when-using-d3d11-in-real-time-mode/357465)
- NVIDIA's CiG example puts inference on a COMPUTE queue at HIGH priority. On Blackwell, HIGH trades L1 cache for kernel shared memory and NVIDIA recommends NORMAL "for heavy concurrent rendering workloads". [NVIDIA, Simultaneous compute and graphics](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/inference-library/compute-graphics.html)
- DirectML's layer-by-layer mode exists so an app can interleave ML dispatches with rendering and use "asynchronous compute or shader units on your GPU that would otherwise be idle". [Microsoft Learn, Introduction to DirectML](https://learn.microsoft.com/en-us/windows/ai/directml/dml)
- Lossless Scaling on two GPUs: the game renders on the primary card, the secondary card runs frame generation and drives the display. Frame generation on the game's GPU reduces the game's real frames because they share resources. Brands can be mixed. [Lossless Scaling dual GPU guide](https://sageinfinity.github.io/docs/Guides/DualGPUGuide)
- Tom's Hardware tested Intel Iris Xe integrated graphics and a GTX 1050 Ti as secondary GPUs and found the offload removes the overhead when the secondary GPU is strong enough, with stronger cards needed at 1440p and above. [Tom's Hardware](https://www.tomshardware.com/video-games/pc-gaming/offloading-lossless-scaling-frame-gen-to-secondary-gpu-eliminates-overhead)
- Users disagree on latency. One reports that "sending the image to the secondary GPU adds latency", another measured dual-GPU added latency at about half that of DLSS 3 frame generation. Anecdotes only. [Steam discussion](https://steamcommunity.com/app/993090/discussions/0/677354977728991300/)
- Community depth tools already run depth out of step with the display. Desktop2Stereo reports 0.06 to 0.30 s end-to-end latency, iw3 streams at 15 to 30 FPS by default. [lc700x/desktop2stereo Benchmark.md](https://github.com/lc700x/desktop2stereo/blob/main/Benchmark.md); [nagadomi/nunif issue #319](https://github.com/nagadomi/nunif/issues/319)
- TensorRT-RTX 1.7 can unload an inactive engine's weights and restore them without a rebuild, and `nv_weight_streaming_budget` caps GPU memory for weights. [NVIDIA, TensorRT-RTX 1.7 release notes](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/getting-started/release-notes-1/1.7.html); [NVIDIA, EP provider options](https://nvidia.github.io/TensorRT-RTX-EP-ABI/_generated/provider-options.html)

### Inferences
- Unishade wants the opposite of Looking Glass for the depth model: the overlay's present path should keep priority and the depth inference should yield to the game. A separate low-priority queue for inference and a normal queue for the overlay matches the documented model. On current Windows the creation-time priority may be ignored unless the dynamic priority flag is available.
- Asynchronous depth is the only design that bounds the damage. Run inference on its own queue, never wait on its fence in the present path, and reuse the last finished depth map. Depth then lags by inference time plus queueing, one or two frames at 60 Hz if inference takes 10 to 25 ms under load.
- Splitting one inference over several frames needs layer-level control. DirectML called directly and custom shaders allow it. ONNX Runtime sessions and TensorRT engines run as one unit, so the only option there is to cut the model into two or three ONNX graphs, for example encoder halves and head.
- An integrated GPU is the natural second adapter on laptops and on desktops with Intel or AMD CPUs. By my arithmetic the input is under 1 MB in FP16 at 392x392 with 3 channels, and the output is one small plane, so the cross-adapter copy moves little data. DirectML on the second adapter needs no new runtime download. Apple's Neural Engine figures suggest an NPU-class device runs V2 Small in 25 to 35 ms, but on Windows that route means QNN, VitisAI or the OpenVINO NPU plugin and INT8 models.
- Process-wide GPU priority changes are risky for an overlay app. Raising it steals from the game, and the highest class has a hang report.

### Gaps
- No source measures game frame-time impact of a 5 to 20 ms compute job at different queue or process priorities, with or without hardware-accelerated GPU scheduling.
- I found no statement of which Windows build or Agility SDK ships `ID3D12CommandQueue1` dynamic priority.
- No measured latency for a D3D12 cross-adapter copy of a small buffer or texture.
- No documentation of how Lossless Scaling moves frames between adapters internally.
- No data on depth inference speed on Intel or AMD integrated GPUs or NPUs under Windows.

## 7. Download size and licence of each runtime

### Takeaway
DirectML plus ONNX Runtime is about 15 MB compressed. TensorRT for RTX adds about 111 MB compressed and 286 MB on disk, under a proprietary NVIDIA licence that allows distribution only inside an application and forbids anything that would put it under an open-source licence. CUDA with cuDNN is over 800 MB and not a serious option.

### Cited findings

Sizes, all measured by me from registries on 2026-11-24, x64 Windows
- ONNX Runtime with DirectML 1.24.4: onnxruntime.dll 17.3 MB, 6.0 MB compressed. The whole NuGet package is 12.4 MB. [NuGet, Microsoft.ML.OnnxRuntime.DirectML](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML)
- DirectML 1.15.4: DirectML.dll 18.5 MB, 9.3 MB compressed. The NuGet package is 202 MB because it holds every architecture plus debug builds and symbols. [NuGet, Microsoft.AI.DirectML](https://www.nuget.org/packages/Microsoft.AI.DirectML/)
- TensorRT RTX EP plugin 0.5.0 with TensorRT-RTX 1.7: 110.9 MB compressed, 286.0 MB unpacked, ONNX Runtime itself not included. [PyPI, onnxruntime-ep-nv-tensorrt-rtx-cu13](https://pypi.org/project/onnxruntime-ep-nv-tensorrt-rtx-cu13/)
- WebGPU EP plugin 0.6.0: 18.1 MB compressed, 43.9 MB unpacked. [PyPI, onnxruntime-ep-webgpu](https://pypi.org/project/onnxruntime-ep-webgpu/)
- OpenVINO 2026.4.1 full wheel: 85.0 MB compressed, 255.7 MB unpacked. The parts a GPU-only ONNX app needs, openvino.dll, the GPU plugin and the ONNX frontend, are 56.6 MB unpacked and about 19.6 MB compressed. [PyPI, openvino](https://pypi.org/project/openvino/)
- ONNX Runtime CUDA build 1.32.0: 157.8 MB compressed. cuDNN for Windows: 696.3 MB. [PyPI, onnxruntime-gpu](https://pypi.org/project/onnxruntime-gpu/); [PyPI, nvidia-cudnn-cu12](https://pypi.org/project/nvidia-cudnn-cu12/)
- Windows ML with the CGC backend: "about 110 MB per architecture". Read in source. [Microsoft Learn, The CGC backend](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/runtime/model-compilation-execution)
- ncnn: 7.1 MB wheel. [PyPI, ncnn](https://pypi.org/project/ncnn/)

Licences
- ONNX Runtime packages are MIT. [PyPI, onnxruntime-directml](https://pypi.org/project/onnxruntime-directml/)
- Microsoft.AI.DirectML is published as the "DirectML Redistributable Package". [NuGet, Microsoft.AI.DirectML](https://www.nuget.org/packages/Microsoft.AI.DirectML/)
- TensorRT for RTX licence, read in source. The software is "licensed, not sold" under a "limited, non-exclusive, revocable, non-transferable, and non-sublicensable" licence. It may be distributed "as incorporated into a software application" if "Your application must have material additional functionality, beyond the included portions of the Software" and "The distributable portions of the Software shall only be accessed by your application". Distribution terms must be consistent with the agreement. [NVIDIA, TensorRT-RTX software licence agreement](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/reference/sla.html)
- Same licence. Section 2.7 forbids to "Use the Software or Derivatives in any manner that would cause them to become subject to an open source software license". Section 1.1 limits it to "develop applications only for use in systems with NVIDIA GPU". [NVIDIA, TensorRT-RTX software licence agreement](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/reference/sla.html)
- Classic TensorRT and the tensorrt-rtx PyPI packages are marked Proprietary. [PyPI, tensorrt-cu13-libs](https://pypi.org/project/tensorrt-cu13-libs/); [PyPI, tensorrt-rtx](https://pypi.org/project/tensorrt-rtx/)
- AMD's GPU EP falls under the Ryzen AI licensing terms. [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)
- ncnn is BSD-3. [PyPI, ncnn](https://pypi.org/project/ncnn/)
- Microsoft tells developers to read each provider's licence before use and keeps a third-party notices page for Windows ML providers. [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)

### Inferences
- An MIT app can load proprietary DLLs at run time without relicensing them, and section 2.7 is about not placing NVIDIA's binaries under an open-source licence. Unishade's model, where the user downloads runtime DLLs on demand and the repository contains none of them, looks compatible if the DLLs come from NVIDIA's own distribution point such as PyPI or NVIDIA's download server. Re-hosting the DLLs on Unishade's servers is "distribution" and must meet the incorporated-in-application conditions. This is my reading, not legal advice, and the project should confirm before shipping.
- Windows ML's catalog sidesteps redistribution: Windows downloads the vendor provider and the user accepts it through the OS. It only works on Windows 11 24H2 or later and ties Unishade to the Windows ML package.
- For an NVIDIA user the total on-demand download becomes roughly 111 MB for the RTX provider plus a newer ONNX Runtime, on top of the DirectML fallback. That is within the "few hundred MB is a lot" limit but about 8 times today's download.
- TensorRT-RTX engines and caches must be rebuilt after each runtime update and per GPU, so Unishade would compile on the user's machine at first use and store the cache per GPU and runtime version.

### Gaps
- I did not read the DirectML redistributable licence text or the OpenVINO licence in this session. From memory OpenVINO is Apache 2.0, unverified here.
- I could not extract the April 2025 TensorRT RTX licence PDF. The quotes above come from the HTML agreement in the current docs.
- The size of the classic TensorRT Windows runtime and of the AMD GPU EP download is not in any source I reached.
- The size of the NvTensorRtRtx provider as delivered by the Windows ML catalog is not published.

## 8. What the evidence suggests is the fastest practical stack per GPU vendor

### Takeaway
On RTX 30 series and newer, ONNX Runtime 1.25 or later with the NvTensorRtRtx plugin EP, D3D12 resource and fence import, FP16, CUDA graphs and a runtime cache is the fastest stack that keeps zero-copy with the app's D3D12 device. Everything else, including AMD, Intel and older NVIDIA cards, has no documented faster zero-copy path than DirectML today, so gains there must come from resolution, scheduling and model changes.

### Cited findings
- NVIDIA RTX 30, 40, 50. The TensorRT RTX EP supports Ampere and later, claims 50 percent or more throughput over DirectML, has D3D12 external resource import since ONNX Runtime 1.25.0, takes the app's `ID3D12CommandQueue` for GPU-side sync, and adds 110.9 MB of download. [ONNX Runtime, TensorRT RTX EP](https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html); [NVIDIA developer blog, Windows ML on RTX PCs](https://developer.nvidia.com/blog/deploy-ai-models-faster-with-windows-ml-on-rtx-pcs); [newreleases.io mirror of ORT 1.25.0 notes](https://newreleases.io/project/pypi/onnxruntime/release/1.25.0); [ONNX Runtime C API, OrtGraphicsInteropConfig](https://onnxruntime.ai/docs/api/c/struct_ort_graphics_interop_config.html); [PyPI, onnxruntime-ep-nv-tensorrt-rtx-cu13](https://pypi.org/project/onnxruntime-ep-nv-tensorrt-rtx-cu13/)
- NVIDIA RTX 20 and GTX. CiG is unsupported on Turing and the EP page lists RTX 30xx and above. [NVIDIA, Simultaneous compute and graphics](https://docs.nvidia.com/deeplearning/tensorrt-rtx/latest/inference-library/compute-graphics.html); [ONNX Runtime, TensorRT RTX EP](https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html)
- AMD. DirectML runs DA3 Large at 31.7 fps at 518 px on an RX 7900 XTX. The AMD GPU EP is the vendor's recommended path in Windows ML 2.x and MIGraphX is deprecated, with no interop documentation for either. [Jens-Duttke/Depth-Anything-3-MONO-ONNX](https://huggingface.co/Jens-Duttke/Depth-Anything-3-MONO-ONNX); [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers)
- Intel. OpenVINO is Intel's provider in Windows ML and what Desktop2Stereo uses on Intel GPUs. [Microsoft Learn, Windows ML execution providers](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/supported-execution-providers); [lc700x/desktop2stereo](https://github.com/lc700x/desktop2stereo)
- All vendors, later. CGC compiles for the app's chosen D3D12 device with vendor backends and DirectML fallback, experimental in November 2026. [Microsoft Learn, The CGC backend](https://learn.microsoft.com/en-us/windows/ai/new-windows-ml/runtime/model-compilation-execution)
- macOS. Core ML F16 runs V2 Small in 24.58 ms on an M3 Max and 32.80 ms on an M1 Max, mostly on the Neural Engine. [apple/coreml-depth-anything-v2-small](https://huggingface.co/apple/coreml-depth-anything-v2-small)
- Linux. The native WebGPU EP runs on Vulkan through Dawn, and ncnn runs on Vulkan with a 7 MB runtime. [ONNX Runtime, WebGPU EP](https://onnxruntime.ai/docs/execution-providers/WebGPU-ExecutionProvider.html); [PyPI, ncnn](https://pypi.org/project/ncnn/)

### Inferences
- NVIDIA RTX 30 and newer. Use the NvTensorRtRtx plugin EP on ONNX Runtime 1.25 or later. Build an FP16 EP context model per fixed resolution, set `nv_runtime_cache_path` and `enable_cuda_graph`, import Unishade's shareable D3D12 input and output buffers with `ImportMemory`, and sync with a shared fence through `WaitSemaphore` and `SignalSemaphore`. Try `InitGraphicsInteropForEpDevice` with a dedicated compute queue and measure it against the default context, because CiG's shared-memory limits can cost more than they save. My estimate for V2 Small at 518 px on an idle mid-range RTX card is 5 to 9 ms, against a DirectML figure I could not source.
- NVIDIA RTX 20, GTX and any card where the RTX provider fails. Stay on DirectML. Validate the FP16 Small engine output against DirectML on first run, given the 17.5 percent FP16 deviation reported for a Small engine in classic TensorRT 10.3.
- AMD. Stay on ONNX Runtime with DirectML for zero-copy. Gains come from lower default resolution, an offline-optimised and fused FP16 graph, a correctness check of pinned shapes on ONNX Runtime 1.24.4, and asynchronous low-priority scheduling. Benchmark the AMD GPU EP through Windows ML as an experiment and expect a CPU or shared-handle copy.
- Intel. DirectML on the same D3D12 device for Arc cards. For systems with an Intel integrated GPU next to a discrete card, running the depth model on the integrated GPU with DirectML or OpenVINO's GPU plugin, about 20 MB compressed, removes contention with the game. The price is a small cross-adapter copy and a slower model.
- Every vendor. The largest safe lever is input size, 392 px has 57 percent of the tokens of 518 px, combined with depth that lags the frame instead of blocking it.
- Watch list. CGC is the only announced path that would bring vendor-compiled kernels to AMD and Intel on the app's D3D12 device. Retest when it leaves experimental status or validates a DINOv2-class model.
- macOS. Keep Core ML. The Neural Engine keeps the GPU free for the game and the overlay, at 25 to 33 ms.
- Linux. No evidence favours any runtime. The WebGPU EP on Vulkan through ONNX Runtime reuses the same model and API, and ncnn is the small-footprint experiment.

### Gaps
- The whole recommendation for NVIDIA rests on vendor claims and API documentation. No independent benchmark of NvTensorRtRtx against DirectML on a ViT exists in public, and no public code shows its D3D12 import path working.
- It is unconfirmed whether one onnxruntime.dll newer than 1.24.4 can host both DirectML and the NvTensorRtRtx plugin, which decides whether NVIDIA users download one ONNX Runtime or two.
- No data on how any of these stacks behave while a game holds the GPU at 95 percent or more, which is the condition Unishade cares about.
