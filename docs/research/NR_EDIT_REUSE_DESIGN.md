# 隔帧复用 NR 编辑结果：实施设计

设计基线：2026-11-12，NR Nexus v0.4.0，源码 `197a9982d932c1c3847f1374c3ce23d5c20734a2`；宿主与依赖以 [host/lock.json](../../host/lock.json) 及兼容补丁为准。本页规定实验顺序和实现边界，不记录逐轮进度，也不表示功能已经实现或发布。

## 目标与推荐路线

部分基础帧跳过 NR 推理，将经过运动补偿的编辑应用于游戏的新画面；游戏 SR 继续每帧执行。目标是降低 GPU 工作量，同时保持转镜、动画和帧时间稳定。平均 FPS 提升若伴随明显微卡、拖影或明暗跳变，不作为成功。

先验证模型历史，再实现最小缓存：固定 x2、单 Pass、固定尺寸、原生 D3D12 Upscaled 单路径，FG 关闭。x2 表示正常连续输入中运行一帧 NR、最多复用一帧；首帧、Reset、源变化和失效仍需刷新。先选择已有真实 SR→NR 离线夹具覆盖的模型请求与色彩路径，不预设兼容所有 Runtime 或模型。

首版不建设 GPU 全局失效反馈控制器、异步计算、长间隔／自适应 cadence、多 Pass、完整玩家 UI 或全 API 验收矩阵。历史实验成立后尽快形成可对比的最小候选，定向实机判断体验，不先铺开扩展能力。

## 现有基础与新增责任

