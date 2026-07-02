# RVX-NG · Tools/Team Workflow & Source Control 详细设计（Tier-1）

**日期**：2026-06-27  
**层级**：Tier-1 · 派生自 Editor、Asset Pipeline、Project/Package  
**里程碑**：M10-M11  
**状态**：详细设计 · v1.0 可评审

---

## 1. 目标与范围

**目标**：定义团队协作与源码/资产版本控制接缝：资产锁、checkout、large file policy、场景拆分、review/submit gate、merge 策略、CI asset validation，使大型团队可以安全并行编辑世界、Prefab、材质、脚本和包。

**范围内**：source control provider、asset checkout/lock、binary asset policy、world/cell 分文件、change list、pre-submit validation、merge hooks、large file storage、ownership metadata。  
**范围外**：具体托管平台运营、代码 review UI、多人实时协作编辑。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| Provider 隔离 | Editor 不直接绑定 Perforce/Git/SVN API |
| 二进制安全 | binary asset 默认锁定编辑或走专用 merge |
| 小粒度协作 | World Partition cell、Prefab、Material、Graph 可独立 checkout |
| 可验证 | submit 前运行 asset/schema/cook/reference 检查 |
| 可追踪 | 资产 GUID、owner、change list、review 状态进入 metadata |

## 3. 公共接口面（设计契约）

```cpp
enum class ScmState : uint8 {
    Unknown,
    CheckedIn,
    CheckedOutByMe,
    LockedByOther,
    Modified,
    Added,
    Deleted,
    Conflict
};

class SourceControlProvider {
public:
    AsyncResult<ScmState> Query(Path path);
    AsyncResult<void> Checkout(Path path, CheckoutMode mode);
    AsyncResult<void> Revert(Path path);
    AsyncResult<SubmitResult> Submit(ChangeListId cl, SubmitOptions options);
};

class AssetEditGuard {
public:
    Result<EditToken> BeginEdit(AssetId asset, EditIntent intent);
    void EndEdit(EditToken token);
};
```

**不变量**：Editor 修改资产前必须通过 AssetEditGuard；锁失败不能产生本地脏写；自动 merge 只能用于声明为 merge-safe 的文本/结构化资产；CI 失败不能进入 protected branch/release manifest。

## 4. 数据结构与内存布局

- **Source Control State Cache**：path、asset id、state、owner、changelist、timestamp、provider revision。
- **Asset Ownership Metadata**：asset owner、reviewer、lock policy、merge policy、large file flag。
- **Change List Manifest**：files、assets、dependencies、generated outputs、tests required。
- **World Edit Unit**：world header、cell file、data layer、actor/entity shard、external object file。
- **Merge Policy**：text merge、schema-aware merge、binary lock-only、manual tool。

## 5. 核心算法

- **Edit Guard**：asset id → path/dependencies → query SCM → checkout/lock → create edit token。
- **World Split**：World Partition cell/entity shard 独立文件，降低场景冲突。
- **Dependency Diff**：change list → affected assets/packages/build nodes/tests。
- **Pre-submit Validation**：schema validate、GUID uniqueness、cook dry run、missing dependency、license/security policy。
- **Merge Dispatch**：根据 asset type 选择 text/structured/binary/manual merge。
- **Generated Artifact Rule**：generated/cooked output 不手工提交，除非明确声明为 checked-in artifact。

## 6. 线程与内存模型

SCM 查询和文件状态刷新走后台 job，Editor UI 使用状态 cache。checkout/submit 是异步命令，修改文件必须等 edit token 成功。状态变化通过 EventBus 发布，不在 asset hot path 同步访问远程服务。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| checkout/lock 失败 | 拒绝编辑，显示 owner/reason |
| provider offline | 进入 offline read-only 或 explicit local edit mode |
| merge conflict | 标记 conflict，禁止 cook/submit |
| GUID 冲突 | validation fail，要求修复 |
| generated artifact stale | CI fail，提示重跑生成 |
| large file 未走 LFS/Depot | pre-submit fail |

**禁止**：Editor 静默修改未 checkout 资产；binary asset 自动文本 merge；source asset 与 cooked output 混入同一提交规则。

## 8. 性能目标

- SCM 状态刷新批量化，避免每个 asset 单独远程查询。
- 大项目 Asset Browser 状态显示使用懒加载和缓存。
- Pre-submit impact selection 只运行受影响测试和 cook 节点。

## 9. 测试计划

- mock provider：checkout、lock、offline、conflict、submit reject。
- AssetEditGuard 防未授权写入。
- world cell 并行编辑、GUID uniqueness、dependency diff。
- merge policy routing、large file validation、generated artifact stale。
- CI pre-submit gate 和 protected branch policy。

## 10. 开放问题 / Spike

- 首期 provider 支持 Perforce、Git LFS，还是只定义 Provider API。
- World Partition 是否采用 one-file-per-entity/actor 风格 shard。
- 哪些 asset type 可以结构化 merge。
- Editor offline local edit 是否允许，以及如何 reconcile。

## 11. 依赖与被依赖

- **依赖**：Editor、Asset Registry、World Partition、Build/Test、Project Package、Security/License。
- **被依赖**：大型团队内容生产、CI gate、Release qualification、Project templates。
