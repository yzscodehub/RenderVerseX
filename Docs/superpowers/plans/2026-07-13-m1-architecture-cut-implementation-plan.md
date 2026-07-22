# M1 Architecture Cut Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move Runtime rendering onto one dedicated Render Thread with immutable frame/resource contracts, bounded publication, generational GPU resources, per-queue completion tracking, and render-thread-only RHI ownership on DX12, Vulkan, and Metal while retaining DX11/OpenGL as explicit compatibility paths.

**Architecture:** Engine invokes RenderExtraction on the update thread and publishes complete `RenderFramePacket` values to RenderSubsystem. ResourceSubsystem is the sole update-thread producer of immutable upload requests through a narrow generational gateway. A dedicated executor owns one `RenderThreadRuntime`, which alone creates, mutates, submits, presents, retires, and destroys RHI state. Frame, upload, release, resize, stop, diagnostics, completion, and shutdown paths are bounded and observable; old synchronous and pointer-bearing paths are deleted before the M1 exit gate.

**Tech Stack:** C++20, CMake 3.21+, GoogleTest/CTest, Python 3 architecture gates, PowerShell Build Truth, GLFW, DX12, Vulkan, Metal, DX11, OpenGL, GitHub Actions, Linux ThreadSanitizer.

## Global Constraints

- The approved design in `Docs/superpowers/specs/2026-07-13-m1-architecture-cut-design.md` is authoritative. Contract changes require architecture review before implementation continues.
- At M1 exit, Runtime and Shipping always select `DedicatedRenderExecutor`. During the named migration window, legacy callers may compile through `LegacySynchronousRenderBridge`; once a caller is connected to the new runtime it cannot fall back, and the bridge is absent from the M1 exit revision. `InlineRenderExecutor` exists only in `RVX_RenderTestSupport` and cannot be selected by production configuration.
- M1 has one Render Thread, no RHI Thread, no parallel command recording, and at most one physical Graphics, Compute, and Copy timeline.
- Render must finish with no source or link dependency on RenderExtraction, ResourceSceneAdapters, World, Scene, or Resource.
- Frame packets and upload requests own values only. No World/Scene/Camera/Resource pointer, RHI object, callback, `Core::Ref<>`, span, raw byte pointer, or external view may cross the thread boundary.
- `AssetId{0}` and any `RenderResourceHandle` with slot zero or generation zero are invalid. Generation wrap permanently retires the slot.
- The fixed status table defaults to 262,144 slots and may not be configured below 1,024. The frame mailbox defaults to three entries and supports only capacities two through four. Upload defaults are 1,024 requests and 256 MiB. The release ring has one usable entry for every nonzero status slot.
- Upload completion becomes `GPUReady` only after its Copy-domain completion point is complete. Completion values from different physical domains are never compared.
- Render drops every Render-owned upload-request reference before release-publishing a terminal state. ResourceSubsystem acquire-loads that state and performs the final update-thread release.
- Normal completion, device loss, and watchdog timeout are distinct terminal outcomes. Render Thread is never detached.
- Runtime is the correctness path. Editor receives only a compile-checked mechanical publication adapter; Editor rendering behavior is not an M1 exit dependency.
- Tier 1 native lifecycle evidence is mandatory for Windows DX12, Linux Vulkan, and macOS Metal. On Apple, HAL/Main Thread owns and attaches `CAMetalLayer`; Render Thread never dereferences `NSWindow`, `NSView`, or `UIView`. Vulkan on Windows and Tier 2 DX11/OpenGL run when the runner exposes the capability.
- Preserve all naming, include-order, ownership, logging, initialization, and Doxygen rules in `AGENTS.md`.
- Each task starts red, ends with focused verification, receives an independent review, and is committed separately. Do not combine cleanup from later tasks into an earlier commit.

---

## Program Sequence

1. Freeze value contracts and the packet builder boundary.
2. Implement the packed status table and update-owned reservation allocator.
3. Add bounded transports and the gateway implementation.
4. Add the shared executor model, thread guards, and platform bootstrap.
5. Replace raw window pointers with the tagged native-surface contract across RHI backends.
6. Add staged subsystem initialization and targeted deinitialization.
7. Introduce the runtime shell, structured results, and immutable diagnostics.
8. Publish physical queue topology and add per-domain submission tracking.
9. Add completion-token retirement while retaining legacy lifetime paths only for intermediate build compatibility.
10. Split registry/upload ownership behind a temporary GPUResourceManager facade.
11. Move upload production, retries, and final request reclamation into ResourceSubsystem.
12. Complete Engine-side extraction, frame settings, and capture values.
13. Convert RenderScene/SceneRenderer and the runtime pump to packet-only rendering.
14. Audit every RHI strong-reference holder and cut over from global frame-count deletion.
15. Harden startup, runtime failure, device loss, and bounded shutdown.
16. Compose extraction/resource/render lifecycle in Engine.
17. Migrate Samples, tests, and the Editor compile-only adapter.
18. Delete every legacy facade/API/link and enable static architecture proof.
19. Enable TSAN, native lifecycle, Build Truth, and the M1 exit review.

## File Responsibility Map

### New public value contracts

- `RenderContracts/Include/RenderContracts/RenderIdentity.h` — `AssetId`, `RenderResourceHandle`, resource kinds, equality, and hashing.
- `RenderContracts/Include/RenderContracts/ResourceUploadRequest.h` — immutable owning upload payloads and the only request factory.
- `RenderContracts/Private/ResourceUploadRequestInternal.h` — private checked byte-accounting seam used by the factory and focused tests; never installed.
- `RenderContracts/Include/RenderContracts/IRenderResourceGateway.h` — reservation/enqueue/release/query outcomes and the narrow public gateway interface, frozen in Task 2.
- `RenderContracts/Include/RenderContracts/RenderFramePacket.h` — immutable frame header, view, primitives, lights, sky/environment, settings, capture request, and feature snapshots.
- `RenderContracts/Private/ResourceUploadRequest.cpp` and `RenderContracts/Private/RenderFramePacket.cpp` — checked construction behind the public immutable contracts.
- `RenderExtraction/Include/RenderExtraction/RenderFramePacketBuilder.h` — mutable update-thread builder whose `Seal()` returns `std::unique_ptr<const RenderFramePacket>`.
- `RenderExtraction/Include/RenderExtraction/RenderFrameExtractor.h` — World/Scene/Resource to complete packet aggregation.
- `Geometry/Include/Geometry/Asset/AssetMetadata.h` — update-side mesh/material metadata that replaces the old Render upload-source interfaces.
- `RHI/Include/RHI/RHINativeSurface.h` — tagged non-owning native surface value.
- `RHI/Include/RHI/RHIQueueTopology.h` — logical-to-physical queue mapping and completion mode.
- `RHI/Include/RHI/RHIDeviceStatus.h` — stable device-ready/lost/fault diagnostics.
- `Render/Include/Render/RenderTransportTypes.h` — public bounded transport capacities and per-iteration budgets shared by Tasks 3 and 7.
- `Render/Include/Render/RenderRuntimeTypes.h` — configuration, lifecycle, structured startup/runtime/shutdown results.
- `Render/Include/Render/RenderDiagnostics.h` — immutable update-readable diagnostics and owned frame-capture result.

### New Render runtime internals

- `Render/Private/Runtime/RenderThreadGuard.h`
- `Render/Private/Runtime/RenderThreadGuard.cpp`
- `Render/Private/Runtime/RenderFrameMailbox.h`
- `Render/Private/Runtime/RenderFrameMailbox.cpp`
- `Render/Private/Runtime/RenderUploadQueue.h`
- `Render/Private/Runtime/RenderUploadQueue.cpp`
- `Render/Private/Runtime/RenderReleaseQueue.h`
- `Render/Private/Runtime/RenderReleaseQueue.cpp`
- `Render/Private/Runtime/RenderControlMailbox.h`
- `Render/Private/Runtime/RenderControlMailbox.cpp`
- `Render/Private/Runtime/RenderResourceStatusTable.h`
- `Render/Private/Runtime/RenderResourceStatusTable.cpp`
- `Render/Private/Runtime/RenderResourceGateway.h`
- `Render/Private/Runtime/RenderResourceGateway.cpp`
- `Render/Private/Runtime/RenderDiagnosticsPublisher.h`
- `Render/Private/Runtime/RenderDiagnosticsPublisher.cpp`
- `Render/Private/Runtime/IRenderExecutor.h`
- `Render/Private/Runtime/DedicatedRenderExecutor.h`
- `Render/Private/Runtime/DedicatedRenderExecutor.cpp`
- `Render/Private/Runtime/RenderThreadPlatform.h`
- `Render/Private/Runtime/RenderThreadPlatform.cpp`
- `Render/Private/Runtime/RenderThreadPlatformApple.mm`
- `Render/Private/Runtime/RenderThreadRuntime.h`
- `Render/Private/Runtime/RenderThreadRuntime.cpp`
- `Render/Private/Resources/RenderSubmissionTracker.h`
- `Render/Private/Resources/RenderSubmissionTracker.cpp`
- `Render/Private/Resources/RenderRetirementQueue.h`
- `Render/Private/Resources/RenderRetirementQueue.cpp`
- `Render/Private/Resources/RenderResourceRegistry.h`
- `Render/Private/Resources/RenderResourceRegistry.cpp`
- `Render/Private/Resources/RenderUploadProcessor.h`
- `Render/Private/Resources/RenderUploadProcessor.cpp`

`Render/CMakeLists.txt` builds the queue/status/diagnostics/executor-independent sources into `RVX_RenderRuntimeCore`, then links that internal target into `RVX_Render`. This keeps the Linux TSAN target small and prevents duplicate production/test implementations.

### New Resource and test support

- `Resource/Include/Resource/RenderUploadRequestBuilder.h` and `Resource/Private/RenderUploadRequestBuilder.cpp` — copy concrete CPU resources into owning request payloads.
- `Resource/Private/ResourceSubsystem.cpp` — update-thread gateway integration, retry policy, terminal-state polling, and request reclamation.
- `Tests/Common/RenderRuntimeTestSupport.h` and `Tests/Common/RenderRuntimeTestSupport.cpp` — Inline executor, fake clock, fake fatal policy, fake RHI factory, fault plan, latches, and thread-ownership recorder.
- `Tests/RenderContractsValidation/main.cpp`
- `Tests/RenderConcurrencyValidation/main.cpp`
- `Tests/RenderExecutorValidation/main.cpp`
- `Tests/RenderSubmissionValidation/main.cpp`
- `Tests/RenderResourceRuntimeValidation/main.cpp`
- `Tests/RenderFrameExtractionValidation/main.cpp`
- `Tests/RenderThreadRuntimeValidation/main.cpp`
- `Tests/EngineRenderCompositionValidation/main.cpp`
- `Tests/RenderLifetimeCutoverValidation/main.cpp`
- `Tests/NativeRenderLifecycleValidation/main.cpp`
- `Tests/RenderConcurrencyTSAN/main.cpp`

### Architecture and evidence

- `Scripts/check_render_contract_fields.py` — declaration-aware field checker for packet/request types.
- `Scripts/test_render_contract_field_checker.py` — positive/negative checker fixtures.
- `Scripts/check_rhi_ownership_inventory.py` and `Scripts/test_rhi_ownership_inventory_checker.py` — fail-closed ownership inventory for every production RHI strong-reference holder.
- `Scripts/check_m1_architecture.py` — M1 module, symbol-absence, executor, ownership, and native-smoke registration gate.
- `Scripts/run_build_truth.ps1`, `Docs/build-truth.md`, `CMakePresets.json`, and `.github/workflows/ci.yml` — TSAN, native lifecycle, Editor compile-only, and final evidence integration.

---

### Task 1: Freeze identities, immutable upload values, frame values, and builder semantics

**Files:**

- Create: `RenderContracts/Include/RenderContracts/RenderIdentity.h`
- Create: `RenderContracts/Include/RenderContracts/ResourceUploadRequest.h`
- Create: `RenderContracts/Include/RenderContracts/RenderFramePacket.h`
- Create: `RenderContracts/Private/ResourceUploadRequestInternal.h`
- Create: `RenderContracts/Private/ResourceUploadRequest.cpp`
- Create: `RenderContracts/Private/RenderFramePacket.cpp`
- Create: `RenderExtraction/Include/RenderExtraction/RenderFramePacketBuilder.h`
- Create: `RenderExtraction/Private/RenderFramePacketBuilder.cpp`
- Create: `Scripts/check_render_contract_fields.py`
- Create: `Scripts/test_render_contract_field_checker.py`
- Create: `Tests/RenderContractsValidation/main.cpp`
- Modify: `RenderExtraction/CMakeLists.txt`
- Modify: `RenderContracts/CMakeLists.txt`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**

```cpp
struct AssetId
{
    uint64 value = 0;

    [[nodiscard]] bool IsValid() const noexcept { return value != 0; }
    auto operator<=>(const AssetId&) const = default;
};

struct RenderResourceHandle
{
    uint32 slot = 0;
    uint32 generation = 0;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return slot != 0 && generation != 0;
    }

    auto operator<=>(const RenderResourceHandle&) const = default;
};

struct AssetIdHash final
{
    [[nodiscard]] size_t operator()(AssetId id) const noexcept
    {
        return std::hash<uint64>{}(id.value);
    }
};

struct RenderResourceHandleHash final
{
    [[nodiscard]] size_t operator()(RenderResourceHandle handle) const noexcept
    {
        const uint64 packed =
            (static_cast<uint64>(handle.slot) << 32U) |
            static_cast<uint64>(handle.generation);
        return std::hash<uint64>{}(packed);
    }
};

enum class RenderResourceKind : uint8
{
    Invalid = 0,
    Mesh = 1,
    Texture = 2,
    Material = 3
};

enum class RenderUploadPriority : uint8
{
    Low = 0,
    Normal = 1,
    High = 2
};

enum class RenderDependencyReadiness : uint8
{
    RequireAll = 0,
    AllowFallback = 1
};

class ResourceUploadRequest;
using ResourceUploadRequestRef = std::shared_ptr<const ResourceUploadRequest>;
```

`RenderIdentity.h` includes `<compare>`, `<cstddef>`, and `<functional>` and defines the identities plus both hashers; `ResourceUploadRequestRef` is declared in `ResourceUploadRequest.h`, not the identity header. All public enums in this task use the explicit values shown by the declarations and receive ABI `static_assert`s. `AssetIdHash` and `RenderResourceHandleHash` are the only contract hashers; Task 2 uses `AssetIdHash` rather than inventing a specialization or ordered-container fallback. Do not include or reuse the pointer-bearing `RenderContracts/RenderResource.h`; new upload types use `MeshUpload*`, `TextureUpload*`, and `MaterialUpload*` names until the legacy header is removed. `ResourceUploadRequest::Create(ResourceUploadRequestCreateInfo)` is the only constructor path. It overflow-checks payload ranges and byte accounting, rejects mismatched diagnostic bytes, and returns immutable shared ownership. `RenderFramePacket` has no public default/copy/move constructor or mutator. `RenderFramePacketBuilder::Seal()` validates schema, strictly positive sequence, complete extraction markers, owned feature snapshots, numeric view validity, and required handles before returning exclusive immutable ownership.

- [ ] **Step 1: Register failing contract and checker tests**

Add `RenderContractsValidation` to `Tests/CMakeLists.txt` with `RVX::Core`, `RVX::RenderContracts`, and `RVX::RenderExtraction`. Test at minimum:

```cpp
static_assert(!std::is_default_constructible_v<RenderFramePacket>);
static_assert(!std::is_copy_constructible_v<RenderFramePacket>);
static_assert(!std::is_move_constructible_v<RenderFramePacket>);
static_assert(!std::is_copy_constructible_v<ResourceUploadRequest>);
static_assert(!std::is_move_constructible_v<ResourceUploadRequest>);
static_assert(!std::is_default_constructible_v<ResourceUploadRequest>);
static_assert(!std::is_copy_constructible_v<RenderFramePacketBuilder>);
static_assert(!std::is_move_constructible_v<RenderFramePacketBuilder>);
static_assert(std::is_same_v<
    decltype(std::declval<RenderFramePacketBuilder>().Seal()),
    std::unique_ptr<const RenderFramePacket>>);

TEST(RenderContractsValidation, ZeroIdentityValuesAreInvalid)
{
    EXPECT_FALSE(AssetId{}.IsValid());
    EXPECT_FALSE(RenderResourceHandle{}.IsValid());
    EXPECT_FALSE((RenderResourceHandle{1, 0}.IsValid()));
    EXPECT_TRUE((RenderResourceHandle{1, 1}.IsValid()));
}

TEST(RenderContractsValidation, UploadRequestOwnsCopiedPayloadBytes)
{
    ResourceUploadRequestCreateInfo info = MakeValidTextureRequestInfo();
    const uint8 expected =
        std::get<TextureUploadPayload>(info.payload).bytes.front();
    const auto created = ResourceUploadRequest::Create(info);
    std::get<TextureUploadPayload>(info.payload).bytes.clear();
    ASSERT_EQ(created.code, ResourceUploadRequestCreateCode::Created);
    ASSERT_TRUE(created.request);
    const auto* payload =
        std::get_if<TextureUploadPayload>(&created.request->GetPayload());
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(payload->bytes.front(), expected);
}
```

The test file defines three request fixtures and one in-place builder populator, with no hidden production helper: `MakeValidMeshRequestInfo()` creates three non-indexed vertices in 36 owned bytes with a position range `{0, 36, 12}`; `MakeValidTextureRequestInfo()` creates one 1x1x1 RGBA8 mip/layer in four bytes with subresource `{bytes={0,4,0}, mip=0, layer=0, rowPitch=4, slicePitch=4}`; `MakeValidMaterialRequestInfo()` uses finite default `MaterialSourceData` with no bindings; and `void PopulateCompletePacketBuilder(RenderFramePacketBuilder& builder, uint32 omittedSingletonMask = 0)` sets every singleton whose bit is not omitted for sequence one, a 1x1 viewport, zero primitive/light/provider counts, `RenderFeatureSnapshot::BeginBuild(1)` followed by `MarkComplete()`, disabled sky/environment handles, default valid settings/capture, and complete zero-skip diagnostics. It never returns a nonmovable builder by value. Each request fixture uses asset one, handle `{1,1}`, sequence one, matching kind, no dependencies, and manually computes its expected `declaredPayloadBytes` from the fixture's known vector sizes without calling the production byte-account helper. Assert both hashers are deterministic and that `RenderResourceHandleHash` equals `std::hash<uint64>` of the documented packed value.

Implement this exact request matrix; each row is an independently named test and changes only the listed field from its valid fixture:

| Test suffix | Mutation | Expected |
|---|---|---|
| `CreatesMesh` / `CreatesTexture` / `CreatesMaterial` | none | `Created`, matching payload alternative and all schema/identity/byte getters |
| `RejectsSchema` | version 2 | `InvalidSchema` |
| `RejectsZeroSequence` | sequence 0 | `InvalidSequence` |
| `RejectsZeroAsset` | `AssetId{}` | `InvalidAsset` |
| `RejectsZeroHandlePart` | `{1,0}` | `InvalidHandle` |
| `RejectsInvalidKind` | `Invalid` | `InvalidKind` |
| `RejectsVariantKindMismatch` | texture kind with mesh payload | `PayloadKindMismatch` |
| `RejectsMeshShape` | empty mesh bytes | `InvalidPayload` |
| `RejectsTextureShape` | duplicate `(0,0)` subresource | `InvalidPayload` |
| `RejectsMaterialShape` | duplicate binding slot | `InvalidPayload` |
| `RejectsRangeOverflow` | texture range `{UINT64_MAX,2,0}` | `PayloadRangeOverflow` |
| `RejectsDeclaredBytes` | increment valid declared total | `DiagnosticByteCountMismatch` |
| `RejectsInvalidDependency` | add `{0,0}` and add one handle element to declared bytes | `InvalidDependency` |
| `RejectsSelfDependency` | add request handle and add one handle element to declared bytes | `SelfDependency` |
| `RejectsDuplicateDependency` | add the same valid non-self handle twice and add two handle elements to declared bytes | `DuplicateDependency` |
| `OwnsCopiedSourceValues` | create each alternative, then clear/overwrite/destroy create-info | request getters retain original values |

The matrix's “single mutation” rule permits only the dependency rows' paired declared-byte update, because dependencies are part of retained-byte accounting. Add parameterized payload-rule cases for every bullet in Step 3, including zero/nonzero index-range symmetry, submesh overflow, optional range canonical zero, texture pair completeness, cubemap/array invariants, invalid top-level readiness/priority enums, invalid material alpha/workflow enums, material finite/range rules, and fallback-free material creation. Call `Detail::AccumulateOwnedBytes` directly for normal multiply/add, multiply overflow, and add overflow; assert overflow returns `PayloadByteCountOverflow` and leaves `total` unchanged. The production factory must return that helper's non-`Created` code unchanged, so the private seam freezes the otherwise non-allocatable error mapping.

