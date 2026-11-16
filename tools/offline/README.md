# NR Nexus 离线验证工具

独立命令行验证入口，复用现有 CPU、D3D12、Shader、提交与资源合同，不需要游戏、
窗口、ReShade 菜单、Steam、OCR 或快捷键。生产算法、配置键与产品功能保持原实现。

## 环境与构建

沿用[项目构建前提](../../README.md#zh-build)中的固定宿主、MSVC 与 Windows SDK。
除隔帧历史／缓存分析器使用 NumPy／Pillow 外，Python 工具仅用标准库。`gpu` 后端复用既有生成 Shader 头和 embed_file 工具，并在
每个隔离构建目录重新用 FXC 编译当前 ControlMask Shader，通过预包含头绑定该字节码；
不改共享生成目录。`replay` 仅复用通用 D3D12 测试助手，不生成或包含 ControlMask Shader。
NGX 探针只用已有 SDK 头，不链接或打包第三方 Runtime。

~~~powershell
pwsh -File tools/offline/build.ps1 -Backend gpu -OutputDirectory tmp/offline/gpu-build
pwsh -File tools/offline/build.ps1 -Backend replay -OutputDirectory tmp/offline/replay-build
pwsh -File tools/offline/build.ps1 -Backend ngx -OutputDirectory tmp/offline/ngx-build
~~~

HostRoot、ShaderIncludeDirectory、Fxc、EmbedFile 参数可显式指定已有输入。
脚本复用 tests/test-support.ps1，记录实际编译依赖、源码与 EXE 的 SHA-256、固定宿主、
编译器和 SDK 身份。Standard／Full／Acceptance 都会清理登记的 obj/lib/exp；离线运行器在统计完成后清理 GPU 原始读回，失败时留下异常样本。留存模式不提供永久保留豁免。构建不是运行通过。
构建／运行目录使用本仓库 tmp/ 或 artifacts/ 下的新目录，不覆盖历史。

入口自动维护已登记的结束运行，tmp/ 与 artifacts/ 共享默认 10 GiB 容量预算，可用
`NR_NEXUS_TEST_BUDGET_BYTES` 指定字节数，设为 0 只关闭额外容量收缩，正常收尾仍然执行。超预算时按运行时间收缩旧目录，
不依赖逐文件登记：保留报告、复现配置、关键日志、输入和异常样本，回收截图、原始读回、
其他大体积中间产物。backup/protected 恢复资料由专门恢复流程释放。仍在运行的 owner、嵌套活动任务和离线构建输入受到保护；
保留资料本身超过预算时不会为满足数字而删除它们。

专项确需完整原始数据集时，在启动前显式设置 `$env:NR_NEXUS_TEST_KEEP_RAW='1'`。
工具将选择写入本次生命周期标记，后续维护继续尊重该选择；Full／Acceptance 本身不会开启它。

## 本地发布预检

~~~powershell
./tools/run-release-preflight.ps1 -Mode Software -OutputDirectory tmp/release-preflight/software
~~~

软件入口先原样执行 `Suite all -SoftwareOnly`（包括默认五组 FG），再复用编译产物执行
18 个独立 FG 阶段、分辨率、生命周期、pre-FG 及隔帧复用入口和 4 个 Bridge 分辨率入口。
`result.json` 逐项记录 Passed、Failed 或 NotRun；Vulkan 在软件模式只编译，执行记为 NotRun。
这些是 CPU／WARP 与模型替身证据，包含有限的隐藏窗口夹具，不提供游戏或动态视觉验收。

已有完整 `all` 结果可通过 `-ReuseAllOutputDirectory` 复用，入口校验每项实际编译输入；
仅 FG 夹具有修正时可加 `-RefreshFg` 重编并重跑默认五组，其他结果仍须通过输入校验。
显式 `run-tests -NativeCase` 会替换默认 FG 组，因此不能用单个专项结果充当默认组已通过。

原生检查使用独立的本地分支，需先取得空闲设备窗口：`-Mode NativeGpu` 运行现有
`native-full-hardware`、`native-prefg-resolution-hardware` 和 `present-chain-hardware`
（真实 D3D12、模型替身，一次编译后串行执行）；`-Mode NativeNgx` 运行
`control-mask-native-ngx-fg`，并须提供 `NativeNgxProjectIdPath`、
`NativeNgxSdkLibraryDirectory`、`NativeNgxRuntimePath`。后者实际执行 NGX
Init／Create／Evaluate；`ngx_probe` 仅检查能力，不能替代此结果。缺依赖或未获窗口时记为 NotRun。
原生分支不重跑软件默认组，也不加入 GitHub CI。

入口要求默认 FG 的 Debug Layer／InfoQueue 检查存在；部分独立夹具的检查范围仍较窄，
详见结果中的 `limits`。统一入口固定复用已有用例，名称不表示所有格式、初始相位和平台组合均已覆盖。

## 阴影调整复用核验

~~~powershell
& ./tests/run-look-shadow-tests.ps1 -Adapter @('warp','nvidia')
~~~

该入口复用 `tests/d3d12_test_gpu.hpp` 和生产 `look_stage.hpp`，从当前源码编译并执行
`look_compose`，比较阴影 1／0.5／0。包含暗—中—亮、floor 过渡、提亮／压暗／偏色及分通道
floor 的 60 个合成输入；采用生产 RGBA16F 存储，CPU float32 参考允许最多 1 个 half ULP。
阴影 1 沿生产中性设置旁路并精确复制 neural，其余两档真实派发，提交及 Fence 完成后读回。
默认只运行 WARP；指定 `-Adapter nvidia` 或两者才使用 NVIDIA GPU，无硬件时不降级。
Python 只需标准库，可用 `-PythonExecutable` 指定；编译器及固定宿主前提与其他定向测试相同。

输出保留 fixture、逐项比较、Shader／编译输入身份、后端／驱动及完成凭据，编译中间文件自动收尾。
`tools/look/shadow_reference.py selftest` 与 `tests/test_look_shadow_reference.py` 可独立验证参考数学和
比较器。神经结果为合成输入：WARP／NATIVE_GPU 只证明结果编辑的响应，不记 NATIVE_NGX、
完整 codec 输出、动态稳定性或游戏视觉通过。恢复参考为送入模型的 proxy；floor 及逐通道
下限附近的残留沿生产保护保留。

## 低档编辑量重建核验

~~~powershell
& ./tests/run-look-transport-tests.ps1
~~~

该定向入口复用 D3D12 软件夹具和生产 Look 根签名，编译、派发当前 `look_upsample`。
只使用 WARP，要求 Debug Layer 和 InfoQueue 可用；检查 SDR 输入采样与亮度指引一致、
无匹配输入时保留原图、正常匹配区的颜色编辑，以及 HDR 原路径和 tile 边界。
可用 `-BaselineShaderPath` 加入旧 shader 负对照；它的预期失败不计为修后通过。

可选 `-RoiFixture` 接受保留完整坐标的原码裁剪数据及 CPU 预期输出，按原格式重新量化，
在原尺寸纹理上执行后只比较已覆盖的 ROI。缺少预期输出时只记数值捕获，不能计为像素回归通过。
原码读回文件在比较和哈希登记后自动收尾；确需继续离线分析时，显式设置 `NR_NEXUS_TEST_KEEP_RAW=1`。
真实游戏数据留本地；WARP 数值一致不代替 NGX、游戏后处理或动态视觉验收。

## Fixture、合成序列与原始数据

nr-nexus-offline-fixture/1 的 ControlMask case 规范化及验证以 control_mask_contracts.py 为准；
contracts.py 维护通用 JSON、数值、文件身份与 Color／Depth／Motion 序列契约；ControlMask 的 Fixture schema 与 CLI 保持原版本和参数。
顶层为 schema、fixture_id、可选 description 和 cases；case 必须有
id、width、height，其他默认如下。未知字段、重复 ID（含 Windows 大小写冲突）、
重复 JSON 键、非有限值及不支持的 schema 均拒绝。

| 字段 | 默认／允许值 |
| --- | --- |
| mode | neutral；也支持 rgb、rect、green_rect、blue_rect |
| rgb | [1,1,1]，各分量 0–1；Alpha 固定 1 |
| rect | [0,0]，左上半开矩形；GRect/BRect 使用尺寸向下取半、忽略 RGB/rect |
| model | A；支持 B，仅验证实际生产参数 setter 的创建请求 |
| auto_mask、blend_mode | false、0；BlendMode 允许 0–3 |
| frames、warmup | 8、2；帧数 1–4096，预热必须小于总帧数 |

~~~powershell
python tools/offline/run.py validate tests/fixtures/offline/control-mask-smoke.json
python tools/offline/run.py matrix --output tmp/offline/matrix.json
python tools/offline/run.py prepare --output tmp/offline/sequence --width 64 --height 36 --frames 8 --seed 7
python tools/offline/run.py prepare --output tmp/offline/resize-sequence --width 64 --height 36 --frames 8 --resize-cycle 64x36 96x54 128x72 --subrect 8,4,40,24
python tools/offline/run.py verify-sequence tmp/offline/sequence/inputs
~~~

矩阵覆盖 96 个 case：模型 A/B × AutoMask × 4 种 BlendMode × 5 种 mask，另含薄纹理、
线程组边界及 R/G/B 对照。matrix --frames 1008 --warmup 8 可声明较长采样；是否适合
正式性能比较还取决于测量 scope、设备、负载与输入身份。

真实尺寸 ControlMask Shader-only Fixture：

~~~powershell
pwsh -File tools/offline/build.ps1 -Backend gpu -OutputDirectory tmp/offline/gpu-build
python tools/offline/run.py run --fixture tests/fixtures/offline/control-mask-raster-performance.json --backend-exe tmp/offline/gpu-build/offline_gpu_tests.exe --adapter nvidia --performance --timeout 300 --output tmp/offline/raster-nvidia
~~~

`--performance` 在同一生产 Dispatch helper 和同一 ControlMask Shader 下批量记录 D3D12 timestamp，稳态样本各自在最后一次 readback／Fence 后核实。Scope 明确为 Shader Dispatch 加其 UAV Barrier；结果另记设备／队列和生产管线初始化、每 case 资源／descriptor／query 分配估计、CPU 资源准备／命令录制／case wall time，以及可用时 `nvidia-smi` 前后时钟／负载快照。D3D12 timestamp frequency 是队列 tick 频率，不是核心频率；没有 GPU clock lock。WARP 只作功能回归，不将其运行时间作为硬件性能结论。该批处理用于重复采样而复用生产 Dispatch 和 Barrier，不代表产品 NR 多 Pass 调度；不包含 NR Evaluate、NGX、FG、输入生成、游戏或端到端成本。

合成序列保存 little-endian 原始 Color R32G32B32A32_FLOAT、Depth R32_FLOAT、
Motion R32G32_FLOAT，逐帧明确 Reset、Jitter、Motion Scale、Subrect、格式、长度与
SHA-256。当前 moving-ramp/1 为周期水平平移、普通深度、零 jitter、首帧 reset；这是
可重建的 CPU Fixture，可由 E0 后端上传到真实 D3D12 资源并完成 GPU copy／读回；
生成阶段本身仅为 STATIC，GPU 传输验证不执行 NGX History。sequence/2 支持逐帧 Width／Height、资源重建及变化的 Subrect 边界；sequence/1 固定尺寸仍可读取。
生成数据有 256 MiB 预算，不纳入 Git。

## E0：float32 GPU 输入回放

使用上面的 `-Backend replay` 构建得到 `offline_replay_tests.exe`。该目标与
`tests/fg_xefg_tests.cpp` 共用 `tests/d3d12_test_gpu.hpp` 中的设备／Adapter、Queue／Command List、资源上传／读回、Barrier、Fence 及通用 timestamp 助手；
不包含 ControlMask GPU harness，也不需要编译 ControlMask HLSL、FXC 或 embed_file。可直接运行或交由
统一 runner 校验。`--sequence` 指向包含 `sequence.json` 和原始文件的目录，而非 JSON
文件本身；`prepare` 示例对应 `tmp/offline/sequence/inputs`。

~~~powershell
tmp/offline/replay-build/offline_replay_tests.exe --sequence tmp/offline/sequence/inputs --output tmp/offline/replay-direct-warp --adapter warp
python tools/offline/run.py replay --sequence tmp/offline/sequence/inputs --backend-exe tmp/offline/replay-build/offline_replay_tests.exe --adapter nvidia --output tmp/offline/replay-nvidia
~~~

可显式选择 `warp` 或 `nvidia`；NVIDIA 不可用时失败，不回退 WARP。每帧分别消费
Color `R32G32B32A32_FLOAT`、Depth `R32_FLOAT`、Motion `R32G32_FLOAT`，按实际
row pitch 完成 upload → 输入纹理 → 第二张 GPU 纹理 copy → readback，再导出紧排的
float32 原始文件并逐字节核对。完成证据使用真实提交／Fence，逐帧完成序号递增；
帧级证据为 WARP 或 NATIVE_GPU，不包含 NGX 执行或游戏画质结论。

后端输出 `replay-backend-result.json`，schema 为 `nr-nexus-offline-replay-backend/1`，
记录逐帧 resources 的格式、字节数、output_file、bitwise_equal 和 GPU completion。
controls 中 Reset、Jitter、Motion Scale、Subrect 仅转录输入，标记 STATIC、
`history_evaluated=false`；不会应用这些值、调用 SDK 或重建 NGX History。

`run.py replay` 另行核验构建身份和输入 SHA-256，运行后再次核对输入，并独立检查
全部读回资源与完成序号。统一输出为 `result.json`，原始后端结果与 float32 文件
记录在 `backend/`；统计核验后按生命周期收尾，删除记录可在 `artifact-lifecycle.json` 查看。直接执行 EXE 不替代 runner 的输入 SHA-256 校验。
后续帧输入损坏时，后端在运行期间仍保留此前已完成帧的原始读回和 Fence 记录；失败帧保持
`status=failed`、`gpu_completed=false`，整个结果为 failed，不覆盖已有输出。三种模式收尾后均保留报告、Fence／统计和一个异常样本；需要完整读回时显式设置 `NR_NEXUS_TEST_KEEP_RAW=1`。

## 既有 FG／XeFG／生命周期套件统一入口

用版本化 Fixture 将现有生产测试入口接入统一结果，不实现第二套 GPU 调度：

~~~powershell
python tools/offline/suites.py tests/fixtures/offline/fg-contracts.json --output tmp/offline/fg-contracts-run
~~~

Runner 调用 `tools/run-tests.ps1` 一次构建并运行 Fixture 指定的 `fg-native`、
`fg-xefg`、`lifecycle` 用例，GPU case 仍由原测试二进制执行。`result.json` 逐 case
核对原始日志／生命周期退出码和 suite 的 `result.json`，记录原始产物 SHA-256、
Fixture 身份及执行前后源码身份；源码变化、缺失产物、错误 suite 结果、非零退出或
超时均为失败，日志与部分产物保留。测试 suite 的输出放在本次报告目录下独立的
`test-run/`，避免覆盖保护拒绝非空目录。

Windows 上 Runner 将既有测试入口放入独立 Job Object；超时会终止并核对本次 Job
中的进程树，再收集 stdout／stderr、部分 suite 产物和失败报告。Job 创建／分配、进程树
回收或管道关闭无法核实都会保留失败状态，不把根进程退出误记作整个进程树已停止。

版本化 Fixture 当前登记 17 项：同 Stage、跨 Stage、Render、Present 双 Pass 的同尺寸／独立尺寸及 WARP／NVIDIA 用例，另含 A/B 模型请求、Feature Switch、Present Chain、XeFG 资源合同与 DLL 生命周期。报告分别列出 STATIC、MOCK、WARP、NATIVE_GPU、NATIVE_NGX、IN_GAME、VISUAL。当前
Fixture 中 FG 模型／SDK 为替身：`same-stage-model-pair`、`same-stage-mixed-pair`、
`present-chain` 与 `feature-switch` 的资源／Shader／提交证据按 WARP 或硬件 case
分别标记，参数与模型请求标 MOCK；XeFG resource lifecycle 是 WARP＋fake SDK MOCK，
DLL 生命周期为 MOCK。NVIDIA GPU 资源通过不表示 NVIDIA NGX／XeFG SDK 已运行。

## 真实 Shader 与参数矩阵

~~~powershell
python tools/offline/run.py run --fixture tests/fixtures/offline/control-mask-smoke.json --backend-exe tmp/offline/gpu-build/offline_gpu_tests.exe --adapter warp --output tmp/offline/smoke-warp
python tools/offline/run.py run --matrix --backend-exe tmp/offline/gpu-build/offline_gpu_tests.exe --adapter nvidia --output tmp/offline/matrix-nvidia
~~~

设备选择明确，NVIDIA 不可用时失败，不回退 WARP。每帧执行生产 mask Shader、真实
Barrier、提交／Fence、GPU timestamp 与读回；最后一帧保存紧凑 RGBA8，所有帧逐分量
检查。FLOAT→UNORM 分数值遵循 D3D12 的 0.6 ULP 转换容差，0/1、Alpha和矩形边界严格
验证；精确参考字节差异仍保留，不把合法量化差异写成 bitwise identical。
Python 再独立核验最后一帧、全部原始 timestamp、完成序号与参数记录。
run-start.json 只有开始凭据，只有完整 result.json 且 status=passed 才表示本次
声明范围通过。超时会终止并等待本次独立后端，保留 failed 结果／日志；没有自动重试。

结果 schema 为 nr-nexus-offline-result/1，原生后端为 nr-nexus-offline-backend/1：

- 每个 case 的 checks 分开标记 Shader／完成／计时的 WARP 或 NATIVE_GPU，
  以及模型请求／AutoMask／BlendMode 的 MOCK。参数写入通过不是 NVIDIA 网络执行。
- build／fixture_sha256／runner_inputs 绑定实际源码、工具、依赖与可执行文件，runner/ 冻结本次 Python 源文件；
  运行前后核对。environment 记录真实适配器、LUID、驱动查询、debug layer 可用性，
  backend-result.json 保留原始 tick、频率、GPU 毫秒、预热和完成序号。
- statistics 对真实有效稳态样本计算均值、范围、最近秩 P50/P95。scope 仅为
  control-mask-dispatch，不含 NR Evaluate，不用于推断游戏帧率或网络性能。
- not_evaluated 明确保留 NATIVE_NGX、IN_GAME、VISUAL。实际内部模型、网络对
  mask 的因果、完整 History 回放、真实 FG SDK、动态拖影／闪烁不是本入口的通过项。

已有失败注入、mask 生命周期、尺寸退休和失败保护由相同二进制复用，不重写：

~~~powershell
tmp/offline/gpu-build/offline_gpu_tests.exe --contracts --adapter warp --output tmp/offline/contracts
~~~

此入口返回独立 contracts 结果（MOCK＋WARP），不计入 96-case Shader 矩阵。已有
tools/run-tests.ps1 的 fg-native suite 可定向执行 gpu-pass-diagnostics、control-mask-capture
合同回归，不需要游戏。

## NGX 可行性与输出对照

~~~powershell
tmp/offline/ngx-build/offline_ngx_tests.exe --runtime-dir <已有本地Runtime目录> --output tmp/offline/ngx-probe
python tools/offline/run.py compare <原始输出1> <原始输出2> --storage f32le --data-range 1
~~~

探针详情、官方来源和阶段语义见 [ngx_probe.md](ngx_probe.md)。退出码 2 是保留证据的
blocked，不是 Feature18 PASS；不能仅凭设备创建、导出名或成功 API 返回宣称原生
Evaluate／GPU 完成。没有满足公开契约的原生路由时，其余离线能力仍可独立运行。

原始对照支持 u8／f32le、exact、MAE、RMSE、PSNR；拒绝不匹配长度与非有限分量。
data-range 必须按实际数据口径指定（例如 RGBA8 为 255）。相同图像的 PSNR 为 null、
identical=true，避免不可移植的 JSON Infinity。比较工具只测给定数组，不推定来源或
自动认定两个序列具有相同 History。

## ControlMask 运动衰减

`control-mask-safety`／`control-mask-safety-hardware` 复用生产生成器，读回运动阈值、
正负 scale、非零 Subrect、低／显示分辨率运动、非有限向量、无效窗口及独立规则回退。
其中生产 Render／Upscaled／Present／FG 输入角色还核对非零运动的 R 与缺少来源凭据时的手动值恢复。
`-Suite bridge-motion -BridgeCase sr` 核对 D3D11 RG16 直拷／RG32 转换后，经生产入口得到的运动 R/G/B/A；
这些链路使用真实 GPU，NR 为 MOCK。`vulkan-control-mask` 核对 Vulkan Shader 的运动采样与深度重叠；
`-VulkanNativeNgx` 另通过公开 SDK ProjectID provider 验证生产参数解析、非零偏移与 scale、
深度重叠、OFF→ON→OFF、重放和 reset。两种原生入口都显式提供已有 SDK、ProjectID 与 Runtime，
不回退到 snippet 身份桥；公开 provider 的失败须按实际 API 结果报告。

~~~powershell
pwsh -File tools/run-tests.ps1 -Suite fg-native -NativeCase control-mask-native-ngx-motion `
    -NativeNgxProjectIdPath $projectIdPath -NativeNgxSdkLibraryDirectory $sdkLibraryDirectory `
    -NativeNgxRuntimePath $runtimePath -UiPreview None

pwsh -File tools/run-tests.ps1 -Suite vulkan-control-mask -VulkanNativeNgx `
    -NativeNgxProjectIdPath $projectIdPath -NativeNgxSdkLibraryDirectory $sdkLibraryDirectory `
    -NativeNgxRuntimePath $runtimePath -UiPreview None
~~~

该用例通过生产 Create／Evaluate／Release 包装器调用真实 DLSS SR，
将 640×360 放大为 960×540 后交给生产 NR 执行链；SR 与 NR 均属于 NATIVE_NGX。
公开 SDK 使用 ProjectID 初始化设备，再将已初始化的 provider 接入生产 owned Feature 状态，
不执行 snippet 身份桥；游戏加载器及生产 Runtime 初始化不在此用例范围内。

同一组真实 SR／NR Feature 执行关闭／开启运动衰减的两段 8 帧合成序列，
各段相同 Reset 前缀，输入平移与低分辨率运动矢量一致，MV scale 为 (-2,1)。
固定帧测光、关闭亮度 governor；Detail stability 保持 Auto，最终结果包含该过滤。
在实际消费边界复制 SR 输出、NR proxy 和 raw neural，Fence 完成后检查两组 SR／proxy
逐字节相同、实际遮罩及 G/B/A、停止后的 R 恢复，以及 neural／最终输出响应；
完全运动衰减时另核对 neural RGB 等于实际 proxy。完成后核对 NR 关闭后的 SR 续跑、
真实 SR Release、SDK shutdown 和身份桥未安装。

`controlmask-motion.json` 保存逐帧位移、遮罩值、两级输出差、Reset、SR／NR 单次调用数、
Fence、provider 边界与实际 NGX 模块身份；原始读回仅在内存配对。GPU timestamp
包含 SR、NR 及消费边界读回，仅作诊断，不与旧 NR wrapper 计时比较，也不用于性能或游戏画质验收。
内部实际模型身份保持未知；动态实机由人工验收。

## NR 隔帧历史实验

`nr-edit-reuse-history` 用例用于检查 NR 不连续调用时的历史行为。固定 D3D12 Upscaled、
单 Pass、640×360 SR 输入及 960×540 NR，关闭 ControlMask 与 Detail stability。
它扩展既有真实 SR→NR 入口，每个条件启动独立进程及独立 SR／NR Feature：

- `baseline` 和 `baseline-repeat`：逐帧 SR 与 NR，重复运行估计基线差异。
- `x2-compose`：SR 每帧执行，NR 仅在偶数帧执行，使用当前到上一次实际 NR 帧的组合运动。
- `x2-reset`：相同 NR 刷新节奏，每次刷新将 NR Reset 设为 1。

~~~powershell
pwsh -File tools/run-tests.ps1 -Suite fg-native -NativeCase nr-edit-reuse-history `
    -NativeNgxProjectIdPath $projectIdPath -NativeNgxSdkLibraryDirectory $sdkLibraryDirectory `
    -NativeNgxRuntimePath $runtimePath -NativeNgxSrRuntimePath $srRuntimePath `
    -PythonExecutable $pythonWithNumpyAndPillow `
    -UiPreview None -OutputDirectory tmp/offline/nr-history
~~~

四组使用相同创建前缀及24帧输入，依次包含静止、变速平移、独立运动前景的遮挡显露、光照变化。
`$srRuntimePath` 指向已有的 `nvngx_dlss.dll`；D3D12 原生 NGX 入口可用该选项显式提供 SR 库。
Runner 仅在测试输出目录建立临时链接，记录输入 SHA-256，并在结束时移除链接。
若省略，SR 依赖本机 NGX 搜索路径；SR Feature 1 返回 `0xBAD0000B` 时先核对 SR 库是否可用。
当前夹具为零 jitter、未含 jitter 的渲染像素运动和单位 scale；不覆盖非零 jitter 或 Subrect。
组合运动从上一基础帧的对应位置取样，不是当前运动乘二。刷新沿生产 wrapper 调用真实 NR，
仅在测试观察器边界替换 NR 运动资源或 Reset；SR 参数和生产实现不改动。

每条件 JSON 保留实际 SR／NR 调用、参数、API 结果、GPU Fence、模块身份与独立 Evaluate 时间戳。
SR 输出与最终结果为线性 HDR；v6 proxy 与 raw neural 虽用 RGBA16F 存储，仍为 sRGB 编码值。
跳过帧只有 SR 读回，其他平面与 NR 时间明确为空，没有显示端缓存或复用输出。

统一入口在收尾前调用 `tools/offline/nr_edit_reuse.py --input <fg-native输出目录>`，
核对输入／SR／proxy 一致性，报告刷新帧差异、带运动补偿的编辑误差变化和重复基线；
生成固定显示变换的对照预览。分析器需要已有 NumPy 和 Pillow，不自动安装依赖。
可独立运行 `python -m unittest discover -s tests -p test_nr_edit_reuse_analysis.py` 检查比较数学与读回身份。
数值报告不自动判定视觉通过；Evaluate 时间不包含完整编码／合成／复用成本，不能换算为游戏 FPS 收益。
Standard 在分析后回收登记的原始平面，保留 JSON、日志、预览和配对失败样本；
完整原始集只由显式 `NR_NEXUS_TEST_KEEP_RAW=1` 保留。使用 GPU 前应避开其他游戏或性能测试窗口。

## NR x2 编辑缓存候选

`nr-edit-reuse-contracts` 在 WARP＋MOCK 上检查生产 Shader 的已知编辑、运动缩放、边界／深度／颜色拒绝、
中性及无效像素的精确原色／alpha；通过真实 wrapper 检查一次复用上限、Reset／设置变化／断档、
非零 jitter 的刷新／复用、超范围或缺轴回退、关闭、失败和丢弃录制的资源生命周期。
1.5 倍采样比例的独立像素检查区分两轴 offset 正确、遗漏和反号，并核对两步 MV 的上一基础帧位置。

~~~powershell
pwsh -File tools/run-tests.ps1 -Suite fg-native -NativeCase nr-edit-reuse-contracts `
    -UiPreview None -OutputDirectory tmp/offline/nr-cache-contracts
~~~

`nr-edit-reuse-cache` 对相同的24帧短序列启动独立逐帧／缓存进程；SR 始终每帧运行，NR 的跳过、
组合运动和最终原色回填由生产 wrapper 完成，测试观察器不替换模型输入。
两组使用缓存开启且每次 Reset 的相同创建前缀，正式第0帧均 Reset；基线此时关闭缓存。
当前条件为请求 B、原生 D3D12、固定100% HDR、单 Pass，关闭 Look、
Detail stability、ControlMask 和自适应分辨率。默认测试 Upscaled（FG 关闭）；
`NR_NEXUS_TEST_NR_CACHE_ROUTE=xefg-record` 选择 XeFG 单 Backbuffer 的生产 `Record` 与私有颜色写回。

~~~powershell
pwsh -File tools/run-tests.ps1 -Suite fg-native -NativeCase nr-edit-reuse-cache `
    -NativeNgxProjectIdPath $projectIdPath -NativeNgxSdkLibraryDirectory $sdkLibraryDirectory `
    -NativeNgxRuntimePath $runtimePath -NativeNgxSrRuntimePath $srRuntimePath `
    -PythonExecutable $pythonWithNumpyAndPillow `
    -UiPreview None -OutputDirectory tmp/offline/nr-cache
~~~

`cache-baseline/` 与 `cache/` 中的报告记录实际调用、锚点代次、复用／回退计数、显存分配及 GPU 完成。
`nr_edit_cache.py` 核对输入、SR及实际 NR source 逐字节一致、所有帧 alpha、复用无效像素与当前 source 完全相同，
并生成 `nr-edit-cache-analysis.json` 和24张对照预览。整链时间覆盖 Encode、真实 NR 或缓存工作、
Resolve、Commit 和最终回填；测试平面读回与查询结果拷贝在计时外。分别报告刷新、复用、整体均值及分位数，
不由调用减半推导 FPS。质量差异保留为测量，动态画质仍需人工验收。
Standard 收尾与上一实验相同；分析器定向测试为 `test_nr_edit_cache_analysis.py`。

XeFG 的 SDK hooks、Service 准入与生命周期通过 `-NativeCase nr-edit-reuse-xefg-contracts`
单独验证（WARP＋MOCK；SR、NR、XeFG 均为替身，不执行插帧）。它覆盖重复 present ID、
刷新／复用／刷新、当前 RGBA、Reset、缺 guides、忙碌跳帧、模型失败恢复和缓存退休，
并检查缓存不持有 DXGI Backbuffer。默认关闭的生产开关只在完整的单 Backbuffer 包上授权缓存，
按基础帧推进复用年龄；HUDless、UI 合成及其他组合沿用原有路径。
PQ 用例使用 `R32G8X24_TYPELESS` 深度资源，按第0平面的 float 深度上传和读取，并检查四字节 footprint；
scRGB 用例保留 `R32_FLOAT`。原有颜色、alpha、深度拒绝与失败恢复断言覆盖两种路径。

原生 `xefg-record` 对照仍使用真实 SR 和 NR，但不调用 XeFG SDK 或 Service 准入。
默认 `NR_NEXUS_TEST_NR_CACHE_FORMAT=scrgb` 使用 RGBA16F scRGB：在同一 command list 上从 SR 读回之后计时，
覆盖 `Record`、原色暂存和私有输出写回，避免跨 CPU 提交空隙污染 GPU 区间。固定白位200 nit；
MV flags/scale 已按 Service 后的 NGX 契约提供。两条件使用独立 Feature 和相同前缀，
报告记录 `route`、`xefg_scope` 及白位，分析器拒绝混用路径；不能由此推导实际插帧画质或游戏兼容性。

同一路径设置 `NR_NEXUS_TEST_NR_CACHE_FORMAT=pq` 选择实际 `R10G10B10A2_UNORM` HDR10 输入。
夹具在真实 SR 完成后把 FP16 BT.709 scRGB（80 nit/unit）转换为 BT.2020／PQ（10000 nit/unit），
按10位 RGB／2位 alpha 量化并上传；`sr` 与 `source` 分别保存，baseline／cache 的两份 source 必须逐字节一致。
该构造增加一次 SR 完成提交；NR 整链计时在 PQ 上传后开始，排除夹具的读回、CPU转换与上传，
继续包含生产原色暂存、缓存／模型、完整 codec 及私有写回。分析器按原码核对无效区域和 alpha，
再将 PQ 转回 BT.709 线性光计算差异及预览，不把 PQ 码值当线性亮度。PQ 仅供 `xefg-record`，
不扩展 Upscaled、其他 FG 或多 Pass。

默认序列保持零 jitter。验证非零 jitter 时，先用相同运行参数将 `-NativeCase` 改为
`nr-edit-reuse-jitter`，输出到新的隔离目录。它只执行九次真实 SR、每次 Reset，不调用 NR：
零 jitter 参考及两个光栅采样符号各四个双轴相位。`sr_jitter.py` 测量 R／G 边缘相对参考的位置，
在 `sr-jitter-analysis.json` 保存误差、符号结论及原始 SHA；结果不明确则停止后续带 jitter 的对照。
该校准建立夹具与实际 SR 参数的对应关系，不替代游戏 guides 契约检查。

校准明确后，将 `NR_NEXUS_TEST_NR_CACHE_SCENE` 设为 `jitter-lighting`，
`NR_NEXUS_TEST_NR_RASTER_SIGN` 设为报告中的 `raster_sampling_sign`，再运行一次 `nr-edit-reuse-cache`。
颜色、深度及运动所属表面的采样共享同一光栅偏移，MV 本身不含 jitter。
固定四个相位为 `(±0.375, ±0.25)` 渲染像素，前景边缘从第0帧存在；
末六帧光照为 `1, 1.15, 1.15, 1.15, 0.85, 0.85`，用于观察突变与下一次刷新后的恢复。
生产 guide 采样使用当前 jitter，组合 MV 使用上一基础帧与当前帧的 jitter 差；
最终编辑位移不加 jitter 差，NR 仍收到当前 API jitter。

分析器在静止背景区域比较 `log2(final luma / SR luma)`，分离光照本身与 NR 编辑消失／恢复；
`lighting_roi` 保存相对逐帧基线的编辑差及额外时间变化。`cache-lighting-strip.png` 为六帧对照，
`cache-lighting-preview.png` 是每帧250ms的慢速 APNG，仅用于定位变化，不模拟游戏播放节奏。
校准分析器的合成回归入口为 `test_sr_jitter_analysis.py`。

### 隔帧编辑复用原生成本

`nr-edit-reuse-cost` 固定真实 SR 输入 2560×1440、输出 3840×2160、RGBA16F scRGB、
单 Pass／请求 B、零 jitter／运动，并关闭自适应分辨率、ControlMask 和时域 Look。
它复用生产 `fg_service::Record` 的原色暂存、缓存／模型、codec 及私有颜色写回；
不调用 XeFG SDK、Service 准入或插帧。默认固定这一路线与格式，不读取其他缓存实验的路线／场景选项。

~~~powershell
pwsh -File tools/run-tests.ps1 -Suite fg-native -NativeCase nr-edit-reuse-cost `
    -NativeNgxProjectIdPath $projectIdPath -NativeNgxSdkLibraryDirectory $sdkLibraryDirectory `
    -NativeNgxRuntimePath $runtimePath -NativeNgxSrRuntimePath $srRuntimePath `
    -UiPreview None -OutputDirectory tmp/offline/nr-cache-cost
~~~

统一入口分别启动独立进程／Feature 的 `baseline`、`cache` 条件，无需 Python 图像分析器。
两组先使用缓存开启、逐帧 Reset 的相同创建前缀，再各执行 32 帧暖机及 128 帧计量。
SR 每帧真实执行；基线 NR 每帧执行，缓存组按刷新／复用交替执行 1／0 次 NR，
暖机首帧 Reset=1，后续刷新 Reset=0。计量期间抑制逐帧日志，不读取图像或 GPU query；
全部完成后才读取已写入的计时槽，复用帧的模型时间为 null。

`cost-baseline/nr-edit-reuse-cost.json` 与 `cost-cache/nr-edit-reuse-cost.json` 保留实际 GPU ticks／频率、
SR／模型／完整 NR 链 GPU 时间、CPU 录制／提交完成／相邻调用间隔、真实调用与复用计数、
Reset、Fence、稳定模型／工作集身份及完整收尾。测量结束只读回一次最终输出，检查有限且非零；
缓存组再读取末帧 Validity，记录 0／1 像素数并要求存在有效复用像素，均不保存全图。
失败保留状态及错误 JSON。相邻调用间隔包含夹具等待与管理开销，结果用于静态成本比较，
不作为游戏／显示 FPS、动态画质或真实 XeFG 帧节奏验收。

设置测试进程环境变量 `NR_NEXUS_TEST_NR_CACHE_TWO_TIERS=1` 后，同一入口改为内部两档实验：
原生 D3D12 Upscaled／FG Off、100% 与 75% 两个完整预备档，其余输入、格式和单 Pass 保持固定。
两组独立进程先用相同设置准备真实 NR Feature／历史，再执行上档→低档→上档；
低档单独运行 32 帧暖机和 128 帧计量，比较逐帧 NR 与 x2 的完整两帧成本，并分别保留刷新与复用时间。
准备、切档及返回上档的记录独立于低档稳态窗口，检查实际调用、Reset、源帧计数、模型／工作集身份和收尾。
该模式使用公开 SDK 预初始化的真实 Core provider；未覆盖游戏 snippet 启动、私有 Runtime 或自动 FPS 调档。
GPU 计时和静态有限输出检查仍沿用上述口径，动态转镜、遮挡、光照和帧节奏须另行验收。
使用新输出目录，运行后恢复该环境变量；不改变插件的默认设置或正式兼容范围。

## ControlMask 原生成本

`fg-native` 的 `control-mask-native-ngx-cost` 用例在固定 4K 合成输入上计量
生产 Upscaled 单 Pass，通过已初始化的公开 NGX Core 执行真实 NR。使用现有
ProjectID、SDK 静态库和 310.9.16.0 Runtime，沿用统一测试入口的参数：

~~~powershell
$env:NR_NEXUS_TEST_CONTROL_MASK_COST_MODE = 'depthfar'
pwsh -File tools/run-tests.ps1 -Suite fg-native -NativeCase control-mask-native-ngx-cost `
    -NativeNgxProjectIdPath $projectIdPath -NativeNgxSdkLibraryDirectory $sdkLibraryDirectory `
    -NativeNgxRuntimePath $runtimePath -UiPreview None -OutputDirectory tmp/offline/depthfar-cost
Remove-Item Env:NR_NEXUS_TEST_CONTROL_MASK_COST_MODE
~~~

模式可选 `off`、`rgb`、`depthfar`，默认 `depthfar`。对照按
`off → rgb → off → depthfar → off` 分别启动独立进程、使用新输出目录，
保持相同输入、模型请求和预热。每进程固定请求模型 B，先执行相同的创建／Reset 前缀，
再预热 16 帧、计量 64 帧；每帧恢复固定 Color／Output，提交并等待 Fence 完成。
输出 `controlmask-cost.json` 保存逐样本 GPU ticks、实际队列频率和提交完成值，
分列整个 wrapper、Core Evaluate 和遮罩 Dispatch。Dispatch 不包含前后绑定和 Barrier，
关闭组该字段为 null。原始 ticks 用于重算分布及核对前后基线漂移。

宿主 SR 为 MOCK，NR Evaluate 与 GPU 完成为 NATIVE_NGX／NATIVE_GPU。
合成输入的串行提交耗时用于离线对照；游戏帧时、FG、画质和长时稳定性各自验收。

## 自适应工作分辨率

控制器和内置输出帧率窗口使用纯 CPU 入口，不需要游戏或帧率采样程序：

~~~powershell
./tools/run-tests.ps1 -Suite adaptive-resolution
~~~

用例覆盖低帧试探、限帧无收益回退、恢复画质、双颜色完整计时、过期样本、
SDK 动态计数、固定倍率失效和来源切换。配置保存／方案另用
`tests/run-present-restore-configuration-tests.ps1 -Case nr-resolution-adaptive,profiles`。

双 Present 的自适应开关与预备使用独立的小型 Service 入口：

~~~powershell
./tools/run-tests.ps1 -Suite fg-native -NativeCase adaptive-fg-service -SoftwareOnly
./tools/run-tests.ps1 -Suite fg-native -NativeCase adaptive-inline -SoftwareOnly
~~~

`adaptive-fg-service` 使用 WARP、模型替身和实际 Service／提交路径，覆盖双颜色双 Pass 的完整所选集合、5 档切回默认、缺输入暂停／恢复及真实合同变化。`adaptive-inline` 使用生产 Render／Upscaled 入口，覆盖 64×64 输入的 15 档／两输出、真实尺寸及写回、准备完成后的切档零新增分配、旧 Stage 工作集归属和清锁恢复。两者还核对临时容量占用、ControlMask 未提交、Off／On、兼容设置复用及失败停止。未提交旧录制阻止槽位复用，额外准备 Evaluate 不计入活动角色成功累计；角色计数与完整 Pass／颜色组完成分开。均为 WARP＋MOCK，不调用硬件 NR Runtime 或游戏。

Bridge 两档自适应使用独立的软件单例：

~~~powershell
./tools/run-tests.ps1 -Suite bridge-motion -BridgeCase adaptive -SoftwareOnly -UiPreview None
~~~

该入口强制 WARP、同一软件适配器及 D3D11／D3D12 Debug Layer、InfoQueue，NR 为 MOCK。
内部默认关闭的实验仅接纳 Upscaled 单组单 Pass、FG Off／x2 Off、显式两档全预备；
共享 twins 保持完整尺寸，检查私有档位的往返、Reset、就绪后零新增创建、失败重开、Adaptive OFF 及源释放。
watchdog 救援用例分别核验 `out`、GPU-only `done` 和实际 Reset／不可重放证明，
防止未完成写回与提前释放。这个软件入口不覆盖真实 NGX 或硬件性能。

同一两档流程另有两个显式硬件入口，不自动追加到软件组或 CI：

~~~powershell
./tools/run-tests.ps1 -Suite bridge-motion -BridgeCase adaptive-hardware -UiPreview None
./tools/run-tests.ps1 -Suite bridge-motion -BridgeCase adaptive-native-ngx -UiPreview None `
  -NativeNgxProjectIdPath <已有ProjectID文件> `
  -NativeNgxSdkLibraryDirectory <同版SDK静态库目录> `
  -NativeNgxRuntimePath <已有nvngx_dlssnr.dll> `
  -NativeNgxSrRuntimePath <已有nvngx_dlss.dll>
~~~

两者要求同一 NVIDIA 硬件适配器及双 API Debug Layer／InfoQueue，拒绝与
`-SoftwareOnly` 合用。输入为固定的 1280×720 合成画面、1920×1080 输出，
私有 NR 仅准备 1920×1080 与 960×540；执行就绪往返、Adaptive OFF、NR OFF 和源释放，
不重复软件失败注入与 watchdog 场景。`adaptive-hardware` 为 NATIVE_GPU＋MOCK；
`adaptive-native-ngx` 分别通过 SDK 初始化真实 D3D11 SR 与私有 D3D12 NR，
使用实际 Create／Evaluate／Release 和 D3D11 读回，失败不会改用 MOCK。
临时 Runtime 链接只在本次输出目录创建，进程结束后移除，不修改原 Runtime。

逐源 JSON 分开记录源调用 CPU、其中的严格 `done` 等待和 Bridge 分段；模型 API 另记
Create／Evaluate 耗时，嵌套值不能重复加总。源调用计时不含夹具尾部读回或 SDK 初始化。
真实模式验证完成及有效像素，不以像素变化宣称模型画质。该 SDK 预初始化路径也未覆盖
游戏的 snippet 初始化。正式 Bridge 自适应兼容门禁仍关闭；自动帧率闭环、更多档位、
有限驻留及游戏动态画质需各自证据。

另一个显式入口 `-BridgeCase adaptive-native-snippet` 使用相同的 SDK／Runtime 参数，
仅 SDK 初始化 D3D11 SR；私有 D3D12 NR 经过生产 loader、caller identity 和
`EnsureDirectRuntime` 的 `Init_Ext`，不预写设备就绪或降级到 SDK12。它复用两档流程，
单独记录 Init／Shutdown 结果，并在完成和退休后核验 caller identity 恢复。
这可隔离生产初始化路径；来源 Hook 安装时机和 FG Off 在夹具中仍是明确前提，需另取游戏证据。

默认 `-Suite fg-native` 会复用同一编译产物，分别在独立进程执行这两个自适应用例；默认 `-Suite ui` 也会复用菜单二进制，在独立进程执行自适应真实 ImGui 交互，包括三种介入点连续往返与配置保存。本地完整发布预检保留这些入口；远端核心回归保留 FG 默认组，UI 全量检查由本地初审承担。`NR_NEXUS_ADAPTIVE_UI_ONLY=1` 仍可单独运行自适应 UI，快照模式不额外运行回归。

真实 NR 尺寸往返复用公开 SDK 预初始化入口：

~~~powershell
./tools/run-tests.ps1 -Suite fg-native -NativeCase control-mask-native-ngx-resize `
  -NativeNgxProjectIdPath <已有ProjectID文件> `
  -NativeNgxSdkLibraryDirectory <同版SDK静态库目录> `
  -NativeNgxRuntimePath <已有nvngx_dlssnr.dll>
~~~

输出逐帧 CPU 创建／wrapper、真实 SR／NR GPU timestamp、完成 Fence、尺寸、
工作集与显存信息。创建后暂未执行 NR 的帧单独记录，不能当作零成本执行。
冷切换、预备成本和 ready 档重新激活分别比较；离线原生结果不包含游戏 FG、
显示帧率对照或动态视觉验收。WARP／硬件遮罩用例另核对自动缩放时的手动上限运动参考。

完整所选集合的原生对照使用同一个已构建 EXE，分别在独立进程设置
`NR_NEXUS_TEST_RESIZE_SELECTED_LEVELS=0`（默认，实际四档）和 `15`。两组固定最低
33%、普通工作集池 4、实验模式 OFF，保持真实 SR 1280×720→1920×1080、Upscaled、
B 请求、单 Pass／单颜色和 RGBA16F。其他 `RESIZE_SETTINGS`、`EXPECT_REUSE`、
`COLD_PREPARE`、`SINGLE_FEATURE`、`REUSE_FEATURE`、`FIXED_INPUT`、`DEMAND_PREPARE`
开关须为 `0` 或未设置，`NR_NEXUS_TEST_RESIZE_READY=1`。

~~~powershell
$env:NR_NEXUS_TEST_RESIZE_SELECTED_LEVELS = '0'
& '<同一已构建的fg_native_tests.exe>' --control-mask-native-ngx-resize '<已有ProjectID文件>'
$env:NR_NEXUS_TEST_RESIZE_SELECTED_LEVELS = '15'
& '<同一已构建的fg_native_tests.exe>' --control-mask-native-ngx-resize '<已有ProjectID文件>'
Remove-Item Env:NR_NEXUS_TEST_RESIZE_SELECTED_LEVELS
~~~

两次运行分别保留 `adaptive-native-resize.json`，核对 EXE／Runtime 身份和输入 CRC 一致。
报告沿用 `nr-nexus-native-resize-v1`，新增 `selected_collection`（子 schema
`nr-nexus-native-selected-v1`）：包含上限首次创建的 prefix、完整准备 CPU 合计、两段准备
墙时、夹具设置间隙、端到端就绪时间、采样显存峰值和最长源调用／源间隔。显存采样包含实际
SR、NR 和退休资源占用；不等于连续测得的瞬时峰值。SDK／设备初始化位于准备计量之前。
prefix 沿用 62 次源调用上限，后续准备与请求合计最多 180 次；整组 `Session.prepared`
及健康提交收据齐全后，连续执行 `{0,last,0,middle,last,0}` 六次请求便结束计时，核对实际
尺寸、Reset、Fence、全部模型／代理／工作集身份，以及零 Create／Release／工作集新增。
选择是 MOCK 直接请求，SR／NR 为真实 NGX；不测输出 FPS。预算拒绝保留 Blocked，实际
Create／Evaluate 失败停止并保存诊断，不降低档数或重启准备。结束后沿用真实 Off 和 SDK 收尾。

冷启用自适应的分步管线验证使用同一入口的单个冷进程。编译原生夹具时同时定义
`NR_NEXUS_TEST_NATIVE_NGX`、`NR_NEXUS_TEST_COLD_CPU_TRACE`，设置
`NR_NEXUS_TEST_RESIZE_PIPELINE_STEPS=1`，`NR_NEXUS_TEST_RESIZE_SELECTED_LEVELS=0`（默认）
或 `15`（最低 33% 的完整网格），以及
`NR_NEXUS_TEST_RESIZE_READY=1`、`NR_NEXUS_TEST_RESIZE_COLD_COMPONENTS=0`，其余专项开关
沿上面的要求关闭。首次 prefix 源调用前就开启自适应，沿用既有 62 次 prefix 与
180 次 selected 源硬界，保留已经推进的同一所选集合；全部档位取得健康完成收据后，
只执行 `{0,last,0}` 三次即时切换，再运行原有 Off、SR 恢复、source Release 与 SDK 收尾。

此模式在 `adaptive-native-resize.json` 增加 `pipeline_steps` 汇总，并复用
`cold_cpu_trace` 为每个 prefix/selected 源保存至多 512 个嵌套 span。记录实际 PSO API
次数、NGX Create、SR/NR Evaluate CPU 成本、最长源调用和准备墙时；每源最多一次插件
PSO 创建，最后一个 PSO 所在源不叠加 NGX Create 或 NR Evaluate，ready 切换不新增管线、
模型或工作集。trace 使用有界堆存储，JSON 转换在 Hook 计时外；墙时和源间隔包含报告、
导出与夹具限速开销。SDK/provider、codec 根签名与 ControlMask PSO 的前置准备状态单独
记录。同步的单个创建仍不可拆分，这项验证不预设总准备时间或单次延迟上限。

有限驻留的原生单例在上述 `SELECTED_LEVELS=15`、`PIPELINE_STEPS=1`、`READY=1`
条件下再设置 `NR_NEXUS_TEST_RESIZE_LIMITED=1`，不启用旧 `DEMAND_PREPARE` 旁路。
它保留15个逻辑档位，验证上限→冷档→上限、旧录制仍可重放时拒绝添档、反向取消、
真实退休后再准备和原有 OFF 收尾；只要求目标整组就绪并实际应用，不把 `Session.prepared`
伪装为真。档位请求是夹具直接驱动，SR／NR、GPU 完成和资源退休是真实执行。
此单例默认显式开启内部私有PSO实验，autoscale与look bands共用单个在途任务；
玩家的有限驻留选项不自动开启它。仅此`LIMITED=1`夹具可用
`NR_NEXUS_TEST_RESIZE_ASYNC_PSO=0`执行同步基线；省略或设为`1`保留私有任务路径。
两个新进程可使用同一二进制对照；这同时切换两个PSO，驱动／磁盘缓存不受控，
不将全局就绪墙时差单独归因Look。此路径需编译`NR_NEXUS_TEST_COLD_CPU_TRACE`。
`async_pso` 使用
`nr-nexus-native-private-pso-v2`，在 `targets.autoscale` 与 `targets.look_bands`
分别记录真实API区间、返回／异常、线程身份及OS退出凭据、pending源返回次数，
并以真实API产物与生产槽中的同一pipeline核对接纳，私有模式还核实际task的shader／slot。
接纳时间是源调用返回后的观测时刻；API返回、槽接纳与线程退出分别判断，
未完成值保留为空，同步主线程没有worker，退出字段为`null / NotApplicable`。
成功收尾也会递增取消代次，`task.canceled_at_snapshot`不代表此前接纳被拒。
取消、失败归属及顺序接纳仍由`adaptive-inline`严格WARP用例覆盖；旧autoscale单项
记录不能证明look bands原生通过。同步API位于主线程嵌套trace内，私有worker另行记录，
不能重复加总。源CPU区间为`HookedEvaluateFeatureSlot`，不含夹具在其前调用的
`StartAsyncAutoscalePreparation`；总就绪墙时及实际源起点间隔包含这些间隙。
前缀沿用 selected 循环的60 Hz节奏，62／180次源界限不变；准备墙时包含该节奏，
不能直接对比旧零节拍前缀。`selected_collection.limited_residency` 保存目标、应用、
驻留与退休数量；本模式的旧 `full_preparation_*` 字段仅继承首次上限前缀合计，完整范围
以 `initial_target` 和 `phase_states` 为准。显存使用 `QueryVideoMemoryInfo` 的进程
LOCAL CurrentUsage采样，含SR、NR、输入输出、工作集及待退休资源，不是额外档位显存或瞬时峰值。

自动控制器单例改用 `NR_NEXUS_TEST_RESIZE_LIMITED=2`、`SELECTED_LEVELS=3`、
`PIPELINE_STEPS=0`、`READY=1`，其他 resize 实验关闭。它通过玩家按需设置进入真实
Controller，不直接调用 `Session.Request`，也不修改档位、blocked 状态或时钟。
三个逻辑档为100%／75%／50%；仅输出FPS为MOCK，按实际Applied尺寸依次提供
80／84／60 FPS，回退75%后保持60 FPS以模拟负载变化。SR／NR、GPU时间戳、
完成凭据和资源退休均真实执行；不注入GPU收益，私有PSO实验保持关闭。
用例在最多900个源机会、含前缀20秒内验证连续冷降档、应用、无益回退和再次探索，
GPU收益门禁不满足即如实失败。回退通过请求代际／trial／blocked转换观察，不依赖
可能在同源被WARMING覆盖的显示文案。最终应累计创建1／1／2个Feature，复用75%
回退锚并先退休旧50%资源，再沿原OFF路径收尾。详细证据位于
`selected_collection.limited_residency.automatic`；旧 `full_preparation_*` 字段仍只表示
首次上限前缀，不是全档预备。此用例不覆盖30秒冷却自然到期、游戏FPS采集准确性、
动态画质、完整15档探索或分配失败；没有与全档模式作成对性能比较。

首调用 CPU 归因另用原生测试编译宏 `NR_NEXUS_TEST_COLD_CPU_TRACE`，配合
`NR_NEXUS_TEST_RESIZE_COLD_COMPONENTS=1`、`NR_NEXUS_TEST_RESIZE_SELECTED_LEVELS=0`
和 `NR_NEXUS_TEST_RESIZE_READY=1`，其他专项关闭，仍调用同一 resize 入口。只运行首两次
prefix 源调用；不足、API 失败或记录溢出即失败，不延长。随后直接执行关闭、一次真实 SR
恢复、source Release 和 SDK 收尾，跳过完整档位准备／切档及最终图像读回。
正常生产构建不含这些计时或回调；测试构建也只在受测 Hook 的调用线程内激活固定内存记录。

`cold_components` 记录边界及结果，`cold_cpu_trace` 使用 `nr-nexus-cold-cpu-trace-v1`，
每次源调用最多 512 个父子 span。起止时间和 inclusive／exclusive 均保留；exclusive 是
父区间减直接子区间，包含未细分工作和观测开销，不能再将 inclusive 父子相加。
覆盖入口生命周期／锁、Runtime helper、管线及实际 PSO、Look 根签名、工作集及资源／描述符、
Feature 参数分配和既有 SDK Create／Evaluate。日志发送区间涵盖同步回调及 stdout，调用者
预先格式化文本的时间留在所属父区间；SDK 内部后台线程未细分。JSON 转换在受测调用后，
提交／Fence、退休和调用至提交间隙独立记录。SDK/provider、codec 根签名和 ControlMask PSO
在 prefix 前已准备，报告明确记录，结果不作为完整游戏冷启动成本。运行后恢复探针环境变量。

设置复用对照可在同一入口显式设置 `NR_NEXUS_TEST_RESIZE_SETTINGS=1`，将原有
180 帧往返扩为 360 帧，依次编辑风格、混合强度及最低比例 80%→50%→67%。
同时设置 `NR_NEXUS_TEST_RESIZE_EXPECT_REUSE=1` 时，断言兼容设置编辑不冷建，
风格变化仍重置历史，并在第 285 帧请求新 50% 档、第 300 帧提高下限后核对合法回退。
结果区分设置编辑与实际新尺寸请求，保留逐帧 Create／Release、GPU Evaluate／Fence、
锁竞争计数及实际源帧间隔。锁等待最大值是累计值；单线程夹具没有竞争不能证明并发创建安全。
该序列不测试模型内部身份、游戏 FPS 或动态画质。运行后恢复这两个环境变量；普通入口仍用原往返。

候选 Feature 容量复用的开发实验默认关闭。设置上述两个开关后，可另外设置
`NR_NEXUS_TEST_RESIZE_REUSE_FEATURE=1`，在同一 360 帧序列中开启容量内复用对照。
范围限定为已核对的 Runtime 310.9.16.0、B 模型、单 Pass DX12 Upscaled 和 FP16 线性 HDR。
实验只保留已完成使用的非活动候选自身 Feature／参数，资源退休、历史重置及就绪凭据仍按原流程核验；
配置不兼容或容量不足时沿用重建。比较时保持其余输入一致，分别记录新端点的创建、录制、
GPU 完成及完整源调用间隔。普通插件没有此实验的配置入口，P1 和异步准备均未开启。

同一开关设为 `2` 时，增加上限容量备用档的首次准备对照：在上述合同的单输出路径中，
最多驻留三档，新备用 Feature 按手动上限容量 Create，工作纹理和 Evaluate 保持实际档位尺寸。
现有阶梯压缩仍生效，默认最低 67% 时三档为 100／85／67%，尚未拆分逻辑档与物理槽。
与开关 `0` 的默认四档串行对照初始 Create 次数／总耗时、就绪帧、显存峰值及新端点成本；
不能只以 Create 次数较少认定初始化更快。开关 `1` 保留原容量内复用实验。

按需准备对照使用 `NR_NEXUS_TEST_RESIZE_DEMAND_PREPARE=1`，分别设
`NR_NEXUS_TEST_RESIZE_REUSE_FEATURE=2`／`3`，不同时开启设置编辑、固定输入或其他专项。
该序列运行 900 帧：目标 100 FPS，前 360 帧提供 MOCK 120 FPS，随后提供 MOCK 80 FPS。
档位由生产控制器决定，没有直接请求档位；SR／NR 及控制器 GPU 成本采样仍为原生执行。
模式 3 沿用上限容量合同，仅准备当前请求的候选。分别核对初始备用 Create、首次请求的
前台 Create、等待期间合法活动档、GPU 完成后切换、无收益回到原上限资源，以及未使用槽。
MOCK FPS 不用于验证 SDK 输出帧率准确性；省掉未使用档与延后必要冷 Create 分开报告。
结束恢复两个环境变量；上述按需实验仍默认 OFF，档位范围沿用旧实验，不启用异步准备。

将 `NR_NEXUS_TEST_RESIZE_DEMAND_PREPARE` 设为 `2`，可用相同入口比较按需模式 `3` 与
一份相邻热备模式 `4`。仍为 900 帧、目标 100；前 360 帧提供 MOCK 120 FPS，之后按
实际活动档提供 80／90／100 FPS，让真实控制器和 GPU 成本判断两次连续降档是否有效。
模式 4 启动只预热最近低档，接受试探且未发出下一请求时才补相邻低档；试探期间保留回退链。
逐次比较请求到应用、补备 Create／首次 Evaluate、后继源调用间隔和峰值显存。
若接受试探时已经立即请求下一档，记录补备窗口不足；不得延后请求来制造热备命中。
请求时无冷创建不等于整段没有停顿，补备的前台成本必须单列。
此模式只由原生夹具开启，开发实时接口仍只接受 0–3；默认行为与三档映射保持，不启用 P1。

设置编辑序列还读回 67% 和 50% 的实际代理输入、NR／最终输出，记录尺寸、RGBA 有限值数量与 CRC。
容量复用会提前一帧激活新端点，因此额外读回其第 298 帧，用于和基线第 299 帧按 Reset 后
Evaluate 次数对齐；比较仍须核对实际输入与历史，CRC 不同不能直接归因于容量。读回在计时
结束后执行，受读回影响的下一次源间隔单独标记。仅 `NR_NEXUS_TEST_KEEP_RAW=1` 输出
`adaptive-pixels-<frame>-<role>.rgba16f`，用于定量差异分析；用途结束后按产物规则收尾。

归因时可另设 `NR_NEXUS_TEST_RESIZE_FIXED_INPUT=1`：在每次实际 Core Evaluate 前，将完整
有效 Color 区域替换为按实际尺寸生成的同一 FP16 渐变，保留生产 Reset 和资源提交流程。
第 285–299 帧还记录实际 Core 参数、资源尺寸及参数可用状态；比较必须分别对齐源帧与
Reset 后的 Evaluate 次数。固定输入上传和复制会计入 wrapper／NR GPU 区间，因此这一组
只用于像素归因，不能据其计时宣称性能收益。最终合成仍使用原 SR 图像，NR 相等不自动使最终图相等。

冷准备专项可单独设置 `NR_NEXUS_TEST_RESIZE_COLD_PREPARE=1`，仍运行同一个
`control-mask-native-ngx-resize` 入口，改为私有 worker Create 与前台真实 SR／NR 的短对照。
worker 使用同设备的独立参数、Feature、allocator 和命令列表，保留现有 NGX 串行门禁；
报告分别记录独立 D3D 工作、前台门禁等待、初始化 GPU 完成和候选首次 Evaluate。
只有实际时间区间重叠才认定覆盖，未重叠记为未判定；SDK 调用超时不会转为并发调用。
结果保存为 `adaptive-native-cold-prepare.json`。该探针不启用生产异步准备，也不测试
Feature18 Create／Evaluate 并发安全；结束后恢复环境变量。

单 Feature 有效尺寸专项可单独设置 `NR_NEXUS_TEST_RESIZE_SINGLE_FEATURE=1`，复用同一入口，
与冷准备开关互斥。一个最大尺寸 Feature 依次运行完整尺寸、较小的标准尺寸／subrect、
同步已知私有尺寸参数、恢复完整尺寸，再与真正使用小纹理创建的独立 Feature 对照。
各阶段使用相同的 ROI 输入字节和只读引导资源，保留两次 Reset 前缀及八次测量。
结果保存为 `adaptive-native-single-feature.json`，分别记录 API、逐次 CPU／GPU 时间、
可见的直接 Dispatch、RGB／alpha 写入范围、非有限值、数值差异及资源完成／清理。
像素覆盖和 API 成功不替代实际成本测量；直接 Dispatch 也不代表完整内部工作量。
该探针不改变生产尺寸策略或开启 P1；结束后恢复环境变量。
将该开关设为 `2` 时，两种缩小请求改为给同一最大 Feature 绑定真正的小纹理，
其余五阶段、参考输入和测量方法不变，用于单独核对物理资源布局的兼容性。

## 自动回归与证据保存

GitHub 分支 CI 使用 [`ci-core.ps1`](../ci-core.ps1)：保留核心 CPU／配置／生命周期检查、FG 的五个默认组，以及复用同一 EXE、在独立进程执行的 pre-FG 异尺寸与复制链回归。完整本地发布初审继续使用 `run-release-preflight.ps1`，不因远端精简删减。远端产物为 `core-regression-<attempt>`（检查结果与失败日志）和 `windows-x64-<attempt>`（安装包及 SHA-256）。

`main` 的 [`ci-evidence.ps1`](../ci-evidence.ps1) 仅复用完整 Git 根 tree 相同的成功核心记录，依赖锁、工具锁和 workflow 均包含在 tree 中；找不到或状态不符则执行核心验证。Tag 必须先取得同身份成功核心证据，再重新构建 Publishing 并通过 [`check-publishing.ps1`](../check-publishing.ps1) 核对 DEV-OFF 编译命令、诊断标记、版本、包白名单与哈希，不重跑完整测试。原始 run／源码／tree 及复用来源显示在 Actions summary。

~~~powershell
python -m unittest discover -s tests -p 'test_offline_*.py' -v
~~~

真实 EXE 边界测试通过 NR_NEXUS_OFFLINE_GPU_EXE、NR_NEXUS_REPLAY_BACKEND_EXE、
NR_NEXUS_NGX_PROBE_EXE 指定已构建二进制。Replay 测试默认使用 WARP，可选设置
NR_NEXUS_REPLAY_TEST_ADAPTER 为 `warp` 或 `nvidia`；NGX 本地 Runtime 核对另需
NR_NEXUS_NGX_RUNTIME_DIR。缺输入明确跳过，不计为设备／Runtime 已测。三类后端的
回归产物保留在 `tmp/offline/` 下每轮唯一的证据目录，测试输出实际目录，不自动清理。
原有配置、预设、UI、Feature／History、FG／XeFG 和多尺寸 suite 按相关改动定向复用，
不附带运行全量或游戏矩阵。

原始 JSON、编译依赖、日志、Runtime 身份与捕获留在本地忽略目录。当前工具没有
上传、游戏部署、驱动修改或发布动作；不要把本地结果、第三方 Runtime、模型和大型
输入加入公开提交。公开仓库只保留代码、合成单测与小型 Fixture 定义。

FLOAT→UNORM 依据：[Microsoft D3D12 constants](https://learn.microsoft.com/en-us/windows/win32/direct3d12/constants)
中的 D3D12_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP，以及
[Direct3D functional spec §3.2.3.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#FLOATtoUNORM)。
