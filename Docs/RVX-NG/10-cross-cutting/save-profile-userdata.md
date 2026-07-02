# RVX-NG · Cross-cutting/Save Profile & User Data 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 Reflection、Platform/VFS、World、Online Services  
**里程碑**：M8-M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义本地存档、用户 profile、设置、checkpoint、云同步、版本迁移和损坏恢复。存档系统必须可版本化、可验证、可加密/签名、可跨平台目录策略运行，并能与 World Partition、Online Cloud Save、Replay 和 Localization/Accessibility 协同。

**范围内**：SaveGame、Profile、Settings、slot、checkpoint、schema/version、migration、atomic write、cloud sync metadata、integrity、privacy。  
**范围外**：具体游戏内容存哪些字段、平台云服务后台实现、用户 UI 具体表现。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 原子性 | 写存档必须 temp + fsync/rename 或平台等价原子提交 |
| 版本化 | 每个 save/profile 有 schema、build、asset manifest hash 和 migration path |
| 可恢复 | 损坏/部分写入有 backup、rollback、诊断 |
| 平台合规 | 用户目录、quota、cloud、加密、隐私按平台 profile 配置 |
| 可测试 | corruption、quota full、version mismatch、cloud conflict 可注入 |

## 3. 公共接口面（设计契约）

```cpp
enum class UserDataKind : uint8 {
    SaveGame,
    Profile,
    Settings,
    Checkpoint,
    ReplayMarker
};

struct SaveHeader {
    Guid saveId;
    UserDataKind kind;
    Version schemaVersion;
    Hash assetManifestHash;
    Hash payloadHash;
    uint64 timestampUtc;
};

class SaveSystem {
public:
    AsyncResult<void> SaveSlot(UserId user, Name slot, SavePayload payload);
    AsyncResult<SavePayload> LoadSlot(UserId user, Name slot);
    AsyncResult<SlotList> EnumerateSlots(UserId user);
    MigrationResult Migrate(SaveHeader from, Version to);
};
```

**不变量**：外部/用户提供的 save 文件走安全 reader + fuzz 覆盖；load 不直接构造裸指针对象；所有 object/entity 引用必须是稳定 ID/GUID；敏感数据按 platform policy 加密或隔离。

## 4. 数据结构与内存布局

- **Save Header**：magic、schema、engine build、platform、user id scope、asset manifest hash、payload hash、compression/encryption flags。
- **Save Payload**：反射序列化数据、World cell state、player profile、inventory/settings、subsystem chunks。
- **Chunk Table**：chunk id、type、offset、size、hash、migration version。
- **Profile Data**：input bindings、graphics/audio/accessibility settings、online cache、local progression。
- **Cloud Metadata**：provider slot id、etag/version、last sync, conflict state。
- **Backup Ring**：最近 N 次成功提交，损坏时自动回退。

## 5. 核心算法

- **Save Build**：各 subsystem 产 chunk → schema validate → compress/encrypt/sign → atomic commit。
- **Load**：header verify → hash/signature → version check → migrate chunks → subsystem restore。
- **Migration**：按 chunk version 执行迁移器，旧字段有 default/rename/remove 规则。
- **World Partition Save**：按 cell/data layer 记录 persistent entity state，streaming cell 可按需恢复。
- **Cloud Sync**：local/remote etag 比较；自动合并只允许安全字段，冲突进入 UI/策略。
- **Settings Split**：profile settings 和 savegame progress 分离，避免云同步覆盖本机图形/输入配置。

## 6. 线程与内存模型

序列化采集在 World safe point 或 subsystem snapshot 上进行。压缩、加密、写盘走后台 job，不阻塞主线程。load 的对象恢复分 staged：parse → validate → allocate → patch references → publish。Cloud sync 异步进入 Online dispatcher。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 写入中断 | 保留旧 slot，临时文件下次清理 |
| quota full | 返回明确错误，提示删除/降级，不覆盖旧存档 |
| hash/signature fail | 拒绝加载，尝试 backup |
| schema 太旧无迁移 | 拒绝或只读导入，不崩溃 |
| asset manifest mismatch | 尝试兼容加载；缺资产走 fallback 并记录 |
| cloud conflict | 标记 conflict，不静默覆盖 |

**禁止**：部分写入覆盖唯一存档；load 未校验数据直接构造 runtime 对象；把 token/PII 写入普通 save payload。

## 8. 性能目标

- autosave 增量化，避免全世界同步序列化。
- 大型存档分 chunk，支持流式加载和进度反馈。
- save/load 时间、大小、压缩率、迁移耗时进入 telemetry/profile。

## 9. 测试计划

- atomic write、power-loss simulation、quota full、corrupt payload。
- schema migration、field rename/remove/default、unknown chunk。
- World Partition cell save/load、stable entity ID、asset missing fallback。
- cloud etag conflict、offline queue、privacy redaction。
- fuzz save reader 和 migration input。

## 10. 开放问题 / Spike

- Save payload 使用统一 binary archive、FlatBuffers/Cap'n Proto 风格格式，还是自定义 chunked format。
- 加密/签名是否默认启用，哪些平台强制。
- autosave granularity：subsystem dirty chunk vs checkpoint snapshot。
- cloud conflict UI 是否由 Online Services 统一提供。

## 11. 依赖与被依赖

- **依赖**：Reflection、Platform/VFS、World Partition、Asset Registry、Online Services、Security、Compression/Hash。
- **被依赖**：Gameplay progression、Settings/Profile、Cloud Save、Replay markers、Crash recovery、Shipping compliance。
