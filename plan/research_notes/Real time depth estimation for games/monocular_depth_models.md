# Monocular depth models for real-time game depth (state as of November 2026)

Conventions used in these notes. "Source number" means I read it in the cited page. "My estimate" or "my arithmetic" means I derived it and no source states it. AbsRel is lower-better, delta1 and boundary F1 are higher-better. Latencies are per single frame unless stated.

## 1. What are the leading models and their small/fast variants as of late 2026?

### Takeaway
The commercially usable real-time tier is still thin. Depth Anything V2 Small (Apache-2.0, 24.8M) remains the only widely exported ViT-S foundation model with a permissive licence, joined in 2025-2026 by DA3-Small/Base (Apache-2.0, multi-view models that also run on one image), MoGe-2 ViT-S (35M, adds normals and metric points), DepthART (6M, July 2026) and Ultralytics YOLO26-depth (6.3M, CNN). The best new small model on paper, Apple Depth Pro 2-S (24M, November 2026), ships under a research-only licence.

### Cited findings

Model catalogue. Every row is a source number unless marked.

| Model | Date | Params | Output | Licence of weights | Exports |
|---|---|---|---|---|---|
| Depth Anything V2 Small / Base / Large | Jun 2024 | 24.8M / 97.5M / 335.3M | relative inverse depth (disparity) | Small Apache-2.0. Base, Large, Giant CC-BY-NC-4.0 | community ONNX (fabio-sim), TensorRT (spacewalk01, zhujiajian98), Apple Core ML for Small |
| DA3-Small / DA3-Base | Nov 2025 | 0.08B / 0.12B in README, 0.03B / 0.11B in paper Table 2 | relative depth, pose, any number of views | Apache 2.0 | community ONNX (devin-lai), TensorRT ROS2 nodes |
| DA3-Large / Giant / Nested | Nov 2025, 1.1 refresh Dec 2025 | 0.35B / 1.15B / 1.40B | depth, pose, Gaussians, metric in Nested | CC BY-NC 4.0 | community |
| DA3Mono-Large | Nov 2025 | 0.35B | relative depth plus sky mask, single image | Apache 2.0 | community ONNX on Hugging Face (Jens-Duttke) |
| DA3Metric-Large | Nov 2025 | 0.35B | metric depth plus sky mask | Apache 2.0 | TensorRT ROS2 node (ika-rwth-aachen) |
| Depth Anything AC | Jul 2025 | ViT-S only, 94.6 MB checkpoint | relative, fine-tune of DAv2-S for dark, fog, snow, blur | CC BY-NC 4.0, commercial use needs permission | none found |
| Distill-Any-Depth Small / Base / Large | Feb-Mar 2025 | 24.8M / 97.5M / 335.3M, same architectures as DAv2 | relative affine-invariant depth | code MIT, weights licence not stated in README | none official |
| Depth Pro | Oct 2024 | 504M | metric depth plus focal length at 1536x1536 | Apple licence, tagged apple-amlr on one HF card and Apple-ASCL on another | none official |
| Depth Pro 2 S / B / L / H+ | 19 Nov 2026 | 24.3M / 96.5M / 334.0M / 872.1M, plus 23.5M focal estimator | metric depth at native resolution, image and video, causal streaming mode | Apple Machine Learning Research Model licence, research use only | safetensors only |
| MoGe-1 ViT-L | Oct 2024 | 314M | affine-invariant point map, depth, FOV | code MIT, weights licence not stated in README | docs/onnx.md in repo |
| MoGe-2 ViT-S / B / L normal | Jul 2025 | 35M / 104M / 331M | metric point map, depth, normals, FOV, mask | same as above | official ONNX doc, community ONNX on HF (Heliosoph) |
| MoGe-3 ViT-L / ViT-G | Jul-Aug 2026 | 370M / 1.25B | as MoGe-2 with a 3-step sparse volumetric refiner | not checked | not checked |
| UniDepth V2 S / B / L | 2025 | L is 354M | metric depth, camera | CC BY-NC 4.0 | TensorRT works per Orin benchmark |
| Metric3D v2 S / L | 2024 | L is 412M | metric depth, normals | code BSD-2-Clause, HF card says non-commercial use, commercial users asked to contact authors | official ONNX export script for ViT-small |
| Marigold v1, Marigold V2 | 2023, Sep 2026 | SD2 UNet. V2 is a Qwen-Image-Edit DiT with 4-bit weights | affine-invariant depth, normals | not confirmed here | diffusers |
| Lotus, Lotus-2, GenPercept, DepthFM | 2024-2025 | SD-scale, about 0.9B | relative depth, some normals | not confirmed here | none relevant |
| DepthART S / B / L | Jul 2026, ACM MM 2026 | 6.0M / 11.4M / 32.6M | relative, metric after fine-tune | not stated | weights and ONNX not confirmed |
| YOLO26-depth n / s / m / l / x | 2026 | 6.3M / 13.2M / 23.3M / 27.7M / 57.0M | metric depth, CNN | not stated on the docs page | ONNX, TensorRT, CoreML |
| AnyDepth S / B / L | Jan 2026 | 26.5M / 95.5M / 313.4M, DINOv3 ViT/16 plus SDT decoder | relative | paper is CC BY-NC-SA 4.0, weights unknown | none found |
| MiDaS v2.1 small, DPT-Hybrid, v3.1 Swin2-T, LeViT | 2020-2023, old | 21M / 123M / 42M / 51M | relative inverse depth | MIT | ONNX, OpenVINO |
| Video Depth Anything Small | Jan 2025 | DAv2-S based | temporally consistent relative depth | Small Apache-2.0, Base and Large CC-BY-NC | community ONNX |

