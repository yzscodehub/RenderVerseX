# RVX-NG · Asset/资产管线 详细设计（Tier-1）

**日期**：2026-06-20
**层级**：Tier-1 · 派生自总纲 §10 Asset、契约 C5
**里程碑**：M8
**状态**：详细设计 · 待 S8/S5 Spike

---

## 1. 目标与范围

**目标**：源资产（DCC）→ 离线 cook 为紧凑运行时格式（mmap/GPU-ready）→ 句柄化异步加载，稳定 GUID 跨重启，版本可迁移；团队规模共享/分布式派生数据缓存（DDC）。

**范围内**：Import（glTF/FBX/纹理/音频）、Cook（运行时格式、BCn/mip/tangent/LOD/meshlet 构建）、Registry/GUID、异步加载/引用计数/驱逐、依赖图、版本化迁移、DDC。
**范围外**：运行时流式（streaming.md）、发行打包（09-shipping）、序列化机制（Reflect）。

## 2. 需求与约束

| 约束 | 要求 |
|---|---|
| 双世界 | 源资产 ↔ cooked 运行时格式（mmap、GPU-ready） |
| GUID | 稳定 GUID 跨重启 |
| 异步 | 加载在 JobSystem，完成帧同步派发（非忙轮询） |
| 内容寻址 | cook 缓存键 = hash(源+导入设置+cooker 版本)；DDC 共享 |
| 版本 | 格式/schema 版本化 + 迁移（前后兼容） |
| 依赖 | 资产依赖图（材质→纹理…），热重载沿图失效 |

## 3. 公共接口面（设计契约）

```cpp
struct AssetId { Guid guid; };
template<class T> struct AssetHandle { AssetId id; /* 句柄化、引用计数 */ };

class AssetRegistry {
    AssetId        Register(path);                 // 稳定 GUID（持久）
    AssetMeta      Meta(AssetId);                  // 类型/依赖/版本
    Span<AssetId>  Dependencies(AssetId);
};

class AssetManager {
    template<class T> AssetHandle<T> Load(AssetId);   // 异步；引用计数
    bool IsReady(AssetId);
    template<class T> const T* Get(AssetHandle<T>);   // 就绪→数据，否则 null/fallback
    void Release(AssetHandle<>);                       // 引用计数→0 可驱逐
    void OnReady(AssetId, Callback);                   // 完成回调（帧同步派发）
};

class Cooker {                                         // 离线
    CookResult Cook(AssetId, BackendTarget);          // → 运行时格式 + DDC 缓存
};
```

**不变量**：GUID 跨重启稳定（持久化）；`Load` 异步非阻塞（完成经回调/帧同步，**不忙轮询**）；cook 内容寻址（命中 DDC 不重 cook）；版本不匹配走迁移而非失败。

## 4. 数据结构与内存布局

- **运行时格式**：紧凑 binary，mmap 友好，GPU-ready 段（纹理 BCn、网格顶点/索引/meshlet）。header 含版本 + 依赖 GUID。
- **GUID 数据库**：path↔GUID 持久映射；元数据（类型/版本/依赖）。
- **依赖图**：资产→依赖资产（材质→纹理/着色器）。
- **DDC**：内容寻址键 → cooked blob（本地 + 共享/分布式）。
- **运行时句柄表**：AssetId→{加载状态、引用计数、数据指针}。

## 5. 核心算法

- **Import**：DCC 解析 → 中间表示 → 写源元数据 + GUID。
- **Cook**：中间表示 → 目标格式（纹理压缩 BCn/mip、网格 tangent/LOD/meshlet 构建、着色器预编译触发）；键命中 DDC 则跳过。
- **异步加载**：`Load` → 入 JobSystem 解码 job（mmap + GPU 上传经 RHI staging）→ 完成**帧边界同步派发**回调（condition_variable，非忙轮询）。
- **驱逐**：引用计数 0 + LRU；内存压力（Memory OOM 事件）触发主动驱逐。
- **版本迁移**：加载时 header 版本 < 当前 → 迁移函数升级（经 Reflect 字段容错）。
- **热重载**：源变更 → 重 cook → 沿依赖图失效 + 运行时热替换。

## 6. 线程与内存模型

- Cook 离线（构建期/cook farm）；运行时加载在 JobSystem worker。
- 数据 mmap（OS 页缓存）+ 池化句柄表；GPU 上传经 RHI staging ring。
- 完成派发在帧安全点（避免渲染中途资源切换）。

## 7. 错误与失败语义

| 情形 | 处理 |
|---|---|
| 加载失败/缺文件 | Result 错误 + fallback 资产（紫纹理/占位网格），不崩 |
| 版本太旧无迁移 | 报错 + fallback；cook 期升级 |
| GUID 冲突 | 注册期检测报错 |
| importer 假成功 | **禁止**：success 必对应真实产物（cook 门禁） |
| 依赖循环 | 检测报错 |

**禁止**：忙轮询等待；importer 未产出真实产物却报 success；无真持久的 GUID。

## 8. 性能目标

- 异步加载零主线程阻塞；完成帧同步派发。
- DDC 命中跳过 cook（团队增量构建快）。
- mmap 加载近零拷贝；GPU 上传走预算（与 streaming/residency 联动）。

## 9. 测试计划

- **单元**：GUID 稳定跨重启、依赖图、引用计数驱逐、版本迁移往返。
- **importer 门禁**：success 必产真实产物（注入 stub importer 应失败，防"假成功"回归）。
- **异步**：加载不阻塞主线程；完成回调帧同步（非忙轮询）。
- **DDC**：内容寻址命中正确、跨机一致。

## 10. 开放问题 / Spike

- 运行时格式 spec（各资产类型字节布局）——子文档化。
- DDC 后端（本地 + 云/共享）选型。
- meshlet/LOD 构建工具（与 Geometry 协调）。
- 大世界资产数据库规模（百万资产）索引。

## 11. 依赖与被依赖

- **依赖**：Foundation（Jobs/Memory/Hash/Compress/VFS）、Reflect（序列化/版本）、RHI（GPU 上传）、ShaderCompiler（着色器 cook）。
- **被依赖**：streaming、所有消费资产的子系统（Render/Audio/Physics…）、Editor（AssetBrowser）。


