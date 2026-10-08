# Kodi (Omega 21.3) — CoreELEC Amlogic 优化分支 / Enhanced Branch

<p align="center">
  <img src="docs/resources/banner.png" alt="Kodi Logo" width="500"/>
</p>

<p align="center">
  <a href="#zh-cn"><img alt="Chinese" src="https://img.shields.io/badge/Language-中文说明-blue.svg?style=flat-square"></a>
  <a href="#en-us"><img alt="English" src="https://img.shields.io/badge/Language-English-green.svg?style=flat-square"></a>
  <a href="LICENSE.md"><img alt="License" src="https://img.shields.io/badge/License-GPLv2-lightgrey.svg?style=flat-square"></a>
  <a href="https://kodi.tv/"><img alt="Kodi Base" src="https://img.shields.io/badge/Kodi-21.3%20Omega-orange.svg?style=flat-square"></a>
</p>

---

<a id="zh-cn"></a>
## 🇨🇳 中文说明

### 项目简介
本项目基于官方 **Kodi 21.3 Omega** 开发，面向 CoreELEC Amlogic 平台（S905X4、S922X-J、A311D2 等芯片）。在社区 `avdvplus` R10 版本的基础上，针对杜比视界双层解码、HDR Vivid 处理、蓝光原盘导航、网络流媒体及显示解复用进行了多项底层修复与改进。

---

### R10 版本后的主要修改内容

#### 1. 杜比视界 Profile 7 (FEL / MEL) 双层解码体系重构
* **PTS 唯一基准两阶段配对**：以 PTS（呈现时间戳）为绝对基准对齐 BL（基础层）与 EL（增强层）；仅在时间戳缺失时回退至保序排队，避免因 DTS 抖动或时钟漂移导致的配对失锁与死锁。
* **解决无缝切段（Seamless Branching）丢帧卡死**：针对蓝光多段衔接处上游播放器抹除 BL 时间戳的特性，引入 BL 时间戳基准锚定与外推机制，消除切段处的帧丢失与卡顿。
* **优化跳章与快进响应**：放宽解码器在 Seek 期间的截断逻辑，改善跳转后的时序重对齐，缩短缓冲等待时间。
* **单轨异构 FEL 降级保护**：识别缺少 SPS 参数的非标单轨 FEL 视频（如部分压制片源），自动降级为 MEL 模式运行，避免硬件双核解码器因时序脱节发生死锁或画面重影。
* **支持 MP4 双轨杜比视界**：补充支持 MP4 容器下的双轨 DV（如流媒体 CMv4.0 规格），防止被误识别为普通 HDR10。

#### 2. HDR Vivid 支持与实时转码
* **实时转码为杜比视界 RPU**：内置 HDR Vivid 动态元数据解析器，在解码时将其动态映射曲线实时转换为 Dolby Vision RPU (Profile 8.1)，使不支持 HDR Vivid 的杜比视界显示设备可呈现动态色调映射。
* **HLG SEI 透传修复**：修复 SEI prefix fast-path 遗漏 147 标识的问题，避免广播 HLG 片源被错误识别为 SDR。

#### 3. 蓝光与 ISO 镜像播放优化
* **移植 Kodi v22 蓝光架构**：同步 Kodi v22 的 `MPLSParser`、`M2TSParser`、虚拟目录结构及 `DiscDirectoryHelper`。
* **主文件模式秒开**：设置为“播放主文件”时，直接定位正片并跳过全盘 MPLS 网络遍历，显著降低云盘与网络挂载场景下的起播耗时。
* **基于 `bd_get_main_title` 启发式选轨**：综合时长、章节完整度及高清音轨权重选择正片，规避混淆播放列表（ScreenPass）。
* **镜像缓存与异常修复**：为光盘镜像启用 `CFileCache` 数据缓存；修复网络 `udf://` 镜像误触发物理光驱命令导致的崩溃；新增智能 AUTO 菜单模式。

#### 4. 网络流与云盘稳定性
* **CDN 限流与鉴权恢复**：限制网络直链并发连接以降低 403 风险；快进跳转遇 4xx 错误时自动重签并恢复连接。
* **移除冗余黑边检测**：彻底移除在网络流播放时频繁发起随机读取的 `ActiveAreaDetector`，减少无谓的网络 I/O 阻塞。

#### 5. 画面解复用与播放引擎
* **修复 4K UHD 原盘 48i 隔行误判**：修复 4K 23.976p HEVC 因 `tbr=47.952` 被误判为 48i 隔行扫描、导致 HDMI 白名单失配并被迫降级为 1080p60 的缺陷。
* **平滑快进设置**：新增最大平滑快进倍速选项，优化快进期间的音频透传（Passthrough）握手状态。
* **解码收尾缓冲排水**：改进 `AMLCodec` 播放结束时的排水处理，避免视频片尾被提前截断。

#### 6. UI 与海报墙性能
* **视口优先加载**：海报墙与列表视图优先加载当前可视区域（Viewport）内的材质与元数据，提升大库滑动流畅度。

---

### 使用建议
> 提示：部分高级设置项需在 Kodi 设置界面左下角将视图模式切换为 **“高级 (Advanced)”** 或 **“专家 (Expert)”** 才会显示。

1. **HDR Vivid 转杜比视界**：位于 `设置 -> 播放器 -> 视频`，按需开启“转换 HDR Vivid 为杜比视界”。
2. **蓝光播放模式**：位于 `设置 -> 播放器 -> 光盘`，推荐设为“自动”或“播放主文件”。