| 现有能力／入口 | 本方案复用与补充 |
| --- | --- |
| [look_stage.hpp](../../look_stage.hpp)、[look_common.hlsli](../../shaders/look_common.hlsli)、[look_compose](../../shaders/look_compose.cs_5_1.hlsl) | 已有对数编辑、Detail stability、运动重投影与上采样。现有过滤依赖当帧 NR 输出，不会节省推理；复用帧没有当前 NR 邻域，不能直接照用原裁剪逻辑。 |
| [dlssnr.hpp](../../dlssnr.hpp)：`NrFeatureSlot`、`SetEvaluationParameters`、`SetModelParameters`、`RecordInline`、`RunLookStage` | 已有独立 Feature／History、参数快照、Reset 和执行链。新增责任是区分上一基础帧与上一次实际推理帧。 |
| [evaluate_chain.hpp](../../evaluate_chain.hpp)、[submission_tracker.hpp](../../submission_tracker.hpp) | 复用真实源调用去重及录制／提交／完成／退休合同，不按 Present 时钟推进 cadence，不另造队列管理器。 |
| [adaptive_resolution.hpp](../../adaptive_resolution.hpp)、[fg_native.hpp](../../fg_native.hpp)、[fg_xefg.hpp](../../fg_xefg.hpp) | 已有动态尺寸、FG 颜色角色和应用帧缓存；属于后续组合接线，不是本轮缺失能力。 |
| [gpu_timers.hpp](../../gpu_timers.hpp)、[gpu_comparison.hpp](../../gpu_comparison.hpp)、[离线入口](../../tools/offline/README.md#controlmask-运动衰减) | 复用真实 SR→NR、Shader、Fence、读回和 GPU A/B；仅扩本实验需要的 case 与分类。 |

## 阶段一：只验证历史可行性

### 先核实实际 wrapper 契约

沿真实 SR→生产 NR wrapper 核对最终传入参数和资源：Color／Output、工作尺寸、MV／Depth 的网格及 Subrect、MV scale／方向、jitter 是否存在及是否已含于 MV、Reset、曝光与模型请求。记录实际 Runtime、驱动、GPU、资源格式及完成证明。

区分公开契约、当前 wrapper 行为和社区经验；不能因 setter 接受某个键或 Evaluate 返回成功，就假定闭源 NR 模型支持不连续调用或已正确消费组合 MV。不新增猜测的时间步长参数，也不为实验改动游戏 SR 的正常历史。

### 三个同输入条件

| 条件 | SR | NR |
| --- | --- | --- |
| 逐帧基线 | 每个输入帧执行 | 每帧执行，使用正常单帧 MV／Reset |
| x2 组合 MV | 每个输入帧执行 | 第 0、2、4…帧执行，提交到上一次真实 NR 帧的组合运动 |
| x2 Reset | 每个输入帧执行 | 相同刷新帧执行，每次刷新显式 Reset，作为对照 |

各条件使用独立 SR／NR Feature 与独立 History，来自相同确定性输入序列和相同起始 Reset／预热前缀；不在同一 Feature 上切换策略后直接比较。确认每帧送入 NR 编码链的 SR 输出一致，先检查无缓存的真实刷新帧输出。本阶段不实现显示端编辑缓存，空档没有 NR 输出就明确记未执行。

组合运动采用同一坐标系中的当前到前帧位移。两步关系为 `d(t→t-2,x) = d(t→t-1,x) + d(t-1→t-2,x+d(t→t-1,x))`，不是把当前 MV 乘二。保存跳过帧的运动；组合只服务 NR／对应 Look 历史，当前帧速度等消费者仍用单帧 MV。jitter 差值按实际坐标契约处理，不照搬其他游戏的符号。

### 编码边界必须显式

所选 v6 路径中，[v6_encode](../../shaders/v6_encode.cs_5_1.hlsl) 将代理写成 sRGB 编码值，即使资源格式是 RGBA16F 也不是线性颜色；`ReadEdit` 对代理和 NR 输出执行一次 `SrgbDecode`，`look_compose` 将整形后的结果用 `SrgbEncode` 写回，之后由既有 resolve 读取。

实验记录每个比较平面的编码。数值参考可在线性域计算，但写入既有代理／Neural／Look 消费位置前须满足该位置的编码契约，避免把线性结果再解码一次。第一阶段不修改 codec；后续虚拟 NR 结果也沿用同一边界。

### 最小验证与继续条件

先用静止／已知平移、遮挡显露、光照变化的短序列检查刷新帧质量，再补实际契约需要的 jitter／scale／Subrect 用例。复用现有夹具，优先只扩测试；不展开模型×API×Stage×Pass 矩阵。

分别报告参数请求、API 返回、GPU 完成、输出与 GPU 耗时。先以正常路径重复运行估计基线差异，再比较 x2；数值差异不是自动判定画质失败，API 成功也不是时域质量通过。若两种 x2 策略都出现明显刷新闪烁、漂移，或必要 wrapper 契约无法表达，则记录具体阻塞并停止缓存实现；不靠无界重试或新增机制强行通过。历史实验成立后选有证据支持的策略，不预先宣布组合 MV 或 Reset 为产品默认。

## 阶段二：最小编辑缓存

在成立的单路径上默认关闭地接入：刷新帧保持现有 `Encode → NR → Look → Upsample／Resolve／Commit`；复用帧保留当前帧 Encode，用重投影编辑构造当前代理结果，再走必要的 transport／resolve。先不追求省掉全部 codec 工作。

- 缓存刷新帧的编辑及必要参考，而非旧整帧。推荐在代理线性域保存带暗部 floor 的对数编辑，具体数学由 Nexus 现有实现独立推导。复用结果不再写成新锚点，最多借用一帧。
- 以边界、深度和当前颜色验证历史；无效像素保留当前游戏原色，不做空间补洞。不能仅令编辑为零就假定所有 codec、特别是低分辨率 SDR，都会精确还原原图；需验证最终选择与 alpha。
- 缓存跟随稳定的 Feature／Stage／颜色流，不绑轮换 backbuffer；复用既有资源跟踪与退休。同队列的已证明先行依赖可用，不为每帧复用强制 CPU 等 Fence。
- 参数快照、尺寸／格式／源改变、明确 Reset 或运动断档使缓存失效。复用分支不得消耗未发送的 `pending_reset`，也不能伪造新的模型历史样本。缺可信 guides 时回现有逐帧路径；动态 ControlMask 组合首轮保持逐帧。
- Detail stability 的历史位置必须对齐上次真正更新的帧；复用不重复施加 Look。其按 Evaluate 次数的权重是否需要换算，由短序列决定；不同时添加新的时域平滑器。
- 只加入必要启用入口及推理／复用／回退诊断；成功复用可有有效输出，但实际 Evaluate 数为零，不借用命令列表 `REPLAYED` 表示编辑复用。GPU 局部无效选择足够，首版不加读回驱动的全局退出／恢复控制器。

定向 CPU／WARP 检查中性与已知编辑、运动方向、失效回原色、参数变化及生命周期；NATIVE_GPU 测实际成本，NATIVE_NGX 比较完整序列。功能关闭保持现有路径。只对实际修改增加必要构建与测试。

## 阶段三：尽早做有限体验对照

最小候选通过必要离线检查后，在明确游戏／部署授权与桌面空闲条件下，尽早选一个已支持场景对比慢速转镜、人物／植被、遮挡显露、镜头切换和光照变化。对照包含逐帧原尺寸 NR、x2、较低工作分辨率的逐帧 NR。基础帧率较低时 x2 的编辑更新更慢，必须观察明暗跳变，不能只看插帧后的 FPS。

需要覆盖常用 FG 时优先 XeFG 单 Backbuffer，按真实应用帧推进，MFG 重复调用不增加缓存年龄；随后才考虑双颜色。一次有针对性的实机比较决定是否继续扩展，不先补齐全部介入点或完整 UI。静态、替身、硬件、真实 NGX、游戏和人工视觉结论分别记录。

### XeFG HDR10/PQ 颜色边界

10位 HDR10 只在 XeFG 单 Backbuffer 路径接入：实际输出须为 `R10G10B10A2_UNORM`，Present 契约为 PQ、BT.2020、10000 nit 绝对范围且与 codec 的 Encoding=4 一致。不能用系统 HDR 状态或用户配置值代替实际颜色契约。缓存身份包含实际输出格式、HDR模式和编码，并沿用现有白位 divisor／恢复配置 epoch；任一合同变化都使旧锚点失效。

复用 Nexus 现有 PQ→线性 BT.709→sRGB 模型代理→线性结果→BT.2020/PQ 写回链。编辑缓存仍保存代理解码域的对数编辑；复用帧写回 Neural 的值必须重新编码为 sRGB，不能直接写线性或 PQ。当前帧 alpha 沿现有 resolve／commit 保留。暗部、遮挡、颜色变化、中性编辑等无效像素在 commit 后直接选当前原始编码 RGBA；完整复用失败则复制当前原图，并重置下一次真实 NR 历史。

定向验证须包含非中性有效编辑的完整生产链、全部10位码值与四种2位 alpha 的原色选择、暗部／遮挡回退及颜色合同变化。原生 SR→NR 对照可在夹具侧把同一真实 SR 输出转换为确定性的 PQ 输入；原始 SR 与实际 PQ source 分别记录身份，夹具转换不计入生产 NR 链成本。CPU数值检查、WARP替身、真实NR与实际XeFG分别验收，不以单纯原色回退替代有效编辑验证。

## 成本、取舍与后续扩展

若现有逐帧整链成本为 C，刷新新增成本为 A，复用帧整链成本为 R，x2 平均为 `(C+A+R)/2`；刷新尖峰仍为 `C+A`。必须测整链与基础帧时间分布、刷新尖峰、实际输出节奏及显存分配，不能把调用减半写成 FPS 翻倍。GPU 采样标记帧种类，避免 250ms 抽样与 x2 相位锁定后只测一类帧；复用现有 profiling，不新增监控平台。

额外显存包括编辑、参考／深度、运动快照／组合结果、有效性和必要在飞版本；按真实尺寸与 allocation 记录，不提前假设可以复用仍在消费的表面。若平均收益被额外工作抵消，或明显恶化转镜、P95／P99 与画质，保留逐帧方案并停止扩展。

后续按实际需求推进：常用 FG 路径、已有自适应尺寸协同、同组多 Pass、Render／其他 API。动态尺寸切换需重建缓存并隔离控制器观察窗；多 Pass 首先整组同步 cadence；Render 须单独处理 jitter；双颜色须整组一致交付。异步计算、长间隔、GPU 全局反馈、指纹／补洞和完整 UI 都不是前述阶段的前置工作。

## 社区证据与许可边界

2026-11-12研究核对的版本：

| 来源 | 可核实机制与借鉴范围 |
| --- | --- |
| Skynizz/optiscaler-dlss5，`main@fc702f8ca22f97b01bd6a02f3386bffb3484fc35` | [设计](https://github.com/Skynizz/optiscaler-dlss5/blob/fc702f8ca22f97b01bd6a02f3386bffb3484fc35/OptiScaler/dlssnr/design/edit-cache.md)、[实际缓存分支](https://github.com/Skynizz/optiscaler-dlss5/blob/fc702f8ca22f97b01bd6a02f3386bffb3484fc35/OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp#L4265)：对数编辑重投影；缓存帧跳过 encode／model／resolve，仍有缓存 GPU 工作；另有跨推理运动累积与 Reset 选择。借鉴数据流，不照搬长间隔与异步系统。 |
| SAOG0721/Magpie，`experimental@a897acd5bef800c4df9440da4b3b5129aec3f052` | [Sparse 实现](https://github.com/SAOG0721/Magpie/blob/a897acd5bef800c4df9440da4b3b5129aec3f052/src/Magpie.Core/DLSSNRSparse.inl#L1)、[合成 Shader](https://github.com/SAOG0721/Magpie/blob/a897acd5bef800c4df9440da4b3b5129aec3f052/src/Magpie.Core/DLSSNRSparseShader.h#L29)：每2–4捕获帧异步推理，组合光流，将加法残差应用当前捕获，失效处保留原色；相同目标可沿用输出。它的捕获光流／零深度指导不同于游戏 MV／深度；Sparse 拒绝 experimental HDR，[自动分辨率也互斥](https://github.com/SAOG0721/Magpie/blob/a897acd5bef800c4df9440da4b3b5129aec3f052/src/Magpie.Core/DLSSNRMultiPass.h#L35)。 |

Prism 主仓是管理器，不计作独立算法。OptiScaler 的 [Witcher 3＋MFG issue #2](https://github.com/Skynizz/optiscaler-dlss5/issues/2)在研究时仍开放，报告慢转镜微卡且关闭缓存后消失；尚无确定因果或已修复结论。作者单场景性能和估算只归原场景，不作为 Nexus 收益承诺。

上述两个实现为 GPL-3.0。Nexus 保持 MIT：参考行为、数学和数据流，基于本项目独立实现；不复制、翻译改写 GPL C++／Shader／测试后标为 MIT。直接复用须另行解决授权或分发许可策略，并核对具体文件来源与声明。此设计未引入第三方代码、模型或资源。
