# RVX-NG · Feature/Media & Video Playback 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 Asset、Audio、Render、Sequencer、Localization  
**里程碑**：M9 / M10 / M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义视频、音频流、过场媒体、纹理视频、字幕/音轨、平台硬解和同步机制。Media 系统必须能服务开场/过场、UI 视频、in-world video surface、cutscene audio、localization subtitle，并能在不同平台硬件能力下可靠降级。

**范围内**：media asset、container/codec profile、decode backend、audio/video sync、subtitle cue、video texture、streaming、Sequencer track、platform hardware decode。  
**范围外**：视频编辑器、平台 DRM 商业授权、直播推流服务。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 平台分级 | codec/container/hardware decode 由 Platform Profile/caps 决定 |
| 同步 | audio/video/subtitle 使用统一 media clock，可与 Sequencer 对齐 |
| 可降级 | unsupported codec、解码失败、带宽不足有 fallback |
| 线程隔离 | decode 不阻塞 audio thread、render submit 或 game thread |
| 可本地化 | 多音轨、多字幕、caption metadata 与 Localization/Accessibility 对接 |

## 3. 公共接口面（设计契约）

```cpp
enum class MediaState : uint8 {
    Closed,
    Opening,
    Ready,
    Playing,
    Paused,
    Ended,
    Error
};

struct MediaOpenDesc {
    AssetId mediaAsset;
    LocaleId locale;
    MediaQualityTier quality;
    bool preferHardwareDecode = true;
};

class MediaPlayer {
public:
    AsyncResult<void> Open(MediaOpenDesc desc);
    void Play();
    void Pause();
    void Seek(Timecode time);
    MediaState State() const;
    TextureHandle CurrentVideoTexture() const;
};
```

**不变量**：media asset 必须通过 cook 产平台可播放 profile；runtime 不使用源视频格式做临时转码；视频纹理更新通过 RHI/RenderGraph 明确同步；字幕 cue 走 Localization/Accessibility。

## 4. 数据结构与内存布局

- **Media Asset**：container、codec、duration、tracks、localized variants、poster frame、fallback asset。
- **Media Profile**：platform codec、bitrate、resolution、HDR/SDR、audio layout、subtitle format。
- **Decode Session**：backend、state、decode queue、timestamp、buffer pool、error state。
- **Video Frame Pool**：YUV/RGBA surfaces、GPU import handle、fence/timeline。
- **Media Clock**：playback time、rate、drift、sync policy、Sequencer binding。
- **Subtitle Cue**：time range、localized text key、speaker、caption style。

## 5. 核心算法

- **Cook Transcode**：source media → platform media profiles → package chunks → metadata/track table。
- **Open**：resolve locale/profile → select decode backend → allocate frame/audio buffers → pre-roll。
- **A/V Sync**：audio clock primary for voiced media；video frames dropped/repeated within tolerance。
- **Video Texture Update**：decoded frame → GPU surface/import/upload → RenderGraph external texture。
- **Sequencer Track**：timecode binds to media clock；scrub uses keyframe seek or proxy frames。
- **Subtitle Routing**：media cue → Localization resolve → Accessibility caption UI。

## 6. 线程与内存模型

decode backend 使用专用 media job/平台 callback，不在 audio realtime thread 做 blocking IO。video frame pool 由 media system 持有，render 只读取当前 ready frame。seek/open/close 通过 command queue 串行化，避免 backend 状态竞态。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| codec unsupported | cook/profile fail 或 runtime fallback 到低档 asset |
| decode error | 显示 poster/fallback frame，audio 静音或跳过 |
| IO underrun | pause/prebuffer 或降 bitrate |
| subtitle 缺失 | 不显示字幕并记录 localization miss |
| hardware decode 不可用 | fallback software decode；超预算则使用 lower profile |
| seek 失败 | 跳到最近 keyframe 或返回错误 |

**禁止**：Shipping 播放未 cook source video；decode 阻塞 audio/render/game thread；字幕文本绕过 Localization。

## 8. 性能目标

- 常规播放不引入 render thread stall。
- 视频纹理上传/导入成本进入 FrameStats。
- media buffer 内存按 Platform Profile 限额，超限降 bitrate/resolution。
- seek/pre-roll 时间有 profile 目标并可观测。

## 9. 测试计划

- media open/play/pause/seek/end/error state machine。
- audio/video/subtitle sync drift fixture。
- hardware unavailable、codec unsupported、IO underrun fault injection。
- video texture RenderGraph integration golden。
- localized subtitles、caption accessibility routing。

## 10. 开放问题 / Spike

- 首批 container/codec：MP4/H.264/H.265/VP9/AV1 的平台矩阵。
- 是否集成 FFmpeg 作为工具/cook 或 runtime fallback。
- HDR video 与 HDR swapchain/color pipeline 的映射。
- DRM/protected video 是否只留平台接缝。

## 11. 依赖与被依赖

- **依赖**：Asset Pipeline、Platform Profile、RHI/RenderGraph、Audio、Localization/Accessibility、Sequencer、Streaming。
- **被依赖**：Cinematic、UI video、in-world video surface、Shipping intro/legal videos、Accessibility captions。
