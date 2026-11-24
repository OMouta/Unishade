# Temporally consistent video depth for a causal real-time pipeline (state as of November 2026)

Reading notes. "A100" numbers come from papers and are not consumer-GPU numbers. Anything marked "my estimate" is mine and is not in a source. All speed figures are PyTorch or TensorRT on Linux unless stated. None of the sources measured ONNX Runtime with DirectML.

## 1. Learned video depth models: causal or not, speed, temporal metrics, licence, export

### Takeaway
Three learned models are both causal and fast enough to matter: Video Depth Anything Small in its experimental streaming mode (Apache-2.0, the only one with a clean licence), oVDA (42 FPS on an A100 at 280x924 with 0.45 GB, but non-commercial share-alike weights), and FlashDepth (Apache-2.0 repo, Mamba state, Linux-only build). Everything diffusion-based (DepthCrafter, Depth Any Video, RollingDepth, ChronoDepth) and every streaming 3D model (CUT3R, StreamVGGT, STream3R, ViGeo) is either offline, far too slow, or non-commercial, usually all three.

### Cited findings

#### Summary table (numbers as read in the cited sources)

| Model | Causal | Temporal design | Speed as reported | Temporal or accuracy metric | Licence |
|---|---|---|---|---|---|
| Video Depth Anything (VDA) S / B / L, offline | No, 32-frame clips | Temporal attention head on a frozen DINOv2 encoder | A100, 518x518: S 9.1 ms/frame FP32, 7.5 ms FP16. L 67 ms FP32, 14 ms FP16 | TAE (ScanNet) S 0.703, L 0.570, DAv2-L 1.140 | S Apache-2.0. B and L CC-BY-NC-4.0 |
| VDA streaming mode (S / B / L) | Yes | Same weights, caches temporal-attention hidden states, one new frame per step | No streaming-specific timing published | ScanNet d1 drops 0.926 to 0.836 against offline | Same as above |
| oVDA (VDA-S based) | Yes | FIFO cache of 16 past frames' latents per motion module, cross-attention to cached K/V | A100 280x924: 42 FPS, 0.45 GB. Jetson Orin NX FP16: 20 FPS, 0.49 GB | KITTI AbsRel 0.140, d1 0.809 (first-frame alignment), lowest scale drift of the online methods tested | NC-SA-UHDV1.0, commercial use prohibited |
| FlashDepth (Full / L / S) | Yes | Mamba blocks after the DPT decoder, hidden state per frame | A100: Full 24 FPS at 2044x1148, L 30 FPS at 924x518. S 60 FPS, 0.69 GB at 280x924 (oVDA's measurement) | No temporal metric in the paper. Authors name "residual flickering" as the main limit | Apache-2.0 repo |
| DyFN (wraps MoGe v1 or DAv2) | Yes | ConvGRU that predicts per-feature mean and std, about 5M trainable params | Not reported | Sintel AbsRel 0.180 vs FlashDepth 0.265, KITTI 0.062 vs 0.103 | No code or licence statement found |
| VeloDepth | Yes | Flow-warped depth and features plus learned residual, keyframes about every 30 frames | RTX 3090 at 0.5 MP: 26.2 FPS. "Over 60 FPS" at 640x360 (supplement claim) | Consistency t5 aggregate 87.5 vs VDA 80.7, DAv2 74.5 | Repo licence CC BY-NC-SA 4.0 |
| NVDS / NVDS+ | Forward-only mode is causal | Stabilizer net with cross-attention over 3 previous frames | NVDS+ Small 35.17 FPS at 896x384 including MiDaS-Small. NVDS 204 ms/frame on A100 | OPW (VDW) 0.165 forward-only vs 0.470 for raw DPT | NVDS code and model MIT. NVDS+ code not in the repo |
| VDPP | Unclear, window formula uses frames t..t+k | Depth-only post-processor, small DINOv2 plus VDA-S head | A6000: 1.4 to 8.6 ms overhead. Jetson Orin Nano 43.5 FPS | TGSE worse than VDA-L on all three test sets | Apache-2.0 repo |
| ChronoDepth | Listed as online by oVDA | Video diffusion, window 10 | A100: 506 ms/frame (VDA paper), 2 FPS and 10.27 GB (oVDA) | TAE 1.022 | MIT repo |
| DepthCrafter | No | Video diffusion, 110-frame clips with 25-frame overlap | A100: 2.1 FPS at 1024x576 with about 26 GB | TAE 0.639 | Academic, research and education only |
| Depth Any Video | No | Video diffusion, max 192 frames | A100 518x518: 159 ms/frame | TAE 0.967 | CC BY-NC 4.0 |
| RollingDepth | No | Image diffusion on dilated snippets plus global alignment | 250 frames in 81 s (fast preset) | TAE 0.65 | Code Apache-2.0, weights RAIL++-M |
| CUT3R | Yes | Recurrent persistent scene state | A100: 14 FPS at 512x288 (FlashDepth), 10 FPS and 6.70 GB (oVDA) | KITTI AbsRel 0.238 in oVDA's table | CC BY-NC-SA 4.0 |
| StreamVGGT | Yes | Causal attention plus unbounded KV cache | 0.07 s for the last frame of a 40-frame stream | KITTI Rel 0.173 in ViGeo's table | CC BY-NC-SA 4.0 |
| STream3R | Yes | Decoder-only causal transformer with KV cache | Not found | KITTI Rel 0.088 in ViGeo's table | S-Lab License 1.0, non-commercial |
| ViGeo | Yes in one-frame-chunk mode | Dynamic chunking attention, KV cache | H20 96 GB: 10.00 FPS for 16 images at 518x518, 1.26B params | KITTI Rel 0.049 online | Not stated |
| Depth Anything 3 | Per-frame, or offline sliding window (DA3-Streaming) | None for mono use | No FPS on the README | Not addressed | Small, Base, Mono-Large, Metric-Large Apache-2.0. Large and Giant CC BY-NC 4.0 |

#### Video Depth Anything, offline and streaming
- The paper's long-video scheme uses 32-frame clips made of 22 new frames, 8 overlap frames and 2 key frames taken every 12 frames going backward, and stitches clips by linear interpolation over the overlap. Source: [VDA paper v3](https://arxiv.org/html/2501.12375v3)
- The paper states that it "still faces challenges in handling streaming videos, which we leave as future work". Source: [VDA paper v3](https://arxiv.org/html/2501.12375v3)
- TAE (temporal alignment error) is the AbsRel between each frame's depth reprojected into its neighbour and the neighbour's depth, averaged both ways. It needs ground-truth camera poses, so it only exists for datasets like ScanNet. Source: [VDA paper v3](https://arxiv.org/html/2501.12375v3)
- VDA Table 1 TAE: DAv2-L 1.140, NVDS 2.176, NVDS + DAv2-L 2.536, ChronoDepth 1.022, DepthCrafter 0.639, DepthAnyVideo 0.967, VDA-S 0.703, VDA-L 0.570. Source: [VDA paper v3](https://arxiv.org/html/2501.12375v3)
- Latency on one A100 at 518x518 per frame: VDA-S 9.1 ms (FP32), VDA-L 67 ms, DAv2-L 60 ms, NVDS 204 ms, DepthAnyVideo 159 ms, ChronoDepth 506 ms, DepthCrafter 910 ms. Source: [VDA paper v3](https://arxiv.org/html/2501.12375v3)
- VRAM for input 1x32x518x518 on an A100: Small 7.3 GB FP32 and 6.8 GB FP16, Large 26.7 GB and 23.6 GB. Parameter counts are 28.4M, 113.1M and 381.8M. Source: [VDA GitHub README](https://github.com/DepthAnything/Video-Depth-Anything)
- The streaming mode shipped on 2025-07-03 as an experimental, training-free feature. It caches the hidden states of the temporal attention layers per frame and feeds one new frame per step. On ScanNet d1 falls from 0.926 to 0.836 against offline. The authors expect fine-tuning in streaming mode to help and left it undone. Metric models gained streaming on 2025-09-12. Source: [VDA GitHub README](https://github.com/DepthAnything/Video-Depth-Anything)
- Issue #83 ("performance drop between the streaming model and the offline model?", opened 2025-08-25) is still open. ONNX and TensorRT export requests #13, #22 and #31 from early 2025 are all still open, and the README does not mention ONNX. Source: [VDA GitHub issues](https://github.com/DepthAnything/Video-Depth-Anything/issues)
- Licences: Small is Apache-2.0, Base and Large are CC-BY-NC-4.0. The README gives no licence for the metric variants. Source: [VDA GitHub README](https://github.com/DepthAnything/Video-Depth-Anything)
- A community ONNX export of VDA-S exists, split into a per-frame encoder (`vda_vits_encoder_518x910_fp16.onnx`, 45 MB) and a temporal head fixed at T=32 (`vda_vits_head_T32_37x65_fp16.onnx`, 14 MB), opset 17, static shapes, fp16 weights with fp32 I/O. It is a fixed 32-frame window, not a streaming export with state inputs. It targets onnxruntime-web on WebGPU and is Apache-2.0. Source: [TheoreticallyTim/theoreticallypose-vda](https://huggingface.co/TheoreticallyTim/theoreticallypose-vda)
- iw3 (nunif) ships `VDA_Stream_S/B/L` models that reuse the normal VDA checkpoints. Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)

#### oVDA (Online Video Depth Anything, 3DV 2026)
- oVDA starts from VDA-S weights, freezes the DINOv2 encoder and fine-tunes only the spatiotemporal head (12 blocks). Training uses a lower-triangular attention mask so each frame only sees past frames. At inference it caches the latents of past frames before every attention operation in each motion module, as a FIFO of c frames (default 16, ablated at 8 and 32). Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- A100 at 280x924 over 5,000 frames: oVDA 42 FPS and 0.45 GB, DAv2-S 72 FPS and 0.25 GB, DAv2-L 12 FPS and 2.04 GB, FlashDepth-S 60 FPS and 0.69 GB, FlashDepth 30 FPS and 2.72 GB, CUT3R 10 FPS and 6.70 GB, NVDS 7 FPS and 9.09 GB, ChronoDepth 2 FPS and 10.27 GB. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- Context ablation on KITTI: c=8 gives 43 FPS, 0.37 GB, d1 0.800. c=16 gives 42 FPS, 0.45 GB, d1 0.809. c=32 gives 39 FPS, 1.36 GB, d1 0.811. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- Jetson Orin NX at 280x924: 9 FPS FP32, 20 FPS FP16 with 0.49 GB, accuracy unchanged (AbsRel 0.1398). Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- Accuracy with first-frame alignment (AbsRel / d1): oVDA KITTI 0.140 / 0.809, Bonn 0.118 / 0.871, Sintel 0.380 / 0.548. FlashDepth-S 0.156 / 0.774, 0.116 / 0.848, 0.355 / 0.532. DAv2-S per frame 0.216 / 0.655, 0.202 / 0.712, 0.390 / 0.514. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- Against offline VDA at 280x924: VDA-S d1 0.832 with 232 ms latency, VDA-L 0.860 with 579 ms, oVDA (c=32) 0.811 with 26 ms. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- The paper does not report TAE or OPW. It measures stability as scale drift on KITTI and finds oVDA lowest, with FlashDepth and CUT3R approaching it after about 300 frames. Stated limits are scale drift on very long sequences and trailing artefacts on fast-moving objects. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- Weights are on Hugging Face under licence NC-SA-UHDV1.0. "Commercial use prohibited without written permission", share-alike, attribution to Heidelberg University. Parts derived from VDA stay Apache-2.0. Source: [FriedFeid/oVDA model card](https://huggingface.co/FriedFeid/oVDA)
- The official repo is `FriedFeid/OnlineVideoDepthAnything` (last push 2026-11-13). A FiftyOne wrapper exposes c16 and c8 variants. Sources: [GitHub repo](https://github.com/FriedFeid/OnlineVideoDepthAnything), [FiftyOne plugin docs](https://docs.voxel51.com/plugins/plugins_ecosystem/online_video_depth_anything.html)

#### FlashDepth (ICCV 2025, Netflix Eyeline)
- FlashDepth adds a Mamba module after the DPT decoder and before the final conv head of Depth Anything V2. It downsamples decoder features, runs four Mamba blocks with hidden state H(t-1), upsamples and adds the result back residually. The module is about 1% of parameters and its output is zero-initialised. Source: [FlashDepth paper](https://arxiv.org/html/2504.07093v1)
- The hybrid "Full" model runs FlashDepth-S (from DAv2-S, about 21M params) at 2044x1148 and FlashDepth-L (from DAv2-L, about 300M) at 924x518, fused by cross-attention. Source: [FlashDepth paper](https://arxiv.org/html/2504.07093v1)
- A100 speeds: Full 24 FPS at 2044x1148, FlashDepth-L 30 FPS at 924x518, FlashDepth-L at 2K 6.0 FPS. The authors measure VDA at 24 FPS at 924x518 with about 40 GB, DepthCrafter at 2.1 FPS at 1024x576, CUT3R at 14 FPS at 512x288. Source: [FlashDepth paper](https://arxiv.org/html/2504.07093v1)
- FlashDepth-S loses 3 to 10% accuracy against FlashDepth-L (for example Sintel AbsRel 0.299 vs 0.265). Source: [FlashDepth paper](https://arxiv.org/html/2504.07093v1)
- The paper has no temporal consistency metric. The authors name residual flickering as the main limitation. Source: [FlashDepth paper](https://arxiv.org/html/2504.07093v1)
- Repo is Apache-2.0, weights on Hugging Face `Eyeline-Labs/FlashDepth`. It needs Python 3.11, PyTorch 2.4 (the README says Mamba2 breaks under torch 2.5+), a locally built mamba package and torch.compile. No ONNX, TensorRT or Windows support is mentioned. Source: [FlashDepth GitHub](https://github.com/Eyeline-Research/FlashDepth)
- Issue #1 "Unable to compile mamba under Windows ?" (2025-06-06) is still open. Issues #4 "very flickery results" and #11 "video depth obviously flickering background" were filed by users. Source: [FlashDepth issues](https://github.com/Eyeline-Research/FlashDepth/issues)
- Later papers measure FlashDepth as weaker than its own paper suggests. DyFN reports KITTI AbsRel 0.103 for FlashDepth against 0.079 for VDA. ViGeo reports Sintel Rel 0.361 for FlashDepth. Sources: [DyFN paper](https://arxiv.org/html/2605.25308), [ViGeo paper](https://arxiv.org/html/2605.30060)

#### 2026 arrivals
- DyFN ("Stabilizing Streaming Video Geometry via Dynamic Feature Normalization", arXiv 2605.25308) keeps the backbone frozen and adds a causal ConvGRU. The GRU state predicts a mean and a standard deviation through two 1x1 conv heads, and those re-modulate the normalised features. Trainable parameters are about 5M, roughly 2% of the total. It is trained on about 1M synthetic frames in 12-frame clips. Source: [DyFN paper](https://arxiv.org/html/2605.25308)
- DyFN on DAv2 raises Sintel video depth from AbsRel 0.367 / d1 55.4 to 0.242 / 64.8 without changing single-frame accuracy. On MoGe v1 it reaches Sintel 0.180 / 73.0, ScanNet 0.073 / 96.6, KITTI 0.062 / 97.3. The paper reports no inference speed, no GPU, and no code release. Source: [DyFN paper](https://arxiv.org/html/2605.25308)
- ViGeo (arXiv 2605.30060) is a 1.26B-parameter geometry model with chunked causal attention. One-frame chunks give strict streaming. It runs 10.00 FPS for 16 images at 518x518 on a 96 GB H20 and uses 23 GB at chunk size 16. Online KITTI Rel 0.049 beats FlashDepth 0.064 and StreamVGGT 0.173. Source: [ViGeo paper](https://arxiv.org/html/2605.30060)
- VeloDepth ("Video Depth Propagation", arXiv 2512.10725) is covered in section 5.
- VDPP (arXiv 2604.06665) is covered in section 3.
- OmniStream (arXiv 2603.12265, March 2026) is a general streaming visual backbone with a causal KV cache. Its abstract gives no depth or speed numbers. Source: [OmniStream abstract](https://arxiv.org/abs/2603.12265)
- A community ranking of TAE on 170-frame ScanNet lists FFN 0.380, GemDepth-VDA 0.47, VDA-L 0.570, DVD v1.1 0.61, DepthCrafter 0.639, RollingDepth 0.65, Depth Any Video 0.967, ChronoDepth 1.022, DAv2-L 1.140, NVDS 2.176. All entries above DAv2-L are marked multi-frame. The page does not say which are causal. Source: [AIVFI rankings](https://github.com/AIVFI/Video-Depth-Estimation-Rankings-and-Stereo-Video-Conversion-Rankings)

#### Offline or impractical models, briefly
- DepthCrafter runs about 2.1 fps at 1024x576 on an A100 with about 26 GB, or 8.6 fps at 512x256 with about 9 GB. Its licence restricts use to "academic, research and education purposes" and forbids "any commercial or production purposes". Sources: [DepthCrafter README](https://github.com/Tencent/DepthCrafter), [DepthCrafter LICENSE](https://github.com/Tencent/DepthCrafter/blob/main/LICENSE)
- DepthCrafter's defaults are 25 diffusion steps and a 25-frame overlap for videos longer than 110 frames. ChronoDepth's defaults are 10 denoising steps and window 10. RollingDepth fast processed 250 frames in 81 s against 121 s for ChronoDepth and 284 s for DepthCrafter. Source: [RollingDepth paper](https://arxiv.org/html/2411.19189v2)
- RollingDepth code is Apache-2.0 and its weights are RAIL++-M. Source: [RollingDepth GitHub](https://github.com/prs-eth/rollingdepth)
- Depth Any Video is CC BY-NC 4.0 and handles at most 192 frames. Sources: [DepthAnyVideo LICENSE](https://github.com/Nightmare-n/DepthAnyVideo), [VDA paper v3](https://arxiv.org/html/2501.12375v3)
- ChronoDepth's repo is MIT. Source: [ChronoDepth GitHub](https://github.com/jhaoshao/ChronoDepth)
- CUT3R and StreamVGGT are both CC BY-NC-SA 4.0. STream3R is S-Lab License 1.0, which allows redistribution "for non-commercial purpose" only. Sources: [CUT3R LICENSE](https://github.com/CUT3R/CUT3R), [StreamVGGT LICENSE](https://github.com/wzzheng/StreamVGGT), [STream3R LICENSE](https://github.com/NIRVANALAN/STream3R)
- StreamVGGT's KV cache is unbounded. A follow-up reports that StreamVGGT and VGGT show large FPS loss and run out of memory as frame count grows, and ViGeo reports StreamVGGT and STream3R out of memory on 300 to 400-frame sequences. Sources: [XStreamVGGT paper](https://arxiv.org/html/2602.21780), [ViGeo paper](https://arxiv.org/html/2605.30060)
- Buffer Anytime (CVPR 2025) fine-tunes image models such as DAv2 with added temporal attention and an optical-flow smoothness loss, with no paired video depth data. Source: [Buffer Anytime paper](https://arxiv.org/html/2411.17249v1)
- FutureDepth (Qualcomm, ECCV 2024) adds a future-prediction network and a masked reconstruction network that both run at inference. Source: [FutureDepth abstract](https://arxiv.org/abs/2403.12953)
- Depth Anything 3 licences: DA3-Small (0.08B) and DA3-Base (0.12B) are Apache 2.0, DA3Mono-Large and DA3Metric-Large (0.35B) are Apache 2.0, DA3-Large and Giant and the Nested model are CC BY-NC 4.0. DA3-Streaming (released 2025-12-11) is sliding-window inference for very long videos in under 12 GB. The README makes no temporal-consistency claim for monocular per-frame use and gives no FPS. Source: [DA3 README](https://raw.githubusercontent.com/ByteDance-Seed/Depth-Anything-3/main/README.md)

### Inferences
- VDA-S streaming is the only learned temporal model whose weights Unishade can ship under terms compatible with an MIT app. oVDA is technically the better fit (trained causal, 0.45 GB, bounded cache) but its non-commercial share-alike licence makes it a "user downloads it at their own risk" option at best, the way iw3 excludes CC-BY-NC models from its default set.
- oVDA's design is the easiest to export. Its state is a fixed-size FIFO of latents per motion module, so an ONNX graph can take the cache tensors as inputs and return the updated ones as outputs with static shapes. I found no published export that does this. The oVDA authors promise "compilation scripts" and reached 20 FPS FP16 on a Jetson, which implies a TensorRT-style path works.
- The same trick should work for VDA-S streaming because it also caches temporal-attention hidden states, but the only public ONNX export uses a fixed 32-frame window, and the upstream ONNX issues have sat open since early 2025. Expect to write the export.
- FlashDepth is the hardest to bring to Windows and DirectML. Mamba depends on a custom CUDA selective-scan kernel, the repo pins torch 2.4, and the Windows build issue is still open after 17 months. A single-step Mamba update is plain linear algebra and could be rewritten with standard ONNX ops, but nobody has published that.
- FlashDepth-L weights derive from Depth Anything V2 Large, which is CC-BY-NC-4.0. The repo says Apache-2.0. iw3 flags the same contradiction for Distill-Any-Depth. Only FlashDepth-S (from the Apache-2.0 DAv2-S) is free of that doubt. This is my reading, not a statement by Eyeline.
- The benchmark tables disagree wildly across papers because alignment protocols differ (per-sequence scale and shift, first-frame only, scale only). ViGeo's table gives VDA Sintel Rel 0.855 while VDA's own protocol gives d1 0.644. Do not compare numbers across papers.
- TAE needs camera poses and OPW needs optical flow, and Khan et al. note OPW is minimised when depth variance is low, so over-smoothing scores well. A game-specific test set with engine depth would be needed to rank these methods for Unishade.

### Gaps
- No source gives per-frame latency for VDA streaming mode on any GPU. oVDA's authors say frame-by-frame VDA is roughly 4 FPS apart from oVDA, which is not a clean number.
- "StableDepth" and "Stable Video Depth" returned nothing. I could not confirm such a model exists under that name.
- I found no real-time numbers, code or licence for FutureDepth or Buffer Anytime, and did not confirm whether Buffer Anytime's temporal attention is causal.
- StreamVGGT and CUT3R publish no FPS on their repo pages. The figures above come from competitor papers.
- DyFN speed, code and licence are unknown. Its project page is https://shawlyu.github.io/DyFN.
- No source measured any of these models through ONNX Runtime with DirectML, or on a consumer RTX card other than VeloDepth's RTX 3090 table.
- I did not check whether the 2026 ranking leaders FFN, GemDepth and DVD are causal. The ranking marks them multi-frame only.

## 2. What is deployable at 30 to 60+ fps on a consumer GPU, and how the streaming models keep state

### Takeaway
At ViT-S scale and around 0.25 megapixels, oVDA, FlashDepth-S and VDA-S all reach 40 to 95 FPS on an A100 with under 2 GB, so 30 to 60 fps on a mid-range RTX card is plausible but unmeasured. No learned video model comes near a 240 fps display rate, and the per-frame DAv2-S baseline (72 FPS on the same A100) is already only 1.7x faster than oVDA.

### Cited findings
- On one A100 at 280x924: DAv2-S 72 FPS (0.25 GB), FlashDepth-S 60 FPS (0.69 GB), oVDA 42 FPS (0.45 GB), FlashDepth 30 FPS (2.72 GB). VDA-S in batch mode reaches 95 FPS with 1.76 GB but with 232 ms latency because it waits for a clip. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- oVDA reaches 20 FPS in FP16 on a Jetson Orin NX with identical accuracy to FP32. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- VeloDepth reaches 26.2 FPS at 0.5 MP on an RTX 3090 in half precision, against 16.9 FPS for DAv2-L and 11.3 FPS for VDA-L on the same card. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- NVDS+ Small adds a 5.04M-parameter stabilizer and runs 35.17 FPS at 896x384 including MiDaS-Small, against 45.06 FPS for MiDaS-Small alone. The GPU for that table is not stated in the section. Source: [NVDS+ paper](https://ar5iv.labs.arxiv.org/html/2307.08695)
- VDPP adds 1.4 to 8.6 ms on an RTX A6000 and reaches 43.5 FPS on a Jetson Orin Nano 8 GB. Source: [VDPP paper](https://arxiv.org/html/2604.06665)
- State designs, as described by each paper:
  - FlashDepth keeps a Mamba hidden state on downsampled decoder features. Fixed memory, state updated every frame. Source: [FlashDepth paper](https://arxiv.org/html/2504.07093v1)
  - oVDA keeps a FIFO of past-frame latents (keys and values) per motion module, drops the oldest entry each frame, and uses plain self-attention until the window fills. Simulating a longer context with several caches did not help. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
  - VDA streaming caches temporal-attention hidden states with the offline weights, untrained for the causal case. Source: [VDA GitHub README](https://github.com/DepthAnything/Video-Depth-Anything)
  - DyFN keeps one ConvGRU hidden state and uses it only to predict feature statistics, so the backbone's spatial detail passes through untouched. ConvGRU beat a plain GRU (Sintel AbsRel 0.180 vs 0.187). Source: [DyFN paper](https://arxiv.org/html/2605.25308)
  - CUT3R reads and writes a learnable persistent scene state. StreamVGGT and STream3R keep every past token in a KV cache, which grows without bound. Sources: [StreamVGGT paper](https://arxiv.org/html/2507.11539v1), [XStreamVGGT paper](https://arxiv.org/html/2602.21780)
  - VDA offline fights long-range drift with key frames every 12 frames carried into the next clip. A user asked in issue #83 whether key-frame retention could close the streaming gap. Nobody has answered. Sources: [VDA paper v3](https://arxiv.org/html/2501.12375v3), [VDA GitHub issues](https://github.com/DepthAnything/Video-Depth-Anything/issues)
  - VeloDepth carries the previous depth map and neck features, warps both with flow, and re-runs the full base model on key frames about every 30 frames. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- Desktop2Stereo, an MIT real-time 2D-to-3D desktop tool, defaults to Depth-Anything-V2-Small at depth resolution 336, also offers Video Depth Anything and DA3 models, and uses TensorRT on NVIDIA and DirectML for other Windows GPUs. Its README documents no temporal filter. Source: [Desktop2Stereo README](https://github.com/lc700x/desktop2stereo)
- iw3's desktop streaming mode defaults to `Any_V2_S` and a 15 FPS stream rate, and warns that batch-size-1 processing makes it "considerably lower" in FPS than offline conversion. Source: [iw3 desktop docs](https://github.com/nagadomi/nunif/blob/master/iw3/docs/desktop.md)

### Inferences
- My estimate, not measured: an A100 is roughly an RTX 3090 Ti to 4080 class card for small-ViT FP16 inference. Scaling the A100 figures down, oVDA or FlashDepth-S at about 280x924 should land around 25 to 40 FPS on an RTX 4060-class card under TensorRT, and less under DirectML. While a game is using the same GPU, expect a further large cut. Unishade's current longest-side range of 266 to 924 px straddles this resolution, so the smaller presets would run faster.
- The temporal part costs 20 to 70% extra over the per-frame backbone at ViT-S scale (72 FPS down to 60 for Mamba, down to 42 for a 16-frame attention cache). A ConvGRU on feature statistics (DyFN) should cost less than either, but its authors give no number.
- A display rate of 120 to 240 fps cannot be met by running any of these every frame. The model has to run at a lower rate with reprojection in between (section 5), or the stabilisation has to be a shader-cost filter (section 3).
- Bounded state matters for a game session that runs for hours. Mamba state, a ConvGRU state and a fixed FIFO are all bounded. Raw KV caches are not, which rules out the StreamVGGT family even ignoring speed and licence.
- Neither practitioner tool (iw3, Desktop2Stereo) ships a learned real-time stabilizer of its own. Both lean on DAv2-S per frame and optional VDA models, which suggests the per-frame small model plus cheap filtering remains the practical baseline in late 2026.

### Gaps
- No public benchmark of oVDA, FlashDepth-S or VDA-S streaming on RTX 30/40/50 cards, on Windows, or under DirectML.
- Desktop2Stereo has a `Benchmark.md` that I did not read, so its per-GPU FPS figures are missing here.
- No data on how these models behave when the GPU is contended by a game.

## 3. Lightweight stabilization layered on a per-frame model

### Takeaway
Published evidence supports two cheap layers. Global scale and shift tracking fixes range pumping, and motion-compensated temporal accumulation with rejection fixes per-pixel shimmer. Learned stabilizers exist (NVDS+ Small at 5M params, VDPP at 1.4 to 8.6 ms, DyFN at about 5M trainable params), but the one with public weights, NVDS, made a modern backbone worse in an independent test.

### Cited findings

#### Learned stabilizers
- NVDS+ takes RGB plus initial disparity from any frozen depth model, in a 4-frame sliding window (target plus 3 previous frames, interval 1). Target tokens cross-attend to pooled windows from the reference frames. Large uses MiT-b5 (88.31M params, 254.53 G FLOPs for four 384x384 frames), Small uses MiT-b0 (5.04M params, 35.51 G FLOPs). Source: [NVDS+ paper](https://ar5iv.labs.arxiv.org/html/2307.08695)
- Forward-only NVDS+ gets most of the gain. On VDW with DPT, OPW goes from 0.470 (raw DPT) to 0.165 forward-only, 0.147 with bidirectional averaging and 0.129 with flow-guided bidirectional fusion. The Small model is forward-only. Bidirectional mode adds about 30% time and needs future frames. Sources: [NVDS+ paper](https://ar5iv.labs.arxiv.org/html/2307.08695), [NVDS GitHub](https://github.com/RaymondWang987/NVDS)
- NVDS+ Small with MiDaS-Small scores d1 0.622, OPW 0.236 on VDW at 35.17 FPS including the depth model. Source: [NVDS+ paper](https://ar5iv.labs.arxiv.org/html/2307.08695)
- NVDS code and model are MIT. The repo lists the NVDS+ paper but holds only NVDS code and checkpoints. The NVDS stabilizer needs about 5 GB at 896x384 on an RTX A6000. Source: [NVDS GitHub](https://github.com/RaymondWang987/NVDS)
- Independent tests are unkind to NVDS. In VDA's Table 1, NVDS on top of DAv2-L drops KITTI d1 from 0.815 to 0.617 and worsens TAE from 1.140 to 2.536. VDPP's table shows DAv2-L + NVDS at NYUv2 d1 0.620. VeloDepth's table gives NVDS a good consistency score (t5 82.6) with poor accuracy (d1 78.6 against 90.5 for DAv2). Sources: [VDA paper v3](https://arxiv.org/html/2501.12375v3), [VDPP paper](https://arxiv.org/html/2604.06665), [VeloDepth paper](https://arxiv.org/html/2512.10725)
- VDPP takes only depth maps (no RGB), downsamples by 0.5, builds a 3-channel input of depth plus Sobel x and y gradients, and predicts a residual with a small DINOv2 encoder and a small VDA head. A learnable "Differentiable Scaler" rescales each frame from its median depth by exp(tanh(-a*m+b)). Removing residual prediction hurts temporal error most (TGSE 1.56 to 1.80 on Sintel). Source: [VDPP paper](https://arxiv.org/html/2604.06665)
- VDPP on DAv2-L reaches Sintel AbsRel 0.412, d1 0.656, TGSE 1.563e-2 against NVDS on DAv2-L at 0.500, 0.574, 2.231e-2, and against end-to-end VDA-L at 0.3816, 0.6722, 1.2769e-2. Its repo is Apache-2.0. Sources: [VDPP paper](https://arxiv.org/html/2604.06665), [VDPP GitHub](https://github.com/injun-baek/VDPP)
- DyFN stabilises DAv2 by re-normalising features with a ConvGRU and leaves single-frame accuracy unchanged. Dropping its alignment loss costs the most (Sintel AbsRel 0.180 to 0.245). Dropping the temporal loss barely matters (0.183). Source: [DyFN paper](https://arxiv.org/html/2605.25308)

#### Motion-compensated accumulation and fusion
- Khan et al. (CVPR 2023) fuse the current per-frame depth with depth reprojected from a global point cloud, d_f = a*d_t + (1-a)*d_p, where a small CNN predicts the blend mask a so static regions keep history and moving regions take the new estimate. A second network blends spatially by learned confidence. The two networks take 7.20 ms and 5.10 ms at 512x512 on an RTX 3080, about 4.4M params each. Source: [Khan et al. 2023](https://arxiv.org/html/2304.07435v1)
- On Sintel with a DPT backbone that method lowers OPW from 0.424 to 0.255. On ScanNet OPW goes from 0.033 to 0.011 with accuracy unchanged. It needs known camera poses and intrinsics. The authors warn that OPW "is minimized when depth variance is low", so a = 0 gives smooth and wrong output that still scores well. Source: [Khan et al. 2023](https://arxiv.org/html/2304.07435v1)
- VeloDepth warps the previous depth and features with optical flow and corrects them with a gated residual. Flow from the CPU DIS algorithm works nearly as well as RAFT at test time (d1/t5 91.3/87.3 vs 91.4/87.2), and codec motion vectors come close (90.0/85.8). A two-layer conv refiner cuts DIS flow error from 8.5 px to 5.2 px EPE. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- PTC-Depth (2026 preprint) warps the previous frame's posterior depth into the current frame with estimated relative pose and runs 156 ms per frame including DAv2, per the search summary of the paper. Source: [PTC-Depth](https://arxiv.org/html/2604.01791)
- A 2026 MDPI paper (STF-Depth) reports a 25.70% consistency gain from post-processing without optical flow or retraining, per the search summary. Source: [STF-Depth](https://doi.org/10.3390/app16010212)

#### What the rendering literature says about temporal accumulation
- Yang, Liu and Salvi's survey splits TAA into sample accumulation and history validation. Source: [A Survey of Temporal Antialiasing Techniques](https://research.nvidia.com/labs/rtr/publication/yang2020survey)
- History rectification usually clamps or clips the reprojected history to the min and max of the current frame's 3x3 neighbourhood, which can leave box-shaped artefacts. Karis's anti-flicker variant lowers the blend toward the new sample the closer a pixel is to being clamped. Source: [TU Wien TAA report](https://www.cg.tuwien.ac.at/research/publications/2023/cech-2023-taa/cech-2023-taa-Report.pdf)
- Variance clipping (Salvi) replaces min/max with the neighbourhood mean and variance, which is more robust to outliers. Source: [Salvi, GDC 2016](https://developer.download.nvidia.com/gameworks/events/GDC2016/msalvi_temporal_supersampling.pdf)
- Blending pixels that belong to different objects after a disocclusion or warp error causes ghosting, and clamping the history is the standard fix. An NVIDIA patent describes a fast-history clamp whose blend weight trades noise against temporal lag. Source: [NVIDIA history clamping patent](https://patents.google.com/patent/US11783455)
- SVGF tracks per-pixel variance from past frames and falls back to a spatial variance estimate on disocclusion. A reimplementation resets history when fewer than 2 of the 4 nearest previous-depth texels pass a relative depth test. Sources: [SVGF preprint](https://cg.ivd.kit.edu/publications/2017/svgf/svgf_preprint.pdf), [three.js SVGF discussion](https://github.com/mrdoob/three.js/pull/32133/files)
- Time-of-flight camera pipelines use the same idea on real depth. A temporal filter averages static areas and reduces smoothing when local variance is high, and fast-moving objects blur when the filter is too strong. Source: [Electronic Specifier on ToF temporal filters](https://www.electronicspecifier.com/?p=206822)
- A stereo-perception study cited in the 2D-to-3D literature found that removing temporal frequencies above 3.6 Hz from a depth step "remains mostly unnoticed". Source: [StereoCueFusion, TVCG](https://resources.mpi-inf.mpg.de/StereoCueFusion/StereoCueFusionPaper_TVCG.pdf)

#### Practitioner filters
- iw3's only temporal filter is `--ema-normalize`, an exponential moving average on the depth range (min and max), with `--ema-decay` recommended between 0.75 and 0.99. The README warns that a high decay clips when the depth range changes suddenly. Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)
- Desktop2Stereo's README documents no temporal smoothing at all. Source: [Desktop2Stereo README](https://github.com/lc700x/desktop2stereo)

### Inferences
- A depth-specific TAA transfers well in principle, with one difference from colour TAA. Colour TAA clamps history to the current frame's neighbourhood because the current frame is trustworthy and merely aliased. Per-frame depth from a network is the thing that flickers, so clamping history hard to it reintroduces the flicker. The useful rejection signals are disocclusion (forward-backward flow mismatch, or large change in the colour frame at the reprojected position) and large relative depth disagreement, not a tight neighbourhood clamp. This is my reasoning from the sources, not a published result.
- Blend in inverse depth (disparity), the space DAv2 predicts and the space VDA uses for alignment. Accumulating in linear depth would let far-plane noise dominate. Log depth is a reasonable alternative but I found no source comparing the two for filtering.
- Unishade has no engine motion vectors, so it must estimate flow from colour frames. VeloDepth's result that DIS flow plus a tiny refiner nearly matches RAFT says cheap flow is good enough for depth propagation. A block-matching or DIS-style flow at the depth map's resolution (a few hundred pixels wide) is a small cost.
- Expected artefacts, by method, from the sources above: plain EMA without motion compensation trails behind moving objects and on camera pans (ToF source, oVDA's "trailing artefacts"), range EMA clips on sudden range change (iw3), flow-warped accumulation ghosts at disocclusions unless rejected (TAA literature), and long propagation over-smooths and shows a visible jump at each key frame (VeloDepth).
- NVDS weights are MIT and causal in forward mode, but the independent results say it damages DAv2 output. I would not ship it. NVDS+ Small looks right-sized (5M params) but has no public weights. VDPP is Apache-2.0 and cheap, but its window appears to use future frames and its temporal error is still worse than VDA-L.
- The strongest learned option for Unishade's exact case (DAv2 backbone, causal, tiny add-on) is the DyFN idea. Its ablation says the scale-alignment loss does the work and the temporal loss almost none, which matches the observation that most visible depth flicker in affine-invariant models is per-frame scale and shift jitter.

### Gaps
- I found no paper or project that applies SVGF or TAA-style accumulation with variance clamping directly to monocular network depth and reports numbers. The transfer is by analogy.
- No source compares a per-pixel Kalman filter, a temporal bilateral filter, and EMA on monocular depth. I found nothing citable on Kalman filtering of DAv2 output.
- The ICLR 2023 tiny paper "Efficient Temporal Denoising" on real-time depth flicker post-processing was behind a bot check and I could not read it: https://openreview.net/pdf?id=nazr0QFvHR
- PTC-Depth and STF-Depth are cited from search summaries only. I did not read them in full.
- No measured cost for DyFN's ConvGRU, and no released weights.

## 4. Keeping scale and shift from pulsing over time

### Takeaway
The published answers are to solve a least-squares scale and shift in inverse depth against already-emitted frames (VDA), to smooth the normalisation range with an EMA and reset it on scene cuts (iw3), to learn a per-frame scale correction (VDPP, DyFN), or to use a model trained for cross-frame scale consistency (oVDA, FlashDepth, metric variants). Drift over hundreds of frames remains unsolved in every streaming model.

### Cited findings
- VDA keeps one scale and shift for the whole video. When stitching clips it fits the new clip to the overlap frames by least squares over inverse depth. Source: [VDA paper](https://arxiv.org/pdf/2501.12375)
- VDA carries 2 key frames, sampled every 12 frames backward, into each new clip to reduce accumulated scale drift. Source: [VDA paper v3](https://arxiv.org/html/2501.12375v3)
- The original VDA normalises output with global min and max over the video. iw3's online path replaces that with a moving average. Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)
- iw3's `--ema-buffer` computes the range over a lookahead of future frames (30 frames is 1 s at 30 FPS, 150 is 5 s). A value of 1 gives the older causal behaviour. Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)
- iw3 resets both the VDA state and the min-max normalisation state at scene boundaries detected by TransNetV2, and "highly recommends" scene detection plus flicker reduction for VDA. Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)
- iw3 offers different normalisations for different displays. `Any_V3_Mono` uses a max scaler for VR and `Any_V3_Mono_01` a min-max scaler for anaglyph or 3D TV. Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)
- oVDA evaluates with a single scale and shift fixed from the first frame, a stricter test than per-sequence alignment, and reports drift as the AbsRel between the first frame's optimal scale and each later frame's. All online methods drift. oVDA drifts least, and "scale drift remains for extremely long sequences". Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- With global alignment oVDA scores KITTI AbsRel 0.103 on the first 500 frames against 0.140 with first-frame alignment, which shows how much of the error is scale and shift drift. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- VDPP adjusts each frame's global scale from its median depth with a learned function. Using the mean instead of the median was worse (Sintel AbsRel 0.4325 vs 0.4142). Source: [VDPP paper](https://arxiv.org/html/2604.06665)
- NVDS+ normalises the initial depth maps within its sliding window to cope with the differing scale and shift of each predictor, per the search summary of the paper. Source: [NVDS+ paper](https://arxiv.org/pdf/2307.08695)
- DyFN applies its correction to feature statistics inside the network. Without its alignment loss, ScanNet AbsRel rises from 0.073 to 0.124. Source: [DyFN paper](https://arxiv.org/html/2605.25308)
- Metric models are steadier per frame than their accuracy suggests. TAE on ScanNet: UniDepthV2-L 1.41, VDA-S-Metric 1.48, VDA-B-Metric 1.26, VDA-L-Metric 1.09, MoGe-2-L 2.56, DepthPro 2.73. VDA-S-Metric has the same 28.4M parameters as VDA-S. Source: [VDA GitHub README](https://github.com/DepthAnything/Video-Depth-Anything)
- VeloDepth reports that global scale jitter still affects its metric output, and that key-frame "jumps" stay visible. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- FlashDepth trains with an L1 loss, not a scale-and-shift-invariant loss, and a user issue discusses the choice. Sources: [FlashDepth paper](https://arxiv.org/html/2504.07093v1), [FlashDepth issue #10](https://github.com/Eyeline-Research/FlashDepth/issues)

### Inferences
- Unishade's smoothed global peak is a one-parameter version of iw3's EMA range. It fixes scale only, and a single peak statistic is fragile because one near pixel (a weapon model, a particle, a HUD element) moves it. The cited work points to two upgrades. First, track both scale and shift. Second, estimate them from many pixels: either percentiles (VDPP's median result favours robust statistics over means) or a least-squares fit of the new frame's inverse depth to the flow-warped previous output, as VDA does across clips.
- A least-squares fit to the previous output is recursive, so errors accumulate and the range can walk. That is the drift oVDA and VDA describe. The VDA remedy is key frames. For a causal filter that means also fitting against an older anchor frame, or pulling the tracked scale slowly back toward the raw model output.
- The fit should exclude pixels that fail the flow consistency check and should be robust (trimmed or iteratively reweighted), because moving objects and disocclusions violate the assumption that depth is unchanged. No source gives a recipe for this on games.
- iw3's lookahead buffer is a clean demonstration of the causal penalty. Its authors added future frames precisely because a purely causal EMA either lags or clips. Unishade cannot use lookahead, so it needs cut detection and a fast-adapt path instead (section 6).
- Anchoring to a metric model is plausible but unproven for games. VDA-S-Metric is the same size as VDA-S, but its d1 on NYUv2 is 0.850 against 0.989 for UniDepthV2-L, its training data is Virtual KITTI and IRS, and its licence is not stated in the README. Game cameras with unusual FOV would stress it.

### Gaps
- No published comparison of percentile normalisation against least-squares alignment against EMA for real-time relative depth.
- No source describes RANSAC-based scale-shift alignment between consecutive frames in a real-time tool. The idea is standard in evaluation code but I found no real-time write-up.
- The licence of the VDA metric checkpoints is not stated on the README I read.
- I found nothing on how scale stability affects a downstream neural renderer.

## 5. Running depth below display rate and reprojecting to intermediate frames

### Takeaway
Yes, and VeloDepth (arXiv 2512.10725, code released January 2026) is the published proof. It runs the full depth model on about one frame in thirty, propagates with CPU optical flow and a learned residual in between, and scores higher temporal consistency than every per-frame and video model in its table at 26 FPS on an RTX 3090. Its weak spots are large motion and visible jumps at key frames, and its licence is non-commercial.

### Cited findings
- VeloDepth warps the previous depth prediction and neck features forward with optical flow, then applies a learned gated residual from a ConvNeXt-Tiny encoder with RGB, depth and flow branches. The propagation module is about 55M of 409M parameters. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- Initial flow comes from the CPU DIS algorithm, and the paper says codec or engine motion vectors can be substituted. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- A key frame (full base-model inference) fires when the warped coverage falls to 0.2*0.9^t or flow magnitude exceeds 0.15*0.9^t + 0.1, with t the frames since the last key frame. The average interval is about 30 frames. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- RTX 3090, 0.5 MP, half precision: VeloDepth 26.2 FPS at 0.7 T FLOPs, against its base-model class at 12 to 13 FPS, DAv2-L 16.9 FPS, VDA-L 11.3 FPS. The supplement claims over 60 FPS at 640x360. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- Consistency (t5, share of pixels within 5% after flow warp, scale-shift aligned) and accuracy (d1), aggregated over TUM, ScanNet, Sintel, Bonn: VeloDepth 91.4 / 87.5, VDA 92.2 / 80.7, DAv2 90.5 / 74.5, UniDepthV2 93.8 / 66.1, NVDS 78.6 / 82.6. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- Test-time flow source barely matters: motion vectors 90.0 / 85.8, DIS 91.3 / 87.3, RAFT 91.4 / 87.2. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- Stated limits: key-frame errors propagate, accuracy degrades when large motion dominates, key-frame jumps are visible (Sintel close-ups), and over-smoothing grows with longer key-frame intervals. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- The repo `lpiccinelli-eth/velodepth` is CC BY-NC-SA 4.0. Source: [VeloDepth GitHub](https://github.com/lpiccinelli-eth/velodepth)
- A patent on video depth notes that errors in optical flow become artefacts in warped depth maps, and another skips flow-based depth at scene changes. Source: [USPTO 12657748](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12657748)
- A 2025 mobile-AR study reuses sensing results across frames by warping between view poses and finds foundation-model depth warping robust across gaps of 10 frames. Source: [arXiv 2511.02215](https://arxiv.org/pdf/2511.02215)
- A 2020 paper reaches dense depth at up to 30 FPS by warping a previous depth map toward the current image without dense optical flow. Source: [arXiv 2002.00297](https://arxiv.org/html/2002.00297)
- Older work selects key frames when accumulated dense-flow magnitude passes a threshold and checks forward-backward flow consistency between key-frame pairs. Source: [arXiv 2208.02709](https://arxiv.org/pdf/2208.02709)
- iw3 handles high-frame-rate sources by capping processing at 30 FPS by default (`--max-fps`), with no depth reprojection. Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)

### Inferences
- The sources support the claim that propagation is more temporally stable than re-inference, because the warped depth inherits the previous frame's values and the network only adds corrections. For shimmer in SSAO or depth of field this is the desirable failure direction: stable and slightly stale beats sharp and flickering.
- A non-learned version fits Unishade's constraints. Run DAv2-S (or a streaming model) on every Nth captured frame, compute cheap flow between consecutive colour frames on the GPU, warp the last depth forward each display frame, and blend in each new inference with the scale-shift fit from section 4. VeloDepth's learned residual is what keeps warped depth accurate over 30 frames. Without it I would expect a usable interval of 2 to 4 frames, which matches the range in the brief. This is my estimate.
- Forward warping leaves holes at disocclusions. VeloDepth fills them with its residual network. A shader pipeline would need to fill from the far side (background depth) or from the next inference. At 2 to 4 frames between inferences the holes are thin at 60+ fps.
- Asynchronous inference introduces latency equal to the model's run time, so the fresh depth describes a frame that is already 1 to 3 display frames old. It must be warped forward by the accumulated flow before blending. None of the sources addresses this asynchronous case.
- A blend at each new inference avoids VeloDepth's key-frame "jump" problem. A hard swap at each inference would produce a pulse at the inference rate, which at 15 to 30 Hz is the worst flicker frequency for visibility.

### Gaps
- No published system runs a monocular depth network at a fraction of display rate and reprojects with plain shader-side flow warping (no learned residual) while reporting quality numbers.
- No data for intervals of 2 to 4 frames. VeloDepth reports only its adaptive scheme averaging 30.
- I found no write-up of this technique in VR or 2D-to-3D community tools. A narrower search on "asynchronous depth" or "timewarp depth" may find some.
- VeloDepth did not test hard scene cuts.

## 6. Scene cuts, fast camera rotation and HUD or UI overlays

### Takeaway
Scene cuts have a documented practice: detect them and reset all temporal state, as iw3 does with TransNetV2. Fast motion is a known failure for every streaming and propagation method, with no fix beyond falling back to fresh per-frame output. I found no published treatment of HUD or UI overlays in monocular depth at all.

### Cited findings
- iw3 runs TransNetV2 scene boundary detection first and at each boundary resets the Video Depth Anything state and the min-max normalisation state. Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)
- A video-language model paper resets its recurrent state at each detected shot, using HSV-histogram Bhattacharyya distance with threshold 0.35. Source: [arXiv 2608.04866](https://arxiv.org/pdf/2608.04866)
- Depth Any Video used PySceneDetect (colour-change based) to find scene transitions in game-engine training footage and filtered those frames, because image and depth were misaligned around menu switches. Source: [Depth Any Video paper](https://arxiv.org/html/2410.10815)
- A video world-model evaluation separates a cut, which "jumps instantaneously with zero transitional frames", from a fast pan, which has transitional frames. Source: [arXiv 2603.13215](https://arxiv.org/pdf/2603.13215)
- VeloDepth's key-frame trigger doubles as a motion and cut detector. It fires when too few pixels survive the warp or flow magnitude is too large, and the thresholds tighten with time since the last key frame. Source: [VeloDepth paper](https://arxiv.org/html/2512.10725)
- Fast motion breaks streaming models. FlashDepth's residual flicker shows "particularly in scenes with rapid motion", oVDA shows trailing artefacts on fast-moving objects, and VeloDepth's accuracy degrades when large motion dominates. Sources: [FlashDepth project summary](https://eyeline-labs.github.io/FlashDepth), [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182), [VeloDepth paper](https://arxiv.org/html/2512.10725)
- oVDA trains with random rectangles zeroed out over 0 to 40% of each frame's pixels as augmentation. Source: [oVDA paper](https://ar5iv.arxiv.org/html/2510.09182)
- iw3 warns that a high EMA decay causes clipping "when the depth range changes suddenly". Source: [iw3 README](https://github.com/nagadomi/nunif/blob/master/iw3/README.md)
- The TAA literature treats a failed reprojection the same way everywhere: detect it and drop or clamp the history for those pixels. Source: [NVIDIA history clamping patent](https://patents.google.com/patent/US11783455)

### Inferences
- Unishade can get cut detection nearly free from data it already computes. The flow-validity fraction (VeloDepth's signal), a downsampled luma histogram distance, and the residual of the scale-shift fit all spike on a cut. When any does, reset the accumulated depth, the scale-shift tracker, and any model hidden state, and output the raw per-frame depth for that frame. TransNetV2 is a separate network and more than a game overlay needs.
- A fast pan is not a cut, but the right response is similar and graded. Raise the blend weight of the new inference as flow magnitude or the rejection fraction rises, the way Karis-style TAA shifts toward the current sample near the clamp. This trades shimmer for lag only when the eye is least able to see shimmer.
- Games produce cut-like events that films do not: menu open and close, loading screens, death cams, instant teleports, scope zoom. Depth Any Video's observation about menu switches says these frames are also where depth labels are unreliable, so resetting on them is correct.
- HUD elements are static in screen space, have zero flow, and are placed by the depth model at arbitrary depths. They corrupt a peak-based normaliser (a crosshair or minimap can become the nearest thing in frame) and they pin temporal history in place while the world moves behind them. A cheap detector is the per-pixel persistence of colour while the frame's dominant flow is nonzero. Masked pixels should be left out of the scale-shift fit and of percentile statistics. What depth value to give them for the shader is a product decision outside this research. All of this is my reasoning. No source covers it.
- oVDA's rectangle-masking augmentation suggests its temporal head tolerates occluding boxes better than untrained streaming VDA, though the authors did not test UI overlays.

### Gaps
- I found no paper, repo or forum post on HUD or UI handling for monocular depth on game captures. This is an open design question.
- No source gives thresholds for telling a fast camera rotation from a cut in game footage.
- No source measures how long each streaming model takes to recover after a cut when its state is not reset.
- Nothing found on feeding monocular depth into NVIDIA DLSS 5 neural rendering or on what temporal stability it would require.