Implement this exact builder matrix using `PopulateCompletePacketBuilder(builder, omittedSingletonMask)`:

| Test | Mutation | Expected |
|---|---|---|
| `MissingEachSingletonIsRepairable` | omit each of header/view/sky/environment/settings/capture/features/diagnostics in a parameterized case, seal, then set it | first `MissingValue`/`Building`, then successful `Sealed` |
| `RejectsSchemaThenRepairs` | header version 2, then replace header | `InvalidSchema`, then success |
| `RejectsZeroSequence` | sequence 0 | `InvalidSequence` |
| `RejectsZeroViewport` | width 0 | `InvalidViewport` |
| `RejectsEachNonFiniteNumericFamily` | one NaN in view, primitive, light, sky/environment, and settings cases | `InvalidNumericValue` |
| `RejectsIncompleteExtraction` | diagnostics incomplete or skipped count nonzero | `IncompleteExtraction` |
| `RejectsIncompleteFeatureSnapshot` | bad schema, sequence, status, or complete bit in aggregate and each nested snapshot | `IncompleteFeatureSnapshot` |
| `RejectsEachCountMismatch` | primitive, light, provider, aggregate item, nested item, and skipped-reason mismatches | `CountMismatch` |
| `RejectsResourceReferenceRules` | invalid required mesh, shadow mismatch, or partial environment trio | `InvalidResourceReference` |
| `RejectsCaptureRules` | invalid `None`, zero-ID capture, zero-extent capture, undeclared kind | `InvalidCaptureRequest` |
| `SuccessfulSealIsImmutable` | valid fixture | non-null packet, exact getter values, `Sealed/Sealed` |
| `PostSealOperationsAreRejected` | call every setter/adder and seal again | setters false/no mutation; seal null, `Sealed/AlreadySealed` |

Register `RenderContractsValidation` exactly with `rvx_add_gtest(NAME RenderContractsValidation SOURCES RenderContractsValidation/main.cpp LIBS RVX::Core RVX::RenderContracts RVX::RenderExtraction INCLUDES ${CMAKE_SOURCE_DIR}/RenderContracts/Private LABELS "unit;architecture" TIMEOUT 60)`. Register the Python regression with `add_test(NAME Architecture.RenderContractFieldChecker COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Scripts/test_render_contract_field_checker.py -v)` and set `LABELS "unit;architecture"`, `TIMEOUT 30`, and `WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"`.

- [ ] **Step 2: Run red verification**

```powershell
cmake --preset win_x64_debug
cmake --build build\win_x64_debug --config Debug --target RenderContractsValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderContractsValidation|Architecture.RenderContractFieldChecker" --output-on-failure
```

Expected: build/test failure because the new headers, builder, and checker do not exist.

- [ ] **Step 3: Implement the value identities and upload payload variant**

Define the request contract below in `ResourceUploadRequest.h`. These names are intentionally distinct from legacy `RenderMeshUploadData`, `RenderTextureUploadData`, and `RenderMaterialTextureBinding`.

```cpp
inline constexpr uint32 RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID = 0x52565855U;
inline constexpr uint32 RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION = 1;

enum class MeshUploadIndexType : uint8
{
    UInt8 = 0,
    UInt16 = 1,
    UInt32 = 2
};

enum class MeshUploadPrimitiveTopology : uint8
{
    Triangles = 0,
    TriangleStrip = 1,
    TriangleFan = 2,
    Lines = 3,
    LineStrip = 4,
    LineLoop = 5,
    Points = 6
};

enum class TextureUploadFormat : uint8
{
    Unknown = 0,
    RGBA8 = 1,
    RGBA16F = 2,
    RGBA32F = 3,
    RGB8 = 4,
    RG8 = 5,
    R8 = 6,
    BC1 = 7,
    BC3 = 8,
    BC5 = 9,
    BC7 = 10
};

enum class MaterialUploadTextureSlot : uint8
{
    BaseColor = 0,
    Normal = 1,
    MetallicRoughness = 2,
    Occlusion = 3,
    Emissive = 4
};

enum class MaterialUploadWrapMode : uint8
{
    Repeat = 0,
    MirrorRepeat = 1,
    ClampToEdge = 2,
    ClampToBorder = 3
};

enum class MaterialUploadFilterMode : uint8
{
    Nearest = 0,
    Linear = 1,
    NearestMipmapNearest = 2,
    LinearMipmapNearest = 3,
    NearestMipmapLinear = 4,
    LinearMipmapLinear = 5
};

struct UploadByteRange
{
    uint64 offset = 0;
    uint64 size = 0;
    uint32 stride = 0;
};

struct MeshUploadCreateInfo
{
    uint64 vertexCount = 0;
    uint64 indexCount = 0;
    MeshUploadIndexType indexType = MeshUploadIndexType::UInt32;
    MeshUploadPrimitiveTopology topology =
        MeshUploadPrimitiveTopology::Triangles;
    Vec3 boundsMin{0.0f};
    Vec3 boundsMax{0.0f};
};

struct MeshUploadSubmesh
{
    uint32 indexOffset = 0;
    uint32 indexCount = 0;
    int32 baseVertex = 0;
    MeshUploadPrimitiveTopology topology =
        MeshUploadPrimitiveTopology::Triangles;
};

struct MeshUploadPayload
{
    MeshUploadCreateInfo createInfo;
    std::vector<uint8> bytes;
    UploadByteRange indexRange;
    UploadByteRange positionRange;
    UploadByteRange normalRange;
    UploadByteRange uvRange;
    UploadByteRange tangentRange;
    UploadByteRange boneIndexRange;
    UploadByteRange boneWeightRange;
    std::vector<MeshUploadSubmesh> submeshes;
};

struct TextureUploadCreateInfo
{
    uint32 width = 0;
    uint32 height = 0;
    uint32 depth = 1;
    uint32 mipLevels = 1;
    uint32 arrayLayers = 1;
    TextureUploadFormat format = TextureUploadFormat::Unknown;
    bool isCubemap = false;
    bool isArray = false;
    bool isSRGB = true;
};

struct TextureUploadSubresource
{
    UploadByteRange bytes;
    uint32 mipLevel = 0;
    uint32 arrayLayer = 0;
    uint64 rowPitch = 0;
    uint64 slicePitch = 0;
};

struct TextureUploadPayload
{
    TextureUploadCreateInfo createInfo;
    std::vector<uint8> bytes;
    std::vector<TextureUploadSubresource> subresources;
};

struct MaterialUploadTextureBinding
{
    MaterialUploadTextureSlot slot = MaterialUploadTextureSlot::BaseColor;
    RenderResourceHandle texture;
    int32 uvSet = 0;
    Vec2 offset{0.0f, 0.0f};
    Vec2 scale{1.0f, 1.0f};
    float32 rotation = 0.0f;
    MaterialUploadWrapMode wrapS = MaterialUploadWrapMode::Repeat;
    MaterialUploadWrapMode wrapT = MaterialUploadWrapMode::Repeat;
    MaterialUploadFilterMode minFilter =
        MaterialUploadFilterMode::LinearMipmapLinear;
    MaterialUploadFilterMode magFilter = MaterialUploadFilterMode::Linear;
};

struct MaterialUploadPayload
{
    MaterialSourceData sourceData;
    std::vector<MaterialUploadTextureBinding> textureBindings;
};

using ResourceUploadPayload = std::variant<
    MeshUploadPayload,
    TextureUploadPayload,
    MaterialUploadPayload>;

struct ResourceUploadDiagnosticProvenance
{
    uint64 sourceRevision = 0;
    uint32 sourceKind = 0;
    uint32 flags = 0;
};

struct ResourceUploadRequestCreateInfo
{
    uint32 schemaId = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID;
    uint32 schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION;
    uint64 sequence = 0;
    AssetId assetId;
    RenderResourceHandle handle;
    RenderResourceKind kind = RenderResourceKind::Invalid;
    ResourceUploadPayload payload;
    std::vector<RenderResourceHandle> dependencies;
    RenderDependencyReadiness dependencyReadiness =
        RenderDependencyReadiness::RequireAll;
    uint64 declaredPayloadBytes = 0;
    RenderUploadPriority priority = RenderUploadPriority::Normal;
    ResourceUploadDiagnosticProvenance provenance;
};

enum class ResourceUploadRequestCreateCode : uint8
{
    Created = 0,
    InvalidSchema = 1,
    InvalidSequence = 2,
    InvalidAsset = 3,
    InvalidHandle = 4,
    InvalidKind = 5,
    PayloadKindMismatch = 6,
    InvalidPayload = 7,
    PayloadRangeOverflow = 8,
    PayloadByteCountOverflow = 9,
    DiagnosticByteCountMismatch = 10,
    InvalidDependency = 11,
    DuplicateDependency = 12,
    SelfDependency = 13
};

struct ResourceUploadRequestCreateResult
{
    ResourceUploadRequestCreateCode code =
        ResourceUploadRequestCreateCode::InvalidPayload;
    ResourceUploadRequestRef request;
};

class ResourceUploadRequest final
{
public:
    static ResourceUploadRequestCreateResult Create(ResourceUploadRequestCreateInfo info);

    ~ResourceUploadRequest() = default;
    ResourceUploadRequest(const ResourceUploadRequest&) = delete;
    ResourceUploadRequest& operator=(const ResourceUploadRequest&) = delete;
    ResourceUploadRequest(ResourceUploadRequest&&) = delete;
    ResourceUploadRequest& operator=(ResourceUploadRequest&&) = delete;

    [[nodiscard]] uint32 GetSchemaId() const noexcept;
    [[nodiscard]] uint32 GetSchemaVersion() const noexcept;
    [[nodiscard]] uint64 GetSequence() const noexcept;
    [[nodiscard]] AssetId GetAssetId() const noexcept;
    [[nodiscard]] RenderResourceHandle GetHandle() const noexcept;
    [[nodiscard]] RenderResourceKind GetKind() const noexcept;
    [[nodiscard]] uint64 GetDerivedPayloadBytes() const noexcept;
    [[nodiscard]] uint64 GetDeclaredPayloadBytes() const noexcept;
    [[nodiscard]] const ResourceUploadPayload& GetPayload() const noexcept;
    [[nodiscard]] const std::vector<RenderResourceHandle>& GetDependencies() const noexcept;
    [[nodiscard]] RenderDependencyReadiness GetDependencyReadiness() const noexcept;
    [[nodiscard]] RenderUploadPriority GetPriority() const noexcept;
    [[nodiscard]] const ResourceUploadDiagnosticProvenance&
        GetProvenance() const noexcept;

private:
    explicit ResourceUploadRequest(ResourceUploadRequestCreateInfo&& info,
                                   uint64 derivedPayloadBytes);

    uint32 m_schemaId = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID;
    uint32 m_schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION;
    uint64 m_sequence = 0;
    AssetId m_assetId{};
    RenderResourceHandle m_handle{};
    RenderResourceKind m_kind = RenderResourceKind::Invalid;
    ResourceUploadPayload m_payload{MeshUploadPayload{}};
    std::vector<RenderResourceHandle> m_dependencies{};
    RenderDependencyReadiness m_dependencyReadiness =
        RenderDependencyReadiness::RequireAll;
    uint64 m_derivedPayloadBytes = 0;
    uint64 m_declaredPayloadBytes = 0;
    RenderUploadPriority m_priority = RenderUploadPriority::Normal;
    ResourceUploadDiagnosticProvenance m_provenance{};
};
```

The factory validates in this exact first-failure order: schema, sequence, asset, handle, kind, payload/variant kind, payload shape/numerics/enums, payload ranges, derived-byte multiplication/addition, declared-byte equality, then dependencies in input order. Top-level readiness and priority must be declared enum values; invalid values are `InvalidPayload`. Within each dependency, invalid wins first, self-reference second, and duplicate third. The returned code is the code for the first failed stage; tests freeze this ordering. Every failed result has a null request; only `Created` has a non-null request. The private factory implementation may use `ResourceUploadRequestRef(new ResourceUploadRequest(...))` to create the standard shared control block; the raw pointer must not escape and no custom deleter is permitted.

Payload validity is exact:

- An empty byte range is exactly `{0, 0, 0}`. A nonempty range requires checked `offset + size` within the owning byte vector; arithmetic overflow is `PayloadRangeOverflow`, while a non-overflowing out-of-bounds end or insufficient size is `InvalidPayload`. Checked count/stride multiplication overflow is `PayloadRangeOverflow`.
- Mesh requires nonempty bytes, `vertexCount > 0`, finite ordered bounds, and a nonempty position range with nonzero stride and checked `vertexCount * stride <= positionRange.size`. Optional vertex ranges are canonical-empty or nonempty/in-bounds with nonzero stride and enough bytes for every vertex; bone-index and bone-weight ranges are both empty or both nonempty. `indexCount == 0` requires an empty index range and no submeshes; otherwise the range is nonempty/in-bounds, its stride is exactly 1/2/4 for `UInt8`/`UInt16`/`UInt32`, checked `indexCount * stride <= size`, and every submesh has nonzero count with checked `indexOffset + indexCount <= createInfo.indexCount`. All enum values must be declared values.
- Texture requires nonzero dimensions/mips/layers, known format, nonempty bytes and subresources. `isCubemap` requires depth one and at least six layers in a multiple of six; `isArray` requires more than one layer; a non-cubemap/non-array requires exactly one layer. Checked `mipLevels * arrayLayers` must equal the subresource count. Every `(mipLevel, arrayLayer)` pair appears exactly once, every byte range is nonempty/in-bounds with stride zero, `rowPitch > 0`, `slicePitch >= rowPitch`, and range size is at least `slicePitch`; duplicate/missing pairs and non-overflowing bounds violations are `InvalidPayload`.
- Material requires every scalar/vector in `MaterialSourceData` and every binding transform to be finite; metallic, roughness, occlusion, alpha cutoff, and color factors are in `[0, 1]`; normal/emissive strengths are nonnegative; alpha mode and workflow are declared enum values; every texture binding has a valid handle, nonnegative UV set, nonzero scale components, a unique declared slot, declared wrap/filter modes, and a `magFilter` of only `Nearest` or `Linear`. An empty binding vector is legal.
- Dependencies must be valid, different from the request handle, and unique. For each dependency scanned from index zero, `InvalidDependency` precedes `SelfDependency`, which precedes `DuplicateDependency`.

Compute `derivedPayloadBytes` with checked `uint64` additions of `size() * sizeof(element)` for every owned vector: mesh bytes/submeshes, texture bytes/subresources, material bindings, and dependencies. Never use `capacity()`. `declaredPayloadBytes` must equal that total exactly.

`ResourceUploadRequestInternal.h` declares only this private implementation helper so overflow is testable without impossible allocations:

```cpp
namespace RVX::Detail
{
    [[nodiscard]] ResourceUploadRequestCreateCode AccumulateOwnedBytes(
        uint64 elementCount,
        uint64 elementSize,
        uint64& total) noexcept;
} // namespace RVX::Detail
```

The helper returns `Created` after updating `total`, or `PayloadByteCountOverflow` without modifying `total`. The factory uses it for every vector multiplication/addition and immediately returns any non-`Created` code unchanged. `AccumulateOwnedBytes(UINT64_MAX, 2, total)` and `total = UINT64_MAX; AccumulateOwnedBytes(1, 1, total)` return `PayloadByteCountOverflow`; the normal case returns `Created` and updates the total. The helper is not installed or included by a public header. `RenderContractsValidation` receives `RenderContracts/Private` as a private test include directory.

Keep the destructor ordinary and constructor private. The factory allocates the object directly into a standard `ResourceUploadRequestRef`; do not add a value-return, raw-pointer API/escape, copy/move, or custom-deleter path. Convert `RVX_RenderContracts` from INTERFACE to STATIC, compile the two private implementation files, keep its public include directory, and link Core publicly; do not add Resource, Scene, RHI, or Render dependencies.

- [ ] **Step 4: Implement the immutable packet and builder**

Define the value types with the exact fields below. Primitive/light types do not reuse pointer-bearing `RenderPrimitiveProxy` or `RenderLightProxy`.

```cpp
inline constexpr uint32 RVX_RENDER_FRAME_PACKET_SCHEMA_ID = 0x52565846U;
inline constexpr uint32 RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION = 1;

enum class RenderLightType : uint8
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

enum class RenderFrameCaptureKind : uint8
{
    None = 0,
    Color = 1,
    Depth = 2,
    ObjectId = 3
};

enum class RenderExtractionCode : uint8
{
    Complete = 0,
    MissingProvider = 1,
    InvalidResourceReference = 2,
    InvalidNumericValue = 3,
    CountMismatch = 4
};

struct RenderFrameHeader
{
    uint32 schemaId = RVX_RENDER_FRAME_PACKET_SCHEMA_ID;
    uint32 schemaVersion = RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION;
    uint64 sequence = 0;
    uint64 worldRevision = 0;
    uint64 temporalEpoch = 0;
    uint32 expectedPrimitiveCount = 0;
    uint32 extractedPrimitiveCount = 0;
    uint32 expectedLightCount = 0;
    uint32 extractedLightCount = 0;
    uint32 expectedFeatureProviderCount = 0;
    uint32 extractedFeatureProviderCount = 0;
    bool explicitDiscontinuity = false;
};

struct RenderViewSnapshot
{
    Mat4 viewMatrix{1.0f};
    Mat4 projectionMatrix{1.0f};
    Mat4 viewProjectionMatrix{1.0f};
    Mat4 inverseViewProjectionMatrix{1.0f};
    Vec3 cameraPosition{0.0f};
    Vec3 cameraDirection{0.0f, 0.0f, -1.0f};
    Vec3 cameraUp{0.0f, 1.0f, 0.0f};
    uint32 viewportX = 0;
    uint32 viewportY = 0;
    uint32 viewportWidth = 0;
    uint32 viewportHeight = 0;
    float32 nearPlane = 0.1f;
    float32 farPlane = 1000.0f;
    float32 absoluteTime = 0.0f;
    float32 deltaTime = 0.0f;
    float32 exposure = 1.0f;
};

struct RenderPrimitiveSnapshot
{
    uint64 objectId = 0;
    RenderResourceHandle mesh;
    RenderResourceHandle material;
    RenderResourceHandle fallbackMesh;
    RenderResourceHandle fallbackMaterial;
    Mat4 worldTransform{1.0f};
    Mat4 previousWorldTransform{1.0f};
    Vec3 boundsMin{0.0f};
    Vec3 boundsMax{0.0f};
    uint32 flags = 0;
    uint32 layerMask = 0xFFFFFFFFU;
    uint64 sortKey = 0;
    std::vector<Mat4> skinMatrices;
};

struct RenderLightSnapshot
{
    uint64 lightId = 0;
    RenderLightType type = RenderLightType::Directional;
    Vec3 position{0.0f};
    Vec3 direction{0.0f, -1.0f, 0.0f};
    Vec3 color{1.0f};
    float32 intensity = 1.0f;
    float32 range = 0.0f;
    float32 innerConeRadians = 0.0f;
    float32 outerConeRadians = 0.0f;
    RenderResourceHandle shadowResource;
    bool castsShadows = false;
};

struct RenderSkySnapshot
{
    RenderResourceHandle skyTexture;
    Vec3 tint{1.0f};
    float32 intensity = 1.0f;
    float32 rotationRadians = 0.0f;
};

struct RenderEnvironmentSnapshot
{
    RenderResourceHandle irradianceTexture;
    RenderResourceHandle prefilteredTexture;
    RenderResourceHandle brdfLutTexture;
    float32 intensity = 1.0f;
};

struct RenderPostProcessSettings
{
    bool enabled = true;
    bool enableTAA = true;
    bool enableBloom = true;
    bool enableSSAO = true;
    bool enableSSR = true;
    float32 bloomThreshold = 1.0f;
    float32 bloomIntensity = 1.0f;
};

struct RenderShadowSettings
{
    bool enabled = true;
    uint32 atlasResolution = 4096;
    uint32 cascadeCount = 4;
    float32 maxDistance = 200.0f;
};

struct RenderGPUCullingSettings
{
    bool enabled = true;
    uint32 maxVisibleObjects = 1048576;
};

struct RenderRayTracingSettings
{
    bool enabled = false;
    bool enableShadows = false;
    bool enableReflections = false;
    uint32 maxInstances = 262144;
    uint32 maxRaysPerPixel = 1;
};

struct RenderTemporalSettings
{
    bool resetHistory = false;
    uint32 jitterIndex = 0;
};

struct RenderFrameSettings
{
    float32 renderScale = 1.0f;
    uint32 debugView = 0;
    RenderPostProcessSettings postProcess;
    RenderShadowSettings shadows;
    RenderGPUCullingSettings gpuCulling;
    RenderRayTracingSettings rayTracing;
    RenderTemporalSettings temporal;
};

struct RenderFrameCaptureRequest
{
    uint64 requestId = 0;
    RenderFrameCaptureKind kind = RenderFrameCaptureKind::None;
    uint32 width = 0;
    uint32 height = 0;
    bool includeAlpha = false;
};

struct RenderExtractionDiagnostics
{
    RenderExtractionCode code = RenderExtractionCode::Complete;
    uint32 skippedPrimitiveCount = 0;
    uint32 skippedLightCount = 0;
    uint32 skippedFeatureProviderCount = 0;
    bool complete = false;
};
```

