# RVX-NG · 能力测试矩阵

**日期**：2026-06-27  
**状态**：v1.0 门禁映射草案

本文把 `capability-tiering.md` 中的能力分级映射到测试门禁。它不是完整测试用例列表，而是实施方案和 CI 设计必须满足的最小覆盖矩阵。

## 1. 门禁规则

- Proven 能力必须有 contract/integration/perf 覆盖，渲染 Proven 还必须有至少一个 golden 或 buffer reference fixture。
- Advanced 能力必须有开关测试、fallback 测试和 FrameStats/Profiler 可观测项。
- Research 能力必须证明关闭后 M0-M11 主线仍可运行、可测试、可出帧。
- Unsupported capability 必须返回 false 或 SKIP reason；SKIP 不是 PASS。
- Shipping profile 不允许 runtime shader compile、source asset 读取、Editor/Test 模块依赖和静默 fallback。

## 2. 全局矩阵

| 能力 | Tier | Fallback | 最小测试 | CI lane | Artifact |
|---|---|---|---|---|---|
| Task graph/work stealing | Proven | 单线程 executor | unit + stress + dependency chain | PR | trace + counter stats |
| Deterministic fixed slicing | Proven | 串行 deterministic lane | replay hash | PR/nightly | seed + state hash |
| Fiber backend | Research | continuation API | backend switch + profiler view | scheduled | trace diff |
| Archetype/SoA hot path | Proven pending S1 | sparse/hybrid path if chosen | 1M entity query + churn | nightly | perf table |
| Authoring facade | Proven | direct data API | roundtrip + lifetime test | PR | serialized fixture |
| RHI caps honesty | Proven pending S3 | false caps + lower path | capability fixtures | PR/nightly | caps report |
| Bindless descriptors | Proven where supported | descriptor table emulation/limited path | allocation/lifetime/invalidation | PR/nightly | descriptor stats |
| Timeline queues | Proven | single queue path | cross-queue wait/signal | nightly | GPU timestamp trace |
| RenderGraph barriers | Proven | explicit pass ordering | barrier golden | PR/nightly | graph dump |
| RenderGraph async compute | Advanced pending S4 | graphics queue path | async on/off output compare | nightly | queue timing |
| RenderGraph aliasing | Advanced pending S4 | no aliasing | alias correctness + memory stats | nightly | resource lifetime graph |
| Meshlet GPU-driven geometry | Proven pending S5 | CPU/reference culling fixture | CPU compare + GBuffer golden | nightly | draw args + image diff |
| Mesh shader path | Advanced | ExecuteIndirect path | caps-gated render fixture | scheduled | backend report |
| VisBuffer/software raster | Research | meshlet deferred path | disabled-path unchanged | scheduled | image diff |
| Probe/烘焙/SSGI GI | Proven | ambient fallback | low-tier GI golden | PR/nightly | image diff |
| Radiance Cache GI | Advanced pending S6 | Probe/SSGI fallback | RT on/off + stability + perf | nightly/scheduled | GI debug atlas |
| ReSTIR GI | Research | Radiance Cache/Probe | disabled-path unchanged | scheduled | reservoir stats |
| Virtual Shadow Maps | Proven | cascaded/atlas shadow fallback if required | page cache + shadow golden | nightly | page stats + diff |
| RT shadows/reflections | Advanced | VSM/SSR/probe | RT on/off compare | scheduled | AS stats |
| Material system | Proven | error material + typed fallback textures | graph cook + parameter layout + PSO enumeration | PR/nightly | material diagnostics |
| Asset cook/DDC | Proven pending S8 | local cook cache | GUID stability + cache hit/miss | PR/nightly | cook manifest |
| Virtual texture/geometry streaming | Advanced pending S7 | resident mip/proxy asset | flythrough no black page/hard fail | nightly | residency trace |
| World partition | Proven | monolithic test world | cell load/unload + stable ID | nightly | cell event log |
| Camera/View runtime | Proven | default spawn camera / last valid camera | blend/modifier/ViewSet contract | PR/nightly | camera trace |
| Gameplay framework seams | Proven | safe map/spectator/default profile | join/spawn/possess/travel/HUD bridge fixtures | PR/nightly | gameplay event log |
| Physics fixed-step | Proven pending S9 | non-deterministic gameplay boundary | replay hash + collision fixture | nightly | state hash |
| Networking prediction/rollback | Advanced pending S9 | server-authoritative snapshot only | packet fuzz + reconciliation | scheduled | packet log |
| Online platform services | Proven where provider exists | offline/null provider | mock auth/session/cloud/permission tests | PR/nightly | provider event log |
| Media playback | Proven where platform codec exists | poster/fallback frame + muted audio | play/seek/sync/codec fallback/subtitle routing | nightly | media diagnostics |
| UI text shaping/CJK/RTL/IME | Proven pending S10 | debug key/tofu fallback | text golden + IME fixture | nightly | glyph atlas dump |
| Editor PIE world fork | Proven pending S11 | isolated launched runtime | contamination + transaction rollback | nightly | world diff |
| Team source control | Proven | read-only/offline local policy | checkout/lock/merge/pre-submit fixtures | PR/nightly | changelist manifest |
| Golden image tolerance | Proven pending S12 | headless contract + smoke golden | tolerance profile validation | nightly | reference/diff bundle |
| Build manifest/profile audit | Proven pending S13 | backend-exported graph validator | include/profile/shipping audit | PR | graph report |
| Project package/plugin | Proven | locked local package set | dependency resolve + lock + plugin safe mode | PR/nightly | project lock |
| Save/Profile/UserData | Proven | backup/last-good slot | atomic write + migration + corruption + cloud conflict | PR/nightly | save diagnostics |
| Platform profiles | Proven | unsupported caps false + lower quality profile | profile resolve + cook closure + lifecycle injection | PR/nightly | profile report |
| Scalability profiles | Proven | static platform preset | pressure injection + degrade order + subscriber coverage | PR/nightly | quality state trace |
| Third-party compliance | Proven | dependency blocked until reviewed | license/SBOM/vulnerability/ABI leak audit | PR/release | SBOM + attribution |
| Package signing/VFS | Proven | unsigned dev package only | verify/mount/failure tests | PR/nightly | package manifest |
| Crash/telemetry | Proven | local crash dump only | crash injection + privacy filter | nightly | minidump + trace ring |

## 3. Promotion Rules

- A pending Proven capability cannot be marked complete until its linked Spike closes or its fallback becomes the explicit Proven path.
- A capability moves from Advanced to Proven only after capability tests, fallback tests, perf budget and documentation all pass.
- Any matrix row with no executable fixture blocks implementation plan approval for that capability.