- DAv2 parameter counts, licences per size, default input size 518 and the list of community ONNX, TensorRT and Core ML ports. [Depth Anything V2 README](https://raw.githubusercontent.com/DepthAnything/Depth-Anything-V2/main/README.md)
- DA3 model zoo, per-model licences, deprecation of the original Large/Giant/Nested checkpoints in favour of 1.1, and no news entries dated 2026. [Depth Anything 3 README](https://raw.githubusercontent.com/ByteDance-Seed/Depth-Anything-3/main/README.md)
- DA3 paper Table 2 gives Small 0.03B, Base 0.11B, Large 0.36B, Giant 1.10B, and Table 8 splits Small into 0.022B backbone, 0.003B DualDPT, 0.001B camera head. This conflicts with the README's 0.08B for DA3-SMALL. [DA3 paper](https://arxiv.org/html/2511.10647v1); contradicted by [DA3 README](https://raw.githubusercontent.com/ByteDance-Seed/Depth-Anything-3/main/README.md)
- DA3 uses a plain DINOv2 ViT with a DPT-style head, base training resolution 504x504. Its teacher is a synthetic-only monocular model that predicts scale-shift-invariant depth in exponential form, not disparity. [DA3 paper](https://arxiv.org/html/2511.10647v1)
- Community DA3 exports exist. devin-lai's repo exports DA3 Small, Base and Large to ONNX and lists CPU, CUDA, TensorRT, DirectML and CoreML as targets. [Depth-Anything-3-Onnx](https://github.com/devin-lai/Depth-Anything-3-Onnx). A mono-large ONNX is on Hugging Face. [Jens-Duttke/Depth-Anything-3-MONO-ONNX](https://huggingface.co/Jens-Duttke/Depth-Anything-3-MONO-ONNX)
- Depth Anything AC is a ViT-S fine-tune of DAv2-Small on 540K unlabeled images, licensed CC BY-NC 4.0 with commercial use by permission only. DA-2K accuracy 0.953 vs 0.952 for DAv2-S on clean images, 0.923 vs 0.910 dark, 0.929 vs 0.922 fog, 0.892 vs 0.880 snow, 0.880 vs 0.862 blur. [DepthAnything-AC model card mirror](https://b-61j.pages.dev/ghost233lism/DepthAnything-AC), [paper](https://arxiv.org/abs/2507.01634v1)
- Distill-Any-Depth releases Small, Base and Large students with DAv2 architectures. Code is MIT. The README states no licence for weights. [Distill-Any-Depth GitHub](https://github.com/Westlake-AGI-Lab/Distill-Any-Depth)
- Depth Pro 2 has four sizes, all DINOv3 backbones with 16x16 patches and a DPT decoder. S is 21.6M backbone plus 2.7M decoder. Weights are research-only. [Depth Pro 2 paper](https://arxiv.org/html/2611.26603v1), [apple/DepthPro2](https://huggingface.co/apple/DepthPro2), [apple/DepthPro2-H](https://huggingface.co/apple/DepthPro2-H)
- Original Depth Pro licence tags disagree across Hugging Face cards, apple-amlr on DepthPro-hf and Apple-ASCL elsewhere. A third-party MLX port describes the v1 weights as under a non-OSI Apple licence that does not address commercial use. [apple/DepthPro-hf](https://huggingface.co/apple/DepthPro-hf), [depthpro-mlx](https://github.com/ntoantoan/depthpro-mlx)
- MoGe model list, parameter counts, MIT code licence, 60 ms per image for ViT-L FP16 on A100 or RTX 3090, ONNX doc, and the 2026-08-18 MoGe-3 news entry. [MoGe README](https://raw.githubusercontent.com/microsoft/MoGe/main/README.md)
- MoGe-3 comes as ViT-L 370M and ViT-G 1.25B. Other README copies date the announcement 2026-07-21. [MoGe-3 paper](https://arxiv.org/html/2607.17967v2), [microsoft/MoGe](https://github.com/microsoft/moge)
- UniDepth is CC BY-NC 4.0. [UniDepth GitHub](https://github.com/lpiccinelli-eth/UniDepth)
- Metric3D v2 code is BSD-2-Clause, the HF card frames it as non-commercial, and the official repo has an ONNX export script for metric3d_vit_small. [Metric3D HF card](https://huggingface.co/zachL1/Metric3D/raw/main/README.md), [Metric3D GitHub](https://github.com/YvanYin/Metric3D)
- DepthART uses a TinyViM encoder with a DPT decoder, distilled from Depth Anything V2 Large on a 1.7M-image subset. [DepthART paper](https://arxiv.org/html/2607.17099v1)
- Ultralytics ships YOLO26n/s/m/l/x-depth weights, trained at 768 px on about 2.19M images. [Ultralytics depth docs](https://raw.githubusercontent.com/ultralytics/ultralytics/main/docs/en/tasks/depth.md), [perf table](https://raw.githubusercontent.com/ultralytics/ultralytics/main/docs/macros/yolo-depth-perf.md)
- AnyDepth pairs a frozen DINOv3 encoder with a "Simple Depth Transformer" decoder that has 5.51M parameters on ViT-S vs 50.83M for the DPT it compares against. [AnyDepth paper](https://arxiv.org/html/2601.02760)
- MiDaS table with parameters and RTX 3090 FPS, MIT licence. [MiDaS README](https://raw.githubusercontent.com/isl-org/MiDaS/master/README.md)
- Prompt Depth Anything needs a low-resolution LiDAR prompt to produce its 4K metric depth. [Depth Anything V2 README](https://raw.githubusercontent.com/DepthAnything/Depth-Anything-V2/main/README.md)
- Video Depth Anything Small is Apache-2.0, Base and Large are CC-BY-NC. [Video-Depth-Anything-Small](https://huggingface.co/depth-anything/Video-Depth-Anything-Small), [third-party ONNX card](https://huggingface.co/TheoreticallyTim/theoreticallypose-vda)
- A September 2026 survey groups current models as metric (UniDepth, UniDepthV2, FoundationGeo, Depth Pro, Metric3D v1/v2, MoGe-2), relative (MoGe, Marigold, GeoWizard, GenPercept, Diffusion-E2E-FT, DA3, VGGT) and disparity (MiDaS 3.1, Depth Anything v1/v2, DPT, Lotus). [Survey 2609.01172](https://arxiv.org/html/2609.01172)
- Other 2026 items found. AsyncMDE runs a 3.83M-parameter fast path at 237 FPS on an RTX 4090 while a frozen 97.5M-parameter foundation model refreshes cached features in the background, recovering 77% of the accuracy gap. [AsyncMDE](https://arxiv.org/abs/2603.10438). XiDepth and RTS-Mono are self-supervised driving models for Raspberry Pi and Jetson. [XiDepth](https://arxiv.org/abs/2608.03666), [RTS-Mono](https://arxiv.org/abs/2511.14107)

### Inferences
- PromptDA does not apply to Unishade. It needs a LiDAR depth prompt and a captured game frame has none.
- No official DA3 monocular Small or Base exists. The Apache-2.0 DA3 small models are the multi-view any-view models run on one image. Whether DA3-Small on one frame beats DAv2-Small is not shown in any table I found.
- Distill-Any-Depth Base and Large reuse the DAv2 Base and Large architectures, whose original weights are CC-BY-NC. The README does not say whether the students were initialized from those weights, so I would treat only the Small student as plausibly clean and still confirm with the authors.
- Depth Pro 2 matters as a design reference, since a 24M DINOv3 ViT-S/16 model with a 2.7M DPT head now handles native-resolution sharp depth plus causal streaming. Unishade cannot distribute it under MIT-style terms.

### Gaps
- I did not confirm licences for Marigold, Lotus, Lotus-2, GenPercept, DepthFM, UniK3D, MoGe weights, MoGe-3, DepthART, YOLO26-depth or ZoeDepth from their LICENSE files. My unverified recollection is that Ultralytics code and weights are AGPL-3.0, which would conflict with shipping in an MIT app, and that DINOv3 weights carry Meta's own DINOv3 licence. Both need checking.
- I found no FLOPs figures for DAv2, DA3 or MoGe-2 from primary sources.
- Lite-Mono and ZoeDepth got no fresh sourcing. ZoeDepth appears only as a weak baseline in the MoGe-2 tables below. Lite-Mono is a KITTI self-supervised model and I found no zero-shot evaluation of it.
- "FoundationGeo" tops several columns in the 2026 survey and I did not research it.

## 2. For each model, what are the accuracy, boundary sharpness, speed, and export status?

### Takeaway
On accuracy, MoGe-2/MoGe-3, DA3 mono and Depth Pro 2 lead. On published speed, only DAv2-S, DepthART and YOLO26-depth sit in the single-digit millisecond range, and the runtime back end changes DAv2-S latency by about 4x on similar hardware. Published numbers mix alignment protocols, so compare within a table and not across tables.

### Cited findings

Zero-shot relative depth per size, AbsRel / delta1, disparity-space alignment as in the DAv2 paper.

| Model | KITTI | NYUv2 | Sintel | ETH3D | DIODE |
|---|---|---|---|---|---|
| DAv2 ViT-S | 0.078 / 0.936 | 0.053 / 0.973 | 0.500 / 0.718 | 0.142 / 0.851 | 0.073 / 0.942 |
| DAv2 ViT-B | 0.078 / 0.939 | 0.049 / 0.976 | 0.495 / 0.734 | 0.137 / 0.858 | 0.068 / 0.950 |
| DAv2 ViT-L | 0.074 / 0.946 | 0.045 / 0.979 | 0.487 / 0.752 | 0.131 / 0.865 | 0.066 / 0.952 |

Source for the table above. [Depth Anything V2 paper](https://arxiv.org/html/2406.09414v1)

- DA3 Table 4, delta1 on KITTI / NYU / Sintel / ETH3D / DIODE. DA2 94.6 / 97.9 / 77.2 / 86.5 / 95.2. DA3 any-view model 95.3 / 97.4 / 75.5 / 98.6 / 95.4. DA3 teacher 97.2 / 97.9 / 81.4 / 99.8 / 96.6. Table 10, the monocular student that became DA3Mono, 97.1 / 98.0 / 82.3 / 98.8 / 96.5. [DA3 paper](https://arxiv.org/html/2511.10647v1)
- DepthART vs DAv2-S, AbsRel / delta1. NYUv2 0.059 / 0.964 vs 0.053 / 0.973. KITTI 0.082 / 0.930 vs 0.078 / 0.936. ETH3D 0.084 / 0.950 vs 0.142 / 0.851. DIODE-Full 0.214 / 0.745 vs 0.228 / 0.751. DDAD 0.095 / 0.900 vs 0.085 / 0.921. These are DepthART-S at 6.0M parameters against DAv2-S at 24.8M. [DepthART paper](https://arxiv.org/html/2607.17099v1)
- Distill-Any-Depth Large student vs DAv2-L, AbsRel / delta1. NYUv2 0.043 / 0.981 vs 0.045 / 0.979. KITTI 0.070 / 0.949 vs 0.074 / 0.946. ETH3D 0.054 / 0.981 vs 0.131 / 0.865. ScanNet 0.043 / 0.980 vs 0.042 / 0.978. DIODE 0.233 / 0.753 vs 0.262 / 0.754. The paper has no per-size table for the Small and Base students and no boundary metric. [Distill Any Depth paper](https://arxiv.org/html/2502.19204v2)
- 2026 survey Table 2, all models aligned with scale and shift in depth space, AbsRel / delta1 in percent on NYUv2, KITTI, ETH3D, DIODE. MoGe-2 3.42 / 98.47, 4.80 / 98.19, 3.49 / 98.78, 4.72 / 96.39. MoGe 3.41 / 98.49, 4.99 / 98.20, 3.44 / 98.75, 5.00 / 95.80. DA3 3.88 / 98.32, 7.24 / 95.56, 5.97 / 96.16, 5.91 / 95.05. Depth Pro 4.18 / 98.15, 6.76 / 96.23, 7.27 / 94.90, 7.41 / 93.73. UniDepthV2 6.08 / 96.50, 8.46 / 93.37, 9.48 / 91.27, 12.02 / 86.74. Metric3D v2 6.77 / 94.07, 5.43 / 98.07, 5.51 / 96.49, 5.48 / 96.32. Marigold 5.20 / 97.32, 10.17 / 90.24, 14.53 / 82.11, 9.64 / 90.49. GenPercept 4.93 / 97.48, 8.60 / 93.72, 6.84 / 95.04, 9.28 / 90.47. Depth Anything V2 7.07 / 95.77, 12.49 / 83.65, 7.79 / 94.28, 8.32 / 92.83. [Survey 2609.01172](https://arxiv.org/html/2609.01172)
- MoGe-2 Table 1, affine-invariant depth averaged over 10 datasets, Rel / delta1. ZoeDepth 10.1 / 88.5. DA V1 8.76 / 90.4. DA V2 8.48 / 90.8. Metric3D V2 7.66 / 92.9. UniDepth V2 6.42 / 93.9. Depth Pro 7.65 / 92.0. MoGe 4.51 / 96.1. MoGe-2 5.62 / 94.8. [MoGe-2 paper](https://arxiv.org/html/2507.02546)
- Diffusion family, Marigold V2 Table 1, AbsRel / delta1 in percent on NYUv2, KITTI, ETH3D, ScanNet, DIODE. Marigold v1 5.5 / 96.4, 9.9 / 91.6, 6.5 / 96.0, 6.4 / 95.1, 10.0 / 90.7. GenPercept 5.2 / 96.6, 9.4 / 92.3, 6.6 / 95.7, 5.6 / 96.5. Lotus 5.4 / 96.8, 8.5 / 92.2, 5.9 / 97.0, 5.9 / 95.7, 9.8 / 92.4. Lotus-2 3.7 / 97.6, 6.7 / 94.1, 4.1 / 98.6, 4.0 / 97.2, 6.5 / 95.5. DepthFM 5.5 / 96.3, 8.9 / 91.3, 5.8 / 96.2, 6.3 / 95.4. Marigold V2 3.6 / 98.0, 5.4 / 97.4, 2.8 / 99.2, 3.7 / 97.9, 5.2 / 97.1. [Marigold V2 paper](https://arxiv.org/html/2609.08084v1)
- AnyDepth loses to DAv2 at every size in its own table, for example ViT-S NYUv2 8.2 / 93.2 vs 5.3 / 97.3 and KITTI 10.2 / 88.3 vs 7.8 / 93.6, though it wins ETH3D 8.4 / 93.5 vs 14.2 / 85.1. [AnyDepth paper](https://arxiv.org/html/2601.02760)
- YOLO26-depth on NYU at 768 px, delta1 / abs_rel. n 0.882 / 0.109, s 0.896 / 0.104, m 0.921 / 0.089, l 0.930 / 0.083, x 0.933 / 0.080. The docs list NYU Depth V2 among supported training datasets, and say their DAv2 comparison covers latency only. [Ultralytics perf table](https://raw.githubusercontent.com/ultralytics/ultralytics/main/docs/macros/yolo-depth-perf.md), [docs](https://raw.githubusercontent.com/ultralytics/ultralytics/main/docs/en/tasks/depth.md)
- MiDaS v2.1 small scores 0.1344 on ETH3D and 0.3370 AbsRel on Sintel, 76% worse than DPT-Large on the README's aggregate. [MiDaS README](https://raw.githubusercontent.com/isl-org/MiDaS/master/README.md)

Published latency.

| Model | Hardware and runtime | Input | Latency | Source |
|---|---|---|---|---|
| Depth Anything S / B / L | RTX 4090, TensorRT FP16, includes pre and post-processing | 518x518 | 3 / 6 / 12 ms | [depth-anything-tensorrt](https://www.github.com/spacewalk01/depth-anything-tensorrt), [Depth Anything README](https://github.com/LiheYoung/Depth-Anything) |
| DAv2 S / B / L | RTX 4080, ONNX Runtime CUDA EP, precision not stated | 518x518 | 13.3 / 29.3 / 83.2 ms | [Depth-Anything-ONNX issue 26](https://github.com/fabio-sim/Depth-Anything-ONNX/issues/26) |
| Depth Anything V2, model not named, likely vits | RTX 5090, direct TensorRT FP16 | 518x518 | about 4-6 ms | [vnk8071 repo](https://github.com/vnk8071/depth-anything-onnx-tensorrt) |
| DAv2 S / B | Tesla T4, TensorRT 10 FP16 | 640 | 13.62 / 35.10 ms | [Ultralytics depth docs](https://raw.githubusercontent.com/ultralytics/ultralytics/main/docs/en/tasks/depth.md) |
| YOLO26 n / s / m / l / x depth | Tesla T4, TensorRT 10 FP16 | 640 | 2.29 / 3.08 / 4.71 / 6.14 / 10.26 ms | same |
| YOLO26 n / s / m / l / x depth | Tesla T4, TensorRT 10 | 768 | 2.7 / 3.8 / 6.0 / 7.7 / 13.6 ms | [perf table](https://raw.githubusercontent.com/ultralytics/ultralytics/main/docs/macros/yolo-depth-perf.md) |
| DepthART S / B / L | RTX A6000, PyTorch strict FP32 | 448x448 | 4.09 / 4.91 / 8.07 ms | [DepthART paper](https://arxiv.org/html/2607.17099v1) |
| DepthART S / B / L | same | 224x224 | 2.88 / 3.36 / 5.23 ms | same |
| DA3 Small / Base / Large / Giant | A100, PyTorch, per image within a 32-image scene | 504x336 | 160.5 / 126.5 / 78.37 / 37.6 FPS | [DA3 paper](https://arxiv.org/html/2511.10647v1) |
| DA3-Small | Jetson Orin NX 16GB, TensorRT 10.3 | not stated | about 23 ms, vs 153 ms PyTorch | [GerdsenAI ROS2 post](https://discourse.openrobotics.org/t/release-gerdsenais-depth-anything-3-ros2-wrapper-with-real-time-tensorrt-for-jetson/52364) |
| DA3Metric-Large | Quadro RTX 6000, TensorRT | not stated | 50 FPS | [ros2-depth-anything-v3-trt](https://github.com/ika-rwth-aachen/ros2-depth-anything-v3-trt) |
| DAv2-L | A100, FP16 / FP32 | 518x518, 1369 tokens | 24 / 86 ms | [MoGe-2 paper Table B.3](https://arxiv.org/html/2507.02546) |
| MoGe-2 ViT-L | A100, FP16 | 1200 / 2500 / 3600 / 7200 tokens | 29 / 39 / 55 / 108 ms | same |
| UniDepth V2 L | A100, FP16 | 448x448 / 774x774 | 33 / 50 ms | same |
| Metric3D V2 L | A100, FP16 | 1064x616 | 87 ms | same |
| Depth Pro | A100, FP16 / FP32 | 1536x1536, 20160 tokens | 139 / 906 ms | same |
| MoGe-3 ViT-L / ViT-G, 3 refine steps | A100, FP16 | 700x700 | 121 / 177 ms | [MoGe-3 paper](https://arxiv.org/html/2607.17967v2) |
| InfiniDepth, Pixel-Perfect Depth | A100, FP16 | 512x672 | 133 / 275 ms | same |
| Depth Pro 2-S, streaming | NVIDIA B200 | 1080p | 53 fps | [Depth Pro 2 paper](https://arxiv.org/html/2611.26603v1) |
| Depth Pro 2-H+ | B200, bfloat16, PyTorch eager, 32-frame clip | VGA / 720p / 1080p / 4K | 95.9 / 20.2 / 5.3 / 0.4 fps | [apple/DepthPro2-H](https://huggingface.co/apple/DepthPro2-H) |
| Marigold V2, Lotus-2, PPD, InfiniDepth | one 32GB GPU | 1024x1024 | 1.9 s / 8.9 s (1.1 s without sharpener) / 1.4 s / 0.2 s | [Marigold V2 paper](https://arxiv.org/html/2609.08084v1) |
| MiDaS v2.1 small 256, DPT-Hybrid 384, Swin2-T 256, LeViT 224 | RTX 3090, PyTorch | training resolution | 90 / 50 / 64 / 73 FPS | [MiDaS README](https://raw.githubusercontent.com/isl-org/MiDaS/master/README.md) |

Jetson AGX Orin 64GB, TensorRT FP16, GPU milliseconds. Source for all rows. [Orin benchmark 2610.27199](https://arxiv.org/html/2610.27199v1)

| Model | Input | GPU ms |
|---|---|---|
| DAv2-S / B / L | 518x770 | 11.5 / 26.8 / 84.5 |
| DAv2-S | 1008x1512 | 76.6 |
| DA3 Metric-L | 350x504 | 33.2 |
| DA3 Metric-L | 700x1008 | 181.3 |
| MoGe-2 S / B / L | 700x1008 | 72.6 / 102.6 / 211.6 |
| UniDepth V2 S / B / L | 644x938 | 42.5 / 85.0 / 182.1 |
| Metric3D v2 S / L | 616x1064 | 54.5 / 265.3 |
| Depth Pro | 1536x1536 | 802.5 |

- Marigold v1 takes about 24 s for a 578x578 input according to the survey. A 2025 Marigold paper claims under 100 ms "on most commodity hardware" with single-step inference, a lightweight VAE and quantized weights. [Survey](https://arxiv.org/html/2609.01172), [Marigold 2025 paper](https://arxiv.org/html/2505.09358v1)
- DepthART reports its S/B/L as 3.2x, 6.7x and 12.7x faster than the matching DAv2 variants in model-only inference on the A6000. [DepthART paper](https://arxiv.org/pdf/2607.17099)
- TensorRT export problems. DA3 and MoGe-2 engines came out wrong under TensorRT 10.3 until the authors re-exported attention as an explicit softmax(q s k^T)v. The MoGe-2 S and B metric-scale head was wrong even in FP32 and had to run outside the engine. The DAv2-S full-resolution FP16 engine deviated 17.5% from FP32. UniDepth V2 engines matched FP32 within 0.05-0.49%. [Orin benchmark](https://arxiv.org/html/2610.27199v1)
- One user measured about 98-100 ms for DAv2-S at 518x518 with ONNX Runtime across T4, A10, A100 and H100, against the repo owner's 13 ms. The thread does not explain the gap. [Depth-Anything-ONNX issue 26](https://github.com/fabio-sim/Depth-Anything-ONNX/issues/26)

### Inferences
- Same model, similar GPU class, 3 ms under TensorRT FP16 on a 4090 against 13.3 ms under ONNX Runtime CUDA on a 4080. The runtime and precision path looks like a bigger lever than any model swap among ViT-S candidates. Unishade's DirectML path should be measured against TensorRT RTX or the ONNX Runtime TensorRT/NvTensorRtRtx providers before changing models.
- The survey's low DAv2 scores come from aligning a disparity model in depth space. The DAv2 paper's own protocol gives much better numbers. Neither is wrong, but only same-table comparisons are safe.
- DA3Mono-Large is the first Apache-2.0 ViT-L mono model that beats DAv2-L, which is CC-BY-NC. It replaces DAv2-L as the quality option Unishade could legally ship.
- YOLO26-depth numbers are likely in-domain for NYU and use median alignment, so the 0.88-0.93 delta1 is not comparable to DAv2-S zero-shot 0.973. The model is a CNN trained on about 2.19M images and I would expect weaker zero-shot behaviour on game art than the DINOv2-based models. That expectation is mine, not sourced.

### Gaps
- No published latency for any model on RTX 4060/4070-class cards, on AMD or Intel GPUs, or under DirectML. One search dedicated to DirectML found nothing.
- No per-size accuracy for DA3-Small or DA3-Base on single images, for MoGe-2 ViT-S/B on standard benchmarks, for Distill-Any-Depth Small, or for Depth Pro 2 S/B/L. The Depth Pro 2 HTML I read was truncated before its runtime appendix.
- No source measures latency while a game is loading the same GPU.

## 3. How well do these models generalize to synthetic, game, stylized and cartoon imagery?

### Takeaway
Evidence is thin but points one way. Foundation models rank cartoon and painted images well (DAv2-S 98.4% on the DA-2K "non-real" split), yet a 2026 dataset of real game z-buffers shows DAv2 at about twice the error of a model fine-tuned on game frames. Almost every strong model already trains on game-engine renders such as TartanAir, UnrealStereo4K, MatrixCity, GTA-SfM and MVS-Synth.

### Cited findings
- DA-2K has eight scenarios including "non-real", which covers AI-generated images, paintings and cartoons. Non-real accuracy is 98.4% for DAv2 ViT-S, 98.7% for ViT-B and 99.0% for ViT-L. Overall DA-2K accuracy is 95.3 / 97.0 / 97.1% for S / B / L against 86.8% for Marigold, 88.1% for GeoWizard and 85.8% for DepthFM. [Depth Anything V2 paper](https://arxiv.org/html/2406.09414v1)
- originlab/game-depth pairs 48,615 1080x1920 frames from 10 anonymized commercial games with engine z-buffer depth, outdoor scenes only. On its 999-frame held-out test split, SSI-MAE / AbsRel are 0.055 / 0.098 for Depth Anything V2, 0.041 / 0.074 for Marigold, 0.035 / 0.064 for released Lotus, and 0.029 / 0.050 for a Lotus-style SD2 UNet trained on 17,799 game frames. The same card lists DAv2 at 0.075 AbsRel on KITTI and 0.055 on NYU. Results are single-seed. The dataset licence forbids commercial use and redistribution. [originlab/game-depth](https://huggingface.co/datasets/originlab/game-depth)
- SoccerNet-Depth uses synthetic frames rendered from two sports video games with relative-scale ground truth, single-view RGB input. [SoccerNet 2025 challenge results](https://arxiv.org/pdf/2508.19182)
- On Sintel, an animated film, DAv2 scores AbsRel / delta1 0.500 / 0.718 for S, 0.495 / 0.734 for B, 0.487 / 0.752 for L. [Depth Anything V2 paper](https://arxiv.org/html/2406.09414v1). The DA3 mono student reaches delta1 82.3 on Sintel against 77.2 for DA2. [DA3 paper](https://arxiv.org/html/2511.10647v1). The 2026 survey lists Sintel AbsRel of 28.55 for FoundationGeo, 38.40 for MoGe-2 and 38.93 for DAv2. [Survey](https://arxiv.org/html/2609.01172)
- Boundary F1 on the synthetic Sintel / Spring sets. DAv2 32.5 / 6.10. UniDepth V2 39.7 / 7.08. Depth Pro 41.6 / 11.0. MoGe-2 35.2 / 8.63. Metric3D V2 25.3 / 7.23. [MoGe-2 paper](https://arxiv.org/html/2507.02546)
- Training data. The DAv2 teacher trains on 595K synthetic images from BlendedMVS, Hypersim, IRS, TartanAir and Virtual KITTI 2, then pseudo-labels 62M real images. [DAv2 paper](https://arxiv.org/html/2406.09414v1). The DA3 teacher's synthetic corpus adds SPRING, MVSSynth, UnrealStereo4K, GTA-SfM, MatrixCity, UrbanSyn, PointOdyssey, Structured3D, Objaverse and others. [DA3 paper](https://arxiv.org/html/2511.10647v1). MoGe-2 uses 16 synthetic sets including GTA-SfM at 2.7% sampling weight, MVS-Synth 1.2%, TartanAir 4.8%, UnrealStereo4K and MatrixCity. [MoGe-2 paper](https://arxiv.org/html/2507.02546). Depth Pro 2 trains on 19 sources that are nearly all synthetic, including 24.15M internal synthetic frames, TartanAir V2 and TartanGround. [Depth Pro 2 paper](https://arxiv.org/html/2611.26603v1)

### Inferences
- GTA-SfM and MVS-Synth are, to my knowledge, rendered from GTA V, and TartanAir, UnrealStereo4K and MatrixCity come from Unreal Engine. The papers do not label them as game data. So DA3 and MoGe-2 have seen clean game-engine frames, though without HUDs.
- The originlab result suggests a cheap win for Unishade. Fine-tuning or distilling a small student on game frames with z-buffer ground truth could halve relative error on games. ReShade's own depth access in injected mode could generate such pairs from games the project is allowed to capture. That plan is mine and no source tests it on a ViT-S model.
- The originlab comparison favours the in-domain model by construction, and the page does not say which DAv2 size it tested. Read it as a ceiling on what domain data can buy, and treat the absolute gap with caution.

### Gaps
- I found no study of HUD or UI overlays, motion blur, film grain, chromatic aberration or upscaler artefacts on monocular depth. Depth Anything AC covers darkness, fog, snow and blur only, on real photos.
- I found no benchmark of stylized, cel-shaded or pixel-art games, and no per-genre results.
- Two searches for foundation-model evaluations on named games returned nothing beyond originlab and SoccerNet.

## 4. How does input resolution trade off against accuracy and speed for ViT-based models?

### Takeaway
Cost follows token count, which is width times height divided by the patch area, and it grows faster than linearly once counts pass a few thousand. More resolution sharpens edges for DAv2 but does not improve, and often hurts, global accuracy beyond the training size near 518.

### Cited findings
- DAv2 runs 1369 tokens at 518x518 with patch 14. [MoGe-2 paper Table B.3](https://arxiv.org/html/2507.02546)
- DAv2's authors say resolution can be raised "almost freely" at test time and that 2x and 4x the 518 base progressively sharpen the prediction. The README default is 518 and suggests raising input size for finer results. [DAv2 paper](https://arxiv.org/html/2406.09414v1), [DAv2 README](https://raw.githubusercontent.com/DepthAnything/Depth-Anything-V2/main/README.md)
- On Middlebury, oracle-scaled AbsRel for DAv2-S goes 8.93% at 518x770, 9.17% at 1008x1512 and 9.73% at 1988x2884, while GPU time on the AGX Orin goes 11.5, 76.6 and 761.6 ms. DAv2-L goes 8.31%, 9.92%, 10.72% at 84.5, 502.7 and 4496.9 ms. DA3 Metric-L goes 5.97% at 350x504 and 6.07% at 700x1008 for 33.2 and 181.3 ms. The paper concludes monocular error does not improve with input size. [Orin benchmark](https://arxiv.org/html/2610.27199v1)
- MoGe exposes a token budget directly. `resolution_level` 0-9 or `num_tokens` in a suggested 1200-2500 range trades detail for speed without changing output size. MoGe-2 ViT-L costs 29, 39, 55 and 108 ms at 1200, 2500, 3600 and 7200 tokens on an A100 in FP16. [MoGe README](https://raw.githubusercontent.com/microsoft/MoGe/main/README.md), [MoGe-2 paper](https://arxiv.org/html/2507.02546)
- DA3 trains at a 504 base with aspect variants such as 504x336, 504x280, 896x504 and 756x504. [DA3 paper](https://arxiv.org/html/2511.10647v1)
- Depth Pro 2 and AnyDepth use DINOv3 with 16x16 patches. [Depth Pro 2 paper](https://arxiv.org/html/2611.26603v1), [AnyDepth paper](https://arxiv.org/html/2601.02760)
- Real deployments cluster near the training size. TensorRT ports benchmark at 518x518, Ultralytics at 640 and 768, the Arm DA3-Small package fixes 504x378, the Orin benchmark's 30 fps pick is DA3 Metric-L at 350x504, and DepthART reports 224 and 448. [depth-anything-tensorrt](https://www.github.com/spacewalk01/depth-anything-tensorrt), [Ultralytics docs](https://raw.githubusercontent.com/ultralytics/ultralytics/main/docs/en/tasks/depth.md), [Arm DA3-Small card](https://huggingface.co/Arm/depth-anything-v3-small-mix-precision), [Orin benchmark](https://arxiv.org/html/2610.27199v1), [DepthART](https://arxiv.org/html/2607.17099v1)

### Inferences
- My arithmetic for Unishade's current sizes on a 16:9 frame with patch 14. Longest side 266 gives 19x11 = 209 tokens. 392 gives 28x16 = 448. 518 gives 37x21 = 777. 924 gives 66x37 = 2442. A square 518 benchmark is 1369 tokens, so published 518x518 timings overstate the cost of Unishade's 518 mode by roughly 1.76x in tokens.
- My arithmetic on the Orin numbers for DAv2-S. 518x770 is 2035 tokens, 1008x1512 is 7776, 1988x2884 is 29,252. A 3.8x token increase cost 6.7x time and a 14.4x increase cost 66x. Below about 2000 tokens the scaling should be close to linear for ViT-S. Above it, attention dominates.
- At equal pixel resolution, patch 16 yields 23% fewer tokens than patch 14, since (14/16)^2 = 0.766. DINOv3-based heads get that discount at the cost of a new backbone licence.
- The 924 mode is the one to question. It costs about 3.1x the tokens of 518 on 16:9, and the Orin data says accuracy does not rise. Its only benefit is edge detail, which a guided upsampler or refinement pass could supply for less.

### Gaps
- No source reports accuracy for DAv2-S or DA3-Small below 518, for example at 266 or 392, on standard benchmarks. The Arm card gives DA3-Small at 504x378 as NYU delta1 91.63% and AbsRel 8.85%, but its alignment protocol differs from the papers.
- The Orin benchmark uses only 10 Middlebury indoor scenes.

## 5. Which models target sharp boundaries or cheap high-resolution output?

### Takeaway
Depth Pro 2 (H+), InfiniDepth, MoGe-3 and Depth Pro lead on boundary F1, and all are far over budget or licence-blocked. Among shippable real-time options, DAv2 is already much sharper than older models, MoGe-2 is sharper than DAv2, and the DA3 any-view models are noticeably blurrier at edges.

### Cited findings
- Boundary F1 in percent on iBims-1 / HAMMER / Sintel / Spring. ZoeDepth 2.47 / 0.17 / 2.30 / 0.43. DA V1 3.68 / 0.76 / 5.64 / 1.09. DA V2 13.9 / 4.74 / 32.5 / 6.10. Metric3D V2 7.36 / 1.40 / 25.3 / 7.23. UniDepth V2 11.2 / 4.40 / 39.7 / 7.08. Depth Pro 14.3 / 5.36 / 41.6 / 11.0. MoGe 11.4 / 3.89 / 26.3 / 8.36. MoGe-2 17.9 / 5.42 / 35.2 / 8.63. [MoGe-2 paper Table 3](https://arxiv.org/html/2507.02546)
- Averaged boundary F1 over Spring, iBims-1, Sintel, HAMMER in the MoGe-3 paper. InfiniDepth 19.3. MoGe-3 ViT-G 17.3. Depth Pro 16.3. MoGe-3 ViT-L 16.0. MoGe-2 15.6. UniK3D 15.1. UniDepth V2 14.5. Pixel-Perfect Depth 12.7. Depth Anything 3 6.61. [MoGe-3 paper](https://arxiv.org/html/2607.17967v2)
- Depth Pro 2-H+ single-image boundary F1 on iBims / Middlebury / Sintel / Spring / DyDToF is 20.5 / 21.9 / 40.2 / 16.3 / 31.6, against 19.5 / 7.9 / 36.3 / 12.7 / 24.8 for Depth Pro and 8.0 / 3.6 / 7.9 / 3.4 / 7.6 for DA3 Nested-Giant-Large. Temporal jitter on Sintel / Spring is 1.39 / 0.86 for Depth Pro 2-H+, 11.26 / 8.99 for Depth Pro, 5.33 / 2.19 for FlashDepth-L, 2.55 / 0.88 for Video Depth Anything-L. [Depth Pro 2 paper](https://arxiv.org/html/2611.26603v1)
- Depth Pro produces a 2.25-megapixel map in 0.3 s on a V100. [Depth Pro paper](https://arxiv.org/html/2410.02073v1)
- Flying points at depth edges on Middlebury with oracle scale. DAv2-S 25.8%. DAv2-L 20.3%. DA3 Metric-L 16.6%. MoGe-2-L 46.1%. Metric3D v2-L 22.8%. UniDepth V2-L 17.1%. Edge AbsRel ranges from 7.46% for MoGe-2-L to 11.94% for DAv2-L. [Orin benchmark](https://arxiv.org/html/2610.27199v1)
- The DA3 README says DA3's monocular results match DA2's detail, and DA3Mono-Large is a separate checkpoint from the any-view models. [DA3 README](https://raw.githubusercontent.com/ByteDance-Seed/Depth-Anything-3/main/README.md)
- FlashDepth runs streaming video depth at 24 FPS at 2044x1148 on an A100 with patch 14. [FlashDepth](https://arxiv.org/html/2504.07093v1)
- Lighter heads. AnyDepth's SDT decoder cuts decoder parameters by 85-89% against its DPT baseline and total latency by about 6-7% on an H100 at 512x512 with ViT-L, 23.17 vs 24.65 ms. [AnyDepth paper](https://arxiv.org/html/2601.02760)
- Feature upsamplers. AnyUp (ICLR 2026) reports better NYUv2 depth and normal probing than FeatUp and LoftUp, and says LoftUp over-smooths per object and does poorly on normals. FeatUp's own table gives its JBU variant RMSE 1.09 on linear-probe depth. A CVPR 2026 paper, Upsample Anything, found LoftUp gave about the same Cityscapes mIoU as bilinear interpolation in its setup. [AnyUp](https://arxiv.org/html/2510.12764v2), [FeatUp](https://arxiv.org/pdf/2403.10516), [Upsample Anything](https://arxiv.org/html/2511.16301v2)
- The 2026 survey notes that benchmark numbers and visual boundary quality still diverge. [Survey](https://arxiv.org/html/2609.01172)

### Inferences
- The MoGe-3 and Depth Pro 2 tables measure DA3's any-view models. Its 6.61 and 8.0 F1 scores sit below DAv2-L's roughly 14 (my average of the four MoGe-2 Table 3 values). DA3-Small and DA3-Base are also any-view models, so I expect them to be softer at edges than DAv2-Small. DA3Mono-Large is trained differently and may not share the weakness. No table I found measures it.
- For depth of field and SSAO, edge alignment with the colour frame matters more than delta1. Unishade has the full-resolution colour frame on the GPU already, so a joint bilateral or guided-filter upsample of low-resolution depth in a shader is the cheap route. The learned feature upsamplers above add network cost and their gains over simple baselines are disputed.
- MoGe-2's high flying-point rate (46.1%) alongside the best edge AbsRel suggests sharp but noisy edges that need the validity mask or a filter before use.

### Gaps
- No boundary metrics for any small model. DAv2-S, DA3-Small, MoGe-2 ViT-S, DepthART, YOLO26-depth and Depth Pro 2-S all lack published boundary F1 in what I read.
- I found no measurement of DAv2 with a lighter or swapped head, and no published use of AnyUp, JBU, FeatUp or LoftUp as a depth-map upsampler in a real-time pipeline. PatchFusion and PatchRefiner were not researched because tile-based refinement costs many forward passes.

## 6. Are there quantized, pruned or distilled versions with measured quality loss?

### Takeaway
FP16 is free in quality when the export is correct. INT8 evidence is sparse and discouraging for ViTs under TensorRT. Distillation is the route that has measured results, with DepthART keeping DAv2-S-level accuracy at a quarter of the parameters.

### Cited findings
- DA3-Small at FP16 compute vs FP32 on NYU Depth V2 shows delta1 91.63% vs 91.63% and AbsRel 8.85% vs 8.85%, with 2.36x higher throughput on an Arm phone CPU, 157.8 ms vs 372.8 ms at 504x378. Apache 2.0. [Arm DA3-Small mixed precision](https://huggingface.co/Arm/depth-anything-v3-small-mix-precision)
- A user on NVIDIA's forum reported INT8 slower than FP16 for DAV2_small_518 under TensorRT, 41.3 FPS vs 127 FPS on an RTX A2000, and the same pattern for MiDaS DPT-Hybrid on Jetson Thor. [NVIDIA forum thread](https://forums.developer.nvidia.com/t/int8-throughput-and-latency-worse-than-fp16-for-midas-dpt-hybrid-model-on-thor/354915)
- A community INT8 ONNX of DAv2-Small reports 0.9987 correlation with the reference on one photo, on CPU. [ipsilondev/depth-anything-v2-small-onnx](https://huggingface.co/ipsilondev/depth-anything-v2-small-onnx)
- A community "DAv2 Tiny" quantization project says 4-bit weight-only formats kept thin structures and edges better than its INT8 QDQ experiments. [Petrouil/Depth-Anything-V2-Tiny-hf-quant](https://huggingface.co/Petrouil/Depth-Anything-V2-Tiny-hf-quant)
- An Android assistant paper reports at most 2% accuracy loss from INT8 dynamic quantization of its depth model. [VisionAId](https://arxiv.org/pdf/2607.02371)
- FP16 TensorRT engines can silently break. DAv2-S at full resolution deviated 17.5% from FP32, Metric3D v2 by 1.3-3.3%, Depth Pro by 1.9%. The Orin benchmark did not evaluate INT8. [Orin benchmark](https://arxiv.org/html/2610.27199v1)
- DepthART-S, 6.0M parameters and 7.0 GFLOPs at 448, distilled from DAv2-L, loses 0.006 AbsRel on NYUv2 and 0.004 on KITTI against DAv2-S and beats it on ETH3D. [DepthART paper](https://arxiv.org/html/2607.17099v1)
- Distill-Any-Depth improves a DAv2-L student with 200K unlabeled SA-1B images using DAv2-L and GenPercept as teachers, mostly on ETH3D, 0.054 vs 0.131 AbsRel. [Distill Any Depth paper](https://arxiv.org/html/2502.19204v2)
- Marigold V2 runs its DiT with 4-bit quantized weights and still needs 1.9 s at 1024x1024. [Marigold V2 paper](https://arxiv.org/html/2609.08084v1)

### Inferences
- INT8 is not worth pursuing first. The one GPU data point shows it slower than FP16 on a ViT, likely because unsupported layers fall back and insert reformatting, and no source gives depth accuracy after INT8 on a GPU.
- Any FP16 engine Unishade ships should be validated numerically against FP32 per resolution. The Orin paper's failures were silent.

### Gaps
- No pruned depth foundation models with measured accuracy turned up. Token merging or pruning for DAv2 was not searched.
- No INT8 accuracy on standard depth benchmarks under TensorRT or DirectML.
- Whether DepthART weights are downloadable, and under what licence, is unconfirmed.

## 7. Which models give surface normals or point maps along with depth at similar cost?

### Takeaway
MoGe-2 ViT-S "normal" is the only small model that returns depth, metric point map, normals, FOV and a validity mask in one pass, at 35M parameters. Published timings put it well above DAv2-S, partly because benchmarks run it at higher token counts.

### Cited findings
- MoGe-2 normal variants exist at 35M, 104M and 331M parameters and output points, depth, intrinsics, mask and normals. [MoGe README](https://raw.githubusercontent.com/microsoft/MoGe/main/README.md)
- On the AGX Orin at 700x1008, MoGe-2-S takes 72.6 ms against 11.5 ms for DAv2-S at 518x770. MoGe-2-S had only 76% valid density and 4.86% oracle-scaled AbsRel, MoGe-2-B 96% and 3.88%, against 8.93% for DAv2-S. [Orin benchmark](https://arxiv.org/html/2610.27199v1)
- A community benchmark table lists a MoGe ViT-S row near 29.7 ms and a ViT-L row near 73.4 ms with unclear column headers. [fastforks/moge](https://huggingface.co/fastforks/moge)
- Metric3D v2 predicts normals with depth. Its ViT-S takes 54.5 ms on the AGX Orin at 616x1064, and its Middlebury density was 65%. [Orin benchmark](https://arxiv.org/html/2610.27199v1)
- AnyDepth says its framework extends to metric depth and normals. Marigold V2 reports a surface-normal variant. [AnyDepth](https://arxiv.org/abs/2601.02760), [Marigold V2](https://arxiv.org/html/2609.08084v1)
- The DA3 teacher trains with a distance-weighted surface-normal loss but the released models output depth, rays, pose and sky masks, no normals. [DA3 paper](https://arxiv.org/html/2511.10647v1), [DA3 README](https://raw.githubusercontent.com/ByteDance-Seed/Depth-Anything-3/main/README.md)
- DA3Mono-Large and DA3Metric-Large output a sky segmentation mask. [DA3 README](https://raw.githubusercontent.com/ByteDance-Seed/Depth-Anything-3/main/README.md)

### Inferences
- My arithmetic says 700x1008 is about 3600 tokens at patch 14 against 2035 for 518x770, so the Orin comparison overstates the MoGe-2-S premium. At an equal token count I would expect MoGe-2-S to cost perhaps 1.5-2.5x DAv2-S because of its extra heads. That is an estimate.
- Normals derived from a monocular depth map by screen-space differentiation are free and are what ReShade shaders already do. A network normal head earns its cost only if it is cleaner on flat surfaces than differentiated depth, and no source compares the two for shading.
- The sky mask in DA3Mono matters for a ReShade depth buffer, since sky needs to map to the far plane and relative models often leave it ambiguous.

### Gaps
- The Heliosoph MoGe-2 ONNX cards returned HTTP 401, so opset, precision, fixed vs dynamic shapes and DirectML compatibility are unverified.
- No latency for MoGe-2 ViT-S on a desktop GPU.

## 8. Which models fit 2-6 ms on a mid-range RTX GPU, and which are best at 10-20 ms?

### Takeaway
Nothing published proves any model at 2-6 ms on a mid-range RTX card. By my scaling from RTX 4090 and T4 numbers, DAv2-S at 392-518 longest side under TensorRT FP16 fits, DepthART-S and YOLO26n/s-depth fit with room, and everything ViT-B or larger does not. At 10-20 ms the candidates are DA3Mono-Large at reduced resolution, DA3-Base, and MoGe-2 ViT-S/B.

### Cited findings
- DAv2 S / B / L take 3 / 6 / 12 ms at 518x518 under TensorRT FP16 on an RTX 4090, including pre and post-processing. [depth-anything-tensorrt](https://www.github.com/spacewalk01/depth-anything-tensorrt)
- DAv2 S takes 13.3 ms at 518x518 under ONNX Runtime CUDA on an RTX 4080. [Depth-Anything-ONNX issue 26](https://github.com/fabio-sim/Depth-Anything-ONNX/issues/26)
- On a T4 under TensorRT FP16 at 640, DAv2-S takes 13.62 ms and YOLO26n-depth 2.29 ms. [Ultralytics docs](https://raw.githubusercontent.com/ultralytics/ultralytics/main/docs/en/tasks/depth.md)
- DepthART-S takes 4.09 ms at 448x448 in PyTorch FP32 on an RTX A6000. [DepthART paper](https://arxiv.org/html/2607.17099v1)
- DA3-Small reaches 160.5 FPS and DA3-Base 126.5 FPS per image at 504x336 on an A100 in PyTorch. [DA3 paper](https://arxiv.org/html/2511.10647v1)
- AsyncMDE shows a split design, a 3.83M fast path at 237 FPS on an RTX 4090 with a 97.5M model updating in the background. [AsyncMDE](https://arxiv.org/abs/2603.10438)
- Depth Pro 2-S streams 1080p at 53 fps on a B200. [Depth Pro 2 paper](https://arxiv.org/html/2611.26603v1)

### Inferences
All figures in this list are my estimates. I assume an RTX 4060/4070-class card is 2-3x slower than an RTX 4090 for these small models, that TensorRT FP16 is the runtime, and that the game leaves the GPU idle. Under game load, expect more.

- DAv2-S at 518x294 (777 tokens) lands near 4-5 ms, at 392x224 (448 tokens) near 2-3 ms. This fits the 2-6 ms budget, and it is the model Unishade already ships. The gain available is in the runtime, the export and the post-filter.
- Distill-Any-Depth Small is a weight swap on the same graph at the same cost, if its weight licence clears. Its accuracy gain for the Small student is unpublished.
- DepthART-S should come in under DAv2-S, perhaps 1.5-3 ms at 448, with NYU and KITTI accuracy slightly below DAv2-S and ETH3D above. Availability and licence are open.
- YOLO26n/s-depth would run near 1-2 ms, with weaker accuracy, unknown edge quality and a licence I expect to be AGPL.
- DA3-Small should cost about the same as DAv2-S per token, since both are ViT-S with a small DPT head. I expect softer edges.
- At 10-20 ms. DA3Mono-Large at about 392x224 to 518x294 would take roughly 10-20 ms by scaling the 12 ms 4090 figure for DAv2-L at 1369 tokens. It is the highest-accuracy Apache-2.0 option with a sky mask. DA3-Base at 518 would take about 7-10 ms. MoGe-2 ViT-S at 1200 tokens might take 8-15 ms and adds normals and metric points. MoGe-2 ViT-B likely needs 15-25 ms.
- Too slow for per-frame use on any consumer GPU. Depth Pro (139 ms on an A100), MoGe-3 (121 ms), UniDepth V2-L, Metric3D v2-L, InfiniDepth, Pixel-Perfect Depth, and every diffusion model, Marigold, Marigold V2, Lotus, Lotus-2, GenPercept and DepthFM.
- Licence-blocked for distribution regardless of speed. DAv2 Base/Large, DA3 Large/Giant/Nested, Depth Anything AC, UniDepth, Depth Pro 2, and probably Metric3D v2 and Depth Pro.
- A two-rate design in the AsyncMDE style suits a GPU the game is already loading. Run a larger model such as DA3Mono-Large every few frames and a tiny network or a reprojection step every frame. That is a suggestion, not a tested result for games.

### Gaps
- Every mid-range number above is extrapolated. Nobody has published DAv2-S, DA3-Small, MoGe-2-S or DepthART latency on RTX 4060/4070/5060/5070, Radeon or Arc hardware, or under DirectML, WinML or TensorRT for RTX.
- No source measures inference latency under concurrent game rendering, where scheduling and VRAM pressure can dominate.
- Requirements for feeding depth to DLSS 5 were out of scope here and I found nothing on them in these sources.