`RenderFramePacket.h` forward-declares `class RenderFramePacketBuilder;`. `RenderFramePacket` owns these values, deletes default/copy/move construction, exposes only the const accessors below, and grants construction access only to `RenderFramePacketBuilder`:

```cpp
class RenderFramePacket final
{
public:
    ~RenderFramePacket() = default;
    RenderFramePacket() = delete;
    RenderFramePacket(const RenderFramePacket&) = delete;
    RenderFramePacket& operator=(const RenderFramePacket&) = delete;
    RenderFramePacket(RenderFramePacket&&) = delete;
    RenderFramePacket& operator=(RenderFramePacket&&) = delete;

    [[nodiscard]] const RenderFrameHeader& GetHeader() const noexcept;
    [[nodiscard]] const RenderViewSnapshot& GetView() const noexcept;
    [[nodiscard]] const std::vector<RenderPrimitiveSnapshot>&
        GetPrimitives() const noexcept;
    [[nodiscard]] const std::vector<RenderLightSnapshot>&
        GetLights() const noexcept;
    [[nodiscard]] const RenderSkySnapshot& GetSky() const noexcept;
    [[nodiscard]] const RenderEnvironmentSnapshot&
        GetEnvironment() const noexcept;
    [[nodiscard]] const RenderFrameSettings& GetSettings() const noexcept;
    [[nodiscard]] const RenderFrameCaptureRequest&
        GetCaptureRequest() const noexcept;
    [[nodiscard]] const RenderFeatureSnapshot& GetFeatures() const noexcept;
    [[nodiscard]] const RenderExtractionDiagnostics&
        GetExtractionDiagnostics() const noexcept;

private:
    friend class RenderFramePacketBuilder;
    RenderFramePacket(RenderFrameHeader header,
                      RenderViewSnapshot view,
                      std::vector<RenderPrimitiveSnapshot> primitives,
                      std::vector<RenderLightSnapshot> lights,
                      RenderSkySnapshot sky,
                      RenderEnvironmentSnapshot environment,
                      RenderFrameSettings settings,
                      RenderFrameCaptureRequest captureRequest,
                      RenderFeatureSnapshot features,
                      RenderExtractionDiagnostics extractionDiagnostics);

    RenderFrameHeader m_header{};
    RenderViewSnapshot m_view{};
    std::vector<RenderPrimitiveSnapshot> m_primitives{};
    std::vector<RenderLightSnapshot> m_lights{};
    RenderSkySnapshot m_sky{};
    RenderEnvironmentSnapshot m_environment{};
    RenderFrameSettings m_settings{};
    RenderFrameCaptureRequest m_captureRequest{};
    RenderFeatureSnapshot m_features{};
    RenderExtractionDiagnostics m_extractionDiagnostics{};
};

enum class RenderFramePacketBuilderState : uint8
{
    Building = 0,
    Sealed = 1
};

enum class RenderFrameSealCode : uint8
{
    None = 0,
    Sealed = 1,
    AlreadySealed = 2,
    InvalidSchema = 3,
    InvalidSequence = 4,
    MissingValue = 5,
    InvalidViewport = 6,
    InvalidNumericValue = 7,
    IncompleteExtraction = 8,
    IncompleteFeatureSnapshot = 9,
    CountMismatch = 10,
    InvalidResourceReference = 11,
    InvalidCaptureRequest = 12
};

class RenderFramePacketBuilder final
{
public:
    RenderFramePacketBuilder() = default;
    RenderFramePacketBuilder(const RenderFramePacketBuilder&) = delete;
    RenderFramePacketBuilder& operator=(const RenderFramePacketBuilder&) = delete;
    RenderFramePacketBuilder(RenderFramePacketBuilder&&) = delete;
    RenderFramePacketBuilder& operator=(RenderFramePacketBuilder&&) = delete;

    bool SetHeader(RenderFrameHeader header);
    bool SetView(RenderViewSnapshot view);
    bool AddPrimitive(RenderPrimitiveSnapshot primitive);
    bool AddLight(RenderLightSnapshot light);
    bool SetSky(RenderSkySnapshot sky);
    bool SetEnvironment(RenderEnvironmentSnapshot environment);
    bool SetSettings(RenderFrameSettings settings);
    bool SetCaptureRequest(RenderFrameCaptureRequest request);
    bool SetFeatures(RenderFeatureSnapshot features);
    bool SetExtractionDiagnostics(RenderExtractionDiagnostics diagnostics);

    [[nodiscard]] std::unique_ptr<const RenderFramePacket> Seal();
    [[nodiscard]] RenderFrameSealCode GetLastSealCode() const noexcept;
    [[nodiscard]] RenderFramePacketBuilderState GetState() const noexcept;

private:
    RenderFramePacketBuilderState m_state =
        RenderFramePacketBuilderState::Building;
    RenderFrameSealCode m_lastSealCode = RenderFrameSealCode::None;
    std::optional<RenderFrameHeader> m_header{};
    std::optional<RenderViewSnapshot> m_view{};
    std::vector<RenderPrimitiveSnapshot> m_primitives{};
    std::vector<RenderLightSnapshot> m_lights{};
    std::optional<RenderSkySnapshot> m_sky{};
    std::optional<RenderEnvironmentSnapshot> m_environment{};
    std::optional<RenderFrameSettings> m_settings{};
    std::optional<RenderFrameCaptureRequest> m_captureRequest{};
    std::optional<RenderFeatureSnapshot> m_features{};
    std::optional<RenderExtractionDiagnostics> m_extractionDiagnostics{};
};
```

The builder starts `Building/None`. A failed `Seal()` returns null, stores the exact failure code, retains every value, remains `Building`, and may be repaired and retried. Successful `Seal()` moves values into one packet and becomes `Sealed/Sealed`; every subsequent setter/adder returns false without mutation, and a second `Seal()` returns null with `AlreadySealed` while state stays `Sealed`. The eight `std::optional` singleton values are the presence bits; primitive/light vectors may legally remain empty when their exact header counts are zero. Because `std::make_unique` is not a friend, the successful builder path constructs `std::unique_ptr<const RenderFramePacket>(new RenderFramePacket(...))` inside the friend method; the raw pointer never escapes and no alternate constructor API is added.

`Seal()` evaluates these stages in order and returns the named first failure:

1. `AlreadySealed` when state is `Sealed`; otherwise `MissingValue` when any singleton optional is absent.
2. `InvalidSchema` unless the packet schema ID/version are exact.
3. `InvalidSequence` unless header sequence is nonzero.
4. `InvalidViewport` unless width and height are nonzero.
5. `InvalidNumericValue` unless every view matrix/vector/scalar, primitive transform/bounds, light numeric value, sky/environment numeric value, and settings scalar is finite; bounds are ordered; `nearPlane > 0`, `farPlane > nearPlane`, `deltaTime >= 0`, `exposure > 0`, and `renderScale > 0`; post-process intensities/thresholds are nonnegative; enabled shadows require nonzero atlas/cascade counts and positive finite max distance; enabled GPU culling requires a nonzero visibility limit; enabled ray tracing requires nonzero instance/ray limits, while disabled ray tracing requires both shadow/reflection sub-switches false.
6. `IncompleteExtraction` unless diagnostics are `Complete`, `complete == true`, and all three skipped counts are zero.
7. `IncompleteFeatureSnapshot` unless aggregate/particle/water/terrain schema versions are exact, sequences equal the header sequence, statuses are `Complete`, and every `complete` flag is true.
8. `CountMismatch` unless header expected/extracted primitive counts equal `m_primitives.size()`, expected/extracted light counts equal `m_lights.size()`, expected/extracted feature-provider counts equal `features.metadata.providerCount`, both feature skipped-provider and particle skipped-instance counts are zero, particle skipped-reason storage is empty, aggregate item counts equal the three nested vector sizes, and each nested item count equals its vector size. Every `size_t -> uint32` comparison rejects values above `UINT32_MAX` rather than truncating.
9. `InvalidResourceReference` unless every primitive has nonzero object ID and a valid mesh handle; material, fallback mesh, and fallback material are independent optional bindings whose invalid value means that binding is disabled; every light has nonzero ID, uses a declared light type, and only carries an optional externally prepared shadow handle when `castsShadows` is true (ordinary shadow maps remain Render-owned); an invalid sky handle means sky is disabled; environment IBL handles are either all invalid (disabled) or all valid.
10. `InvalidCaptureRequest` unless `None` has zero request ID/extent and false alpha, or a non-`None` declared kind has nonzero ID/extent.

Cross-packet monotonic sequence checking belongs to Task 3's mailbox, not the builder. A repaired seal reruns the whole ordered table; no earlier failed stage is cached.

- [ ] **Step 5: Implement a declaration-aware contract checker**

`check_render_contract_fields.py` starts from the `RenderFramePacket` and `ResourceUploadRequest` instance-field graphs, tokenizes quoted includes recursively, and builds a cross-header index of aggregate/enum declarations plus `using`/`typedef` aliases. It traverses public, protected, and private instance fields while ignoring only methods, constructors/destructors, friends, and static data. The only owning templates are recursively traversed `std::vector`, `std::array`, `std::optional`, and `std::variant`; the only standard owning leaf is `std::string`. Primitive leaves are C++ fundamental types, RVX fixed-width numeric aliases, `size_t`/`std::size_t`, and resolved enums. The only external math leaves are resolved glm vec2/3/4, mat3/4, and quat aliases; project `AABB` must resolve and be traversed as an aggregate.

Any reachable non-allowlisted type that cannot be resolved is a failure with `unresolved reachable type: <type> via <field path>`. Missing roots/includes, parser/CLI errors, and conflicting duplicate declarations also fail closed. Raw pointers/references/function pointers and `std::function`, `std::span`, `std::string_view`, `std::reference_wrapper`, all smart pointers, `Core::Ref`/`Ref<>`, and RHI/DX11/DX12/Vulkan/Metal/OpenGL types or aliases are forbidden at any reachable depth. The traversal must cover `RenderFramePacket -> RenderFeatureSnapshot -> Particle/Water/TerrainRenderSnapshot` and `ResourceUploadRequest -> ResourceUploadPayload -> MaterialSourceData`; accessor return/parameter wrappers are ignored because they are methods rather than instance fields.

The checker regression creates isolated temporary include graphs for: a legal three-level vector/variant/optional/array/string contract; separate raw pointer, reference, function pointer, callback, span, string-view, `Ref`, shared-pointer, and RHI-alias failures; a forbidden second-level aggregate field; an alias-to-pointer in another header; an unresolved reachable type; and a root const-reference accessor that passes because methods are ignored. Finally it scans both real roots, requires exit zero, and asserts each reported reachable instance-field count is greater than zero. A same-file-only nested test or unknown-type-as-leaf behavior is insufficient.

- [ ] **Step 6: Run green verification**

```powershell
cmake --preset win_x64_debug
cmake --build build\win_x64_debug --config Debug --target RenderContractsValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderContractsValidation|Architecture.RenderContractFieldChecker" --output-on-failure
```

Expected: all selected tests pass; the checker reports both contract headers scanned with zero forbidden fields.

- [ ] **Step 7: Review and commit**

Review packet/request fields against design sections 5–7, run `git diff --check`, then:

```powershell
git add RenderContracts\Include\RenderContracts\RenderIdentity.h RenderContracts\Include\RenderContracts\ResourceUploadRequest.h RenderContracts\Include\RenderContracts\RenderFramePacket.h RenderContracts\Private\ResourceUploadRequestInternal.h RenderContracts\Private\ResourceUploadRequest.cpp RenderContracts\Private\RenderFramePacket.cpp RenderContracts\CMakeLists.txt RenderExtraction\Include\RenderExtraction\RenderFramePacketBuilder.h RenderExtraction\Private\RenderFramePacketBuilder.cpp RenderExtraction\CMakeLists.txt Scripts\check_render_contract_fields.py Scripts\test_render_contract_field_checker.py Tests\RenderContractsValidation\main.cpp Tests\CMakeLists.txt
git commit -m "feat: freeze m1 render contracts"
```

---

### Task 2: Implement the packed status table and update-owned generational allocator

**Files:**

- Create: `RenderContracts/Include/RenderContracts/IRenderResourceGateway.h`
- Create: `Render/Private/Runtime/RenderResourceStatusTable.h`
- Create: `Render/Private/Runtime/RenderResourceStatusTable.cpp`
- Create: `Render/Private/Runtime/RenderResourceReservationDirectory.h`
- Create: `Render/Private/Runtime/RenderResourceReservationDirectory.cpp`
- Create: `Tests/RenderConcurrencyValidation/main.cpp`
- Modify: `Render/CMakeLists.txt`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**

Freeze the public gateway/status ABI before implementing storage. Task 3 consumes this header unchanged:

```cpp
enum class RenderResourcePublicState : uint8
{
    Released = 0,
    Reserved = 1,
    UploadQueued = 2,
    Uploading = 3,
    GPUReady = 4,
    Failed = 5,
    Evicting = 6
};

enum class RenderResourceFailureCode : uint16
{
    None = 0,
    InvalidPayload = 1,
    DependencyUnavailable = 2,
    ResourceCreationFailed = 3,
    UploadSubmissionFailed = 4,
    DeviceLost = 5,
    Cancelled = 6,
    RuntimeFailure = 7
};

enum class RenderResourceReserveCode : uint8
{
    Reserved = 0,
    Existing = 1,
    InvalidAsset = 2,
    KindMismatch = 3,
    CapacityExceeded = 4,
    ShuttingDown = 5
};

enum class RenderUploadEnqueueCode : uint8
{
    Accepted = 0,
    QueueFullByCount = 1,
    QueueFullByBytes = 2,
    InvalidRequest = 3,
    StaleGeneration = 4,
    Cancelled = 5,
    ShuttingDown = 6
};

enum class RenderReleaseCode : uint8
{
    Accepted = 0,
    StaleGeneration = 1,
    AlreadyPending = 2,
    ShuttingDown = 3
};

enum class RenderResourceStatusCode : uint8
{
    Current = 0,
    InvalidHandle = 1,
    StaleGeneration = 2
};

struct RenderResourceStatus
{
    RenderResourceStatusCode code = RenderResourceStatusCode::InvalidHandle;
    RenderResourcePublicState state = RenderResourcePublicState::Released;
    RenderResourceFailureCode failure = RenderResourceFailureCode::None;
};

struct RenderResourceReserveResult
{
    RenderResourceReserveCode code = RenderResourceReserveCode::InvalidAsset;
    RenderResourceHandle handle;
    RenderResourceStatus status;
};

struct RenderUploadEnqueueResult
{
    RenderUploadEnqueueCode code = RenderUploadEnqueueCode::InvalidRequest;
};

struct RenderReleaseResult
{
    RenderReleaseCode code = RenderReleaseCode::StaleGeneration;
};

class IRenderResourceGateway
{
public:
    virtual ~IRenderResourceGateway() = default;
    virtual RenderResourceReserveResult ReserveResource(
        AssetId assetId,
        RenderResourceKind kind) noexcept = 0;
    virtual RenderUploadEnqueueResult TryEnqueueUpload(
        const ResourceUploadRequestRef& request) noexcept = 0;
    virtual RenderReleaseResult RequestRelease(
        RenderResourceHandle handle) noexcept = 0;
    [[nodiscard]] virtual RenderResourceStatus QueryResourceStatus(
        RenderResourceHandle handle) const noexcept = 0;
};
```

`Existing` is the only non-new success and returns the existing valid handle plus the status observed by the same acquire read. Every reserve failure returns an invalid handle. Invalid/stale queries never expose the newer generation's state. Tasks 2 and 3 may not add or rename public enum values/result fields; a discovered contract gap requires architecture review.

The atomic word layout is fixed as `generation[63:32] | failure[31:16] | state[7:0]`; bits 15:8 remain zero and are rejected when decoding. The table allocates `capacity` atomics before thread start, requires capacity >= 1,024, reserves slot zero, and never resizes.

```cpp
struct PackedRenderResourceStatus
{
    uint32 generation = 0;
    RenderResourcePublicState state = RenderResourcePublicState::Released;
    RenderResourceFailureCode failure = RenderResourceFailureCode::None;
};

enum class RenderStatusWriter : uint8
{
    Update = 0,
    Render = 1
};

class RenderResourceStatusTable final
{
public:
    explicit RenderResourceStatusTable(uint32 capacity);
    RenderResourceStatus Query(RenderResourceHandle handle) const noexcept;
    bool CompareExchange(RenderResourceHandle handle,
                         PackedRenderResourceStatus expected,
                         PackedRenderResourceStatus desired,
                         RenderStatusWriter writer) noexcept;
};
```

- [ ] **Step 1: Write failing state/generation tests**

Add ABI `static_assert`s for every public state/failure/outcome enum value and for both internal `RenderStatusWriter` values, plus default-result tests proving failures carry invalid handles. Cover minimum capacity rejection, slot zero, first generation one, exact acquire query, stale generation without newer-state disclosure, `Existing` handle/status consistency, update-writer transition whitelist, render-writer transition whitelist, rejection of an undeclared writer value, release/reuse, and generation wrap retirement. Add a two-thread latch test proving a released terminal payload value is visible after an acquire query observes the terminal state.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderConcurrencyValidation
```

Expected: failure because status table and reservation directory are absent.

- [ ] **Step 3: Implement packed publication and writer-specific CAS validation**

Use acquire loads, release successful CAS, and relaxed failed-CAS retry only when the caller revalidates the newly observed word. Reject stale generations before state comparison. The update writer allows only the design’s `Released -> Reserved`, `Reserved -> UploadQueued`, and live-state-to-`Evicting` transitions. The render writer allows only `UploadQueued -> Uploading`, `Uploading -> GPUReady/Failed`, and `Evicting -> Released`.

In `Render/CMakeLists.txt`, create internal static target `RVX_RenderRuntimeCore` with private runtime headers exposed only to Render/tests, link it to Core and RenderContracts, and link `RVX_Render` privately to it. Add later task sources to this target instead of compiling duplicate copies into tests.

- [ ] **Step 4: Implement reservation ownership and wrap retirement**

The directory owns `std::unordered_map<AssetId, ReservationEntry, AssetIdHash>` where `ReservationEntry` is exactly `{RenderResourceKind kind; RenderResourceHandle handle;}`, plus the free-slot list and retired-slot bits on the update thread. First use assigns generation one. Reuse increments before publication. `UINT32_MAX` is never incremented to zero; that slot is retired and capacity decreases observably. Removing an asset mapping on accepted release permits a replacement asset generation to reserve a different free slot while the old generation remains independently evicting.

- [ ] **Step 5: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderConcurrencyValidation
ctest --test-dir build\win_x64_debug -C Debug -R RenderConcurrencyValidation --output-on-failure
```

Expected: status, stale-generation, writer-whitelist, replacement, and wrap-retirement tests pass.

- [ ] **Step 6: Review and commit**

```powershell
git diff --check
git add RenderContracts\Include\RenderContracts\IRenderResourceGateway.h Render\Private\Runtime Render\CMakeLists.txt Tests\RenderConcurrencyValidation Tests\CMakeLists.txt
git commit -m "feat: add generational render status table"
```

---

### Task 3: Add bounded frame/upload/release/control transports and the gateway core

**Files:**

- Create: `Render/Include/Render/RenderTransportTypes.h`
- Create: `Render/Private/Runtime/RenderFrameMailbox.h`
- Create: `Render/Private/Runtime/RenderFrameMailbox.cpp`
- Create: `Render/Private/Runtime/RenderUploadQueue.h`
- Create: `Render/Private/Runtime/RenderUploadQueue.cpp`
- Create: `Render/Private/Runtime/RenderReleaseQueue.h`
- Create: `Render/Private/Runtime/RenderReleaseQueue.cpp`
- Create: `Render/Private/Runtime/RenderControlMailbox.h`
- Create: `Render/Private/Runtime/RenderControlMailbox.cpp`
- Create: `Render/Private/Runtime/RenderResourceGateway.h`
- Create: `Render/Private/Runtime/RenderResourceGateway.cpp`
- Modify: `Render/Private/Runtime/RenderResourceStatusTable.h`
- Modify: `Render/Private/Runtime/RenderResourceStatusTable.cpp`
- Modify: `Tests/RenderConcurrencyValidation/main.cpp`
- Modify: `Render/CMakeLists.txt`

**Interfaces:**

```cpp
struct RenderTransportConfig
{
    uint32 frameCapacity = 3;
    uint32 uploadRequestCapacity = 1024;
    uint64 uploadByteCapacity = 256ull * 1024ull * 1024ull;
    uint32 statusSlotCapacity = 262144;
};

struct RenderIterationBudgets
{
    uint32 uploadRequestCount = 64;
    uint64 uploadBytes = 32ull * 1024ull * 1024ull;
    std::chrono::milliseconds uploadTime{2};
    uint32 releaseCount = 1024;
    std::chrono::milliseconds releaseTime{1};
};
```