---

### 编译说明
本项目通过 CoreELEC 构建系统统一编译：

```bash
cd ~/Project/CoreELEC/coreelec
# 仅重新编译 Kodi
PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm scripts/build kodi:target
# 构建完整固件
PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm make release
```

---
---

<a id="en-us"></a>
## 🇬🇧 English Documentation

### Introduction
This project is an enhanced branch of **Kodi 21.3 Omega** tailored for Amlogic SoCs (such as S905X4, S922X-J, A311D2) running on CoreELEC. Building upon the community `avdvplus` R10 release, it provides targeted bug fixes and architectural improvements across Dolby Vision dual-layer decoding, HDR Vivid processing, Blu-ray disc navigation, cloud/network streaming, and video demuxing.

---

### Key Updates Since R10

#### 1. Dolby Vision Profile 7 (FEL / MEL) Dual-Layer Overhaul
* **PTS-Only Two-Stage Pairing**: Uses Presentation Timestamp (PTS) as the sole matching reference between Base Layer (BL) and Enhancement Layer (EL); falls back to ordered queue pairing only when timestamps are absent, avoiding pairing desync caused by DTS jitter or clock drift.
* **Seamless Branching Fix**: Addresses timestamp stripping by the upstream player at disc segment seams with BL timestamp anchoring and extrapolation, eliminating frame drops and playback freezes across multi-segment titles.
* **Optimized Seek & Chapter Skipping**: Relaxes aggressive decoder truncation during seek operations and improves clock realignment, reducing buffering delay after chapter skips.
* **ST-DL Heterogeneous FEL Fallback**: Detects non-standard single-track FEL streams with missing or delayed SPS headers and automatically falls back to single-core MEL mode to prevent hardware decoder deadlocks and visual ghosting.
* **MP4 Dual-Track DV Support**: Added support for dual-track Dolby Vision in MP4 containers (e.g., streaming CMv4.0 profiles), preventing incorrect fallback to standard HDR10.

#### 2. HDR Vivid Support & Real-Time Transcoding
* **Real-Time Conversion to Dolby Vision RPU**: Parses HDR Vivid dynamic metadata and converts tone-mapping curves on the fly to Dolby Vision RPU (Profile 8.1), enabling dynamic tone mapping on Dolby Vision displays that do not natively support HDR Vivid.
* **HLG SEI Passthrough Fix**: Corrects an omission in the SEI prefix fast-path where type 147 metadata was discarded, preventing broadcast HLG content from being misidentified as SDR.

#### 3. Blu-ray & Disc Image Optimization
* **Backported Kodi v22 Disc Infrastructure**: Incorporates `MPLSParser`, `M2TSParser`, virtual directory handling, and `DiscDirectoryHelper` from Kodi v22.
* **Instant Direct Play for Main Title**: Directly locates the main feature without full-disc MPLS traversal over UDF when "Play main title" is selected, significantly reducing startup latency over cloud storage and network shares.
* **Heuristic Playlist Selection via `bd_get_main_title`**: Uses duration, chapter completeness, and high-definition audio track priorities to identify the true main title, bypassing obfuscated playlists (ScreenPass).
* **Disc Cache & Stability Fixes**: Enables `CFileCache` for disc images; fixes a crash where virtual `udf://` mounts invoked physical drive routines (`bd_open_disc`); adds an automatic (AUTO) menu mode.

#### 4. Cloud Drive & Network Streaming Resilience
* **CDN Rate Limiting & Re-authentication**: Limits concurrent requests to avoid CDN access bans; automatically re-authenticates and resumes connections when encountering 4xx errors during seek.
* **Removed ActiveAreaDetector**: Completely removed legacy black-bar detection code that performed frequent out-of-order range requests over network connections.

#### 5. Video Demuxing & Playback Engine
* **Fixed 4K UHD 48i Interlaced Misdetection**: Corrected an issue where 4K 23.976p HEVC streams with `tbr=47.952` were misidentified as 48i interlaced, which caused HDMI display whitelist mismatches and forced a resolution downgrade to 1080p60.
* **Smooth Fast-Forward Setting**: Added a configurable maximum speed limit for smooth fast-forward and improved audio passthrough stability during speed changes.
* **Decoder Buffer Draining**: Refined `AMLCodec` buffer drain handling at playback end to prevent movies from cutting off prematurely.

#### 6. UI & Library Performance
* **Viewport-Prioritized Loading**: Prioritizes textures and metadata for items currently visible in the active viewport, improving scrolling responsiveness in large media libraries.

---

### Configuration Notes
> Note: Some settings require switching the settings level to **"Advanced"** or **"Expert"** via the gear icon in the bottom-left corner of the Kodi settings window.

1. **HDR Vivid to Dolby Vision Conversion**: Located in `Settings -> Player -> Video`. Toggle "Convert HDR Vivid to Dolby Vision" as needed.
2. **Blu-ray Playback Mode**: Located in `Settings -> Player -> Discs`. "Auto" or "Play main title" is recommended.

---

### Build Instructions
This package is built as part of the CoreELEC build environment:

```bash
cd ~/Project/CoreELEC/coreelec
# Rebuild Kodi package only
PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm scripts/build kodi:target
# Build full firmware release
PROJECT=Amlogic-ce DEVICE=Amlogic-ng ARCH=arm make release
```
