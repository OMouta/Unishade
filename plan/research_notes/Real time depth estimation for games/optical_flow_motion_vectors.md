# Real-time motion vectors from color frames alone, state as of November 2026

Reading notes. "Read" means I saw the number or text in the linked source. "My estimate" means I derived it and no source states it. Where I read a repository file through the GitHub API I link the file on github.com.

## 1. Vendor and OS APIs: NVOFA, FRUC, D3D12 video motion estimation, AMF, Intel, Apple, Vulkan

### Takeaway
NVIDIA's hardware optical flow engine is the only vendor API that meets the cost target with headroom, because it runs on a dedicated engine outside the 3D cores. It is NVIDIA-only, its SDK documentation has not moved since SDK 5.0 in February 2023, and its SDK licence forbids use that would put the SDK under an open-source licence. The D3D12 video motion estimation API is vendor-neutral on paper but gives 8x8 or 16x16 block vectors from the encoder, takes NV12 only, and I found no reliable list of drivers that implement it.

### Cited findings

NVIDIA Optical Flow SDK, NVOFA.
- The optical flow engine is a dedicated hardware accelerator that "works independently of graphics/NVIDIA CUDA cores". [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note)
- Supported on "all Turing GPUs (except TU117) and above". Capability table: 4x4 grid on Turing, Ampere and Ada. 2x2 and 1x1 grids, hardware cost output and ROI only on Ampere and Ada. Max resolution 4096x4096 on Turing, 8192x8192 on Ampere and Ada. [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note)
- Interfaces: CUDA on Windows and Linux, D3D11 on Windows 10+, D3D12 on Windows 10 20H1+, Vulkan on Windows and Linux but not WSL. Vulkan does not work on Turing. [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note)
- Read throughput at 1080p, YUV 4:2:0, 4x4 grid, in frames per second. SLOW preset 225 Turing, 200 Ampere, 536 Ada. MEDIUM 468, 405, 1000. FAST 768, 613, 1296. Test GPUs were RTX 6000, RTX 3090, RTX 4090 on Windows 11. NVIDIA calls the numbers indicative and says performance scales almost linearly with the video clock. [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note)
- The 2x2 and 1x1 rows of that table came through garbled in my extraction. My reading, with Turing not applicable, is 2x2 SLOW 94 Ampere and 210 Ada, MEDIUM 154 and 336, FAST 261 and 711. 1x1 SLOW 29 and 98, MEDIUM 45 and 113, FAST 85 and 222. Treat these as needing a second check against the page. [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note)
- KITTI 2015 outlier rate Fl-all in non-occluded regions for the 4x4 grid: SLOW 21.37 Turing, 18.65 Ampere, 17.26 Ada. FAST 26.39, 25.44, 23.48. [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note)
- Flow vectors are 16 bits per component with a sign bit, 10 integer bits and 5 fractional bits, so 1/32 pixel steps. Forward flow is pixel movement from the input frame to the reference frame. `NV_OF_PRED_DIRECTION_BOTH` returns forward and backward flow in one execute call. [NVOFA programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html)
- A per-vector cost buffer is available. "Higher cost value implies the flow vector to be less accurate." NVIDIA recommends the 8-bit cost format. `enableGlobalFlow` returns one global vector for the frame in the same call. [NVOFA programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html)
- Temporal hints from the previous frame's vectors are on by default, and external hints get top priority in the hardware search. "Using the SLOW preset may result in increase in graphics/CUDA engine utilization." [NVOFA programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html)
- The D3D12 path uses `NvOFAPICreateInstanceD3D12` and `NvCreateOpticalFlowD3D12` on an `ID3D12Device`, with resources registered through `NvOFRegisterResourceD3D12` and synchronised by `ID3D12Fence` input and output fence points. [NVOFA programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html)
- Status. SDK 5.0 needs R525 drivers or newer. The application note carries a 2018 to 2023 copyright and was last updated on 27 February 2023. The only deprecation it announces is Stereo Disparity mode. It lists Turing, Ampere and Ada and does not mention RTX 50. [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note), [developer.nvidia.com Optical Flow SDK](https://developer.nvidia.com/opticalflow-sdk)
- Licence. SDK 5.0 ships under the NVIDIA DesignWorks SDK License of May 2022. "You may not distribute or sublicense the SDK as a stand-alone product." "You may not use the SDK in any manner that would cause it to become subject to an open source software license." Sample code may ship only "as incorporated in object code format into a software application". [Optical Flow SDK licence](https://docs.nvidia.com/video-technologies/optical-flow-sdk/license/index.html)
- NVIDIA's public GitHub repository holds only `nvOpticalFlowCommon.h` and `nvOpticalFlowCuda.h`. The header I read carries a three-clause BSD-style permission text. The repository was last pushed in July 2021 and has no licence file and no D3D12 header. [NVIDIA/NVIDIAOpticalFlowSDK](https://github.com/NVIDIA/NVIDIAOpticalFlowSDK)
- Vulkan exposes the same engine through the public `VK_NV_optical_flow` extension, with no SDK needed. Output and hint grid sizes are flag bits, flow uses `VK_FORMAT_R16G16_SFIXED5_NV`, cost maps use `VK_FORMAT_R8_UINT` or `R32_UINT`, and the device reports `bidirectionalFlowSupported`. [Vulkan spec, VK_NV_optical_flow](https://docs.vulkan.org/spec/latest/chapters/VK_NV_optical_flow/optical_flow.html)
- NVIDIA says the Vulkan interface works "on NVIDIA Ampere architecture and newer generation GPUs". [NVIDIA blog on Vulkan optical flow](https://developer.nvidia.com/blog/accelerated-motion-processing-brought-to-vulkan-with-optical-flow-sdk/)

NVIDIA FRUC library.
- NvOFFRUC is a frame rate up-conversion library built on NVOFA. Read average time to interpolate one 1920x1080 frame: 12.01 ms on RTX 2080 Ti, 9.23 ms on RTX 3090 Ti, 4.41 ms on RTX 4090. [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note)

Direct3D 12 video motion estimation.
- Shipped with Windows 10 May 2019 Update. It exposes "fixed function acceleration for motion searching", often the GPU video encoder's motion estimator. Named uses are video encoding, frame rate conversion, optical flow and Windows Mixed Reality motion reprojection. [DirectX blog, New in D3D12 Motion Estimation](https://devblogs.microsoft.com/directx/new-in-d3d12-motion-estimation/)
- Input must be `DXGI_FORMAT_NV12` and the search is luma-only. Output is resolved from a hardware-specific motion vector heap into a `DXGI_FORMAT_R16G16_SINT` texture in quarter-pel units, X in R and Y in G, one vector per block. The work runs on a video encode command list and queue. A previous motion vector heap can be passed as a hint. [D3D12 Video Motion Estimation spec](https://microsoft.github.io/DirectX-Specs/d3d/D3D12_Video_Motion_Estimation.html)
- The spec's performance target is "Generating motion vectors for both eyes should take less than 5 ms." [D3D12 Video Motion Estimation spec](https://microsoft.github.io/DirectX-Specs/d3d/D3D12_Video_Motion_Estimation.html)
- Support is found at run time with `D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR`, which reports block size flags, precision flags and a size range. Microsoft's sample uses 16x16 blocks and quarter-pel precision. [DirectX blog](https://devblogs.microsoft.com/directx/new-in-d3d12-motion-estimation/)
- FFmpeg trunk has a `mestimate_d3d12` filter that accepts only block sizes 8 and 16 and fails if the driver lacks quarter-pel precision. [FFmpeg vf_mestimate_d3d12 source](https://ffmpeg.org/doxygen/trunk/vf__mestimate__d3d12_8c_source.html)
- Vendor support is poorly documented. In an older Intel community thread, which the search tool dated to roughly 2019 to 2021, a developer reported the Intel driver returning zero supported block sizes while the same test app worked on AMD and NVIDIA GPUs, and Intel answered that the NUC in question did not support it. [Intel community thread](https://community.intel.com/t5/Graphics/DirectX-12-Motion-Estimator-API-support/m-p/1307513)

AMD AMF.
- AMF has a frame rate conversion component. FFmpeg's `frc_amf` filter turns it on with an `AMF_FRC_MODE` property and doubles the output rate. I found no AMF interface that returns the motion field itself. [FFmpeg vf_frc_amf source](https://ffmpeg.org/doxygen/trunk/vf__frc__amf_8c_source.html)

Apple.
- macOS 15.4 added the VideoToolbox `VTFrameProcessor` API. It includes `VTOpticalFlowConfiguration`, which enables optical flow on a frame processing session, and `VTFrameProcessorOpticalFlow`, which wraps bidirectional flow. [Apple VideoToolbox frame processing](https://developer.apple.com/documentation/videotoolbox/frame-processing.md)
- Apple's WWDC 2025 session splits the effects by use. Frame rate conversion, super resolution and motion blur are for editing. Low latency frame interpolation and low latency super resolution are for "applications with real-time processing needs". The session says "flow calculation can be expensive" and that apps may pre-compute it. [WWDC 2025 session 300](https://developer.apple.com/videos/play/wwdc2025/300)

### Inferences
- My estimate from the read throughput: a 4x4 grid on an RTX 3090 costs about 1.6 ms at FAST and 2.5 ms at MEDIUM for 1080p. On an RTX 4090 it costs about 0.8 ms and 1.0 ms. A 1x1 grid on Ada costs about 4.5 ms at FAST. These are engine times on hardware that the game does not use, so they do not take frame time from the 3D cores the way a compute shader does.
- A 4x4 grid at 1080p gives a 480x270 field. That matches the resolution of the depth network's output more closely than it matches the display, so 4x4 is a reasonable grid for depth reprojection.
- NVOFA expects 8-bit luma or ABGR input at capture resolution. For 4K capture, running it on a half-resolution copy keeps cost near the 1080p figures.
- The licence is the real obstacle. The DesignWorks clause against open-source subjection conflicts with compiling SDK headers and wrapper sources into an MIT project. The Vulkan extension avoids the SDK entirely, so on Linux, and on Windows through a small Vulkan device sharing textures with D3D12, the hardware is reachable with no NVIDIA SDK code. That interop route is my suggestion and I have not seen it done.
- D3D12 video motion estimation looks unattractive for this project. Encoder motion search minimises residual bits and does not track true motion, the grid is 8x8 at best, NV12 conversion and a separate video encode queue add plumbing, and support cannot be assumed.

### Gaps
- I found no NVIDIA statement on whether RTX 50 GPUs keep the optical flow engine or whether a newer Optical Flow SDK than 5.0 exists. The docs stop at Ada and at February 2023. NVIDIA has not marked the SDK deprecated in anything I read.
- No current matrix of which NVIDIA, AMD and Intel drivers report `D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR` support, and no measured cost per frame. The only vendor data point is one old forum thread.
- The direction convention of D3D12 motion estimation output, current to reference or the reverse, was not stated in what I read.
- Intel VPL and Vulkan Video returned nothing in my searches. I believe neither exposes a motion field output, but I have no source for that.
- Apple's optical flow cost in milliseconds, its output resolution and whether `VTOpticalFlowConfiguration` is fast enough for per-frame use were not in the sources I read. I also found nothing on a Metal or MetalFX optical flow call.

## 2. Open-source shader flow from frame generation: FidelityFX Optical Flow, LSFG, XeSS-FG

### Takeaway
AMD's FidelityFX Optical Flow is the best documented drop-in. It is a compute-shader block matcher with D3D12 and Vulkan backends, its sources stay MIT even in the 2026 SDK, and the whole FSR 3.1 frame generation step that contains it costs 0.5 to 2.1 ms at 1080p on GPUs from RX 7900 XTX down to RX 5700 XT. Its output is coarse: one integer vector per 8x8 block. LSFG and XeSS-FG are closed and cannot be reused.

### Cited findings

FidelityFX Optical Flow 1.1.2.
- It "estimates the motion between the current and the previous scene inputs", derives from AMD Fluid Motion Frames, and FSR 3 uses it alongside upscaled game motion vectors. The FSR 3 version is "native HLSL Optical Flow reengineered from original OpenCL FMF codebase". [FidelityFX Optical Flow manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/)
- Motion is computed on 8x8 blocks, and `opticalFlowBlockSize` is "always 8 in this release". Output size is `(displaySize + 7) / 8` per axis. Output format is `R16G16_SINT`. A second output is a 3x1 `R32_UINT` scene change resource. [FidelityFX Optical Flow manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/)
- Preparation phase: color to luminance, then a pyramid of 6 more downscaled levels built with the Single Pass Downsampler, with two pyramids ping-ponged for history. Scene change detection compares luminance histograms of 9 screen sections with the previous frame and sets a flag past a threshold. [FidelityFX Optical Flow manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/)
- Main phase: 7 iterations, coarse to fine, of search, filter, upscale. Search area is "24x24 pixels (an 8px radius around an 8x8 pixel block)", exhaustive, scored by SAD of luminance. Filtering is a 3x3 vector median. Upscaling doubles vectors and picks the best of four candidates by 4x4 block SAD. If scene change fires, the search returns zero motion. [FidelityFX Optical Flow manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/)
- "The maximum tracking movement is 512 pixels." Shader Model 6.2 is required, and the shader uses wave operations and the `msad4` intrinsic. [FidelityFX Optical Flow manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/)
- Read memory use at 4K: 28 MB on RX 7900 XTX and RX 6600, 26 MB on RTX 4080 and RTX 3060. [FidelityFX Optical Flow manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/)
- The shader source confirms `BlockSizeX 8`, `BlockSizeY 8`, `SearchRadiusX (8)`, `SearchRadiusY (8)` and an `msad4` path. [ffx_opticalflow_compute_optical_flow_v5.h, SDK v2.4.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/framegeneration/fsr3/include/gpu/opticalflow/ffx_opticalflow_compute_optical_flow_v5.h)
- Cost. AMD publishes no number for optical flow alone. The FSR 3.1 table gives the full frame generation step, optical flow plus interpolation, as "up to" values with async compute off. At 1080p Native AA: 0.7 ms RX 7900 XTX, 1.0 ms RX 6800 XT, 2.1 ms RX 5700 XT. At 1440p Native AA: 1.0, 1.6, 3.3 ms. At 4K Native AA: 2.4, 3.8, 7.3 ms. [AMD FSR 3 page](https://gpuopen.com/fidelityfx-superresolution-3/)
- Optical flow and frame generation workloads can run on the presentation queue or on an async compute queue supplied by the frame interpolation swapchain. [AMD FSR 3 page](https://gpuopen.com/fidelityfx-superresolution-3/)
- How FSR uses it. Frame interpolation computes one result from game motion vectors and one from optical flow and blends by color match: "if optical flow motion vectors result in a better match the algorithm prefers them to the game motion vectors". Optical flow vectors get a priority from magnitude and from color similarity of the reprojected colors. [FidelityFX frame interpolation manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/)
- Licence. SDK v1.1.4 is MIT throughout. [FidelityFX SDK v1.1.4 LICENSE.txt](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/LICENSE.txt)
- Licence change. The repository is now the "AMD FSR SDK 2.4.0 Redstone". Its default licence grants redistribution "in binary form only" and forbids reverse engineering. A long list of named source files is exempt and stays MIT. That list includes the FSR 3 optical flow headers and shaders under `Kits\FidelityFX\framegeneration\fsr3\`, and the optical flow shader I opened carries the MIT text in its header with a 2026 copyright. [FidelityFX SDK docs/license.md](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/HEAD/docs/license.md), [optical flow shader header](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/Kits/FidelityFX/framegeneration/fsr3/internal/shaders/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.hlsl)
- FSR 4 era frame generation is a separate machine-learning path that needs RX 9000 or newer and costs about 2.2 ms at 4K on an RX 9070 XT and about 2.1 ms at 1440p on an RX 9060 XT. It is not the analytic optical flow. [AMD FSR Frame Generation page](https://gpuopen.com/amd-fsr-framegeneration/)
- Known reuse outside FSR: the LumeniteFX ReShade author lists FidelityFX Optical Flow 1.1.2 as a studied source for his own flow shader. [LumeniteFX NOTICE](https://github.com/umar-afzaal/LumeniteFX/blob/HEAD/NOTICE)

Lossless Scaling LSFG and lsfg-vk.
- The Steam page calls LSFG "a proprietary machine learning model developed from scratch specifically for Lossless Scaling", with a separate performance model for low-power GPUs. [Lossless Scaling on Steam](https://store.steampowered.com/app/993090/Lossless_Scaling)
- The "Resolution Scale" option was renamed "Flow Scale" to stop users confusing it with image scaling. [Lossless Scaling patch notes on SteamDB](https://steamdb.info/patchnotes/17646233)
- Press coverage of the 2.12 beta says the resolution scale feature "gives the frame generation algorithm slightly less information to work with". [Syndicated Digital Trends article](https://tech.yahoo.com/computing/articles/fps-doubling-app-now-even-153159100.html)
- Notebookcheck's guide describes Flow Scale as a performance modifier and says turning performance mode on risks visual artifacts while off costs more GPU time. The search summary I got of its Flow Scale wording had the quality and cost directions the opposite way round from the Digital Trends remark, so I do not rely on it for direction. [Notebookcheck guide](https://www.notebookcheck.net/You-Should-Be-Modding-Lossless-Scaling-Guide.1295503.0.html)
- LSFG 3 reportedly cut GPU load by about 40% against LSFG 2 and latency by about 24%, per the developer and one community benchmark. [Tom's Hardware](https://www.tomshardware.com/video-games/pc-gaming/lossless-scaling-3-update-touts-greatly-improved-latency-and-performance-universal-frame-gen-tool-boasts-24-percent-reduced-latency)
- lsfg-vk is a Vulkan layer that brings LSFG 3.1 to Linux. It "depends on an existing installation of Lossless Scaling" and takes the frame generation shaders from `Lossless.dll`. The author ported them by hooking shader loads under DXVK and comparing DXBC with SPIR-V output. [lsfg-vk site](https://lsfg-vk.dev/), [lsfg-vk mirror README](https://github.com/LoveDoLove/lsfg-vk), [TechPowerUp forum post](https://www.techpowerup.com/forums/posts/5550681)
- lsfg-vk v1 was MIT, the v2 rewrite went GPLv3 and then CC BY-NC-ND 4.0, which per its author "prevents any commercial use and all derivative works". The source moved off GitHub to a self-hosted server and v1 source was removed from the main repository. v2 adds a bindless pipeline with FP16 and a claimed 2x to 3x speedup on FP16-capable hardware. [lsfg-vk blog](https://lsfg-vk.dev/blog/important-changes-to-lsfg-vk/), [GamingOnLinux on lsfg-vk 2.0](https://www.gamingonlinux.com/2026/09/lossless-scalings-frame-generation-for-linux-lsfg-vk-2-0-is-out-now/)

Intel XeSS-FG.
- XeSS 2 frame generation uses an AI model trained for optical flow reprojection plus a blend model that merges optical flow and engine motion vector reprojection. Intel notes that optical flow picks up shadows, reflections and particles that engine vectors miss. [Intel XeSS 2 whitepaper](https://intel.com/content/www/us/en/developer/articles/technical/xess2-whitepaper.html)
- XeSS 3 multi frame generation runs one optical flow pass per batch of generated frames. [TechSpot](https://www.techspot.com/news/111085-intel-xess-3-multi-frame-generation-now-rolling.html)
- The XeSS SDK repository has no SPDX-detectable open-source licence and ships as binaries. [intel/xess](https://github.com/intel/xess)

### Inferences
- My estimate: FidelityFX optical flow alone, at 1080p on a mid-range GPU such as an RX 6600 or RTX 3060, lands around 0.5 to 1 ms. Basis: it is part of a frame generation step that totals 2.1 ms on the slower RX 5700 XT, and interpolation plus inpainting at full resolution should take the larger share. Nobody publishes the split, so measure it.
- For 4K capture, feed it a half-resolution luma copy. Cost follows input pixels, and an 8x8 block at half resolution is a 16x16 block at display resolution, which is too coarse for DLSS but tolerable for a depth field that comes out of a low-resolution network anyway.
- The integer output format means sub-pixel motion is rounded. Slow pans under one pixel per frame at 240 fps will read as zero or jitter between 0 and 1. A sub-pixel refinement pass, for example one Lucas-Kanade step per block seeded with the block vector, would be my addition and is not in AMD's code.
- It needs SM 6.2 wave ops and `msad4`. The FidelityFX backends cover D3D12 and Vulkan. Metal would need a port of the HLSL.
- LSFG's network cannot be reused legally or practically. The shaders are extracted from a paid proprietary DLL, and lsfg-vk's own licence now forbids derivatives.

### Gaps
- No public per-pass timing for FidelityFX optical flow alone, and no numbers for NVIDIA mid-range cards.
- Nothing I read describes LSFG's network architecture, its pass list or its flow resolution. The Steam page and press give settings only.
- I did not confirm the exact text of every entry in the SDK 2.4.0 MIT exemption list. I saw four optical flow paths in the list, counted 22 lines matching "opticalflow", and read the MIT header in one shader.
- Intel publishes no description of the XeSS-FG flow network's size or cost.

## 3. ReShade-ecosystem optical flow shaders

### Takeaway
The ReShade community has several dense motion estimators that work from color, or color plus depth, and output exactly what this project wants: a full or 1/8 resolution RG16F current-to-previous field, some with a disocclusion or confidence texture. Almost none can be copied into an MIT project. The licences are CC BY-NC, a no-redistribution licence, or the AGNYA licence that requires the author's permission. CShade, BSD-3-Clause, is the one permissive exception I found, and it uses Lucas-Kanade instead of block matching.

### Cited findings

Jakob Wapenhensch, ReshadeMotionEstimation, also known as DRME.
- "Dense Realtime Motion Estimation, based on Block Matching and Pyramids", developed 2019 to 2022. Licence is CC BY-NC 4.0. Last pushed October 2023. [JakobPCoder/ReshadeMotionEstimation LICENSE.md](https://github.com/JakobPCoder/ReshadeMotionEstimation/blob/HEAD/LICENSE.md)

Pascal Gilcher, qUINT_motionvectors.
- CC BY-NC 4.0. Based on Jakob Wapenhensch's shader and shares its `texMotionVectors` texture name so either can feed other shaders. [qUINT_motionvectors gist](https://gist.github.com/martymcmodding/69c775f844124ec2c71c37541801c053)
- Method read from the code: a feature texture of Rec. 601 luma plus linear depth in RG16F with 8 mips. A pyramid of RGBA16F motion textures from 1/128 resolution up to full. Blocks of 3x3 samples by default, configurable 2 to 7. Similarity is uncentered cosine similarity on luma and on depth, taking the minimum. Search is up to 2 iterations of 4 candidates along a rotating direction jittered by blue noise, step halving each iteration. Coarse levels are upscaled with a 3x3 filter weighted by depth difference, match quality, feature variance and motion magnitude. [qUINT_motionvectors gist](https://gist.github.com/martymcmodding/69c775f844124ec2c71c37541801c053)
- Output is RG16F at full resolution in UV units. The code samples the previous frame at current UV plus the vector, so it points from current to previous. The default "Half Resolution" option matches from level 1 up and upscales to level 0. [qUINT_motionvectors gist](https://gist.github.com/martymcmodding/69c775f844124ec2c71c37541801c053)

Pascal Gilcher, iMMERSE Launchpad.
- Launchpad "prepares several resources that other shaders require, such as normal vectors and optical flow". It has a "Flow Quality" setting of Low, Medium or High. [iMMERSE README](https://github.com/martymcmodding/iMMERSE), [MartysMods_LAUNCHPAD.fx](https://github.com/martymcmodding/iMMERSE/blob/HEAD/Shaders/MartysMods_LAUNCHPAD.fx)
- Licence text: "Public propagation of this project or parts of it is strictly forbidden." "Using parts of this projects source code as part of another, original project requires explicit permission from this projects author." Monetisation or commercial use of modified versions is prohibited without permission. The file header says "Unauthorized copying of this file, via any medium is strictly prohibited. Proprietary and confidential". [iMMERSE LICENSE](https://github.com/martymcmodding/iMMERSE/blob/HEAD/LICENSE)

Zenteon Motion.
- Declares `texMotionVectors` as full-resolution RG16F and a second full-resolution R8 texture `tDOC`. It keeps 8-level pyramids, 1/1 to 1/128, of a 16-bit single-channel texture and of RGBA8 current and previous color. [Zenteon_Motion.fx](https://github.com/Zenteon/ZenteonFX/blob/HEAD/Shaders/Zenteon_Motion.fx)
- Licence is the AGNYA License rev 1.3, "All rights reserved unless explicitly stated". [ZenteonFX license.md](https://github.com/Zenteon/ZenteonFX/blob/HEAD/license.md)

LumeniteFX: Kernel, LumaFlow, PocketMV.
- LumaFlow is "a motion estimation shader written for ReShade. It determines where each pixel in the current frame originated from in the previous frame", aimed at TAA, frame generation and temporal reprojection. Kernel computes "normals, motion vectors, motion confidence" once for other shaders. [LumeniteFX README](https://github.com/umar-afzaal/LumeniteFX)
- PocketMV, version 2026.10.17, is "Superfast motion vectors for low-end hardware". It writes flow as RG16F at 1/8 resolution plus an R16F confidence texture at 1/8 resolution, from R16F luma with 8 mips and a flow pyramid from 1/128 up. [lumenite_PocketMV.fx](https://github.com/umar-afzaal/LumeniteFX/blob/HEAD/Shaders/lumenite_PocketMV.fx)
- The author studied FidelityFX Optical Flow 1.1.2, Zenteon's August 2025 motion vectors and the Video++ library. He borrowed from Zenteon "the use of sparse samples for patch matching" and the idea "that we should provide a disocclusion score alongside each motion vector". His own additions are a continuous UV-space technique and "the specific use of previous frame flow and global motion direction", without which "its quality noticeably lacks". [LumeniteFX NOTICE](https://github.com/umar-afzaal/LumeniteFX/blob/HEAD/NOTICE)
- Licence is AGNYA rev 1.4. Reuse of parts in another project "requires explicit permission from this project's author", modified versions must stay source-available under the same licence, and monetisation is prohibited without permission. [LumeniteFX LICENSE.md](https://github.com/umar-afzaal/LumeniteFX/blob/HEAD/LICENSE.md)

vort_Motion.
- The repository is MIT, but `vort_MotionVectors.fxh` is CC BY-NC 4.0 because it derives from Jakob Wapenhensch's and Pascal Gilcher's shaders. It matches on color and depth together because "fg vs bg color are sometimes too similar and cause issues", uses a 9-tap diamond pattern and 8 mip levels, 9 at 2160p. Last pushed June 2025. [vort_MotionVectors.fxh](https://github.com/vortigern11/vort_Shaders/blob/HEAD/Shaders/Includes/vort_MotionVectors.fxh)
- The author recommends the REST add-on "in order to ignore the UI of the game". [vort_Shaders README](https://github.com/vortigern11/vort_Shaders)

CShade.
- BSD-3-Clause ReShade collection with Lucas-Kanade optical flow and motion blur shaders, active as of November 2026. [papadanku/CShade](https://github.com/papadanku/CShade)

### Inferences
- These shaders share one design: a mip pyramid of a small per-pixel feature, a few stochastic candidates per level seeded from the coarser level, an edge-aware upscale, and a quality value carried along as a filter weight. That costs a fixed, small number of texture samples per pixel per level, which is why they run inside ReShade next to the game. The algorithm is not protected, only the code is. A clean-room compute implementation with the same structure is legally safe and is what LumeniteFX's author did.
- Several of them use the depth buffer as a second matching feature. Unishade has only estimated depth, which flickers frame to frame. Matching on it would feed depth noise back into the motion used to stabilise depth. I would match on color or luma only.
- Sparse-sample matching with temporal and global-motion seeding should beat exhaustive 8x8 SAD on thin objects and sub-pixel motion, since these shaders output float vectors per pixel. It should lose on large motion in low-texture areas where a random candidate set misses.
- CC BY-NC code is incompatible with MIT distribution because MIT permits commercial use downstream. The iMMERSE and AGNYA licences need written permission from the author.

### Gaps
- None of the repositories I read states a cost in milliseconds. Community claims exist on Discord and forums but I did not find a citable measurement.
- I did not read the Launchpad or Zenteon matching code in detail, so their block sizes, descriptors and candidate counts are not recorded here. Launchpad's licence makes reading it for reimplementation a poor idea anyway.
- I found no side-by-side quality comparison between these shaders on game footage.

## 4. Classical algorithms on GPU

### Takeaway
Hierarchical block matching is what every shipping real-time system uses, because its cost is fixed and it maps to compute shaders. DIS is the fastest published dense classical method but its numbers are CPU numbers and I found no maintained GPU version. I found little that is citable on GPU cost for Lucas-Kanade, Farneback, PatchMatch or phase correlation on game content.

### Cited findings
- DIS, Dense Inverse Search, "runs at 300Hz up to 600Hz on a single CPU core" and is "competitive on standard optical flow benchmarks with large displacements". [Kroeger et al., arXiv 1603.03590](https://arxiv.org/abs/1603.03590)
- OpenCV ships DIS with ultrafast, fast and medium presets. Patch stride must be smaller than patch size and lower stride gives better flow. [OpenCV DISOpticalFlow docs](https://docs.opencv.org/4.x/javadoc/org/opencv/video/DISOpticalFlow.html)
- An unofficial DLSS pipeline uses CPU Farneback as its "fastest" motion option, with pyramid scale 0.5, 5 levels and window 21, and treats RAFT as the quality option. [ComfyUI-DLSS5 nodes.py](https://github.com/HECer/ComfyUI-DLSS5/blob/HEAD/nodes.py)
- Hierarchical block matching with SAD on a 7-level luma pyramid, median filtering and candidate-based upscaling is the method AMD ships in FSR 3. [FidelityFX Optical Flow manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/)
- A pyramidal Lucas-Kanade implementation in ReShade FX exists under BSD-3-Clause. [papadanku/CShade](https://github.com/papadanku/CShade)
- The census transform "can compensate for additive and multiplicative illumination changes as well as changes to gamma", which plain brightness constancy cannot. [UnFlow, arXiv 1711.07837](https://arxiv.org/abs/1711.07837)

### Inferences
- DIS is inverse-compositional Lucas-Kanade on a sparse patch grid, then densification, then an optional variational refinement. The patch stage ports to compute shaders. The variational stage is iterative and is the part to drop. With refinement off, DIS is close to what the ReShade shaders do with gradient descent in place of random candidates.
- Lucas-Kanade and Farneback assume small motion and smooth brightness. They fail on repeated textures and on large displacements unless the pyramid is deep, and they blur motion boundaries. Block matching with a median filter keeps boundaries sharper at block resolution. A sound hybrid is block matching for the integer part and one Lucas-Kanade step for the sub-pixel part.
- Census or gradient-based matching costs are worth the extra samples for games. Exposure adaptation, flashes, fog and bloom all break raw luma SAD.
- A global motion estimate is useful as a seed and as a fallback. Camera pans dominate game motion, and both NVOFA's global flow output and LumeniteFX's use of "global motion direction" point the same way. Phase correlation on a 256x256 luma thumbnail would give it, and so would the mode of the coarsest pyramid level's vectors, which is free.
- My estimate of cost for a compute-shader pyramid matcher: at 1080p with flow resolved at 1/4 or 1/8 resolution, under 1 ms on a mid-range GPU is realistic, in line with the FSR 3.1 frame generation totals of 0.7 to 2.1 ms that include more work than flow alone.

### Gaps
- No citable GPU timings for DIS, Farneback, pyramidal Lucas-Kanade or PatchMatch at 1080p on consumer GPUs. OpenCV's CUDA module has Farneback and Lucas-Kanade but I did not find current benchmarks, and OpenCV CUDA is not a D3D12 option anyway.
- No source on phase correlation used for game camera motion.
- No published evaluation of classical flow on game footage with HUDs. Benchmarks are Sintel and KITTI.

## 5. Neural flow at real-time rates

### Takeaway
No published flow network reaches 1 to 2 ms at 540p or above on a mid-range GPU that is also rendering a game. The fastest accurate models read about 9 to 15 ms at 1024x436 on an RTX 2080 in PyTorch. Neural flow makes sense here only at a small input, a quarter-megapixel or less, with a shader-based upsample, or as an offline reference to grade the shader method against.

### Cited findings
- NeuFlow v2, 9 iterations, about 9 M parameters: 15 ms on RTX 2080 and 106 ms on Jetson Orin Nano at Sintel resolution 1024x436, with Sintel train EPE 1.24 clean and 2.67 final after FlyingThings training. The abstract claims over 20 FPS at 512x384 on a Jetson Orin Nano. [NeuFlow v2, arXiv 2408.10161](https://arxiv.org/html/2408.10161v1)
- Same table, same hardware, RTX 2080 time at 1024x436: NeuFlow v1 9 ms, RAPIDFlow 38 ms, GMFlow one-iteration 46 ms, SEA-RAFT M 61 ms, SEA-RAFT L 96 ms, RAFT 32 iterations 125 ms, FlowFormer 1007 ms. NeuFlow v1 trades accuracy for speed, with Sintel final EPE 3.13 and KITTI EPE 12.4 against v2's 4.33. [NeuFlow v2, arXiv 2408.10161](https://arxiv.org/html/2408.10161v1)
- NeuFlow v2 runs a CNN backbone, global matching by cross-attention at 1/16 scale, then 8 recurrent refinements at 1/8 scale with 9x9 local correlation, then convex upsampling. [NeuFlow v2, arXiv 2408.10161](https://arxiv.org/html/2408.10161v1)
- NeuFlow v2 code is Apache-2.0, last pushed September 2024. [neufieldrobotics/NeuFlow_v2](https://github.com/neufieldrobotics/NeuFlow_v2)
- SEA-RAFT's smallest model runs 1080p at 21 fps on an RTX 3090, 3x faster than RAFT. At 540x960 on an RTX 3090, RAFT takes 140.7 ms and SEA-RAFT M about 71 ms. [SEA-RAFT, arXiv 2405.14793](https://arxiv.org/pdf/2405.14793)
- SEA-RAFT code is BSD-3-Clause. [princeton-vl/SEA-RAFT](https://github.com/princeton-vl/SEA-RAFT)
- FastFlowNet has 1.37 M parameters and 12.2 G FLOPs and takes 11 ms on a GTX 1080 Ti at Sintel resolution in PyTorch, against 34 ms for PWC-Net, 55 ms for LiteFlowNet, 50 ms for SPyNet and 116 ms for FlowNet2. Sintel clean test EPE is 4.89 against 4.39 for PWC-Net. With TensorRT FP16 it "can run at 220FPS with input size of 512x512"; the README line does not name the GPU. Licence is MIT. [ltkong218/FastFlowNet](https://github.com/ltkong218/FastFlowNet)
- RIFE "can run 30+FPS for 2X 720p interpolation on a 2080Ti GPU". That figure covers flow plus frame synthesis. Licence is MIT, and Practical-RIFE was still receiving updates in August 2026. [hzwer/ECCV2022-RIFE](https://github.com/hzwer/ECCV2022-RIFE), [hzwer/Practical-RIFE](https://github.com/hzwer/Practical-RIFE)
- Community TensorRT numbers for RIFE 4.6 at 1080p FP16, from a video-player forum and not independently verified: about 417 fps on an RTX 4090 and about 110 fps on an RTX 3070 Ti with one pipeline, and far lower figures with others. [SVP forum](https://www.svp-team.com/forum/viewtopic.php?pid=81538), [vs-mlrt releases](https://github.com/AmusementClub/vs-mlrt/releases/)
- WAFT, an ICLR 2026 oral, replaces cost volumes with warping alone and claims up to 4.1x speedup over comparable methods. BSD-3-Clause. [princeton-vl/WAFT](https://github.com/princeton-vl/WAFT)
- CompactFlowNet targets phones and claims real-time on an iPhone 8. [CompactFlowNet, arXiv 2412.13273](https://arxiv.org/abs/2412.13273v1)
- Unimatch, MIT, is one model family for flow, stereo and depth from the GMFlow line. [autonomousvision/unimatch](https://github.com/autonomousvision/unimatch)
- ptlflow, Apache-2.0, collects PyTorch implementations and weights for many flow models, including DPFlow and RAPIDFlow, and was active in October 2026. [hmorimitsu/ptlflow](https://github.com/hmorimitsu/ptlflow)
- torchvision's RAFT Large and RAFT Small are what the unofficial DLSS 5 ComfyUI pack uses for motion. Its documentation warns that "fast motion, large parallax, and heavy occlusion can degrade it" and that ghosting in fast pans is probably the flow's fault. [DLSS 5 RAFT Motion Guide node](https://comfy.icu/node/DLSS5RAFTFlow)

### Inferences
- My estimate, scaling by pixel count. NeuFlow v2 at 960x540 on an RTX 2080 is about 17 ms and at 1080p about 70 ms in PyTorch. TensorRT FP16 on an RTX 4060-class card might cut that three to five times, which still leaves 4 to 6 ms at 540p with the GPU to itself. FastFlowNet at 512x512 is 4.5 ms by its own TensorRT figure. Under contention with a game, expect worse. None of this reaches 1 to 2 ms.
- All of these networks compute flow at 1/8 scale internally and upsample. A 540p input gives a 120x68 native field, coarser than FidelityFX's 8x8 blocks at 1080p. The network's gain is in matching quality through occlusion and low texture, and it gives no extra spatial detail.
- Weights trained on FlyingChairs, FlyingThings, Sintel and KITTI have never seen a HUD, a crosshair, or screen-space UI that stays still while the scene moves. I would expect them to smooth UI motion into the background along edges. I found no evaluation of this.
- Code licences above are permissive, but weights trained on Sintel, KITTI or HD1K inherit those datasets' research-use terms. Check each checkpoint before shipping it. FlyingThings-only checkpoints are the safer choice.
- Running a second network beside Depth Anything V2 Small doubles the scheduling problem on the game's GPU. A shader matcher has a fixed cost and no runtime dependency, which matters more than a point of EPE here.
- RIFE's IFNet estimates flow to the intermediate frame, from the midpoint to each input. It is not a current-to-previous field, so it does not drop into reprojection or DLSS without retraining or conversion.

### Gaps
- No source gives measured latency for any flow network on a mid-range consumer GPU, RTX 3060 or 4060 class, at 540p or 1080p with TensorRT or DirectML. Every number above is from a paper's hardware or a forum.
- I did not find speed tables for WAFT's small variants, DPFlow or FlowSeek, nor any 2026 paper that claims single-digit milliseconds at 1080p.
- No ONNX or TensorRT export is published by the NeuFlow v2 or SEA-RAFT authors in the READMEs I read. FastFlowNet has a TensorRT path that needs a custom correlation plugin.
- No joint depth plus flow network at real-time rates turned up. Unimatch shares an architecture across tasks but runs them as separate models.
- No published test of any flow network on captured game footage with HUD.

## 6. Motion vector conventions for DLSS and upscalers, and how engine-free frame generation copes

### Takeaway
DLSS wants RG16F or RG32F vectors in pixels at render resolution that point from the current pixel to where it was in the previous frame, with scale parameters to convert anything else. Low-resolution vectors are the preferred input and DLSS dilates them itself. NVIDIA has published no interface for the DLSS 5 neural rendering feature as of October 2026, so the DLSSNR input contract is known only through an unofficial wrapper. Driver-level frame generation does not solve HUD or occlusion without engine data. It accepts HUD artifacts and switches itself off in fast motion.

### Cited findings

DLSS Super Resolution conventions, programming guide revision 310.6.0 dated 31 March 2026.
- "The motion vectors map a pixel from the current frame to its position in the previous frame. That is, when the motion vector for the pixel is added to the pixel's current location, the result is the location the pixel occupied in the previous frame." [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)
- "Motion vectors must be sent as floats with a render target of format RG32_FLOAT or RG16_FLOAT." Values are "the number of pixels calculated in screen space (ie the amount a pixel has moved at the render resolution)", with [0,0] at the upper left. Fractional values are valid. [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)
- Worked example: at 1080p, for an object that moved from the left edge to the right edge, "the x value of the motion vector for that pixel on the right edge will be -1080.0". "Using integer values will not take sub-pixel values into account." [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)
- `NVSDK_NGX_DLSS_Feature_Flags_MVLowRes`: vectors are normally at render resolution. Display-resolution vectors are accepted with the flag cleared, but this "is not the preferred option" because "the DLSS dll will typically apply its own optimized and targeted dilation". "If standard input resolution motion vectors are sent they do not need to be dilated, DLSS dilates them internally. If display resolution motion vectors are sent, they must be dilated." [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)
- `InMVScaleX` and `InMVScaleY` exist for vectors that point "in the direction of motion rather than towards the previous frame" or that are "in UV space rather than pixel space". Set them to 1.0 if unused and never to 0. [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)
- "Incorrect or poor precision motion vectors are the most common cause of visual artifacts when DLSS is enabled." Problem content the guide lists includes small particles, scrolling textures, thin objects, objects with missing vectors, and "disoccluding objects with motion vectors that have very large values". [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)
- Depth can be "any format with one channel in it (for instance R32_FLOAT or D32_FLOAT)". DLSS uses depth "to assist in better object tracking and pixel alignment". [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)
- Streamline normalises instead. `mvecScale` must bring values into [-1,1], so pixel-space vectors use `{1/renderWidth, 1/renderHeight}`. Constants include `motionVectorsDilated`, `motionVectorsJittered`, `cameraMotionIncluded`, `motionVectors3D`, `depthInverted` and `motionVectorsInvalidValue`. The low-resolution flag is set automatically from the tagged buffer's size. [Streamline ProgrammingGuideDLSS.md](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md), [Streamline ProgrammingGuide.md](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md)
- Intel's XeSS-FG follows the same direction: vectors "must be from current frame to previous", `R16G16_FLOAT` or similar, NDC or pixel space, low resolution preferred, with a depth buffer the same size as the motion buffer. [XeSS-FG developer guide](https://raw.githubusercontent.com/intel/xess/main/doc/xess_fg_developer_guide_english.md)

DLSS 5 neural rendering.
- NVIDIA Research's page, published 1 September 2026, says the model is conditioned on the current rendered frame, engine motion vectors, carried temporal state and artistic-direction values, runs on GeForce RTX 50 Series at up to 4K under a strict per-frame budget, and is "the first productized generative rendering model to operate in real time". Depth does not appear in that list of inference inputs. [NVIDIA Research DLSS 5](https://research.nvidia.com/labs/adlr/DLSS5/)
- NVIDIA says integration goes through Streamline and an Unreal Engine 5 plugin, and that DLSS 5 is an independent feature that can run beside Super Resolution, Multi Frame Generation and Ray Reconstruction. [NVIDIA developer blog on DLSS 5](https://developer.nvidia.com/blog/whats-new-for-game-developers-dlss-5-with-3d-guided-neural-rendering-nvidia-ace-updates-and-new-rtx-kit-capabilities)
- Streamline's changelog adds "support for DLSS 3D-Guided Neural Rendering with a new plugin: `sl.dlss_nr`". [Streamline changelog.txt](https://github.com/NVIDIA-RTX/Streamline/blob/main/changelog.txt)
- A developer who inspected Streamline 2.14.1 reported on 24 September 2026 that it declares `kFeatureDLSS_NR = 1004` but ships no neural rendering plugin, no feature header and no integration guide. The thread shows no NVIDIA reply. [NVIDIA developer forum thread](https://forums.developer.nvidia.com/t/dlss-5-neural-rendering-sdk-access-for-a-custom-windows-engine/384226)
- The public DLSS SDK repository, last pushed 16 October 2026, ships `nvngx_dlss.dll`, `nvngx_dlssd.dll` and `nvngx_dlssg.dll` and headers for those three. It has no `nvngx_dlssnr.dll` and no neural rendering header. [NVIDIA/DLSS](https://github.com/NVIDIA/DLSS)
- Prior art for exactly this project's plan exists. ComfyUI-DLSS5, GPL-3.0 and unaffiliated with NVIDIA, runs `nvngx_dlssnr.dll` on ordinary video through a VapourSynth wrapper over D3D12, with "depth and motion estimated from pixels". Its bridge passes `depth`, `mvec`, `depth_inverted`, `mvec_scale_x` and `mvec_scale_y`, and requires depth and motion together. [HECer/ComfyUI-DLSS5](https://github.com/HECer/ComfyUI-DLSS5), [bridge_runner.py](https://github.com/HECer/ComfyUI-DLSS5/blob/HEAD/bridge_runner.py)
- That pack's motion is current-to-previous in pixels. It stores flow normalised as `0.5 + flow / (2 * size)` and decodes with `(mv - 0.5) * 2 * size` before handing it to the runtime. Its Farneback path calls `calcOpticalFlowFarneback(current, previous)` with the comment "Match the current-to-previous convention used by RAFT and reprojection". [nodes.py](https://github.com/HECer/ComfyUI-DLSS5/blob/HEAD/nodes.py)
- Its recommended video graph is RAFT flow, a temporally consistent depth source, and a "Temporal Depth Stabilizer" that uses the same motion vectors "to warp previous depth into the current frame". [DLSS 5 RAFT Motion Guide node](https://comfy.icu/node/DLSS5RAFTFlow)
- Its runtime notes say "The bridge requests the VapourKit wrapper's caller-check compatibility option; it does not modify the proprietary DLL. Confirm that the applicable license permits redistribution and this mode of use." It also states that engine data such as geometry, material buffers, exposure, jitter and engine-authored motion "cannot be recovered" from video, that its examples are single runs, and that measured 4K throughput is below 30 source frames per second. [RUNTIME_SOURCES.md](https://github.com/HECer/ComfyUI-DLSS5/blob/HEAD/docs/RUNTIME_SOURCES.md), [HECer/ComfyUI-DLSS5](https://github.com/HECer/ComfyUI-DLSS5)

Frame generation without engine data.
- AFMF is driver-level. It takes two rendered frames and estimates one between them. Because it works on the whole presented image, "things like a crosshair will sputter and ghost as you flick around your camera", where in-engine FSR 3 or DLSS 3 can keep the HUD out of interpolation. [PC Gamer on AFMF](https://www.pcgamer.com/amd-fluid-motion-frames-finally-goes-official-driver-based-frame-generation-for-any-dx1112-game)
- The first AFMF turned itself off during fast motion, which made frame rate swing. AFMF 2 added a smoother fallback that "temporarily disables AFMF 2 in high-motion scenes to preserve image quality". [Digital Trends AFMF 2 test](https://www.digitaltrends.com/computing/amd-afmf-2-tested/)
- AFMF 3 strings appear in AMD driver branches. AMD has announced no details, and press guesses that it moves to an ML model are unconfirmed. [Tom's Hardware on AFMF 3](https://www.tomshardware.com/pc-components/gpu-drivers/amd-fluid-motion-frames-3-spotted-in-the-upcoming-amd-adrenalin-25-20-driver-branch-could-lean-on-ai-model-used-in-fsr-4)
- NVIDIA Smooth Motion is "a driver-based AI model" that inserts one frame between two rendered frames, on RTX 40 Series and newer. On Linux it is a Vulkan layer enabled with `NVPRESENT_ENABLE_SMOOTH_MOTION=1`. [NVIDIA Linux driver README, Smooth Motion](https://download.nvidia.com/XFree86/Linux-x86_64/595.84/README/nvpresent.html)
- On Windows it applies to DX11 and DX12 games and needs no game data such as motion vectors. [TechSpot on Smooth Motion](https://www.techspot.com/news/106625-nvidia-answer-amd-fluid-motion-frames-works-all.html)
- Press reports that Smooth Motion interpolates uniformly, so interface elements and subtitles can distort in generated frames. [PCGamesN on Smooth Motion](https://www.pcgamesn.com/nvidia/frame-gen-smooth-motion)
- FSR 3 with engine data takes a HUD-less backbuffer as the interpolation source when the game provides one and cleans up UI during inpainting. It builds two disocclusion masks, interpolated against previous and interpolated against current, to decide which reprojection direction to discard. [FidelityFX frame interpolation manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/)
- Optical flow "captures effects like shadows, reflections, and particles that the engine does not track as objects". [Intel XeSS 2 whitepaper](https://intel.com/content/www/us/en/developer/articles/technical/xess2-whitepaper.html)

### Inferences
- For DLSS, hand over RG16F pixel-space vectors at the color input's resolution, current to previous, scale 1.0, low-res flag set, not dilated. If the flow field is at 1/4 or 1/8 resolution, upsample it to the color resolution in a shader first. The guide documents render-resolution and display-resolution vectors only, and nothing I read says DLSS accepts a motion buffer smaller than the color input.
- Flow estimators that return vectors in the direction of motion, such as NVOFA forward flow from previous to current, need both a sign flip and a resample, because the vector must sit at the current pixel. Estimating directly with the current frame as the anchor avoids the resample. With NVOFA, pass the current frame as input and the previous frame as reference.
- Estimated vectors differ from engine vectors in ways DLSS was not trained for. They are apparent motion, so they follow shadows, reflections, specular highlights and scrolling textures where engine vectors follow the surface. They are wrong or undefined in disoccluded regions where engine vectors are still geometrically valid. They have no jitter relationship to the camera. They treat a static HUD over a moving scene as a motion boundary, with bleeding at its edges. They lose thin objects and particles below the block size. DLSS documents the last case and large-vector disocclusion as known artifact sources even with engine data.
- Captured frames already have the game's anti-aliasing resolved and carry no sub-pixel jitter, so the DLSS jitter inputs are zero and the jittered-vector flag stays off. A Super Resolution pass on such input has nothing to accumulate. That is a limit of the whole capture approach and not of the motion estimation.
- Every shipping engine-free frame generator handles failure by detecting it and backing off. That means scene change detection, a high-motion fallback and blending by color match. None of them reconstructs the HUD. Unishade should do the same: zero vectors plus a low-confidence flag on scene cuts and on blocks with poor match cost, and treat low confidence as "do not reuse history".
- A static-HUD mask is cheap to estimate from color alone. Pixels whose color stays constant across many frames while their neighbourhood's flow is non-zero are almost certainly UI. Force their vectors to zero. This is my suggestion and I found no source describing AFMF or Smooth Motion doing it.
- Loading `nvngx_dlssnr.dll` from a non-game overlay has two risks that the unofficial wrapper's notes point at. The DLL appears to check its caller, and NVIDIA has published no licence terms that cover this use or redistribution. An MIT project should not bundle the DLL.

### Gaps
- NVIDIA has published no programming guide, header or parameter list for DLSS 5 neural rendering that I could find. The claim that DLSSNR takes R16G16_FLOAT pixel-space vectors plus depth rests on the unofficial wrapper's argument names and on the project brief, and NVIDIA's own research page lists motion vectors without depth.
- I found no statement of whether DLSSNR accepts low-resolution or dilated vectors, how sensitive it is to wrong vectors, or what it does on disocclusion.
- No technical description from NVIDIA of how Smooth Motion estimates motion, and none from AMD for AFMF 2.1's search. Press coverage describes behaviour only.
- No measurement anywhere of DLSS quality with estimated vectors against engine vectors. The ComfyUI pack explicitly declines to claim one.

## 7. Confidence and occlusion estimation

### Takeaway
Three cheap signals cover most bad vectors: the match cost of the winning candidate, a forward-backward consistency check, and a per-frame scene change flag. The ReShade shaders and NVOFA already output a confidence or disocclusion channel beside the vectors, and that is the interface to copy.

### Cited findings
- Forward-backward check. A pixel is occluded when `|wf(x) + wb(x + wf(x))|^2 < a1 (|wf(x)|^2 + |wb(x + wf(x))|^2) + a2` is violated, with a1 = 0.01 and a2 = 0.5 in pixels. "Our occlusion detection is based on the forward-backward consistency assumption [Sundaram et al., 2010]." [UnFlow, arXiv 1711.07837](https://arxiv.org/abs/1711.07837)
- NVOFA returns forward and backward flow in one call and a per-vector cost where higher means less accurate, on Ampere and newer. [NVOFA programming guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html), [NVOFA application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note)
- FidelityFX optical flow removes outliers with a 3x3 vector median, picks upscaled vectors by block SAD, and returns zero motion when its histogram-based scene change detector fires. [FidelityFX Optical Flow manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/optical-flow/)
- FSR frame interpolation gives optical flow vectors a priority from vector magnitude and from color similarity of the reprojected colors, and prefers whichever motion source matches the real frames better. [FidelityFX frame interpolation manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/frame-interpolation/)
- qUINT_motionvectors carries match quality, cosine similarity raised to the fourth power, in the alpha channel of its pyramid and uses it with depth difference, feature variance and motion magnitude as upscale filter weights. [qUINT_motionvectors gist](https://gist.github.com/martymcmodding/69c775f844124ec2c71c37541801c053)
- Zenteon Motion writes a full-resolution R8 `tDOC` texture beside its vectors. PocketMV writes an R16F confidence texture at 1/8 resolution. LumeniteFX's author credits Zenteon with the idea "that we should provide a disocclusion score alongside each motion vector". [Zenteon_Motion.fx](https://github.com/Zenteon/ZenteonFX/blob/HEAD/Shaders/Zenteon_Motion.fx), [lumenite_PocketMV.fx](https://github.com/umar-afzaal/LumeniteFX/blob/HEAD/Shaders/lumenite_PocketMV.fx), [LumeniteFX NOTICE](https://github.com/umar-afzaal/LumeniteFX/blob/HEAD/NOTICE)
- DLSS itself names "disoccluding objects with motion vectors that have very large values" as an artifact source. [DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)

### Inferences
- A full backward flow pass doubles the cost. The cheap approximation is a reprojection check in the consumer. Warp the previous color with the vector and compare it with the current color in a small neighbourhood. For depth, also compare warped previous depth against current estimated depth with a relative tolerance. That costs a few samples per pixel in the resolve pass that already exists.
- Match cost alone is a weak confidence signal in flat regions such as sky or fog, where every candidate scores well. Pair it with a texture measure, the local luma variance or gradient magnitude, and mark low-texture blocks as "vector unreliable but harmless". qUINT's use of feature variance as a weight is the same idea.
- The forward-backward threshold above is in pixels at the flow's working resolution. At 1/8 resolution, scale a2 down, or the half-pixel tolerance becomes four display pixels.
- For depth stabilisation, the useful output is one scalar per pixel in [0,1] that sets the history blend weight. Combine min of reprojected-color agreement, depth agreement and a scene change gate. Fresh depth replaces history where it is 0. This also gives the right behaviour when the depth network runs every second or third frame. Reuse warped depth where confidence is high and hold the last network output unwarped where it is low.
- For DLSS, a confidence channel has no input slot in the documented Super Resolution interface. The only levers are zeroing or damping low-confidence vectors and the reset flag on scene change.
- Apparent-motion vectors on shadows and reflections pass the color reprojection check while being wrong for depth, since the surface under a moving shadow did not move. The depth agreement test catches part of this. A median or a dominant-motion prior over each depth segment would catch more, at higher cost.

### Gaps
- No source measures how much forward-backward checking improves on cost-based confidence for game content, or what either costs in a compute shader.
- I did not read the Zenteon or LumeniteFX code that fills their disocclusion and confidence textures, so the formulas are unknown to me.
- No source describes confidence handling inside Smooth Motion, AFMF or LSFG.

#### Cross-method comparison, synthesis of all sections

Costs marked "read" come from the linked sources in the sections above. Costs marked "est." are mine.

| Method | Output grid and precision | Cost at 1080p | Main limits | Licence | Platforms | D3D12 integration effort |
|---|---|---|---|---|---|---|
| NVOFA, NVIDIA hardware | 4x4, 2x2 or 1x1 px, 1/32 px steps, cost and backward flow optional | read 613 fps on RTX 3090 and 1296 fps on RTX 4090 at 4x4 FAST, so est. 1.6 and 0.8 ms, on a dedicated engine | NVIDIA only, Turing gets 4x4 only, docs frozen at 2023, RTX 50 status unknown | DesignWorks SDK licence, conflicts with open-source use. Vulkan extension needs no SDK | Windows D3D11, D3D12, Vulkan. Linux Vulkan, CUDA | Medium. Native D3D12 API with fences, but SDK headers are the licence problem |
| D3D12 video motion estimation | 8x8 or 16x16 blocks, quarter-pel, R16G16_SINT | unknown, spec target under 5 ms for two eyes | NV12 only, encoder-style search, support unclear per vendor | Windows API, no licence issue | Windows 10 1903+ | Medium to high. Video encode queue, NV12 conversion, capability checks |
| FidelityFX Optical Flow 1.1.2 | 8x8 blocks at input resolution, integer pixels, R16G16_SINT, scene change flag | read 0.7 to 2.1 ms for the whole FSR 3.1 FG step, RX 7900 XTX to RX 5700 XT. Flow alone est. 0.5 to 1 ms mid-range | coarse, no sub-pixel, 512 px max motion, luma SAD breaks on exposure changes | MIT for these files, both in SDK 1.1.4 and in the 2.4.0 exemption list | D3D12 and Vulkan, SM 6.2 | Low. Ships as a D3D12 compute component |
| Own pyramid matcher in the ReShade style | per pixel or 1/4 to 1/8 res, float, with confidence | est. under 1 ms at 1/4 to 1/8 flow resolution | must be written and tuned. Random candidates can miss large motion in flat areas | own code. Existing shaders are CC BY-NC, AGNYA or no-redistribution and cannot be copied | any compute API | Medium. New code, but plain compute shaders that port to Metal and Vulkan |
| CShade Lucas-Kanade | per pixel, float | unknown | small-motion assumption, blurred boundaries | BSD-3-Clause | ReShade FX source, portable | Medium. Port from ReShade FX |
| DIS without refinement | patch grid, float | read 300 to 600 Hz on one CPU core. GPU cost unknown | no maintained GPU version found | OpenCV implementation, licence not checked here | CPU | High. Needs a GPU port |
| FastFlowNet | 1/4 scale internal, float | read 11 ms on GTX 1080 Ti at 1024x436 in PyTorch, 220 FPS at 512x512 with TensorRT FP16 | older accuracy level, custom correlation layer for export | MIT | any ML runtime with the correlation op | High. Second network beside depth |
| NeuFlow v2 | 1/8 scale internal with convex upsampling, float | read 15 ms on RTX 2080 at 1024x436 in PyTorch. est. about 70 ms at 1080p | too slow at useful resolutions, no published export | Apache-2.0 | PyTorch | High |
| SEA-RAFT small | 1/8 scale internal, float | read 21 fps on RTX 3090, about 48 ms | far over budget | BSD-3-Clause | PyTorch | High |
| RIFE IFNet | midpoint flow, float | read 30+ FPS for 720p 2x on RTX 2080 Ti including synthesis | wrong flow definition for reprojection | MIT | PyTorch, ncnn, TensorRT | High |
| LSFG, Smooth Motion, AFMF, XeSS-FG flow | internal only | not exposed | closed, no motion output | proprietary | vendor or app specific | Not available |