Both structs are defined once in public `Render/RenderTransportTypes.h`, which includes only Core/standard value dependencies and has no private runtime declarations. Task 3 private queues include this header; Task 7's public `RenderRuntimeTypes.h` includes it. Neither task redeclares the structs, and no public header includes `Render/Private`.

`RenderResourceGateway final : public IRenderResourceGateway` owns the update-side reservation directory and composes the fixed table plus upload/release transports. It implements the Task 2 signatures with `noexcept` unchanged and is the object delegated to by RenderSubsystem. Task 3 may map internal pressure/state outcomes to the frozen public codes but may not alter `ResourceUploadRequest`, packet/builder, or any public gateway enum/result.

- [ ] **Step 1: Extend failing concurrency tests**

Add parameterized frame capacities 2, 3, and 4; latest-complete-wins replacement; consumer coalescing; out-of-order rejection; destruction outside the mailbox lock; upload count and derived-byte pressure; state unchanged on rejected upload; release-ring capacity proof; duplicate release; stale release; resize/surface generation coalescing; and stop preemption.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderConcurrencyValidation
```

Expected: compile failure on missing mailbox/gateway types.

- [ ] **Step 3: Implement the frame mailbox**

Publication validates schema/completeness/sequence before taking the mutex. When full, move the oldest packet to a local `unique_ptr`, append the newest under the lock, unlock, then destroy the replaced packet. Consumer acquisition moves the newest packet and all older discard packets under the lock, records their sequences, unlocks, then destroys discards. Never replace an acquired packet.

- [ ] **Step 4: Implement the upload queue and atomic enqueue transition**

Under the producer mutex: validate request, generation, count, and `GetDerivedPayloadBytes()`; CAS `Reserved -> UploadQueued`; append the shared immutable request; update exact retained-byte counters; unlock; wake Render. Count/byte pressure leaves the status `Reserved`. Consumer dequeue decrements counters in the same critical section and enforces the configured request/byte/time drain budgets at the runtime layer.

- [ ] **Step 5: Implement release and coalesced control publication**

Allocate the release ring with usable capacity `statusSlotCapacity - 1`. CAS to `Evicting` before ring publication. A failed publication after successful CAS calls the runtime-fatal sink; it is not converted to queue pressure. Store only newest generation-ordered surface and resize values. Stop is an atomic out-of-band flag and every publication path invokes one wake callback.

- [ ] **Step 6: Implement the narrow gateway outcomes**

Implement every outcome listed in design section 8.1. A failed reserve returns an invalid handle. `Existing` returns the matching observed state. Query mismatch returns `StaleGeneration`, never the newer state. Do not throw on pressure or stale input.

- [ ] **Step 7: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderConcurrencyValidation
ctest --test-dir build\win_x64_debug -C Debug -R RenderConcurrencyValidation --output-on-failure
```

Expected: all queue capacities, pressure outcomes, coalescing, stale generation, and release capacity tests pass without sleeps.

- [ ] **Step 8: Review and commit**

```powershell
git diff --check
git add Render\Include\Render\RenderTransportTypes.h Render\Private\Runtime Render\CMakeLists.txt Tests\RenderConcurrencyValidation
git commit -m "feat: add bounded render transports"
```

---

### Task 4: Add shared executor semantics, ownership guards, and platform thread bootstrap

**Files:**

- Create: `Render/Private/Runtime/IRenderExecutor.h`
- Create: `Render/Private/Runtime/RenderThreadGuard.h`
- Create: `Render/Private/Runtime/RenderThreadGuard.cpp`
- Create: `Render/Private/Runtime/DedicatedRenderExecutor.h`
- Create: `Render/Private/Runtime/DedicatedRenderExecutor.cpp`
- Create: `Render/Private/Runtime/RenderThreadPlatform.h`
- Create: `Render/Private/Runtime/RenderThreadPlatform.cpp`
- Create: `Render/Private/Runtime/RenderThreadPlatformApple.mm`
- Create: `Tests/Common/RenderRuntimeTestSupport.h`
- Create: `Tests/Common/RenderRuntimeTestSupport.cpp`
- Create: `Tests/RenderExecutorValidation/main.cpp`
- Modify: `Render/CMakeLists.txt`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**

```cpp
enum class RenderPumpDecision : uint8
{
    Progressed = 0,
    Idle,
    Stop
};

enum class RenderExecutorStartCode : uint8
{
    Started = 0,
    AlreadyStarted = 1,
    ThreadCreationFailed = 2
};

struct RenderExecutorStartResult
{
    RenderExecutorStartCode code =
        RenderExecutorStartCode::ThreadCreationFailed;
    uint32 nativeError = 0;
};

enum class RenderExecutorJoinCode : uint8
{
    Joined = 0,
    NotStarted = 1,
    TimedOut = 2
};

struct RenderExecutorJoinResult
{
    RenderExecutorJoinCode code = RenderExecutorJoinCode::NotStarted;
};

class IRenderExecutorPump
{
public:
    virtual ~IRenderExecutorPump() = default;
    virtual RenderPumpDecision PumpOnce() noexcept = 0;
    virtual void WaitForWork() noexcept = 0;
    virtual void Wake() noexcept = 0;
    virtual void OnUnhandledExecutorException() noexcept = 0;
};

class IRenderExecutor
{
public:
    virtual ~IRenderExecutor() = default;
    virtual RenderExecutorStartResult Start(IRenderExecutorPump& pump) = 0;
    virtual void NotifyWork() noexcept = 0;
    virtual RenderExecutorJoinResult JoinUntil(
        std::chrono::steady_clock::time_point deadline) = 0;
};
```

Production CMake exposes only a `CreateDedicatedRenderExecutor()` factory. `InlineRenderExecutor` is defined and linked only by `RVX_RenderTestSupport`.

These executor-local result types are defined in `IRenderExecutor.h` in Task 4 and are independently buildable; Task 7's public runtime/startup/shutdown results wrap them but do not redefine or replace them. `Start()` returns `AlreadyStarted` without mutation, maps `std::system_error` during thread creation to `ThreadCreationFailed` plus the native error when available, and returns `Started` only after the worker has entered its platform bootstrap. `JoinUntil()` returns `NotStarted` before a successful start, `TimedOut` without detaching or joining when the deadline expires, and `Joined` only after exit was signaled and `join()` completed.

- [ ] **Step 1: Write failing shared and dedicated executor tests**

Parameterize a pump-conformance fixture over Inline and Dedicated executors. Prove the same `PumpOnce()` sequence, thread guard ownership, wake from idle, stop while idle, exception containment, and no repeated work while idle. Add ABI/default-result tests for every executor enum/result and explicit already-started/not-started/timed-out cases. Dedicated-only tests prove construction and destruction thread IDs differ from update, the name is `RVX Render`, and join returns only after the pump stops.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderExecutorValidation
```

Expected: failure because executor/test-support targets are missing.

- [ ] **Step 3: Implement guards and the dedicated loop**

`RenderThreadGuard` records the owner `std::thread::id` once and provides debug assertions plus structured release-build failure codes. Dedicated executor starts exactly one `std::thread`, calls platform bootstrap before the first pump, loops on `PumpOnce()`, calls `WaitForWork()` only after `Idle`, catches every exception at the thread entry, records a terminal fault through `OnUnhandledExecutorException()`, and signals an exit condition before returning. `JoinUntil` waits on that condition, then calls `join()`; it never detaches.

- [ ] **Step 4: Implement platform policy**

Windows uses `SetThreadDescription` and below-normal/non-realtime priority. Linux uses `pthread_setname_np` and a documented normal scheduling policy. Apple uses Objective-C++ to set the name/QoS, owns a top-level autorelease pool, and wraps every pump iteration with a nested `@autoreleasepool`. Non-Apple iteration scope is a no-op object with the same internal interface.

- [ ] **Step 5: Implement test-only Inline execution**

Inline `Start()` records an explicit render-thread test scope and never starts a thread. Its `NotifyWork()` drives one or more non-waiting `PumpOnce()` calls until the pump reports `Idle` or `Stop`; it never calls `WaitForWork()`. No production header or config enum mentions Inline.

- [ ] **Step 6: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderExecutorValidation
ctest --test-dir build\win_x64_debug -C Debug -R RenderExecutorValidation --output-on-failure
```

Expected: shared conformance and dedicated separation/wakeup/stop tests pass with zero arbitrary sleeps.

- [ ] **Step 7: Review and commit**

```powershell
git diff --check
git add Render\Private\Runtime Render\CMakeLists.txt Tests\Common Tests\RenderExecutorValidation Tests\CMakeLists.txt
git commit -m "feat: add dedicated render executor"
```

---

### Task 5: Replace raw window handles with a cross-platform native-surface contract

**Files:**

- Create: `RHI/Include/RHI/RHINativeSurface.h`
- Modify: `RHI/Include/RHI/RHIDevice.h`
- Modify: `RHI/Include/RHI/RHISwapChain.h`
- Modify: `Render/Include/Render/Context/RenderContext.h`
- Modify: `Render/Private/Context/RenderContext.cpp`
- Modify: `Render/Include/Render/RenderSubsystem.h`
- Modify: `Render/Private/RenderSubsystem.cpp`
- Create: `HAL/Include/HAL/Window/WindowRenderSurfaceHandles.h`
- Modify: `HAL/Include/HAL/Window/IWindow.h`
- Modify: `HAL/Private/GLFW/GLFWWindow.h`
- Modify: `HAL/Private/GLFW/GLFWWindow.cpp`
- Create: `HAL/Private/Apple/GLFWMetalLayerBridge.h`
- Create: `HAL/Private/Apple/GLFWMetalLayerBridge.mm`
- Modify: `HAL/CMakeLists.txt`
- Modify: `Runtime/Include/Runtime/Window/WindowSubsystem.h`
- Modify: `Runtime/Private/Window/WindowSubsystem.cpp`
- Modify: `RHI_DX12/Private/DX12SwapChain.cpp`
- Modify: `RHI_DX11/Private/DX11SwapChain.cpp`
- Modify: `RHI_Vulkan/Private/VulkanDevice.cpp`
- Modify: `RHI_Vulkan/Private/VulkanSwapChain.h`
- Modify: `RHI_Vulkan/Private/VulkanSwapChain.cpp`
- Modify: `RHI_Vulkan/CMakeLists.txt`
- Modify: `RHI_Metal/Private/MetalSwapChain.h`
- Modify: `RHI_Metal/Private/MetalSwapChain.mm`
- Modify: `RHI_Metal/Private/MetalCommandContext.h`
- Modify: `RHI_Metal/Private/MetalCommandContext.mm`
- Modify: `RHI_OpenGL/Private/OpenGLDevice.cpp`
- Modify: `RHI_OpenGL/Private/OpenGLSwapChain.cpp`
- Modify: `Samples/Basic/BasicRHI/main.cpp`
- Modify: `Samples/Basic/ComputeDemo/main.cpp`
- Modify: `Editor/Include/Editor/EditorMainSwapChainService.h`
- Modify: `Editor/Private/EditorMainSwapChainService.cpp`
- Modify: `Editor/Private/EditorRenderBootstrapService.cpp`
- Modify: `Tests/EditorContextValidation/main.cpp`
- Modify: `Tests/RHIContractValidation/main.cpp`

**Interface:**

```cpp
enum class NativeSurfacePlatform : uint8
{
    None = 0,
    Win32,
    X11,
    Wayland,
    Cocoa,
    UIKit,
    GLFW
};

struct NativeSurfaceDesc
{
    NativeSurfacePlatform platform = NativeSurfacePlatform::None;
    uintptr_t nativeWindow = 0;
    uintptr_t nativeDisplay = 0;
    uintptr_t nativeLayer = 0;
    uintptr_t backendWindow = 0;
    uint32 width = 0;
    uint32 height = 0;
    float32 contentScale = 1.0f;
    RHIFormat preferredFormat = RHIFormat::BGRA8_UNORM;
    bool vsync = true;
    uint64 generation = 0;

    [[nodiscard]] bool IsValidFor(RHIBackendType backend) const noexcept;
};

struct RHISwapChainDesc
{
    NativeSurfaceDesc surface;
    uint32 bufferCount = 3;
    const char* debugName = nullptr;
};
```

HAL does not depend on RHI. `IWindow` instead publishes the platform handles through:

```cpp
struct WindowRenderSurfaceHandles
{
    uintptr_t nativeWindow = 0;
    uintptr_t nativeDisplay = 0;
    uintptr_t nativeLayer = 0;
    uintptr_t backendWindow = 0;
    uint32 width = 0;
    uint32 height = 0;
    float32 contentScale = 1.0f;
};

virtual WindowRenderSurfaceHandles CaptureRenderSurfaceHandles() = 0;
virtual void ReleaseGraphicsContextFromCurrentThread() = 0;
```

WindowSubsystem captures this value on Main/Update Thread and maps it into `NativeSurfaceDesc`; no HAL header includes RHI.

`nativeWindow` is the platform window handle required by DX/Vulkan, `nativeLayer` is an already attached `CAMetalLayer*` required by Metal, and `backendWindow` is the GLFW window used by Vulkan/OpenGL. The descriptor never owns any object. On Apple, `nativeWindow` is diagnostic only for Metal and Metal RHI code may not dereference it. Extent, format, and vsync have one source of truth in `NativeSurfaceDesc`; `RHISwapChainDesc` must not retain duplicate width/height/format/vsync fields whose values could diverge.

`RenderContext` changes atomically to `bool Initialize(const RenderContextConfig&, const NativeSurfaceDesc& initialSurface = {})` and `bool CreateSwapChain(const NativeSurfaceDesc&)`; the old raw-pointer overload is removed in this task. RenderSubsystem, Editor's compile-only bootstrap/service, and both Basic diagnostic samples are migrated in the same commit, so no intermediate caller still writes the removed `RHISwapChainDesc` fields. Editor's result stores the captured `NativeSurfaceDesc` rather than `void* windowHandle`. Tests that only implement `IRHIDevice::CreateSwapChain(const RHISwapChainDesc&)` need no source change because the virtual signature itself is unchanged.

- [ ] **Step 1: Add failing RHI surface contract tests**

Test backend-specific validation, zero extent/generation, non-finite/non-positive `contentScale`, stale generation ordering, absence of the old window/width/height/format/vsync fields in `RHISwapChainDesc`, and that OpenGL initialization receives the backend window before GLAD loading. Add compile/source checks proving BasicRHI, ComputeDemo, RenderSubsystem, and Editor no longer assign those removed fields or call the removed raw `RenderContext::CreateSwapChain` overload. Add a source-boundary test that fails while `MetalSwapChain.mm` references `NSWindow`, `NSView`, `UIView`, `contentView`, `setWantsLayer:`, `setLayer:`, or `[m_currentDrawable present]`; prove the acquired drawable is forwarded to `MetalCommandContext` and scheduled through `presentDrawable:` before commit.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RHIContractValidation
```

Expected: failure until the descriptor and backend migrations exist.

- [ ] **Step 3: Add the descriptor to RHI device/swapchain creation**

Add `NativeSurfaceDesc initialSurface` to `RHIDeviceDesc`; replace `RHISwapChainDesc::windowHandle/width/height/format/vsync` with the single `NativeSurfaceDesc surface`. `RenderContext::Initialize(config, initialSurface)` passes that value to device creation, and `CreateSwapChain` accepts one `NativeSurfaceDesc`. RenderSubsystem captures/maps the WindowSubsystem surface before context initialization. Editor captures its compile-only surface before bootstrap initialization. BasicRHI/ComputeDemo construct a tagged descriptor before direct device creation and reuse the same value for swapchain creation; their macOS diagnostic path may report unsupported when no HAL-owned layer is available, but it must compile and may not synthesize a Render-thread view/layer mutation.

- [ ] **Step 4: Transfer OpenGL context ownership**

Add `IWindow::ReleaseGraphicsContextFromCurrentThread()` and forward it through WindowSubsystem. GLFW calls `glfwMakeContextCurrent(nullptr)` only for OpenGL windows. Engine will call this after surface capture and before Render startup. OpenGLDevice validates `initialSurface.backendWindow`, makes that GLFW context current on Render Thread, then loads GLAD. OpenGLSwapChain uses the same backend handle and never rebinds on update.

- [ ] **Step 5: Make Vulkan surface creation cross-platform**

Link `RVX_RHI_Vulkan` privately to GLFW. Build the Vulkan instance-extension list from `glfwGetRequiredInstanceExtensions` when `initialSurface.backendWindow` is present. Replace Win32-only surface construction with `glfwCreateWindowSurface`; reject missing/failed surfaces before present-support queries. Keep backend-native diagnostic codes in the error result.

- [ ] **Step 6: Attach the Apple presentation layer on Main Thread and migrate DX/Metal consumers**

DX11/DX12 validate `Win32` and cast `surface.nativeWindow` through `uintptr_t` to HWND. `GLFWMetalLayerBridge.mm` asserts the caller is Main Thread, obtains the Cocoa window/view through GLFW, creates or reuses one attached `CAMetalLayer`, and returns its non-owning handle without creating an `MTLDevice`. `GLFWWindow::CaptureRenderSurfaceHandles()` invokes that bridge only on Apple and returns the layer plus content scale; HAL keeps the window/view/layer alive until Render shutdown acknowledgement and detaches it on Main Thread afterward. Metal validates Cocoa/UIKit plus nonzero `nativeLayer`, bridges only that field to `CAMetalLayer*`, and never imports or messages `NSWindow`, `NSView`, or `UIView`. Render Thread sets the Metal-facing layer properties (`device`, pixel format, framebuffer-only, drawable size/count, presentation policy); resize updates layer/swapchain properties without acquiring a drawable. Only an actionable frame may call `nextDrawable`; that drawable is forwarded to `MetalCommandContext` and scheduled through `presentDrawable:` before command-buffer commit. Drawable unavailability is a structured non-terminal skip; idle and resize-only work do not acquire/present, and `MetalSwapChain::Present()` never invokes direct drawable presentation. GLFW exposes `glfwGetWin32Window` on Windows; Linux supplies the GLFW backend handle for Vulkan/OpenGL.

- [ ] **Step 7: Run focused backend contract verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RHIContractValidation DX12Validation VulkanValidation DX11Validation BasicRHI ComputeDemo
ctest --test-dir build\win_x64_debug -C Debug -R "RHIContractValidation|DX12Validation.DeviceCreation|VulkanValidation.DeviceCreation|DX11Validation.DeviceCreation" --output-on-failure
cmake --preset win_x64_debug -B build\win_x64_editor_compile -DRVX_BUILD_EDITOR=ON
cmake --build build\win_x64_editor_compile --config Debug --target RVXEditor EditorContextValidation
ctest --test-dir build\win_x64_editor_compile -C Debug -R EditorContextValidation --output-on-failure
```

Expected: contract tests and available backend device tests pass. This task does not yet claim native lifecycle smoke completion.

- [ ] **Step 8: Review and commit**

```powershell
git diff --check
git add RHI RHI_DX11 RHI_DX12 RHI_Vulkan RHI_Metal RHI_OpenGL HAL Runtime Render\Include\Render\Context Render\Private\Context Render\Include\Render\RenderSubsystem.h Render\Private\RenderSubsystem.cpp Samples\Basic\BasicRHI\main.cpp Samples\Basic\ComputeDemo\main.cpp Editor\Include\Editor\EditorMainSwapChainService.h Editor\Private\EditorMainSwapChainService.cpp Editor\Private\EditorRenderBootstrapService.cpp Tests\RHIContractValidation\main.cpp Tests\EditorContextValidation\main.cpp Tests\CMakeLists.txt
git commit -m "feat: add tagged native surface contract"
```

---

### Task 6: Add staged subsystem initialization and targeted shutdown

**Files:**

- Modify: `Core/Include/Core/Subsystem/SubsystemCollection.h`
- Modify: `Tests/AppModeBoundaryValidation/main.cpp`

**Interface:**

```cpp
using InitializeHook = std::function<void(TBase&)>;

bool InitializeAll(const InitializeHook& beforeInitialize = {},
                   const InitializeHook& afterInitialize = {});

template<typename T>
bool DeinitializeSubsystem();
```

- [ ] **Step 1: Write failing lifecycle tests**

Prove the before hook runs after dependencies are initialized but before the target initializes, the after hook sees the target marked initialized, a throwing hook unwinds only successfully initialized subsystems, targeted deinitialization happens once, later `DeinitializeAll()` skips it, ticking skips deinitialized subsystems, and collection state becomes false after the final live subsystem stops.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target AppModeBoundaryValidation
ctest --test-dir build\win_x64_debug -C Debug -R AppModeBoundaryValidation --output-on-failure
```

Expected: new lifecycle tests fail to compile.

- [ ] **Step 3: Implement hooks inside the existing unwind boundary**

Invoke `beforeInitialize(*subsystem)`, `subsystem->Initialize()`, set initialized, then `afterInitialize(*subsystem)` within the same try block. If after-hook fails, include that subsystem in reverse unwind. `DeinitializeAll` and tick paths check `IsInitialized()`.

- [ ] **Step 4: Implement typed targeted deinitialization**

Look up the subsystem, return true if already stopped, call its `Deinitialize()` under the same exception logging policy, always clear its initialized flag, and recompute collection state from remaining initialized entries. Do not rebuild dependency order during shutdown.

- [ ] **Step 5: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target AppModeBoundaryValidation
ctest --test-dir build\win_x64_debug -C Debug -R AppModeBoundaryValidation --output-on-failure
```

Expected: existing unwind tests and all new staged/targeted lifecycle tests pass.

- [ ] **Step 6: Review and commit**

```powershell
git diff --check
git add Core\Include\Core\Subsystem\SubsystemCollection.h Tests\AppModeBoundaryValidation\main.cpp
git commit -m "feat: stage subsystem startup and shutdown"
```

---

### Task 7: Introduce the render runtime shell, structured results, and immutable diagnostics

**Files:**

- Create: `Render/Include/Render/RenderRuntimeTypes.h`
- Create: `Render/Include/Render/RenderDiagnostics.h`
- Create: `Render/Private/Runtime/RenderDiagnosticsPublisher.h`
- Create: `Render/Private/Runtime/RenderDiagnosticsPublisher.cpp`
- Create: `Render/Private/Runtime/RenderThreadRuntime.h`
- Create: `Render/Private/Runtime/RenderThreadRuntime.cpp`
- Create: `Tests/RenderThreadRuntimeValidation/main.cpp`
- Modify: `Render/Include/Render/RenderSubsystem.h`
- Modify: `Render/Private/RenderSubsystem.cpp`
- Modify: `Render/CMakeLists.txt`
- Modify: `Tests/Common/RenderRuntimeTestSupport.h`
- Modify: `Tests/Common/RenderRuntimeTestSupport.cpp`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**

```cpp
enum class RenderExecutorKind : uint8
{
    None = 0,
    Dedicated = 1,
    InlineTest = 2
};

enum class RenderLifecycleState : uint8
{
    Stopped = 0,
    Starting = 1,
    Running = 2,
    StopRequested = 3,
    Draining = 4,
    Failed = 5
};

enum class RenderResultClass : uint8
{
    Success = 0,
    ExpectedPressure = 1,
    RecoverableFrame = 2,
    FrameFatal = 3,
    RuntimeFatal = 4
};

enum class RenderFramePublishCode : uint8
{
    Accepted = 0,
    ReplacedOlder = 1,
    InvalidPacket = 2,
    OutOfOrder = 3,
    ShuttingDown = 4,
    NotRunning = 5
};

struct RenderFramePublishResult
{
    RenderFramePublishCode code = RenderFramePublishCode::InvalidPacket;
    RenderResultClass resultClass = RenderResultClass::RecoverableFrame;
    uint64 sequence = 0;
    uint64 replacedSequence = 0;
};

enum class RenderResizeCode : uint8
{
    Accepted = 0,
    CoalescedOlder = 1,
    StaleGeneration = 2,
    InvalidSurface = 3,
    ShuttingDown = 4,
    NotRunning = 5
};

struct RenderResizeResult
{
    RenderResizeCode code = RenderResizeCode::InvalidSurface;
    RenderResultClass resultClass = RenderResultClass::RecoverableFrame;
    uint64 generation = 0;
    uint64 replacedGeneration = 0;
};

enum class RenderRuntimeCode : uint8
{
    None = 0,
    Running = 1,
    StopRequested = 2,
    Stopped = 3,
    InvalidConfiguration = 4,
    InvalidSurface = 5,
    ExecutorStartFailed = 6,
    DeviceCreationFailed = 7,
    SurfaceCreationFailed = 8,
    RenderGraphValidationFailed = 9,
    DeviceLost = 10,
    UnhandledException = 11,
    OwnershipViolation = 12,
    StartupTimedOut = 13,
    ShutdownTimedOut = 14
};

enum class RenderTerminalCause : uint8
{
    None = 0,
    NormalStop = 1,
    StartupFailure = 2,
    DeviceLost = 3,
    UnhandledException = 4,
    OwnershipViolation = 5,
    WatchdogTimeout = 6,
    ExecutorFailure = 7
};

enum class RenderTeardownMode : uint8
{
    None = 0,
    NormalDrain = 1,
    DeviceLostTeardown = 2,
    FatalTimeout = 3
};

struct RenderRuntimeResult
{
    RenderRuntimeCode code = RenderRuntimeCode::None;
    RenderResultClass resultClass = RenderResultClass::Success;
    RenderLifecycleState lifecycle = RenderLifecycleState::Stopped;
    RenderExecutorKind executor = RenderExecutorKind::None;
    RenderTerminalCause terminalCause = RenderTerminalCause::None;
    RenderTeardownMode teardownMode = RenderTeardownMode::None;
    RHIBackendType backend = RHIBackendType::None;
    uint64 frameSequence = 0;
    uint64 requestSequence = 0;
    AssetId assetId{};
    RenderResourceHandle handle{};
    uint64 surfaceGeneration = 0;
    uint32 nativeError = 0;
    std::string message{};
};

enum class RenderShutdownCode : uint8
{
    None = 0,
    Completed = 1,
    AlreadyStopped = 2,
    DeviceLost = 3,
    TimedOut = 4,
    ExecutorJoinFailed = 5
};

struct RenderShutdownResult
{
    RenderShutdownCode code = RenderShutdownCode::None;
    RenderResultClass resultClass = RenderResultClass::Success;
    RenderLifecycleState lifecycle = RenderLifecycleState::Stopped;
    RenderTerminalCause terminalCause = RenderTerminalCause::None;
    RenderTeardownMode teardownMode = RenderTeardownMode::None;
    RHIBackendType backend = RHIBackendType::None;
    uint64 lastSubmittedFrameSequence = 0;
    uint64 surfaceGeneration = 0;
    uint32 nativeError = 0;
    std::string message{};
};

struct RenderRuntimeConfig
{
    RHIBackendType backendType = RHIBackendType::Auto;
    bool enableValidation = true;
    bool enableGPUValidation = false;
    uint32 frameBuffering = 2;
    RenderTransportConfig transports{};
    RenderIterationBudgets iterationBudgets{};
    std::chrono::milliseconds startupWatchdog{60000};
    std::chrono::milliseconds shutdownWatchdog{30000};
};

void Configure(const RenderRuntimeConfig& config,
               const NativeSurfaceDesc& surface);
RenderFramePublishResult TryPublishFrame(
    std::unique_ptr<const RenderFramePacket> packet);
RenderResizeResult RequestResize(const NativeSurfaceDesc& surface);
RenderDiagnosticsSnapshot GetDiagnosticsSnapshot() const;
RenderRuntimeResult GetLastRuntimeResult() const;
RenderShutdownResult GetLastShutdownResult() const;
```

Every enum/result/config declaration above lives in public `RenderRuntimeTypes.h`; the six composition/publication/control/getter signatures are `RenderSubsystem` members declared in `RenderSubsystem.h`, not free functions. No public type includes private `IRenderExecutor.h`. Task 4's `RenderExecutorStartResult`/`RenderExecutorJoinResult` are mapped into `RenderRuntimeCode`/`RenderShutdownCode` and `nativeError` at the RenderSubsystem boundary.

Outcome classification is fixed. Frame `Accepted` is `Success`; `ReplacedOlder`, `ShuttingDown`, and `NotRunning` are `ExpectedPressure`; invalid/out-of-order packets are `RecoverableFrame`. Resize `Accepted` is `Success`; `CoalescedOlder`, `ShuttingDown`, and `NotRunning` are `ExpectedPressure`; stale/invalid surfaces are `RecoverableFrame`.

Runtime code mappings are exact: `None -> Success/Stopped/None/None`; `Running -> Success/Running/None/None`; `StopRequested -> Success/StopRequested/NormalStop/NormalDrain`; `Stopped -> Success/Stopped/NormalStop/NormalDrain`; configuration/surface/executor/device/surface-creation failures each map to `RuntimeFatal/Failed/StartupFailure/NormalDrain`; graph validation maps to `FrameFatal/Running/None/None`; device loss maps to `RuntimeFatal/Failed/DeviceLost/DeviceLostTeardown`; unhandled exception maps to `RuntimeFatal/Failed/UnhandledException/NormalDrain`; ownership violation maps to `RuntimeFatal/Failed/OwnershipViolation/NormalDrain`; and either watchdog code maps to `RuntimeFatal/Failed/WatchdogTimeout/FatalTimeout`.

Shutdown mappings are exact: `None -> Success/Stopped/None/None`; `Completed -> Success/Stopped/NormalStop/NormalDrain`; `AlreadyStopped -> Success/Stopped/NormalStop/None`; `DeviceLost -> RuntimeFatal/Failed/DeviceLost/DeviceLostTeardown`; `TimedOut -> RuntimeFatal/Failed/WatchdogTimeout/FatalTimeout`; and `ExecutorJoinFailed -> RuntimeFatal/Failed/ExecutorFailure/FatalTimeout`. Executor join failure never detaches. Context fields carry the exact latest known value or their declared zero/invalid default; messages never affect identity or mapping.

RenderSubsystem also overrides all `IRenderResourceGateway` methods by delegating to its owned gateway core. During tasks 7–17 only, the old synchronous implementation remains behind a clearly named `LegacySynchronousRenderBridge` so not-yet-migrated callers compile. The bridge is a compile-preservation adapter, not a selectable executor: after any caller invokes `Configure()`, all frame/control work for that caller goes only through `DedicatedRenderExecutor`, startup failure is terminal, and fallback to the bridge is forbidden. The bridge is deleted in task 18.

- [ ] **Step 1: Write failing runtime-shell tests**

Add underlying-type/value `static_assert`s for every enum above and exact default-value tests for all four public result structs. Use the Inline and Dedicated fixtures to cover configure-only-before-initialize, invalid configuration, startup acknowledgement, state sequence `Stopped -> Starting -> Running`, every publish/resize code-to-class mapping, immutable diagnostics publication, frame/control wakeup, idle blocking/no repeated present, stop acknowledgement, and teardown on the same owner thread. Assert `GetDiagnosticsSnapshot()` remains valid after a newer shared snapshot is published.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderThreadRuntimeValidation
```

Expected: failure because runtime/result/diagnostics types are absent.

- [ ] **Step 3: Implement structured public result types**

Implement the exact enums, values, fields, defaults, and mapping table above. Reject undeclared enum values when decoding persisted/test input. Context fields include backend, frame/request sequence, asset/handle, surface generation, native error, and owned supplemental message. Do not use messages for control flow or test identity and do not add alternate result structs.

- [ ] **Step 4: Implement immutable diagnostics publication**

Render Thread builds a complete `RenderDiagnosticsSnapshot` value and publishes `std::shared_ptr<const RenderDiagnosticsSnapshot>` with atomic release. Readers atomic-acquire the pointer and return a value copy. Include every counter and queue/timeline/resource field listed in design section 14; initialize unavailable fields explicitly rather than omitting them.

- [ ] **Step 5: Implement the runtime shell and exact pump order**

Allocate transports and diagnostics before starting the executor. Render Thread records its guard before creating any render-owned object. `PumpOnce()` observes stop/surface, coalesces a frame, processes releases then uploads, invokes the current frame-consumer seam, polls completion, retires, publishes diagnostics, then waits on a condition variable when no actionable work remains. At this stage the production frame-consumer seam performs only a deterministic clear/present for a valid new packet; task 13 replaces it with SceneRenderer without changing the pump.

- [ ] **Step 6: Wire the new RenderSubsystem surface without exposing live objects**

`Configure()` validates and stores config/surface, creates the fixed gateway/status storage, and rejects calls after Starting. New getters return values only. Do not add device, swapchain, context, renderer, graph, registry, or manager getters. Startup failure throws `RenderSubsystemInitializationError` after storing the structured startup result.

- [ ] **Step 7: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderThreadRuntimeValidation
ctest --test-dir build\win_x64_debug -C Debug -R RenderThreadRuntimeValidation --output-on-failure
```

Expected: shared runtime-shell cases and dedicated thread-ownership/idle cases pass; diagnostics snapshots contain no RHI references.

- [ ] **Step 8: Review and commit**

```powershell
git diff --check
git add Render\Include\Render Render\Private\Runtime Render\Include\Render\RenderSubsystem.h Render\Private\RenderSubsystem.cpp Render\CMakeLists.txt Tests\Common Tests\RenderThreadRuntimeValidation Tests\CMakeLists.txt
git commit -m "feat: add render thread runtime shell"
```

---

### Task 8: Publish physical queue topology and track per-domain submissions

**Files:**

- Create: `RHI/Include/RHI/RHIQueueTopology.h`
- Create: `Render/Private/Resources/RenderSubmissionTracker.h`
- Create: `Render/Private/Resources/RenderSubmissionTracker.cpp`
- Create: `Tests/RenderSubmissionValidation/main.cpp`
- Modify: `RHI/Include/RHI/RHICapabilities.h`
- Modify: `RHI_DX12/Private/DX12Device.cpp`
- Modify: `RHI_Vulkan/Private/VulkanDevice.cpp`
- Modify: `RHI_Metal/Private/MetalDevice.mm`
- Modify: `RHI_DX11/Private/DX11Device.cpp`
- Modify: `RHI_OpenGL/Private/OpenGLDevice.cpp`
- Modify: `Render/Include/Render/Context/RenderContext.h`
- Modify: `Render/Private/Context/RenderContext.cpp`
- Modify: `Render/Include/Render/Context/FrameSynchronizer.h`
- Modify: `Render/Private/Context/FrameSynchronizer.cpp`
- Modify: `Tests/RHIContractValidation/main.cpp`
- Modify: `Tests/DX12Validation/main.cpp`
- Modify: `Tests/VulkanValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**

```cpp
enum class GPUQueueDomain : uint8
{
    Graphics = 0,
    Compute,
    Copy
};

struct GPUCompletionPoint
{
    GPUQueueDomain domain = GPUQueueDomain::Graphics;
    uint64 value = 0;
};

struct GPUCompletionToken
{
    std::array<GPUCompletionPoint, 3> points{};
    uint8 count = 0;
};
```

RHI publishes a logical queue to stable physical-domain mapping plus `NativeTimeline` or `CompatibilityWaitIdle`. RenderSubmissionTracker owns exactly one long-lived fence for every distinct active physical domain.

- [ ] **Step 1: Write failing topology and token tests**

Test distinct DX12-style topology, collapsed Metal-style topology, compatibility single-queue topology, token insertion, same-domain max merge, different-domain preservation, completion requiring every point, lost timeline, zero-value rejection, and exact submit fence selection. Add a test whose Graphics value is numerically larger than Copy and prove no cross-domain comparison affects completion.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderSubmissionValidation RHIContractValidation
```

Expected: failure on missing topology/tracker types.

- [ ] **Step 3: Publish backend topology honestly**

DX12 maps all three logical queues to distinct domains. Vulkan compares actual queue-family/queue identities and collapses only aliases. Metal maps its current single command queue to Graphics. DX11/OpenGL map all logical queues to Graphics with `CompatibilityWaitIdle`. Tests reject a capability report that claims distinct/native domains while the backend uses one emulated queue.

- [ ] **Step 4: Implement token operations and timeline ownership**

Tracker initializes one fence per active physical domain, selects the matching fence when submitting a command context, records last submitted/completed values independently, and reports `Completed`, `Pending`, `Lost`, or `CompatibilityWaitIdle`. Merge sorts by domain for deterministic diagnostics and takes max only within an identical domain.

Add RHI as a private dependency of `RVX_RenderRuntimeCore` when these tracker sources join that target. Do not link any backend implementation into the core target.

- [ ] **Step 5: Route RenderContext submission and frame-slot throttling through the tracker**

Replace per-frame submission signaling with the tracker’s long-lived Graphics timeline. `RenderContext::EndFrame` returns the Graphics `GPUCompletionPoint`. FrameSynchronizer stores the last Graphics point used by each frame slot and waits through RenderSubmissionTracker before reuse; it no longer creates a fence per frame. Resource lifetime, frame throttling, and diagnostics therefore agree on the same physical-domain timeline without inferring a device-global scalar.

- [ ] **Step 6: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderSubmissionValidation RHIContractValidation DX12Validation VulkanValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderSubmissionValidation|RHIContractValidation|SubmitReturnsMonotonicFenceValues" --output-on-failure
```

Expected: token/topology tests and available backend monotonic submit tests pass.

- [ ] **Step 7: Review and commit**

```powershell
git diff --check
git add RHI RHI_DX11 RHI_DX12 RHI_Vulkan RHI_Metal RHI_OpenGL Render\Private\Resources Render\Include\Render\Context Render\Private\Context Tests
git commit -m "feat: track per-queue gpu completion"
```

---

### Task 9: Add completion-token retirement without cutting over global deletion

**Files:**

- Create: `Render/Private/Resources/RenderRetirementQueue.h`
- Create: `Render/Private/Resources/RenderRetirementQueue.cpp`
- Modify: `Render/CMakeLists.txt`
- Modify: `Tests/RenderSubmissionValidation/main.cpp`

**Interface:**

```cpp
struct RenderRetirementEntry
{
    GPUCompletionToken completion;
    Ref<RefCounted> object;
    uint64 estimatedBytes = 0;
};
```

- [ ] **Step 1: Write failing retirement tests**

Use fake RefCounted RHI objects that record their destructor thread. Prove an entry remains alive until all token points complete, releasing one queue is insufficient, same-domain token merge uses the maximum, zero-point objects retire on Render Thread, compatibility mode performs and records one bounded WaitIdle, and device-lost teardown is reported as Lost rather than normal completion.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderSubmissionValidation
```

Expected: missing retirement queue failures.

- [ ] **Step 3: Implement explicit retirement ownership**

Only Render Thread may enqueue, poll, or force device-lost teardown. Normal polling erases an entry only when tracker completion says every point is complete. Erasure drops the final strong reference on Render Thread. Diagnostics track count, estimated bytes, and oldest point per domain.

- [ ] **Step 4: Integrate the queue without removing the migration fallback**

Expose the retirement queue to later Render-owned registry/upload/cache work, but do not delete or weaken `IDeferredDeleter`, `DeferredDeleterRegistry`, or `FrameResourceManager` in this task. Their definitions remain only to preserve intermediate build compatibility; M1 correctness does not rely on them and no task may claim they are a proven GPU-safety fallback. Migrated code transfers its strong reference and completion token into `RenderRetirementQueue`. No new producer may enqueue directly into the global mechanism, and tests run with the registry uninstalled so they prove exact completion-token behavior rather than frame-count behavior.

- [ ] **Step 5: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderSubmissionValidation RenderGraphValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderSubmissionValidation|RenderGraphValidation" --output-on-failure
```

Expected: retirement ownership tests and existing RenderGraph lifetime tests pass; `DeferredDeleterRegistry` and `FrameResourceManager` remain unchanged for the bounded migration window.

- [ ] **Step 6: Review and commit**

```powershell
git diff --check
git diff --name-only -- Core\Include\Core\RefCounted.h Render\Private\Graph\FrameResourceManager.cpp
git add Render\Private\Resources Render\CMakeLists.txt Tests\RenderSubmissionValidation Tests\CMakeLists.txt
git commit -m "feat: add completion-token retirement queue"
```

Expected scoped diff result before commit: no output; the cutover files are deliberately untouched.

---

### Task 10: Split RenderResourceRegistry and RenderUploadProcessor behind the temporary facade

**Files:**

- Create: `Render/Private/Resources/RenderResourceRegistry.h`
- Create: `Render/Private/Resources/RenderResourceRegistry.cpp`
- Create: `Render/Private/Resources/RenderUploadProcessor.h`
- Create: `Render/Private/Resources/RenderUploadProcessor.cpp`
- Modify: `Render/Include/Render/GPUUploadService.h`
- Modify: `Render/Private/GPUUploadService.cpp`
- Modify: `Render/Include/Render/GPUResourceManager.h`
- Modify: `Render/Private/GPUResourceManager.cpp`
- Modify: `Render/Private/Runtime/RenderThreadRuntime.h`
- Modify: `Render/Private/Runtime/RenderThreadRuntime.cpp`
- Modify: `Tests/GPUUploadServiceValidation/main.cpp`
- Modify: `Tests/GPUResourceManagerValidation/main.cpp`
- Create: `Tests/RenderResourceRuntimeValidation/main.cpp`
- Modify: `Render/CMakeLists.txt`
- Modify: `Tests/CMakeLists.txt`

**Responsibilities:**

- `RenderResourceRegistry` owns exact slot/generation entries and every strong mesh/texture/material RHI reference.
- `RenderUploadProcessor` validates immutable requests, creates pending entries, records Copy submissions, waits for Copy completion, commits or retires partial state, drops Render request refs, then publishes terminal status.
- `GPUResourceManager` temporarily delegates legacy query/upload calls to those components. New code may not add a facade dependency.

- [ ] **Step 1: Write failing two-phase resource tests**

Cover successful mesh/texture/material creation, invalid schema/range/dependency, Nth RHI creation failure, upload submit failure, Copy pending not ready, Copy completion commit, release while reserved/queued/uploading/ready/failed, stale generation, replacement independence, exact dependency expansion, and partial-object retirement. Hold a weak request reference and prove it is still alive after Render drops its ref but before the update-side owner releases.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderResourceRuntimeValidation
```

Expected: failure until registry and processor exist.

- [ ] **Step 3: Implement exact-generation registry entries**

Store entries by slot with the generation in every entry. Resolve requires exact match and `GPUReady`; stale lookup never returns a newer generation. Pending and committed data are separate. Registry records dependencies and last-use token per generation. Release transfers all strong RHI refs plus the recorded token to RenderRetirementQueue.

- [ ] **Step 4: Refactor GPUUploadService onto the tracker**

Remove per-upload fence pools and bare fence values. Staged upload records carry `GPUCompletionPoint`/token from RenderSubmissionTracker. Staging buffers and command contexts remain strongly owned until completion. A backend with no real completion performs the explicit compatibility WaitIdle path and records that mode.

- [ ] **Step 5: Implement two-phase processing and terminal release order**

Validate request, create pending registry data, submit Copy work, and retain the request in the in-flight record. On completion, commit or transfer partial objects to retirement. Copy the compact terminal result, erase the in-flight record and every other Render request reference, then CAS/release-store `GPUReady` or `Failed`. If state is already `Evicting`, never publish ready; retire partial state and release-store `Released` when safe.

- [ ] **Step 6: Convert the facade into a pure migration adapter**

The facade contains no maps, queues, status state, fence pools, or RHI ownership. Its legacy pointer overloads construct owned requests through one explicitly named legacy adapter and forward; task 18 deletes both. Add deprecation comments with the task-18 removal condition, not an open-ended compatibility promise.

- [ ] **Step 7: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderResourceRuntimeValidation GPUUploadServiceValidation GPUResourceManagerValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderResourceRuntimeValidation|GPUUploadServiceValidation|GPUResourceManagerValidation" --output-on-failure
```

Expected: two-phase, Copy-completion, failure, stale generation, request lifetime, and migrated legacy coverage pass.

- [ ] **Step 8: Review and commit**

```powershell
git diff --check
git add Render\Private\Resources Render\Private\Runtime Render\Include\Render\GPUUploadService.h Render\Private\GPUUploadService.cpp Render\Include\Render\GPUResourceManager.h Render\Private\GPUResourceManager.cpp Render\CMakeLists.txt Tests
git commit -m "refactor: split render resource ownership"
```

---

### Task 11: Move upload production, retry, and final reclamation into ResourceSubsystem

**Files:**

- Create: `Resource/Include/Resource/RenderUploadRequestBuilder.h`
- Create: `Resource/Private/RenderUploadRequestBuilder.cpp`
- Create: `Resource/Private/ResourceSubsystem.cpp`
- Modify: `Resource/Include/Resource/ResourceSubsystem.h`
- Modify: `Resource/Include/Resource/ResourceManager.h`
- Modify: `Resource/Private/ResourceManager.cpp`
- Modify: `Resource/CMakeLists.txt`
- Modify: `Tests/RenderResourceRuntimeValidation/main.cpp`
- Modify: `Tests/ResourceRuntimePolicyValidation/main.cpp`

**Interface:**

```cpp
void SetRenderResourceGateway(IRenderResourceGateway* gateway);
RenderResourceResolveResult ResolveRenderResource(AssetId assetId,
                                                  RenderResourceKind kind);
void BeginRenderShutdown();
void DrainTerminalRenderRequests();
```

ResourceSubsystem is the sole gateway producer. Worker loads may produce CPU resources, but only its update-thread Tick reserves, enqueues, releases, retries, and reacts to public status.

- [ ] **Step 1: Write failing ResourceSubsystem ownership tests**

Test mesh/texture/material request copies, material texture dependencies, count/byte pressure retry without blocking, only-one-enqueue per generation, hot-reload replacement on another slot, unload release, shutdown seal, and terminal request reclamation. For each `GPUReady`, `Failed`, cancelled, and `Released` path: retain a `weak_ptr` to the request, prove it is not expired immediately after Render terminal publication, call update-thread `DrainTerminalRenderRequests`, and prove it expires on that call.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderResourceRuntimeValidation ResourceRuntimePolicyValidation
```

Expected: missing ResourceSubsystem gateway/builder failures.

- [ ] **Step 3: Move ResourceSubsystem implementation out of the header**

Keep templates in the header; move lifecycle, Tick, gateway, retry, and diagnostics logic to `ResourceSubsystem.cpp`. Record/update-thread identity on Initialize and reject gateway mutation from worker threads with an assertion plus structured failure.

- [ ] **Step 4: Build owning payloads from concrete resources**

Mesh builder copies all attribute/index bytes into one checked owned byte vector and emits ranges/submeshes. Texture builder copies metadata and bytes. Material builder copies `MaterialSourceData` values and resolves each texture dependency to a generational handle. The builder never stores ResourceHandle, Resource pointer, upload view, or string view in the request.

- [ ] **Step 5: Marshal resource lifecycle to the update phase**

Extend ResourceManager with ready/reload/before-unload notifications delivered by `ProcessCompletedLoads()` on its registered update thread. Async workers may only enqueue completion values; they never call the render gateway. Synchronous load through ResourceSubsystem enters the same update-side handling path.

- [ ] **Step 6: Implement deterministic pressure and terminal policies**

Keep CPU-ready resources in a priority/FIFO retry structure when count or bytes are full. Poll public status with acquire semantics. Drop the retained request only after terminal observation. Accepted release removes the asset’s current reservation mapping so hot reload can reserve another slot without mutating the evicting generation.

- [ ] **Step 7: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderResourceRuntimeValidation ResourceRuntimePolicyValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderResourceRuntimeValidation|ResourceRuntimePolicyValidation" --output-on-failure
```

Expected: request copying, retry, replacement, release, shutdown seal, and all terminal weak-reference reclamation cases pass.

- [ ] **Step 8: Review and commit**

```powershell
git diff --check
git add Resource Tests\RenderResourceRuntimeValidation Tests\ResourceRuntimePolicyValidation
git commit -m "feat: publish gpu uploads from resource subsystem"
```

---

### Task 12: Complete update-side extraction, frame settings, and capture values

**Files:**

- Create: `Geometry/Include/Geometry/Asset/AssetMetadata.h`
- Create: `RenderExtraction/Include/RenderExtraction/RenderFrameExtractor.h`
- Create: `RenderExtraction/Private/RenderFrameExtractor.cpp`
- Create: `Tests/RenderFrameExtractionValidation/main.cpp`
- Modify: `RenderContracts/Include/RenderContracts/RenderProxy.h`
- Modify: `RenderContracts/Include/RenderContracts/RenderFramePacket.h`
- Modify: `RenderExtraction/Include/RenderExtraction/WorldCameraBridge.h`
- Modify: `RenderExtraction/Private/WorldCameraBridge.cpp`
- Modify: `RenderExtraction/Include/RenderExtraction/SceneEnvironmentIBLBridge.h`
- Modify: `RenderExtraction/Private/SceneEnvironmentIBLBridge.cpp`
- Replace: `RenderExtraction/Include/RenderExtraction/SceneSkyboxPassBridge.h`
- Replace: `RenderExtraction/Private/SceneSkyboxPassBridge.cpp`
- Modify: `RenderExtraction/Include/RenderExtraction/RenderProxySceneBridge.h`
- Modify: `RenderExtraction/Private/RenderProxySceneBridge.cpp`
- Modify: `Scene/Private/Components/StaticMeshComponent.cpp`
- Modify: `Scene/Private/Components/MeshRendererComponent.cpp`
- Modify: `Scene/Private/Components/LODComponent.cpp`
- Modify: `Resource/Include/Resource/Types/MeshResource.h`
- Modify: `Resource/Include/Resource/Types/MaterialResource.h`
- Modify: `RenderExtraction/CMakeLists.txt`
- Modify: `Tests/CMakeLists.txt`

**Contract cut:**

`RenderPrimitiveProxy` becomes update-only value data with `AssetId meshAssetId` and owned material AssetIds/modes. Environment and sky snapshots use AssetIds and numeric values only. The extractor resolves AssetIds through ResourceSubsystem into `RenderResourceHandle`s before adding packet primitives. The old skybox callbacks and selected RHI texture disappear.

- [ ] **Step 1: Write failing complete-packet tests**

Build a World with camera, mesh/material, lights, sky/IBL, particle, water, and terrain providers. Prove the packet owns all values after World/Scene temporary extraction storage is destroyed. Cover null world/camera, incomplete provider, unresolved optional resource, strictly increasing sequence, world revision, temporal epoch, explicit discontinuity, settings propagation, capture request, and refusal to publish an incomplete packet.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderFrameExtractionValidation
```

Expected: failure because the aggregate extractor and value-only bridges are absent.

- [ ] **Step 3: Replace Render upload-source metadata used by Scene**

Add render-neutral `IMeshAssetMetadata` and `IMaterialAssetMetadata` in Geometry. MeshResource/MaterialResource implement bounds, submesh count, and material-mode metadata there. Scene components query these update-only interfaces through their retained SceneAssetHandle. Do not move upload bytes or RHI concepts into Geometry.

- [ ] **Step 4: Convert every extraction bridge to owned values**

WorldCameraBridge fills matrices, position/direction, clipping, viewport, exposure, time, and delta values. RenderProxySceneBridge emits AssetIds and owned arrays. Environment bridge emits texture AssetIds and numeric IBL settings. Sky bridge becomes `Extract()` returning one value snapshot; it has no `std::function`, upload request, GPU-ready query, or RHI selection.

- [ ] **Step 5: Implement the aggregate extractor**

The extractor receives sequence, world revision, temporal epoch, discontinuity, update-owned settings/capture request, World, and ResourceSubsystem. It invokes all bridges/providers, resolves required handles, fills completeness counters, and calls `Seal()`. A bridge/provider failure produces `RenderFrameExtractionResult` with stable code and no packet. Missing GPU readiness is not extraction failure because the stable handle remains valid and Render will fallback/skip.

- [ ] **Step 6: Put mutable render controls into the packet**

Populate the Task 1 `RenderFrameSettings` sub-values and exact `RenderFrameCaptureRequest` without adding fields or alternate control objects. Validate settings/capture on the update side before passing them to the builder. No settings object references SceneRenderer. These frozen values are the migration target for ModelViewer/RenderingShowcase in task 17.

- [ ] **Step 7: Run green verification and the contract checker**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderFrameExtractionValidation RenderContractsValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderFrameExtractionValidation|RenderContractsValidation|Architecture.RenderContractFieldChecker" --output-on-failure
```

Expected: complete/incomplete/ownership/settings/capture tests and the forbidden-field checker pass.

- [ ] **Step 8: Review and commit**

```powershell
git diff --check
git add Geometry RenderContracts RenderExtraction Scene Resource Tests\RenderFrameExtractionValidation Tests\CMakeLists.txt
git commit -m "feat: extract complete immutable render frames"
```

---

### Task 13: Convert RenderScene, SceneRenderer, and the runtime pump to packet-only rendering

**Files:**

- Modify: `Render/Include/Render/Renderer/RenderScene.h`
- Modify: `Render/Private/Renderer/RenderScene.cpp`
- Modify: `Render/Include/Render/Renderer/ViewData.h`
- Modify: `Render/Private/Renderer/ViewData.cpp`
- Modify: `Render/Include/Render/Renderer/SceneRenderer.h`
- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Modify: `Render/Private/Renderer/RenderDrawItem.cpp`
- Modify: `Render/Private/GPUDriven/GPUCulling.cpp`
- Modify: `Render/Include/Render/Material/MaterialBinder.h`
- Modify: `Render/Private/Material/MaterialBinder.cpp`
- Modify: `Render/Include/Render/Material/MaterialSystem.h`
- Modify: `Render/Private/Material/MaterialSystem.cpp`
- Modify: `Render/Include/Render/RayTracing/RayTracingScene.h`
- Modify: `Render/Private/RayTracing/RayTracingScene.cpp`
- Modify: `Render/Include/Render/Passes/DepthPrepass.h`
- Modify: `Render/Private/Passes/DepthPrepass.cpp`
- Modify: `Render/Include/Render/Passes/GBufferPass.h`
- Modify: `Render/Private/Passes/GBufferPass.cpp`
- Modify: `Render/Include/Render/Passes/ObjectVelocityPass.h`
- Modify: `Render/Private/Passes/ObjectVelocityPass.cpp`
- Modify: `Render/Include/Render/Passes/OpaquePass.h`
- Modify: `Render/Private/Passes/OpaquePass.cpp`
- Modify: `Render/Include/Render/Passes/ShadowPass.h`
- Modify: `Render/Private/Passes/ShadowPass.cpp`
- Modify: `Render/Include/Render/Passes/TransparentPass.h`
- Modify: `Render/Private/Passes/TransparentPass.cpp`
- Modify: `Render/Include/Render/Passes/RayTracedShadowPass.h`
- Modify: `Render/Private/Passes/RayTracedShadowPass.cpp`
- Modify: `Render/Include/Render/Passes/RayTracedReflectionPass.h`
- Modify: `Render/Private/Passes/RayTracedReflectionPass.cpp`
- Modify: `Render/Private/Runtime/RenderThreadRuntime.h`
- Modify: `Render/Private/Runtime/RenderThreadRuntime.cpp`
- Rewrite: `Tests/RenderSceneValidation/main.cpp`
- Modify: `Tests/RenderPassValidation/main.cpp`
- Modify: `Tests/RenderThreadRuntimeValidation/main.cpp`

**Interfaces:**

```cpp
RenderFrameApplyResult ApplyFramePacket(const RenderFramePacket& packet,
                                        const RenderResourceRegistry& registry);
RenderFrameExecutionResult RenderAcceptedFrame();
```

`RenderObject` stores mesh/material `RenderResourceHandle`s. SceneRenderer receives registry/upload/submission/retirement dependencies explicitly and does not construct GPUResourceManager.

- [ ] **Step 1: Write failing transactional packet tests**

Cover successful full apply, unsupported schema, out-of-order packet, stale required handle, missing-pending fallback, no partial scene mutation on failure, value survival after packet destruction, and replacement after prior packet destruction. Prove RenderScene stores no pointer/span/iterator into packet storage.

- [ ] **Step 2: Write failing temporal-continuity tests**

Apply/render sequences 10 then 13 with unchanged world/epoch and prove sequence gap is diagnosed but history remains valid. Then independently change world revision, temporal epoch, surface compatibility, and explicit discontinuity and prove each resets history. Previous camera/object transforms must come from the last packet actually rendered; acquiring then rejecting/replacing a packet must not advance previous state.

- [ ] **Step 3: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderSceneValidation RenderThreadRuntimeValidation
```

Expected: failures on old Camera/World/pointer-based scene APIs.

- [ ] **Step 4: Make RenderScene apply transactionally by value**

Build candidate object/light arrays locally, validate every required generation and vector cardinality, then swap into live scene only on success. Copy skinning and all feature values. Store the accepted header and last-rendered temporal state as values. Culling consumes `RenderViewSnapshot::viewProjectionMatrix` and camera position, not a Camera object.

- [ ] **Step 5: Convert ViewData and SceneRenderer setup**

Add `ViewData::SetupFromSnapshot` for numeric values. SceneRenderer applies packet settings, sky/environment, features, and capture request; resolves resources through RenderResourceRegistry; and never asks Resource to upload. Missing/non-ready resources choose the packet-declared fallback or skip draw and increment diagnostics. Remove new-path use of RenderExtraction bridges.

- [ ] **Step 6: Replace GPUResourceManager dependencies in passes/material/ray tracing**

Inject `RenderResourceRegistry&` into every listed consumer. Query mesh/texture/material by exact generational handle. Material dependencies expand through registry entries. RenderGraph/SceneRenderer collects the exact handles referenced by submitted draw/dispatch contexts, expands registry dependencies, and returns them in `RenderFrameExecutionResult`.

- [ ] **Step 7: Stamp last-use tokens at submission**

After successful Graphics/Compute submission, merge the returned queue point into every exact referenced registry generation. If graph validation fails, submit nothing and stamp nothing. A successful present advances `lastRenderedFrameSequence`; packet apply alone does not.

- [ ] **Step 8: Replace the bootstrap frame consumer in RenderThreadRuntime**

Runtime applies the newest acquired packet, renders/presents only a newly accepted packet or explicit resize redraw, polls uploads/timelines, then retires. Upload-only work never presents. Idle wakeups do not present. Resize redraw uses the last rendered packet or one deterministic clear if none exists.

- [ ] **Step 9: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderSceneValidation RenderPassValidation RenderThreadRuntimeValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderSceneValidation|RenderThreadRuntimeValidation|RenderPassValidation" --output-on-failure
```

Expected: packet ownership, transactionality, temporal rules, handle-only binding, exact last-use stamping, idle/no-present, and existing render-pass coverage pass.

- [ ] **Step 10: Review and commit**

```powershell
git diff --check
git add Render Tests\RenderSceneValidation Tests\RenderPassValidation Tests\RenderThreadRuntimeValidation
git commit -m "refactor: render immutable frame packets"
```

---

### Task 14: Cut over every RHI strong-reference holder to completion-token ownership

**Files:**

- Create: `Render/Private/Resources/RenderSubmissionResourceBatch.h`
- Create: `Render/Private/Resources/RenderSubmissionResourceBatch.cpp`
- Create: `Tests/RenderLifetimeCutoverValidation/main.cpp`
- Create: `Scripts/rhi_ownership_inventory_m1.json`
- Create: `Scripts/check_rhi_ownership_inventory.py`
- Create: `Scripts/test_rhi_ownership_inventory_checker.py`
- Modify: `Render/Private/Resources/RenderRetirementQueue.h`
- Modify: `Render/Private/Resources/RenderRetirementQueue.cpp`
- Modify: `Render/Private/Resources/RenderSubmissionTracker.h`
- Modify: `Render/Private/Resources/RenderSubmissionTracker.cpp`
- Modify: `Render/Private/Runtime/RenderThreadRuntime.h`
- Modify: `Render/Private/Runtime/RenderThreadRuntime.cpp`
- Modify: `Render/Private/Graph/RenderGraphInternal.h`
- Modify: `Render/Private/Graph/RenderGraph.cpp`
- Modify: `Render/Include/Render/Graph/TransientResourcePool.h`
- Modify: `Render/Private/Graph/TransientResourcePool.cpp`
- Modify: `Render/Include/Render/Graph/ResourceViewCache.h`
- Modify: `Render/Private/Graph/ResourceViewCache.cpp`
- Modify: `Render/Include/Render/Context/RenderContext.h`
- Modify: `Render/Private/Context/RenderContext.cpp`
- Modify: `Render/Include/Render/Renderer/SceneRenderer.h`
- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Modify: `Render/Include/Render/Passes/CameraVelocityPass.h`
- Modify: `Render/Private/Passes/CameraVelocityPass.cpp`
- Modify: `Render/Include/Render/Passes/RayTracedReflectionCompositePass.h`
- Modify: `Render/Private/Passes/RayTracedReflectionCompositePass.cpp`
- Modify: `Render/Include/Render/Passes/RayTracedReflectionDenoisePass.h`
- Modify: `Render/Private/Passes/RayTracedReflectionDenoisePass.cpp`
- Modify: `Render/Include/Render/Passes/RayTracedReflectionPass.h`
- Modify: `Render/Private/Passes/RayTracedReflectionPass.cpp`
- Modify: `Render/Include/Render/Passes/RayTracedShadowPass.h`
- Modify: `Render/Private/Passes/RayTracedShadowPass.cpp`
- Modify: `Render/Include/Render/Passes/SkyboxPass.h`
- Modify: `Render/Private/Passes/SkyboxPass.cpp`
- Modify: `Render/Include/Render/PostProcess/Bloom.h`
- Modify: `Render/Private/PostProcess/Bloom.cpp`
- Modify: `Render/Include/Render/PostProcess/ChromaticAberration.h`
- Modify: `Render/Private/PostProcess/ChromaticAberration.cpp`
- Modify: `Render/Include/Render/PostProcess/ColorGrading.h`
- Modify: `Render/Private/PostProcess/ColorGrading.cpp`
- Modify: `Render/Include/Render/PostProcess/FilmGrain.h`
- Modify: `Render/Private/PostProcess/FilmGrain.cpp`
- Modify: `Render/Include/Render/PostProcess/FXAA.h`
- Modify: `Render/Private/PostProcess/FXAA.cpp`
- Modify: `Render/Include/Render/PostProcess/SSAO.h`
- Modify: `Render/Private/PostProcess/SSAO.cpp`
- Modify: `Render/Include/Render/PostProcess/ToneMapping.h`
- Modify: `Render/Private/PostProcess/ToneMapping.cpp`
- Modify: `Render/Include/Render/PostProcess/Vignette.h`
- Modify: `Render/Private/PostProcess/Vignette.cpp`
- Modify: `Render/Include/Render/GPUDriven/GPUCulling.h`
- Modify: `Render/Private/GPUDriven/GPUCulling.cpp`
- Modify: `Render/Include/Render/RayTracing/RayTracingSceneManager.h`
- Modify: `Render/Private/RayTracing/RayTracingSceneManager.cpp`
- Modify: `Render/Include/Render/PipelineCache.h`
- Modify: `Render/Private/PipelineCache.cpp`
- Modify: `Render/Include/Render/Material/MaterialSystem.h`
- Modify: `Render/Private/Material/MaterialSystem.cpp`
- Modify: `Core/Include/Core/RefCounted.h`
- Delete: `Render/Private/Graph/FrameResourceManager.cpp`
- Modify: `Render/CMakeLists.txt`
- Modify: `Tests/RenderGraphValidation/main.cpp`
- Modify: `Tests/ResourceViewCacheValidation/main.cpp`
- Modify: `Tests/PipelineCacheValidation/main.cpp`
- Modify: `Tests/MaterialSystemValidation/main.cpp`
- Modify: `Tests/GPUResourceManagerValidation/main.cpp`
- Modify: `Tests/GPUUploadServiceValidation/main.cpp`
- Modify: `Tests/RenderSubmissionValidation/main.cpp`
- Modify: `Tests/RenderPassValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Interfaces:**

```cpp
enum class RenderRHILifetimePolicy : uint8
{
    RegistryExactGeneration = 0,
    SubmissionBatch = 1,
    PoolAvailability = 2,
    OwnerSnapshot = 3,
    SurfaceGeneration = 4,
    ShutdownAfterDrain = 5
};

class RenderSubmissionResourceBatch final
{
public:
    bool Retain(Ref<RefCounted> object,
                uint64 estimatedBytes = 0);
    void SealAndTransfer(const GPUCompletionToken& completion,
                         RenderRetirementQueue& retirement);
    void ReleaseUnsubmitted(RenderRetirementQueue& retirement);
    [[nodiscard]] bool IsSealed() const noexcept;
};
```

`Retain` runs only while recording on Render Thread, rejects null/sealed input, and deduplicates by object address. A successful submission seals the batch with the exact returned token and transfers all refs to retirement. Validation/submission failure releases an unsubmitted batch with an empty token on Render Thread. Partially successful submission uses the actual token. No code uses packet sequence, presentation count, CPU frame, or “N frames later” as completion evidence.

The inventory JSON schema is fixed as `schemaVersion` plus entries containing `path`, `owner`, `symbol`, `policy`, `completionSource`, `normalRelease`, and `deviceLostRelease`. The checker scans production Render/RHI/backend headers and sources for `RHI*Ref`, `Ref<RHI...>`, their containers, and ownership-bearing backend wrappers; every discovered holder must have exactly one live inventory entry and every entry must resolve to a discovered holder. `--phase pre-delete` requires complete classification and rejects every fixed-frame/global invocation or registration while permitting only the dormant definitions in `Core/Include/Core/RefCounted.h` and `Render/Private/Graph/FrameResourceManager.cpp`. `--phase final` also rejects those definitions. Any other phase/path combination fails closed.

- [ ] **Step 1: Add failing inventory-checker tests**

Register `Architecture.RHIOwnershipInventory` with `architecture;unit` labels. Fixtures prove a complete classified holder passes and that an unclassified holder, stale manifest entry, invalid policy, missing completion source, fixed-frame retention, and residual global-deleter symbol each fail closed. The production inventory initially fails because current holders are unclassified.

- [ ] **Step 2: Add failing Fake-RHI cutover tests**

Cover multi-domain submission batches, deduplication, zero-submit failure, failure after a real submission, destructor thread, pooled-resource reuse/eviction, ResourceViewCache invalidation, pipeline/material/ray-tracing replacement, every former fixed-frame descriptor-retention owner, old surface-generation resize, compatibility `WaitIdle`, normal drain, and device-lost teardown. A fake object records `objectId`, last-use token, destruction thread, and teardown mode; destruction is valid only after every token point completes, with an empty never-submitted token, or under explicit Lost teardown.

- [ ] **Step 3: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderLifetimeCutoverValidation RenderGraphValidation
python Scripts\check_rhi_ownership_inventory.py --root . --phase pre-delete
```

Expected: tests/checker report fixed-frame retention, unclassified holders, unsafe pool/view/cache release, and the still-present global path.

- [ ] **Step 4: Implement submission-batch and pool-availability ownership**

Command recording retains every ephemeral descriptor set, upload/staging object, resource view, and pass-local RHI object into one batch. Seal only after the backend returns its actual queue point. RenderGraph pooled textures/buffers store `GPUCompletionToken availableAfter`; CPU release only marks them unused, acquisition cannot reuse them until the tracker reports that token complete, and eviction transfers the same token into retirement.

- [ ] **Step 5: Migrate replacement and surface ownership**

Cache-, material-, pipeline-, history-, GPU-culling-, and ray-tracing-owned replacements use a per-domain last-submitted snapshot captured on Render Thread after the last recording that could reference the old object. Mutation is permitted only before recording or after submission. Each surface generation stores its last Graphics completion point; resize/recreation clears old back-buffer wrappers only after completion, or after one bounded compatibility `WaitIdle`. Objects never replaced at runtime use `ShutdownAfterDrain` and remain component-owned until the Render Thread drains active timelines. Lost teardown is always classified separately.

- [ ] **Step 6: Complete the fail-closed ownership inventory**

Inventory every discovered strong holder, including RenderGraph internal resources, transient pools, ResourceViewCache, upload staging, pipeline/material/ray-tracing/history caches, GPU-driven buffers, pass/post-process retained descriptors, swapchain/back buffers, runtime members, and backend wrappers. Each entry names the concrete completion source and both normal/device-lost release paths; directory-level wildcard entries are invalid.

- [ ] **Step 7: Remove frame-count/global deletion only after the cutover gates pass**

First run the mandatory migration-complete/pre-deletion gate against the still-present dormant definitions:

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderLifetimeCutoverValidation RenderSubmissionValidation RenderGraphValidation ResourceViewCacheValidation PipelineCacheValidation MaterialSystemValidation GPUResourceManagerValidation GPUUploadServiceValidation RenderPassValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderLifetimeCutoverValidation|RenderSubmissionValidation|RenderGraphValidation|ResourceViewCacheValidation|PipelineCacheValidation|MaterialSystemValidation|GPUResourceManagerValidation|GPUUploadServiceValidation|RenderPassValidation" --output-on-failure
python Scripts\check_rhi_ownership_inventory.py --root . --phase pre-delete
```

Expected before deletion: all Fake-RHI/lifetime suites pass; inventory reports zero missing/stale holders; every runtime drop/reuse path has exactly one non-frame policy; the only legacy-symbol matches are the two permitted dormant definition files; and Task 13's packet-only exact-generation stamping is green. If any condition fails, stop and do not edit either legacy definition.

Only after that gate, delete `IDeferredDeleter`, `DeferredDeleterRegistry`, and `FrameResourceManager`; restore direct last-reference deletion in `Ref<T>`. Remove `Private/Graph/FrameResourceManager.cpp` from CMake.

```powershell
rg -n "DeferredDeleterRegistry|IDeferredDeleter|FrameResourceManager" Core Render RHI RHI_DX11 RHI_DX12 RHI_Vulkan RHI_Metal RHI_OpenGL
rg -n "kViewRetireFrameLag|retiredFrameResources|RVX_MAX_FRAME_COUNT\s*\+\s*1" Render\Private\Graph Render\Private\Passes Render\Private\PostProcess
```

Expected after deletion: no matches.

- [ ] **Step 8: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderLifetimeCutoverValidation RenderSubmissionValidation RenderGraphValidation ResourceViewCacheValidation PipelineCacheValidation MaterialSystemValidation GPUResourceManagerValidation GPUUploadServiceValidation RenderPassValidation
ctest --test-dir build\win_x64_debug -C Debug -R "RenderLifetimeCutoverValidation|RenderSubmissionValidation|RenderGraphValidation|ResourceViewCacheValidation|PipelineCacheValidation|MaterialSystemValidation|GPUResourceManagerValidation|GPUUploadServiceValidation|RenderPassValidation|Architecture.RHIOwnershipInventory" --output-on-failure
python Scripts\check_rhi_ownership_inventory.py --root . --phase final
```

Expected: every focused lifetime suite passes, inventory reports zero unclassified/stale holders and zero forbidden frame/global patterns, and all recorded final destructors run on Render Thread under completed or explicit Lost evidence.

- [ ] **Step 9: Review and commit**

```powershell
git diff --check
git add Core\Include\Core\RefCounted.h Render Scripts\rhi_ownership_inventory_m1.json Scripts\check_rhi_ownership_inventory.py Scripts\test_rhi_ownership_inventory_checker.py Tests
git commit -m "refactor: cut over rhi completion ownership"
```

---

### Task 15: Harden startup, runtime failure, device loss, and bounded shutdown

**Files:**

- Create: `RHI/Include/RHI/RHIDeviceStatus.h`
- Modify: `RHI/Include/RHI/RHIDevice.h`
- Modify: `RHI_DX12/Private/DX12Device.h`
- Modify: `RHI_DX12/Private/DX12Device.cpp`
- Modify: `RHI_DX12/Private/DX12SwapChain.cpp`
- Modify: `RHI_Vulkan/Private/VulkanDevice.h`
- Modify: `RHI_Vulkan/Private/VulkanDevice.cpp`
- Modify: `RHI_Vulkan/Private/VulkanSwapChain.cpp`
- Modify: `RHI_Metal/Private/MetalDevice.h`
- Modify: `RHI_Metal/Private/MetalDevice.mm`
- Modify: `RHI_Metal/Private/MetalCommandContext.mm`
- Modify: `RHI_Metal/Private/MetalSwapChain.mm`
- Modify: `RHI_DX11/Private/DX11Device.h`
- Modify: `RHI_DX11/Private/DX11Device.cpp`
- Modify: `RHI_OpenGL/Private/OpenGLDevice.h`
- Modify: `RHI_OpenGL/Private/OpenGLDevice.cpp`
- Modify: `Render/Private/Runtime/DedicatedRenderExecutor.cpp`
- Modify: `Render/Private/Runtime/RenderThreadRuntime.h`
- Modify: `Render/Private/Runtime/RenderThreadRuntime.cpp`
- Modify: `Render/Private/Runtime/RenderDiagnosticsPublisher.h`
- Modify: `Render/Private/Runtime/RenderDiagnosticsPublisher.cpp`
- Modify: `Render/Private/Resources/RenderSubmissionTracker.h`
- Modify: `Render/Private/Resources/RenderSubmissionTracker.cpp`
- Modify: `Render/Private/Resources/RenderUploadProcessor.h`
- Modify: `Render/Private/Resources/RenderUploadProcessor.cpp`
- Modify: `Tests/Common/RenderRuntimeTestSupport.h`
- Modify: `Tests/Common/RenderRuntimeTestSupport.cpp`
- Modify: `Tests/RenderThreadRuntimeValidation/main.cpp`

**Failure seam:**

Test support injects an `IRenderRuntimeFactory`, fake monotonic clock, `RenderRuntimeFaultPlan`, and `IRenderFatalPolicy`. Production uses the real RHI factory, `steady_clock`, and a fatal policy that writes the latest diagnostics artifact before terminating on an unrecoverable watchdog.

The stable terminal cause is `RenderTerminalCause::DeviceLost`, paired with `RenderTeardownMode::DeviceLostTeardown`; neither value may be reused for a normal completion or timeout.

- [ ] **Step 1: Add failing fault-matrix tests**

Inject device, swapchain, context, renderer, Nth resource, upload submit, fence poll/wait, resize, present, and exception failures. Inject device loss before submission, with work in flight, and during shutdown. For every case assert stable failure code, no exception crossing, same-thread fake RHI construction/destruction, zero live fake objects, sealed publication, and terminal lifecycle.

- [ ] **Step 2: Add failing watchdog tests**

With a fake clock, test 60-second default startup and 30-second default shutdown policy, shorter explicit test policies, diagnostics artifact before fatal policy, no join before exit acknowledgement, and no detach path. Correctness tests use latches/condition variables, not sleeps.

- [ ] **Step 3: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderThreadRuntimeValidation
```

Expected: new fault/watchdog cases fail.

- [ ] **Step 4: Publish backend device status honestly**

Add `QueryRuntimeStatus()` and `GetLastDeviceFault()` value APIs. DX12 captures `GetDeviceRemovedReason` and DRED data. Vulkan records `VK_ERROR_DEVICE_LOST` and available device-fault data. Metal completion handlers publish only compact atomic completion/error codes; Render Thread converts those codes into owned diagnostics on its next poll. DX11/OpenGL map fatal backend/context failures to stable compatibility codes. No callback mutates registry, scene, diagnostics containers, or retirement storage.

- [ ] **Step 5: Implement ordered normal shutdown**

Seal producers, set stop/wake, discard unacquired packets, cancel unstarted uploads with request-release ordering, finish/classify submitted uploads, wait for each active queue point within the global budget, drain safe retirement, destroy registry/upload/renderer/context/swapchain/device on Render Thread, publish result, signal exit, and join.

- [ ] **Step 6: Implement distinct device-lost teardown**

Seal and stop submissions, preserve last normal timeline values, capture backend evidence, mark timelines Lost, cancel requests after dropping Render refs, bypass the normal completion predicate, and destroy child/device state on Render Thread in dependency order. Never report outstanding tokens as Completed. A watchdog expiry invokes fatal policy without detaching.

- [ ] **Step 7: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target RenderThreadRuntimeValidation
ctest --test-dir build\win_x64_debug -C Debug -R RenderThreadRuntimeValidation --output-on-failure
```

Expected: full fault matrix, normal shutdown, device-lost teardown, and fake-clock watchdog tests pass; live fake RHI count is zero in every parameter.

- [ ] **Step 8: Review and commit**

```powershell
git diff --check
git add RHI RHI_DX11 RHI_DX12 RHI_Vulkan RHI_Metal RHI_OpenGL Render\Private\Runtime Render\Private\Resources Tests\Common Tests\RenderThreadRuntimeValidation
git commit -m "feat: harden render runtime failure handling"
```

---

### Task 16: Compose extraction, resources, surface lifetime, and shutdown in Engine

**Files:**

- Create: `Engine/Private/RenderRuntimeComposition.h`
- Create: `Engine/Private/RenderRuntimeComposition.cpp`
- Create: `Tests/EngineRenderCompositionValidation/main.cpp`
- Modify: `Engine/Include/Engine/Engine.h`
- Modify: `Engine/Private/Engine.cpp`
- Modify: `Engine/CMakeLists.txt`
- Modify: `Render/Include/Render/RenderSubsystem.h`
- Modify: `Render/Private/RenderSubsystem.cpp`
- Modify: `Runtime/Include/Runtime/Window/WindowSubsystem.h`
- Modify: `Runtime/Private/Window/WindowSubsystem.cpp`
- Modify: `Resource/Include/Resource/ResourceSubsystem.h`
- Modify: `Tests/CMakeLists.txt`

**Runtime tick order:**

```text
events -> update subsystems -> worlds -> extraction -> TryPublishFrame
       -> window-close/surface control -> frame sequence advance
```

Engine never calls upload processing or rendering work directly.

- [ ] **Step 1: Write failing composition tests**

Prove Window initializes before surface capture/Render startup; Resource receives the gateway before its first Tick; Engine invokes extraction after World Tick; incomplete extraction is not published; publish pressure does not block; surface generation increases on resize; OpenGL context is released before Render starts; and Engine never calls old Begin/Render/End/Present/upload APIs.

- [ ] **Step 2: Write failing shutdown-order tests**

Record: stop simulation/load production, seal Resource, stop/join Render, drain terminal Resource request refs, destroy Worlds, deinitialize Resource, then Window/HAL. Prove Window/native surface remains alive through Render acknowledgement and initialization failure unwinds without leaving a thread.

- [ ] **Step 3: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target EngineRenderCompositionValidation
```

Expected: missing Engine composition failures.

- [ ] **Step 4: Implement pre-initialize composition**

Store `RenderRuntimeConfig` and initial `RenderFrameSettings` in EngineConfig. Before collection initialization, inject RenderSubsystem as ResourceSubsystem’s gateway. In the staged before-hook for RenderSubsystem, require initialized WindowSubsystem, capture one `NativeSurfaceDesc`, release an OpenGL context from update if needed, and call `Configure`. Do not add a Runtime-to-RHI dependency; Engine constructs the descriptor from Window values and platform compile definitions.

- [ ] **Step 5: Move extraction into Engine Tick**

Engine owns `RenderFrameExtractor`, frame sequence, world revision, temporal epoch, settings, and pending capture request. Active-world replacement increments world revision. `RequestRenderTemporalReset()` increments temporal epoch. A successful extraction moves the unique packet into `TryPublishFrame`; rejection is counted and never retried with the same sequence.

- [ ] **Step 6: Route resize and capture as value controls**

WindowSubsystem owns monotonically increasing surface generation. Engine compares the last published generation and calls `RequestResize` with the newest descriptor. Frame capture is a one-shot request ID embedded in the next packet; Render diagnostics returns an owned result value. No sample receives a backbuffer pointer.

- [ ] **Step 7: Implement ordered Engine shutdown**

Stop ticking worlds and new loads, call `ResourceSubsystem::BeginRenderShutdown`, targeted-deinitialize RenderSubsystem and wait for its result, call `DrainTerminalRenderRequests`, shut down Worlds, then `DeinitializeAll()` remaining CPU subsystems. Preserve diagnostic result values after Render object teardown.

- [ ] **Step 8: Run green verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target EngineRenderCompositionValidation SystemIntegrationTest
ctest --test-dir build\win_x64_debug -C Debug -R "EngineRenderCompositionValidation|SystemIntegrationTest" --output-on-failure
```

Expected: tick order, surface ownership, extraction publication, pressure, temporal revision, startup unwind, and ordered shutdown tests pass.

- [ ] **Step 9: Review and commit**

```powershell
git diff --check
git add Engine Render\Include\Render\RenderSubsystem.h Render\Private\RenderSubsystem.cpp Runtime\Include\Runtime\Window Runtime\Private\Window Resource\Include\Resource\ResourceSubsystem.h Tests\EngineRenderCompositionValidation Tests\CMakeLists.txt
git commit -m "feat: compose dedicated rendering in engine"
```

---

### Task 17: Migrate Runtime Samples, tests, and the Editor compile-only adapter

**Files:**

- Create: `Samples/Common/Include/Samples/RuntimeFrameDriver.h`
- Create: `Samples/Common/Private/RuntimeFrameDriver.cpp`
- Modify: `Samples/Common/CMakeLists.txt`
- Modify: `Samples/Showcase/ModelViewer/main.cpp`
- Modify: `Samples/Showcase/RenderingShowcase/main.cpp`
- Create: `Editor/Include/Editor/EditorRenderRuntimeAdapter.h`
- Create: `Editor/Private/EditorRenderRuntimeAdapter.cpp`
- Modify: `Editor/CMakeLists.txt`
- Modify: `Editor/Private/EditorApplication.cpp`
- Modify: `Editor/Private/EditorRenderBootstrapService.cpp`
- Modify: `Editor/Private/EditorRenderFrameService.cpp`
- Modify: `Editor/Private/EditorRenderShutdownService.cpp`
- Modify: `Editor/Private/EditorViewportRenderService.cpp`
- Modify: `Editor/Private/EditorFrameSubmissionService.cpp`
- Modify: `Editor/Private/EditorMainSwapChainService.cpp`
- Modify: `Editor/Private/EditorScreenshotService.cpp`
- Modify or delete: `Editor/Include/Editor/EditorApplication.h`
- Modify or delete: `Editor/Include/Editor/EditorEndFrameService.h`
- Modify or delete: `Editor/Include/Editor/EditorFrameCoordinator.h`
- Modify or delete: `Editor/Include/Editor/EditorFrameLifecycleService.h`
- Modify or delete: `Editor/Include/Editor/EditorFrameSubmissionService.h`
- Modify or delete: `Editor/Include/Editor/EditorMainFramebufferService.h`
- Modify or delete: `Editor/Include/Editor/EditorMainFramePresentationService.h`
- Modify or delete: `Editor/Include/Editor/EditorMainSwapChainService.h`
- Modify or delete: `Editor/Include/Editor/EditorNativeUISubmissionService.h`
- Modify or delete: `Editor/Include/Editor/EditorRenderBootstrapService.h`
- Modify or delete: `Editor/Include/Editor/EditorRenderShutdownService.h`
- Modify or delete: `Editor/Include/Editor/EditorScreenshotRequestService.h`
- Modify or delete: `Editor/Include/Editor/EditorScreenshotService.h`
- Modify or delete: `Editor/Include/Editor/EditorServiceRegistrationService.h`
- Modify or delete: `Editor/Include/Editor/EditorUIBootstrapService.h`
- Modify or delete: `Editor/Include/Editor/EditorViewportRenderService.h`
- Modify or delete: `Editor/Include/Editor/Panels/NativeViewport.h`
- Modify or delete: `Editor/Include/Editor/Panels/Viewport.h`
- Modify or delete: `Editor/Include/Editor/UI/EditorUIBackendFactory.h`
- Modify or delete: `Editor/Include/Editor/UI/EditorUIBackendTypes.h`
- Modify or delete: `Editor/Include/Editor/UI/EditorUIHost.h`
- Modify or delete: `Editor/Private/EditorFrameCoordinator.cpp`
- Modify or delete: `Editor/Private/EditorMainFramebufferService.cpp`
- Modify or delete: `Editor/Private/EditorNativeUISubmissionService.cpp`
- Modify or delete: `Editor/Private/EditorServiceRegistrationService.cpp`
- Modify or delete: `Editor/Private/Panels/NativeViewport.cpp`
- Modify or delete: `Editor/Private/Panels/Viewport.cpp`
- Modify: `Tests/EditorContextValidation/main.cpp`
- Modify: `Tests/RenderPassValidation/main.cpp`
- Modify: `Tests/GPUResourceManagerValidation/main.cpp`
- Modify: `Tests/ParticleValidation/main.cpp`
- Modify: `Tests/CMakeLists.txt`

**Migration rule:**

Production callers use Engine settings/capture and RenderSubsystem value diagnostics only. Tests that need live SceneRenderer/RHI internals link `RVX_RenderTestSupport` and create an explicit Inline runtime fixture; they do not restore production getters.

- [ ] **Step 1: Add failing source-boundary tests**

Add assertions that ModelViewer and RenderingShowcase contain none of `GetSceneRenderer`, `GetGPUResourceManager`, `GetRenderContext`, `renderSubsystem->GetDevice`, `renderSubsystem->BeginFrame`, `renderSubsystem->Render(`, `renderSubsystem->EndFrame`, or `renderSubsystem->Present`. Add an Editor compile target requiring the new adapter and forbidding RenderSubsystem live-object getters.

- [ ] **Step 2: Run red verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target ModelViewer RenderingShowcase
cmake --preset win_x64_debug -B build\win_x64_editor_compile -DRVX_BUILD_EDITOR=ON
cmake --build build\win_x64_editor_compile --config Debug --target RVXEditor EditorContextValidation
```

Expected: source-boundary assertions or missing adapter build fails.

- [ ] **Step 3: Migrate ModelViewer settings and diagnostics**

Move post-process, shadow, GPU-driven, ray-tracing budget, history reset, and resize controls into update-owned `RenderFrameSettings`/Engine APIs. Convert readiness helpers to consume `RenderDiagnosticsSnapshot` sub-values. Replace immediate GPU upload/readiness with ResourceSubsystem handle/status flow. Replace backbuffer screenshot access with frame capture request/result values.

- [ ] **Step 4: Migrate ModelViewer smoke synchronization**

`RuntimeFrameDriver` advances Engine ticks until diagnostics reports the requested submitted/presented frame or a bounded timeout. It never accesses Render Thread state. Dynamic budget-recovery smoke changes the next packet settings only after observing the prior diagnostics sequence. Final assertions use the last immutable snapshot retained before shutdown.

- [ ] **Step 5: Migrate RenderingShowcase**

Publish its shadow/post-process settings through Engine, render with normal Engine Tick, and generate its report from RenderDiagnosticsSnapshot. Remove its local manual render lifecycle and direct RHI/backend queries; backend identity comes from diagnostics.

- [ ] **Step 6: Add the Editor mechanical adapter**

Adapter captures the Editor window surface, publishes the same packet/control contracts, and exposes value diagnostics. Services that require direct RenderContext/device/swapchain are compile-time disabled behind the adapter with an explicit `EditorRenderFeatureStatus::UnavailableDuringM1ArchitectureCut`; they do not receive dummy RHI pointers. Editor viewport/native UI rendering is not implemented or tested for correctness in M1.

- [ ] **Step 7: Migrate internal tests to explicit test support**

Tests of passes/materials/renderer may construct the Inline fixture and access test-owned objects through `RenderRuntimeTestHarness`; production RenderSubsystem remains opaque. Rewrite GPUResourceManager tests to registry/processor APIs. Keep BasicRHI/ComputeDemo classified as non-shipping backend diagnostic samples; Showcase Runtime samples must use Engine publication.

- [ ] **Step 8: Run focused migration verification**

```powershell
cmake --build build\win_x64_debug --config Debug --target ModelViewer RenderingShowcase RenderPassValidation RenderResourceRuntimeValidation
ctest --test-dir build\win_x64_debug -C Debug -R "SampleCLIValidation|RenderPassValidation|RenderResourceRuntimeValidation" --output-on-failure
cmake --build build\win_x64_editor_compile --config Debug --target RVXEditor EditorContextValidation
ctest --test-dir build\win_x64_editor_compile -C Debug -R EditorContextValidation --output-on-failure
```

Expected: Showcase samples and Editor compile; selected tests pass; source-boundary assertions find no production backdoor calls.

- [ ] **Step 9: Review and commit**

```powershell
git diff --check
git add Samples Editor Tests
git commit -m "refactor: migrate callers to render publication"
```

---

### Task 18: Delete legacy APIs/facades and enforce the final module boundary

**Files:**

- Delete: `Render/Include/Render/GPUResourceManager.h`
- Delete: `Render/Private/GPUResourceManager.cpp`
- Delete: `Render/Include/Render/RenderService.h`
- Delete: `Render/Private/RenderService.cpp`
- Delete or reduce to non-upload values: `RenderContracts/Include/RenderContracts/RenderResource.h`
- Modify: `Resource/Include/Resource/Types/MeshResource.h`
- Modify: `Resource/Include/Resource/Types/TextureResource.h`
- Modify: `Resource/Include/Resource/Types/MaterialResource.h`
- Modify: `Resource/Private/Types/MeshResource.cpp`
- Modify: `Resource/Private/Types/TextureResource.cpp`
- Modify: `Resource/Private/Types/MaterialResource.cpp`
- Modify: `Render/Include/Render/RenderSubsystem.h`
- Modify: `Render/Private/RenderSubsystem.cpp`
- Modify: `Render/Include/Render/Renderer/SceneRenderer.h`
- Modify: `Render/Private/Renderer/SceneRenderer.cpp`
- Modify: `Render/CMakeLists.txt`
- Modify: `Docs/module-boundaries.json`
- Create: `Scripts/check_m1_architecture.py`
- Modify: `Scripts/check_render_contract_fields.py`
- Modify: `Scripts/check_architecture_phase_gates.py`
- Modify: `Tests/CMakeLists.txt`
- Modify: `Tests/GPUResourceManagerValidation/main.cpp`
- Modify: `Tests/MaterialSystemValidation/main.cpp`
- Modify: `Tests/ParticleValidation/main.cpp`
- Modify: `Tests/PipelineCacheValidation/main.cpp`
- Modify: `Tests/RenderPassValidation/main.cpp`
- Modify: `Tests/RenderSceneValidation/main.cpp`

**Required absence:**

- `IRenderResourceSource`, `IRenderMeshUploadSource`, `IRenderTextureUploadSource`, `IRenderMaterialSource`
- `GPUResourceManager`, `LegacySynchronousRenderBridge`
- `RenderSubsystem::{BeginFrame,Render,EndFrame,Present,RenderFrame,ProcessGPUUploads}`
- live-object RenderSubsystem getters
- World/SceneManager/Camera SceneRenderer setup overloads
- Render source/link dependencies on RenderExtraction, ResourceSceneAdapters, World, Scene, Resource, Runtime, or HAL

- [ ] **Step 1: Add the failing M1 architecture gate**

`check_m1_architecture.py` parses Render CMake links, scans Render includes, verifies required public methods, forbids the named symbols, verifies production has no Inline factory/config, checks Runtime Sample source boundaries, and confirms the core M1 contract/executor/resource/runtime/lifetime validation targets are registered. Add it to CTest as `Architecture.M1ArchitectureCut` with `architecture;unit` labels. Task 19 extends the same gate with TSAN/native evidence registration.

- [ ] **Step 2: Run red verification**

```powershell
ctest --test-dir build\win_x64_debug -C Debug -R Architecture.M1ArchitectureCut --output-on-failure
```

Expected: gate reports the temporary facade, legacy bridge/APIs, source interfaces, and forbidden Render links.

- [ ] **Step 3: Delete old pointer-bearing Resource contracts**

Resource types stop implementing the `IRender*Source` hierarchy. Delete pointer upload views and material texture-source bindings. Retain shared enums only in their new value-contract headers. Confirm Resource owns CPU data and ResourceUploadRequestBuilder is the only upload conversion path.

- [ ] **Step 4: Delete the temporary facade and synchronous subsystem surface**

Remove facade/bridge files from CMake and all includes. RenderSubsystem keeps only Configure, frame/resize publication, gateway methods, diagnostics/results, and EngineSubsystem lifecycle. Remove SceneRenderer World/SceneManager/Camera entry points and all production live-object getters.

- [ ] **Step 5: Remove forbidden Render dependencies**

`RVX_Render` links only Core, RenderContracts, RHI, ShaderCompiler, and still-proven internal render-only dependencies such as Spatial. Remove `RVX::RenderExtraction` and `RVX_Runtime`; remove corresponding source includes. Update `Docs/module-boundaries.json` so Render allowed edges no longer include HAL, Runtime, or RenderExtraction.

- [ ] **Step 6: Expand static contract proof**

Run the declaration-aware field checker across every published packet/request nested aggregate and value proxy used inside them. Add symbol-absence and module-link checks to `check_architecture_phase_gates.py`. Do not replace these checks with repository-wide pointer grep.

- [ ] **Step 7: Run full architecture and unit verification**

```powershell
cmake --preset win_x64_debug
cmake --build build\win_x64_debug --config Debug --target RVXValidationInventory
ctest --test-dir build\win_x64_debug -C Debug -L "architecture|unit" --output-on-failure
```

Expected: all architecture/unit tests pass and no removed symbol is present.

- [ ] **Step 8: Run explicit absence checks**

```powershell
rg -n "IRender(ResourceSource|MeshUploadSource|TextureUploadSource|MaterialSource)|GPUResourceManager|LegacySynchronousRenderBridge|ProcessGPUUploads\(|RenderFrame\(World|GetSceneRenderer\(|GetRenderContext\(|GetGPUResourceManager\(" Render RenderContracts Resource Engine Samples
```

Expected: no matches. Test-support names must use `RenderRuntimeTestHarness`, not removed production symbols.

- [ ] **Step 9: Review and commit**

```powershell
git diff --check
git add -A Render RenderContracts Resource Engine Samples Scripts Tests Docs\module-boundaries.json
git commit -m "refactor: enforce m1 render ownership boundary"
```

---

### Task 19: Enable TSAN, native lifecycle, Build Truth, and final M1 evidence

**Files:**

- Create: `Tests/NativeRenderLifecycleValidation/main.cpp`
- Create: `Tests/RenderConcurrencyTSAN/main.cpp`
- Modify: `Tests/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `CMakePresets.json`
- Modify: `Scripts/run_build_truth.ps1`
- Modify: `Scripts/run_architecture_baseline.ps1`
- Modify: `Scripts/check_m1_architecture.py`
- Modify: `Docs/build-truth.md`
- Modify: `.github/workflows/ci.yml`
- Modify: `Editor/CMakeLists.txt`
- Modify: `Docs/superpowers/specs/phase-log.md`

**Required native scenario:**

Create a real hidden native window on Main/Update Thread; on Apple also create/attach `CAMetalLayer` there and record its owner thread. Start Render; prove device/surface wrappers/children are created on Render Thread and Metal never touches the view hierarchy; publish/present one deterministic frame; request one newer-generation resize; prove old surface resources survive until their Graphics completion point; wait idle and prove no drawable acquisition/re-present; stop; prove every RHI child/device destructor ran on Render Thread, then detach/destroy the Apple layer/window on Main Thread after shutdown acknowledgement.

- [ ] **Step 1: Extend the M1 gate with failing TSAN/native registration requirements**

First extend `check_m1_architecture.py` to require CMake registration for the small TSAN target and the platform-native lifecycle suite. Then add `RenderConcurrencyTSAN` only when `RVX_ENABLE_TSAN=ON`, linking `RVX_RenderRuntimeCore`, RenderContracts, Core, RHI interfaces, and fake RHI/test support—not full Render, Vulkan drivers, Samples, Editor, or unrelated third-party modules. Add `NativeRenderLifecycleValidation` per enabled platform/backend with explicit capability skips only for optional Vulkan-on-Windows and Tier 2 paths.

- [ ] **Step 2: Run red registration verification**

```powershell
python Scripts\check_m1_architecture.py --root .
```

Expected: exit code 1 with missing `NativeRenderLifecycleValidation` and `RenderConcurrencyTSAN` registration findings.

- [ ] **Step 3: Implement the small TSAN stress suite**

Stress frame replacement, upload count/bytes, release/status CAS, diagnostics shared publication, dedicated wake/stop, stale generations, registry transitions, and fake-RHI lifetime for many deterministic barrier-synchronized iterations. Treat every TSAN report as failure. Do not suppress project races; only documented third-party runtime suppressions are allowed, and this target should not link those libraries.

- [ ] **Step 4: Implement native lifecycle smoke**

Use DX12 on Windows, Vulkan on Linux, and Metal on macOS as required non-fake cases. The Metal case asserts Main Thread layer attachment/detachment, Render Thread device/`nextDrawable`/command-buffer `presentDrawable:`/RHI destruction, and zero `NSWindow`/`NSView` access from Render Thread. Permit WARP/software Vulkan for M1 and record adapter identity. Windows Vulkan and DX11/OpenGL execute when capability exists; capability absence records an environment skip, not pass. Assertions consume Render diagnostics and test ownership records, not live Render objects.

- [ ] **Step 5: Add presets and CI environment support**

Add `linux_x64_tsan` configure/build/test presets with Clang and `-fsanitize=thread`. Linux CI installs Xvfb and Mesa Vulkan, runs native smoke under Xvfb, then TSAN separately. Windows runs DX12 required plus optional Vulkan/DX11/OpenGL. macOS runs Metal required. Add a separate Editor-on compile-only job/target; do not add Editor tests to Runtime Build Truth.

- [ ] **Step 6: Extend Build Truth evidence**

`run_build_truth.ps1` records M1 architecture gate, executor suites, native lifecycle result/adapter, TSAN artifact path where applicable, source commit, and Editor compile-only result. Fresh Build Truth still fails closed on missing executables, zero tests, unavailable required native backend, or missing artifact.

- [ ] **Step 7: Review and commit the evidence plumbing**

```powershell
git diff --check
git add CMakeLists.txt CMakePresets.json Tests Scripts Docs\build-truth.md .github\workflows\ci.yml Editor\CMakeLists.txt
git commit -m "ci: enforce m1 runtime architecture evidence"
```

This commit is the candidate implementation revision. All following local and CI commands must report this exact commit; if a code/config fix is needed, create a new candidate and restart the matrix.

- [ ] **Step 8: Run Windows local final verification on the committed revision**

```powershell
pwsh -NoProfile -File Scripts\run_build_truth.ps1 -ConfigurePreset win_x64_debug -BuildPreset win_x64_debug -TestPreset win_x64_debug_unit_lint -BuildDir build\win_x64_debug -Configuration Debug -BaseRef origin/master -Fresh
ctest --test-dir build\win_x64_debug -C Debug -R "NativeRenderLifecycleValidation.*DX12" --output-on-failure
```

Expected: Fresh Build Truth passes, required DX12 native lifecycle passes, and the report records the exact HEAD commit.

- [ ] **Step 9: Push the candidate and observe Linux/macOS gates**

Linux:

```bash
cmake --preset linux_x64_debug
cmake --build --preset linux_x64_debug --target RVXValidationInventory
xvfb-run -a ctest --test-dir build/linux_x64_debug -R 'NativeRenderLifecycleValidation.*Vulkan' --output-on-failure
cmake --preset linux_x64_tsan
cmake --build --preset linux_x64_tsan --target RenderConcurrencyTSAN
ctest --preset linux_x64_tsan --output-on-failure
```

macOS:

```bash
cmake --preset mac_arm64_debug
cmake --build --preset mac_arm64_debug --target RVXValidationInventory
ctest --test-dir build/mac_arm64_debug -R 'NativeRenderLifecycleValidation.*Metal' --output-on-failure
```

Expected: Vulkan lifecycle plus TSAN pass on Linux; Metal lifecycle passes on macOS. CI retains all artifacts.

- [ ] **Step 10: Execute and record the M1 exit checklist**

Record evidence for all 12 exit criteria from design section 18 in `Docs/superpowers/specs/phase-log.md`. Include exact commit, presets, test counts, adapter/device names, optional capability skips, TSAN result, startup/shutdown timing, and Build Truth artifact paths. Do not mark M1 complete if any required platform result is missing.

- [ ] **Step 11: Commit the evidence record separately**

```powershell
git diff --check
git add Docs\superpowers\specs\phase-log.md
git commit -m "docs: record m1 architecture evidence"
```

The phase log records the candidate implementation revision validated by the artifacts; the documentation-only evidence commit is not substituted for that source revision. Do not write a final “M1 complete” entry until every required artifact exists. If code or configuration changes after the candidate commit, rerun the full matrix and record the replacement candidate.

---

## Specification Coverage Matrix

| Approved design section | Implementation tasks | Primary proof |
|---|---|---|
| 5–7 Identity, frame packet, upload request | 1, 11, 12 | RenderContractsValidation, field checker, RenderFrameExtractionValidation, request reclamation cases |
| 8 Gateway and fixed status table | 2, 3, 11 | RenderConcurrencyValidation, stale-generation and pressure/retry cases |
| 9 RenderSubsystem, lifecycle, executor, native surface | 4–7, 15–17, 19 | RenderExecutorValidation, RHIContractValidation, EngineRenderCompositionValidation, native lifecycle smoke, M1 architecture gate |
| 10–11 Bounded queues and runtime pump | 3, 7, 13 | capacity/coalescing tests, shared runtime suite, idle/no-present proof |
| 12 Registry, completion, retirement | 8–10, 13–14 | RenderSubmissionValidation, RenderResourceRuntimeValidation, ownership inventory, exact last-use and non-registry-holder cutover |
| 13 Failure/device loss | 15 | fake-RHI fault matrix and distinct lost teardown assertions |
| 14 Diagnostics | 7, 13, 15–17 | immutable snapshot retention, settings/result/capture migrations |
| 15 Ordered shutdown/watchdogs | 6, 15–16 | staged subsystem tests, fake-clock watchdogs, Engine shutdown trace |
| 16 Migration strategy | 1–18 in order | per-task build/review/commit gates and final legacy deletion |
| 17 Validation design | 1–19 | Inline/Dedicated suites, static gates, fake RHI, TSAN, native smoke |
| 18 Exit criteria | 18–19 | absence/module gates and commit-tied three-platform Build Truth evidence |

---

## Per-Task Review Protocol

After each task and before its commit:

1. Compare the diff only with that task’s file list and interfaces.
2. Run its red/green test commands and inspect full output.
3. Run `git diff --check`.
4. Search for newly introduced pointer/callback/RHI fields in packet/request declarations with the specialized checker.
5. Confirm no new production code depends on the temporary GPUResourceManager facade or legacy synchronous bridge.
6. Review thread ownership, memory order, state writer, queue capacity, and shutdown behavior explicitly.
7. Record reviewer findings before moving to the next task.

## Plan Self-Review Checklist

- [x] Every design section 5–18 maps to at least one implementation task and one verification.
- [x] Every new type has one owning module and no dependency cycle.
- [x] Every cross-thread publication is bounded or coalesced and has an explicit pressure result.
- [x] Every atomic state transition names its permitted writer and memory order.
- [x] Every RHI strong-reference path ends with Render-thread final release or explicit device-lost teardown.
- [x] Every upload terminal path drops Render request refs before release publication and update-side final reclamation is tested.
- [x] Every old synchronous/pointer API named by the design is removed and covered by an absence gate.
- [x] Runtime Samples use packet publication and value diagnostics; Editor remains compile-only.
- [x] Required DX12/Vulkan/Metal native lifecycle, Linux TSAN, local Fresh Build Truth, and three-platform CI evidence are tied to one final source commit.
- [x] No stub code, deferred-acceptance marker, unbounded queue, arbitrary correctness sleep, fake native success, or unsupported production fallback remains.

## Execution Handoff

Execute strictly in task order. The recommended mode is `superpowers:subagent-driven-development`, one fresh implementation worker and one independent reviewer per task, because tasks 1–19 have explicit buildable checkpoints. If execution remains in the current agent context, use `superpowers:executing-plans` and stop at each review/commit gate. Do not start Task 2 until Task 1’s focused tests and review are complete, and do not declare M1 complete until Task 19’s cross-platform artifacts match the recorded commit.
