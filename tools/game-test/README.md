# 巫师3本地测试入口

本入口支持 Steam／巫师3 DX12。默认用游戏脚本桥操作标题菜单、精确加载存档及正常退出；runner 在受保护会话中临时部署本仓库的 source-only bridge Mod，使用 -net -debugscripts 与本机 loopback 调试端口通信。现有存档／配置保护、进程归属、崩溃检查、恢复流程继续复用。它不是通用游戏驱动，不包含悟空无人选档。

客户端连接 `127.0.0.1:37001`；已验证的游戏调试服务监听 `0.0.0.0:37001`，外部可达范围取决于现有防火墙设置。调试服务随本次所属游戏进程退出而结束。

默认 smoke／menu／calibrate 不建 OCR，也不以鼠标或键盘导航游戏菜单。Smoke 必须每轮传 -Save 和当前存档文件名；游戏通过 ListSavedGames 按唯一完整文件名载入，runner 先核对该存档 SHA-256，再连续两次确认 GetLoadGameProgress 非错误且玩家对象有效，才记为游戏态就绪。菜单退出调用游戏 API 并等待所属进程退出。需要 CaptureScript 捕获桌面时，采集边界仍核对 HWND、PID、创建时间、路径和真实前台；只在采集要求下激活窗口。确定性脚本错误立即失败，不拖到整段超时；默认自动重试为 0。

代码桥接会等待游戏标题菜单可用，但不会跳过启动片头。与本机 Witcher 3 5.00 匹配的 Skip Intro Movies - Remastered 尚未在本机安装或实测；没有该验证时，不能把片头已跳过作为测试能力。

两个 PowerShell 入口接受 `-CaptureScript 指向 tools/ 或 tmp/ 下的 Python 脚本`，同时指定非零 `-CaptureHoldSeconds`。入口在安装前校验参数，普通桌面上下文重启保留该绝对路径。runner 预检记录脚本 SHA256，在 `CAPTURE_READY` 后复核并仅启动一次，调用形式为同一 Python 的 `-B <脚本> <本次运行目录>`，输出写 `automatic-capture.log`。该选项不能与 `ManualImeSession` 或 `InstallOnly` 搭配。

采集器读取 capture-ready.json 的 automatic_foreground=true 和本次 owned-process.json，核对唯一进程／窗口身份并自动激活。动态采样与转镜期间持续核验前台和几何，失焦即停止，不在采样中反复激活。收到 capture-cancel.json 应停止输入并结束自己的采样子进程；异常写 capture-failed.json，成功收尾才写匹配 PID 的 capture-complete.json。使用 CaptureScript 时还必须由父进程观察到脚本零码退出并写 capture-script-result.json，worker 才允许请求正常退出。提前完成、失败／损坏凭据或超时不能释放门禁。恢复前先结束采集器和 worker，文件保护保持到本次游戏退出并恢复完成。

静态阶段采证使用 `-CaptureScript tools/game-test/static_capture.py`（入口会解析为绝对路径）。采集器直接采用 runner 传入的本次目录，核对 `target.json` 哈希、脚本哈希和 owner／ready 身份，不限定 case 目录名。先生成本轮目标副本，原目标不改：

```powershell
& $pythonPath -B ./tools/game-test/static_capture.py --prepare-target $targetPath `
    --output-target "$taskRoot/target.json" --hold-seconds 600 `
    --exit-reserve-seconds 60 --stage-timeout-seconds 60
```

开发接口接线需显式给目标副本设置 `static_capture.dev_control=true`，或在上述准备命令增加 `--development-control`。默认仍沿用手工菜单路径。接口客户端 `tools/dev_control.py` 只依赖 Python 标准库，连接 `\\.\pipe\nr-nexus-dev-<pid>`；每帧为 4 字节小端长度和最多 64 KiB 的 UTF-8 JSON。连接等待、OVERLAPPED 读写、部分帧和确认往返共用有界 deadline。客户端核对管道服务 PID，并在请求前后核对 `owned-process.json.created` 的原始 Windows FILETIME；`--process-created` 使用该整数的十进制字符串，不转换成 Unix 时间。接口需由开发候选显式启用；本说明不授予安装或游戏启动权限。

开发候选使用 `tools/build.ps1 -ToolRoot <既有工具目录> -Configuration Release -DevControl -BuildDirectory build/dev-control`；该开关不能使用普通 `build` 输出目录。普通构建显式设置 `NR_NEXUS_DEV_CONTROL=OFF`，开发目录与普通目录的 DLL 分别核对哈希和实际加载身份。无面板／绘制请求时接口仍由选定 ReShade 帧回调执行。只读配置状态不建立 GPU 完成证据；区域遮罩的 `resolved_masks` 也只表示逐层解析参数。

```powershell
& $pythonPath -B ./tools/dev_control.py --pid 42 --process-created 123 status
& $pythonPath -B ./tools/dev_control.py --pid 42 --process-created 123 set NRHookPoint 1
```

Mafia 单 Pass Upscaled 的恢复诊断使用独立请求 `inline-restore-roi <绝对目录>`，协议操作为 `inline_restore_roi`，字段为 `output_directory`。它只布防一次求值，不提交画质设置或切换接入点。目标目录须为 D 盘本项目 `tmp/mafia-same-frame-roi/<capture_id>/` 下尚未创建的批次目录，`capture_id` 使用英文字母、数字、连字符或下划线，最长 96 个字符。请求接受时冻结目录；目录无效或写盘失败会报告失败，不使用游戏截图目录。

这条诊断的状态、API、取样插点和热路径判断全部受 `NR_NEXUS_DEV_CONTROL` 编译条件保护。普通发布构建关闭该选项，保留原有结构、函数签名和调用路径；开发构建才包含采集能力。认领一次合格求值后新增四次 `CopyTextureRegion` 和八次复制前后的资源状态转换，净原码为 175,392 字节；READBACK 还包含行填充与堆分配对齐。它复用已有求值、提交与完成证明，不新增模型调用或渲染线程等待。实机 GPU 拷贝耗时及帧时影响尚未测量。

```powershell
# 仅在另行授权并核实 PID／创建时间的实机窗口中执行。
$taskRoiDirectory = [IO.Path]::GetFullPath((Join-Path $PWD 'tmp/mafia-same-frame-roi/indoor-001'))
& $pythonPath -B ./tools/dev_control.py --pid 42 --process-created 123 `
    inline-restore-roi $taskRoiDirectory
& $pythonPath -B ./tools/dev_control.py --pid 42 --process-created 123 status
```

保持原室内机位、K 请求、HDR Proxy 和 NR On；采集合同固定为 3840×2160、legacy codec 2、RGBA16 UNORM、单 Pass Upscaled。三张颜色面 `original`／`post_resolve`／`post_restore` 的 ROI 为 `[1936,2088)×[1106,1154)`；`restore_blocks` 为块坐标 `[60,66)×[34,37)` 的真实 RGBA32F 统计。四份同名 `.raw` 共 175,392 字节，均为小端紧密行；`capture.meta.txt` 记录同次求值序号、recording、实际恢复参数与提交完成凭据。请求接收与最终落盘分开查看：用 `status` 中的 `state.inline_restore_roi` 跟踪结果，最终核对 `written`、侧车 `capture_valid=1`、四面齐全及 GPU 完成、输出写回、桥接回拷记录。失败或缺面不从下一帧补齐；连接结果不明时仍只查询原请求回执，不重复布防。请求设有 30 秒期限，并沿用已有提交完成检查的有界失败处理。

后续分析先确认目标轮廓确实位于 `post_restore`，再用实际块统计按恢复 Shader 插值，比较恢复前后 RGB 扣减和跨门槛位置。当前诊断入口不改变恢复算法；只有同帧数据满足判据才能归因。该入口不新增 NR 开关、双 Pass 或 Present 对照要求。

开发状态还包含输出 FPS 的来源、有效窗口、generation、FPS／MAD 及请求／观测倍率；无效或过期测量为 null。
`adaptive_caches` 分开报告物理槽、逻辑尺寸、Feature 的 Create 容量与就绪状态。Python 客户端的
`adaptive_feature_experiment(0|1|2|3)` 专用于默认关闭的容量实验，不是 INI 设置：分别请求关闭、已有容量复用、
三槽上限容量、上限容量按需准备。模式 3 只准备控制器当前请求的候选；首次请求的冷 Create 仍在前台。
请求只在 NR OFF、真实资源已释放且配置空闲时接受；实验取证应临时使用现有
`NRDisableRelease=1` 和 `NRWorksetFreePolicy=0`，结束恢复原值。DEV 的 `set NRDisableRelease 1` 复用正式配置校验和保存路径；策略接受后仍须等待 `disabled_resources_released=true`，接口不制造资源释放证明。模式请求不等于实际准入，仍须检查槽、容量和 GPU 完成。

Bridge 两档接入使用独立的 DEV 请求：`Client.bridge_adaptive_experiment(True|False)`，
协议为 `{"id":"bridge-enable-1","op":"bridge_adaptive_experiment","enabled":true}`。
开启和关闭都要求 NR OFF、资源已释放、配置空闲；它不写 INI，也不替用户改动其他设置。
`state.experiments.bridge_adaptive_experiment` 与 `identity.experiments` 返回请求开关，
`bridge_adaptive_evidence=requested_enabled_not_activation` 标明尚不能据此判断执行。
实际每源仍只接纳两档全预备、Upscaled 单 Pass、可信 FG Off、x2 Off 和有效来源代际；
接口不会制造 FG Off 或 snippet 初始化成功。关闭流程先沿现有 NR OFF 收尾，待释放凭据成立后请求 false；
释放未完成时保持原请求状态。该通道仅编入 `NR_NEXUS_DEV_CONTROL` 开发候选，普通 Publishing 不含入口，
不是玩家兼容开关；`status`／原 ID 回执查询及同一 deadline 的规则与其他开发请求相同。

内部两档 x2 接线复用 `Client.edit_reuse_adaptive_experiment(True|False)`，协议为
`{"id":"edit-reuse-enable-1","op":"edit_reuse_adaptive_experiment","enabled":true}`。
它沿用上述双向 OFF／释放／配置空闲门禁，仅改变 DEV 实验请求；启动配置 `NREditReuse`、默认值和生产准入不变。
该组合仍限原生 D3D12 Upscaled、FG Off、单 Pass／单输出和两个完整预备档。
`state.edit_reuse` 返回请求开关和进程级刷新／复用／回退计数；`adaptive_caches` 中的 `source_frame`、`trial`、
`sampling` 分别反映源序号、试档状态和已完成采样。统计有效时 `median_us`／`mad_us` 是每源完整 NR 链成本，
x2 先平均相邻两源再统计；未满窗口或过期时为 null。请求、计数、已完成成本和实际档位须分别判断。
FG Off 的现成输出来源为 ReShade Present 回调的 `Present cadence estimate`；它不报告显示器扫描完成。
离屏原生 NGX 夹具无 Present 循环，其串行吞吐或合成 FPS 不可替代该来源。

可信 FG Off 可以来自成功的 Streamline `SetOptions(eOff)`，或绑定当前 XeFG context／proxy、
D3D11 设备和源代次的成功 `SetEnabled(false)`。XeFG 路径要求关闭调用前后均能观察到同一个已创建来源；
重新启用、重建、来源变化或失败／在途调用撤销旧回执。默认 `enabled=false` 与配置请求不提供此证明。
`Bridge Adaptive XeFG Off receipt` 日志报告接纳及缺失原因；它不改写 `identity.fg` 中的 Streamline 声明。
实际游戏返回的身份和回调顺序仍须现场核实，不能以软件 SDK 替身的成功代替。

`set` 一次提交一个现有 INI key 和 JSON 类型值。返回 `confirmation_required` 时默认停止；只有明确使用 `--confirm`，或阶段请求显式写入 `confirmation_required=true`，才将服务返回的完整 `identity` 和 `changes` 原样携带到新请求，由服务复核提案。服务复用菜单的配置缓存／持久化提交语义，受保护 runner 在退出时恢复原文件。不能把接口配置当作临时无副作用事务。`status` 遇到 `available=false` 且原因为 `runtime_busy`／`configuration_changed` 时，只在同一 deadline 内重试读取。连接失败或 `outcome_unknown` 后仅以 `status(query_id=<原请求ID>)` 查询 `receipt`，不会自动重放设置；deadline 内没有明确回执则抛出 `OutcomeUnknown`，可另用 `--query-id <原请求ID> status` 核查。

collector 在每个 active 阶段先读状态，仅在 NR 关闭时请求 `NeuralUplift=true`，再设置 `NRHookPoint`（0 Upscaled／1 Render／2 Present）、所需 `NRPasses` 及第 2–4 层的 `NRPassNStage`（1 Render／2 Upscaled／3 Present，0 为继承）。依赖改动与危险阶段确认由服务生成提案，不在客户端复制菜单规则。接口模式的多层请求须提供从 Pass1 开始连续完整的 `expected_plan`；省略时为单层 Pass1。请求、实际 `changes` 和回执 `state.settings` 写入 `static-events.json`，随后仍等待当前完整计划和新序列的成功 `last_nr_evaluate`，才记录 `stage_observed`。赋值只提供配置证据，API 成功单独记为 API 证据；GPU 完成只读取 snapshot 的 `completion.current`，collector 不由配置或 API 成功推导 GPU 完成或图像质量。

目标副本可进一步设置 `static_capture.stages`，按顺序自动执行三至四个阶段，免去外部脚本逐次写入 control 文件。例如：

```json
{
  "dev_control": true,
  "stages": [
    {"label":"baseline","expected_stage":"Upscaled","expected_plan":{"1":"Upscaled"}},
    {"label":"render","expected_stage":"Render","expected_plan":{"1":"Render"}},
    {"label":"present","expected_stage":"Present","expected_plan":{"1":"Present"},"confirmation_required":true}
  ]
}
```

将这些字段合并进目标已有的 `static_capture` 预算对象；阶段 label 必须唯一。全部阶段观察完毕后自动进入 cleanup，请求 `NeuralUplift=false`，记录 `applied`／`unchanged` 回执及实际关闭状态。接口模式随后读取新的 status，要求 `settings.NeuralUplift=false`、来自真实 overlay 事件的 `overlay_open=false`，再等 cleanup 开始后新增 OFF 日志并采集截图，以 `API_OBSERVED_OVERLAY_CLOSED` 记录关板证据，无需外部写入手工完成文件。overlay 仍打开时持续等待至有界清理截止，不自动发送 Home 键。`disabled_resources_released` 单独记录，不由 OFF 或 API 回执推导 GPU 资源释放。父进程观察到采集脚本零码退出的完成门禁保留；非接口模式仍要求主代理观察画面后填写 overlay closed／`nr_off`。`static-control.json` 的手工阶段请求、取消与完成入口保留。少量菜单开关、确认框和 overlay 开关的实机 smoke 仍由主代理按当次授权验收。

在已获授权的原受保护入口同时使用 `-TargetPath "$taskRoot/target.json" -CaptureHoldSeconds 600`。副本按原目标启动／载档预算加退出预留派生 `run_timeout`；启动120＋载档120＋采集600＋退出60秒时为900秒，原默认420秒不足。`static_capture` 专用字段记录本轮预算，collector 拒绝 Hold 与副本不一致或 overall 不足；从 ready 时间扣除已耗时间，以 Hold 减退出预留得到采证截止时间。候选、备份范围和输入／确认路线在启动前备齐，运行中只做约定阶段及进度核验，长日志和源码归因放到退出后；只安排本轮必要的三个阶段，有结果立即收尾。

主代理原子写入本次 `static-control.json`，例如 `{"pid":42,"label":"render","expected_stage":"Render","expected_pass":1,"expected_plan":{"1":"Render","2":"Upscaled"},"confirmation_required":false}`（PID 取本次 ready，Pass 默认1）。等 `static-progress.json` 的 `state=awaiting_stage`、`pending_label=render` 后操作；需要确认的 UI 先实际完成确认，再在同一请求写 `confirmation_completed=true`。`expected_plan` 核对整个当前请求／有效计划；只指定 `expected_pass` 时仅核对该层，其他层不推导。Render＋Upscaled 的双层请求观察Pass1；同阶段双层周期日志常保留最后一层，Present双层应指定 `expected_pass=2`、两层Present及 `confirmation_required=true`。输入由主代理复用 `desktop.click(window,x,y,activate=False)`／`desktop.key(window,name,activate=False)`：window 来自已核对的本次 owner／ready，操作前检查取消／失败凭据和前台，坐标来自刚观察的原图，并把动作与观察时间写入本次 journal。现有 click 按下120ms、key 默认200ms，均在 finally 释放。旧的带 case 路径临时 action 脚本和一次 CUA 点击记录不能充当切换成功。collector 不自动点坐标；只有请求后的完整新增日志、超过请求前基线的序号、按首次观察区间核对的 age_ms，以及当前有效计划和成功 `last_nr_evaluate` 一致才记录 `stage_observed`。规划、旧 Evaluate、点击和未完成确认均不足；API 回执不提供 GPU 完成或视觉验收结论。

实机脚本的键盘输入统一调用 `desktop.key`，默认持键 200 ms，down/up 分开发送，finally 释放，沿用目标窗口身份和前台校验；不以短促桌面按键替代。Mafia 的“继续游戏”仍使用已验证的持续虚拟手柄。Home 输入后必须读取真实 overlay 状态和画面，未出现菜单时保留输入回执并定位，不能仅凭输入调用成功或重复按键记为通过。

键名不区分大小写，按 Windows Scan 1 的物理位置命名：A–Z、0–9、F1–F24、导航键、普通/小键盘 Enter、Tab/Space/Backspace、左右 Ctrl/Alt/Shift/Win、菜单键、锁定键，以及标点和小键盘数字/运算/小数。常用别名包括 `RETURN`、`ESCAPE`、`PGUP/PGDN`、`CTRL/ALT/SHIFT`（左侧）、`ALTGR`（右 Alt）、`KP_0`–`KP_9`、`KP_ENTER` 和 `PRTSC`。标点可用 US 位置名称（如 `SEMICOLON`）或未按 Shift 的符号；这不是任意字符输入，实际字符受布局、IME 和锁定状态影响。

导航区、右 Ctrl/Alt、小键盘 Enter/Divide、Win/菜单与 PrintScreen 使用 E0 扩展标志；小键盘数字、左右 Shift 和锁定键沿用各自普通扫描码。PrintScreen 使用 Windows 定义的 E0 37，不拼接原始 PS/2 序列；Pause/Break 的 E1/组合语义不适用此单键持键入口，明确拒绝。未登记的厂商/Fn/媒体键及组合宏不作支持声明。映射依据 [Microsoft 键盘输入文档](https://learn.microsoft.com/windows/win32/inputdev/about-keyboard-input#scan-codes) 与 [KEYBDINPUT](https://learn.microsoft.com/windows/win32/api/winuser/ns-winuser-keybdinput)。

已获授权的主代理动作可复用同一身份检查；下面的坐标和观察时间来自刚捕获的原图，完成点击后仍等待 collector 的实际阶段回执：

```python
run, window, _, _ = static_capture.context(run_directory, Path('tools/game-test/static_capture.py'))
if any((run / name).exists() for name in ('capture-cancel.json', 'capture-failed.json')):
    raise RuntimeError('Protected capture is stopping')
progress = static_capture.read(run / 'static-progress.json')
if progress['state'] != 'awaiting_stage' or progress['seconds_remaining'] <= 0:
    raise RuntimeError('Static stage input window is closed')
desktop.require_foreground(window, 'static input')
started = stamp()
desktop.click(window, observed_x, observed_y, activate=False)
with (run / 'actions.jsonl').open('a', encoding='utf-8') as journal:
    journal.write(json.dumps(dict(started=started, finished=stamp(), action='click',
        observed_at=observed_at, x=observed_x, y=observed_y, stage_result='pending')) + '\n')
```

首次控制接管前的等待受总采证截止时间约束；600秒总采集、60秒退出预留时，初次请求须在 ready 后540秒前到达，已耗时间始终扣除。首个实际阶段请求到来后，每阶段执行和阶段成功后等待下一请求均限60秒；重复 control 不延长窗口。阶段无进展即记 `functionalOutcome=Blocked`，停止接受新阶段并进入最多60秒的清理窗口；总采证预算到期也如此，清理仍不得越过总600秒。正常完成时先写同 PID 的 `{"completed":true}` 请求清理，等 `state=cleanup` 后实际关闭面板、关闭 NR，再补 `overlay_closed=true,nr_off=true`。`overlay_closed` 是主代理对实际画面的确认，不由 Home 按键推断。collector 要求清理开始后新增且最新的 NR OFF 日志，并留一张 `addon-menu-cleanup.png`，才写完成凭据并零码退出；缺证、取消、失焦、身份变化和清理超时仍失败。`static-progress.json` 分别记录当前窗口和总采集预算余量；`static-complete.json` 保留 `Blocked`／`Observed`／`NotTested`，runner 的启动／退出成功不把 `Blocked` 变为功能通过。

回归覆盖后台激活、最小化恢复、身份变化拒绝、失败门禁、脚本单次启动、完成与退出顺序及保护恢复。单测属于 MOCK／本地文件合同；独立 Windows 窗口探针验证前台 API 路径，IN_GAME／VISUAL 和长时稳定性另行验收。

旧桌面选档曾依赖唯一时间戳、焦点和鼠标输入；默认脚本流程不再调用这段路径。游戏桥按存档文件名加载，并将游戏引擎状态与本地存档哈希分别记录。采集前的进程响应检查仍执行；它不替代画面、视觉效果或性能验收。

## 本地配置与依赖

将 `target.example.json` 复制到仓库根 `.local/game-test/witcher3-steam.json`，填入原有的 `default_save`、`expected_hdr_monitor` 和 `performance_target_fps`。该目录被 Git 忽略；不要提交实际存档标识、显示器身份或运行凭据。已有本地配置无需重新生成。

`--target`／`-TargetPath` 指定该文件。适配器仍限于巫师3 Steam；换配置不表示支持其他游戏。`--output-root`／`-OutputRoot` 只接受本仓库 `tmp/` 或 `artifacts/` 下的目录；工作区互斥锁不随输出目录改变。

多显示环境可同时配置 `expected_hdr_adapter_luid`（查询返回的16位十六进制字符串）和 `expected_hdr_target_id`（JSON uint32 整数）。入口只接受唯一匹配该身份的 active 对象，并继续核对 `expected_hdr_monitor` 精确名称与 HDR enabled；不按同名对象的顺序猜测。仅配置一个字段、身份缺失／不匹配、重复身份或 HDR 未启用均在安装前拒绝。两字段省略或均为 null 时，保留原有“恰好一个 active 输出”的检查。先只读确认物理输出映射，再填写本地配置；该选择不会改变系统显示设置。

需要 Python 3（含 Pillow）、PowerShell 7、已登录的 Steam；桌面采集另需可交互的 Windows 桌面。通过 -Python 明确传入解释器，不依赖 WindowsApps 占位命令。默认脚本后端不需要 OCR；HDR 查询工具使用 MSVC 14.44／SDK 26100，不随源码分发其生成 EXE。

Windows 入口会在替换安装文件前，通过合成文件的最终句柄路径检查 AppData 是否被应用环境重定向。发生重定向时，整个入口从当前用户的普通桌面上下文重新运行；备份、游戏启动、存档保护和恢复继续处于同一上下文。验证失败会拒绝安装，不以包身份查询或日志中的逻辑路径代替实际落点检查。该流程不改变插件保存路径、系统配置或宿主，启动输出仍实时转发给原采集流程。

已经运行的平台客户端可能保留旧启动环境；首次切换前须正常退出并从普通桌面环境重启 Steam，不接管已有游戏。预设仍保存到普通 `%LOCALAPPDATA%\NR Nexus\Presets`。排查时可单独运行 `normal_process.py --probe`（正常返回0，重定向返回2，检查失败返回1）；直接调用 `runner.py` 遇到重定向会拒绝启动，应改用上述 PowerShell 入口。

## 不启动游戏的回归

在仓库根运行，`$pythonPath` 为已核验的解释器绝对路径：

```powershell
& $pythonPath -B -m unittest discover -s tools/game-test -p 'test_*.py' -v
```

只验证静态采集合同可使用独立 CPU／MOCK 入口；窗口 API、进程和输入均为替身：

```powershell
& $pythonPath -B -m unittest discover -s tools/game-test -p test_static_capture.py -v
& $pythonPath -B -m unittest discover -s tests -p test_dev_control.py -v
```

单测包含真实 Windows 文件共享锁检查和 mock 编排检查，不启动游戏。符号链接用例可能因系统权限跳过。

真实 Windows 后台／最小化激活探针使用两个短命独立测试窗口，不启动游戏、不发送键鼠输入：

```powershell
& $pythonPath -B ./tools/game-test/probe_foreground.py --output-root tmp/automatic-foreground-probe
```

普通桌面上下文可通过 `normal_process.py --run <Python绝对路径> -B <probe_foreground.py绝对路径> --output-root <本仓库tmp绝对路径>` 运行同一探针。核对 result.json 的各场景和 cleanup，不能将请求激活当作实际成功。

NR 编辑复用静态对照使用 PowerShell 入口的 `-NrEditReuse 0|1`（逐帧基线／x2候选）。
它要求 `-RestoreInstallation -NrPreset 2 -NrPasses 1 -NrHookPoint 2 -BlockOptionalHudless -XeFGUiComposition 0`，
要求实际输出为 FP16 scRGB 或10位 HDR10/PQ；不能与 ControlMask case、SR-only 或 InstallOnly 混用。
两组都临时关闭自适应、Look、Detail stability、ControlMask 和 UI 校正，固定100%尺寸并要求真实 guides；
除缓存开关外设置相同，实际设置写入 receipt，沿原备份和完整恢复流程。
可用受保护 `-CaptureScript` 采集静态日志／画面；实际 SDK 状态、NR Evaluate 频率和动态画质分别判断。
先从运行日志核对实际颜色格式与 `hdr` 契约；系统 HDR 开启不能替代实际格式核验。PQ 须为 format24／hdr2，
且实际颜色合同为 PQ／BT.2020／10000 nit／Encoding4；合同不符回到逐帧 NR。以 `NR edit reuse` 和服务 Evaluate
频率确认缓存生效，不能把配置开关或 runner 的启动／退出通过当作功能通过。

## 后续获授权的实机入口

下列步骤会写本地输出；完整入口还会临时替换游戏文件并启动游戏。执行前核对进程、候选 manifest、目标安装和存档，并取得本轮实机授权。

所有 smoke 游戏验收每轮都要显式指定本轮存档文件名（PowerShell 入口 -Save，Python 入口 --save）。入口会在启动前核验文件存在并记录存档身份；不使用 target 配置中的默认存档。

```powershell
# 仅构建并查询显示状态；不改变 Windows HDR。
& ./tools/game-test/hdr-state.ps1 -Command query

# 本轮要求完整恢复原安装；必须显式传入恢复开关。
& $pythonPath -B ./tools/game-test/fg_acceptance.py `
    --candidate-manifest $candidateManifest `
    --game-binary-directory $gameBinaryDirectory `
    --target .local/game-test/witcher3-steam.json `
    --save $saveName `
    --python $pythonPath `
    --output-root artifacts/game-tests `
    --hdr-tool tmp/hdr-state/hdr-state.exe `
    --restore-installation
```

`--restore-installation` 只透传 wrapper 原有的 `-RestoreInstallation`，使其在游戏退出后恢复原 DLL 和完整配置；原存档保护及恢复校验由既有 runner 承担。进程仍存活时保留 `restore_pending`，不能将该状态报告为已恢复。默认留装分支仍为旧逻辑，因此本轮禁止省略该开关。

直接 wrapper 入口可省略 -Python；工具会优先使用仓库的 .local/game-compare-venv/Scripts/python.exe，并忽略 WindowsApps 占位符，也可显式用 -Python $pythonPath -TargetPath 指定目标配置。-OutputRoot 指定输出目录，-RestoreInstallation 控制完整恢复。普通 smoke／menu／calibrate 入口为 tools/run-game-test.ps1，继续接受原 -Game/-Platform/-Scenario/-Save/-CaptureHoldSeconds/-Retries/-Preflight 参数，需要自动采集时附加 -CaptureScript 指向 tools/ 或 tmp/ 下的 Python 脚本。-Preflight 解析目标并打印本地配置；默认脚本后端不构建 OCR，也不启动或安装游戏。

`-IsolateOverlayKey` 将本轮 ReShade 叠加层热键临时切到 F7，并在结束后恢复。游戏菜单、存档和退出由脚本桥处理；采集脚本需要开关叠加层时使用对应热键，并在完成采集前确认面板已关闭。

IME 等交互专项可在 `menu` 场景显式传 `-CaptureHoldSeconds 900`，只进入主菜单、核验实际加载插件并等待，不加载存档；`smoke` 仍在载档后等待。范围为 0 或 10–900 秒，默认不变；同步在本地目标副本中给 `run_timeout` 留足启动、采集和退出时间。完成后向本次 `capture-ready.json` 的同一 PID 写入 `capture-complete.json`，内容为 `{"pid": <本次PID>, "completed": true}`；超时仍记为未完成验收。发送前必须取消未提交组合、关闭原生编辑窗及 ReShade 叠加层，并核对实际游戏画面，随后由既有菜单退出流程收尾，保护持续到退出恢复完成。

由用户操作的 IME 验收须额外传 `-ManualImeSession -Scenario menu -RestoreInstallation`，不自动重试。此模式到期只请求介入并继续保护，不向游戏发退出按键；即使工作线程／看门狗失败，恢复路径也保持文件锁等待用户退出，不强退游戏。完成输入后先关闭编辑窗及叠加层，写入上述完成信号，再由用户从游戏菜单正常退出。实际退出、插件 detach 日志、崩溃检查及原文件恢复仍分别验证；提前退出或没有完成信号不记为通过。无人处理时保护会一直保持到所属游戏进程退出，不能关闭保护器进程代替收尾。

人工模式启动后不执行 OCR、截图、激活窗口或按键；程序仅确认本次进程／DX12 路径和实际加载模块后等待。用户负责进入主菜单并操作界面；自动记录的进程路径不等于已确认主菜单。非人工 menu／smoke 由脚本桥处理，不执行 OCR 菜单导航。脚本桥使用 [Rusty Witcher 3 Debugger](https://github.com/SpontanCombust/rusty_witcher_debugger) v0.8.0 CLI；将 rw3d_cli.exe 放在被忽略的 .local/game-test/rw3d_cli.exe，SHA-256 必须为 0f04cccc69a9d833d59006fc9a7ff57b6342ac0c7a5b4c985d9379fbc07adf4c。不自动下载或随项目分发；runner 每个脚本请求调用一次 CLI，并记录退出码及回执。冻结 worker 快照包含 runner、desktop、guard、normal_process、桥接客户端及脚本 Mod；依赖导入回归不启动游戏。

双模型定向验证可向直接 wrapper 传 `-NrRuntimePath <310.9.16-DLL>`、
`-NrPreset 0|1|2|3`、`-NrPass2Preset 0|1|2` 和 `-NrPasses 1..4`。
逐层阶段定向验证可额外传 `-NrHookPoint 0|1|2`、`-NrPass2Stage 0..3`、
`-NrPass3Stage 0..3`、`-NrPass4Stage 0..3`；省略保留原 wrapper 的 Present 选择。
例如 `-NrHookPoint 1 -NrPass2Stage 2 -NrPasses 2 -NrPreset 1 -NrPass2Preset 2`
请求 Render A → 游戏 SR → Upscaled B。Stage 参数同样只允许完整恢复的受保护会话，
不能与 `-InstallOnly` 合用；新增参数不授权恢复已暂停的无人流程。
巫师3的 P0 SR 定向可加 `-SrOnly`，在完整备份的游戏配置中临时请求关闭 RR、DLSS FG、
XeFG 和 FSR FG；保持 HDR／分辨率。退出后完整恢复，日志须另外确认真实源 Feature 与运行路径。
`-TelemetrySeconds 1` 可为短场景临时请求逐秒性能／阶段遥测，沿用完整恢复保护；默认不覆盖游戏现有值。

固定场景的 XeFG 编辑复用成本对照可在同一候选下分别使用 `-NrEditReuse 0|1 -StartNrDisabled -XeFgGpuCapture -GpuTimers`，并通过既有 `-ControlMaskGpuSamples/-ControlMaskGpuSeconds/-ControlMaskGpuWarmupSeconds` 限定采样。采集器确认场景后开启 NR，等待 `control-mask-*.json` 的 `xefg_single_backbuffer_service` 终态，再关闭 NR 和退出；有效记录包含原色暂存至最终写回的 GPU 成本、刷新／复用身份及基础帧 Service 入口间隔。后者与 ReShade Present 入口间隔、XeFG SDK 输出帧计数均分别报告，不作为屏幕 scanout 测量。需要排除游戏上限时显式传 `-UncapGameFps`，仅将巫师3 `LimitFPS` 临时设为 unlimited；原文件整体备份并在退出后校验恢复。上述覆盖只允许完整恢复的测试会话，默认均不启用，测量窗口内不截图。
仅指定的键会临时变化；Runtime 先核对版本和 SHA-256，与已安装文件相同则直接复用，
不同才备份替换并在退出后恢复。恢复哈希核验完成且游戏／CrashReporter 已退出时自动删除冗余二进制备份，保留版本、哈希及回执；恢复未完成时继续保护备份。必须同时使用 `-RestoreInstallation`，不以创建
参数日志代替实际网络确认或画质／性能验收。

runner 与 wrapper 复用公共测试产物生命周期。结果写出后自动删除导航／OCR 截图；失败保留关键样本，恢复核验后释放存档副本，配置副本保留供验收使用。Full／Acceptance 与 Standard 同样收尾；显式 `NR_NEXUS_TEST_KEEP_RAW=1` 才保留完整数据。tmp/ 与 artifacts/ 共享默认 10 GiB 软预算（`NR_NEXUS_TEST_BUDGET_BYTES`），预算只用于额外收缩，设为 0 不影响正常收尾。活动任务、恢复材料及明确保留的证据不受预算删除。

看门狗发现持续窗口无响应或整轮超时时，会先结束采集器及 worker，在游戏恢复前用独立进程保存 `hang-evidence/game.dmp` 和结果清单。转储通过 Windows DbgHelp 捕获线程栈、线程信息及模块，不包含完整进程内存；工具核对所属 PID、创建时间和路径，默认限时20秒。证据仅留本地，失败或超时仍继续原有受保护恢复，结果不会因此转为通过。

画面停止但窗口仍响应时，窗口无响应检查可能不触发。可在本次受保护会话仍存活时，从另一个终端保存一次转储；`$runDirectory` 必须指向本次 runner 输出，输出目录必须尚不存在。此命令不操作游戏输入、不结束游戏，也不释放原 runner 的保护：

```powershell
& $pythonPath -B ./tools/game-test/hang_evidence.py `
    --process "$runDirectory/owned-process.json" --output "$runDirectory/hang-evidence-manual"
```

启动异常需要在进程退出前预先附加。已获本轮采证授权时，可在所属进程文件出现后立即运行以下独立观察器；它重新核对 PID、创建时间和路径，最多观察180秒，在首个 `C00000FD` 或任意第二次机会异常暂停期间采集一次，再继续原异常并解除附加，不操作游戏输入或关闭游戏：

```powershell
& $pythonPath -B ./tools/game-test/startup_evidence.py `
    --process "$runDirectory/owned-process.json" --output "$runDirectory/startup-evidence" `
    --stop-file "$runDirectory/stop-startup-evidence.json" --timeout 180 --capture-timeout 20 `
    --module-seeds "$runDirectory/module-seeds.json"
```

输出目录须为新目录，解释器和目标须为原生 Windows x64。可省略 `--module-seeds`；指定时文件为 JSON 数组，每项含 `module`（绝对路径或模块文件名）、`sha256`、`timestamp`、`size_of_image`、`rva`，数值可写十六进制字符串。只有磁盘 SHA256、磁盘及运行模块 PE 时间戳／映像大小均匹配才读取对应一页，绝对路径种子还要求运行路径一致。采集保存异常记录、对应线程 CONTEXT、完整虚拟内存元数据和非映像可执行分配的可读区域（含同一 AllocationBase 的区域头，原始页总计上限16MiB、每分配256KiB），并复用普通栈／模块转储。普通转储无 ExceptionStream，首发异常以侧车为准；AllocationBase 本身不标识分配者。

`state.json` 的 `observing` 同时确认初始附加断点已处理且 `kill_on_exit=false`。无需继续观察时创建 stop 文件，等观察器退出并确认 `state=detached` 后再开始 UI 采集。附加会改变时序及异常派发；附加前退出、未出现目标异常、读取失败或预算截断均按实际状态记录，不视为复现通过。仅异常暂停期间读取页面，不改系统 WER。定向离线验证为 `python -B -m unittest discover -s tools/game-test -p test_startup_evidence.py -v`，包含一个自有无窗口 CPU 进程的真实附加、合成异常穿透和解除附加。

连续录制自动核验各原帧哈希，生成不超过 1280×720 的 WebP 回看预览并核验解码帧数／时间间隔；保留预览、摘要、录制记录及首尾原图，自动释放其余原帧。转换失败则保留整段原帧及错误记录。预览只用于采样回看；原分辨率画质分析或完整原帧专项须在启动前显式 KEEP_RAW。

已授权的历史游戏产物清理可运行 `python tools/game-test/cleanup_artifacts.py --root artifacts`，只接入有完整 guard／恢复／退出证据的旧运行；游戏或 CrashReporter 仍在运行时拒绝执行。

生产路径使用 owned backend。旧诊断参数 `-ControlMaskUseCoreContext`／`--control-mask-use-core-context` 保留名称，但在安装和游戏启动前明确拒绝；不再通过游戏入口启用 P0 Core 借用。depth-far 游戏 case 使用 Present 路径。P0 Core 诊断改用离线 `tools/run-tests.ps1 -Suite fg-native -NativeCase control-mask-safety`；真实 NGX 单阶段诊断可选 `control-mask-native-ngx-toggle`，沿用该入口要求的 SDK／Runtime／ProjectID 参数。
`-ControlMaskAutoMask 0|1` 与 `-ControlMaskBlendMode 0..3` 可在同一受保护 case 上覆盖对应测试参数；省略保留 case 默认值。无显式 case 时提前拒绝，receipt 记录请求值，实际采用仍须核对运行日志。
`fg_acceptance.py` 可用 `--capture-script <tools/ 或 tmp/ 下的 Python 脚本>` 将 capture-ready 阶段委托给受保护 runner 启动的采样流程；`--start-nr-disabled` 使该流程从 NR OFF 开始，并且必须与采样脚本同时指定。省略时保持原有静态采集。

ControlMaskDepthNear 和 ControlMaskDepthFar 须成对指定，仅用于 depth-far case，并满足 0 <= Near < Far <= 1。Python入口参数为 --control-mask-depth-near 与 --control-mask-depth-far；receipt 记录请求值，首次 ControlMask active 日志也记录实际采用的深度范围。

需要同帧模型／遮罩定位时，PowerShell 包装器可显式传 `-ModelDiagnosticCapture`：本轮截图键临时改为 F10，捕获来源为 Evaluated model buffers，并开启可选诊断平面；整个 ReShade.ini 仍按原流程备份和恢复。F5 在巫师 3 同时会快存，诊断采集不使用它。成功执行遮罩的 Inline 最后求值层可随同帧模型输入／输出导出 `control_mask`，远景模式另导出本次实际 Depth Subrect 的 `control_mask_depth`。仅在有模型诊断请求且开启平面时读回；一般场景图、API 成功或请求参数不能代替这些 GPU 数据。

完整 FG 入口保持原 HDR 显示条件检查、NR 开关／移动及退出流程；显示器名称和性能目标来自本地 target。测试通过仅覆盖相应保护与编排合同，截图仍需单独判断。

性能对照前核对原 `NRPresentBlockHudless` 值：入口省略 `--block-optional-hudless` 时临时设为 `0`，传入时临时设为 `1`。应与既有验收条件一致，分别记录两个颜色面和单面路由，不能将不同路由的 FPS 直接比较；完整恢复开关会在退出后恢复原值。

XeFG A/B/C 可复用同一候选和目标配置：默认 `--nr-hook-point 2` 运行 Present，`--block-optional-hudless` 仅把 AUTO Present 固定为 Backbuffer 单颜色，`--nr-hook-point 0` 选择 Upscaled 对照。可用 `--nr-passes 1|2` 在受保护会话里临时指定 Pass 数；省略时保留原配置值。每轮均显式使用 `--restore-installation`；`--gpu-timers` 开启逐颜色／Pass GPU Timer，`--telemetry-seconds 1` 记录逐秒实际 Evaluate 计数。比较时保持模型 Preset、工作分辨率、Pass 数及 HDR 条件一致；结果会分别保留应用／显示 FPS、FG 服务 Evaluate 计数、遥测和 GPU Timer 行，视觉质量仍须独立人工判断。

单颜色适用条件还要求 UI Composition 关闭。`--xefg-ui-composition 0|1`（PowerShell：`-XeFGUiComposition 0|1`）仅在完整恢复的受保护会话中，临时修改 `OptiScaler.ini` 已有 `[XeFG] UIComposition`；省略保留用户值，非 AUTO 或启用 UI Composition 时可能保留双链。原文件先备份，退出后整文件校验恢复，不修改 SDK 行为。入口会给临时 NR 策略写入迁移标记，使明确的单／双链测试选择不被一次性升级覆盖；生产迁移由配置测试单独验证。纯合成变换检查为 `tests/run-fg-acceptance-config-tests.ps1`。

XeFG SDK 隔离候选使用 `--nr-off-control --disable-xefg-sdk-detours`，并且必须同时使用 `--start-nr-disabled`（PowerShell：`-StartNrDisabled -DisableXeFGSdkDetours`）和 `--restore-installation`。该开关在启动前决定，整个会话不安装 Nexus 的 XeFG SDK 导出／Swapchain Detour；不在运行中的 Context 上拆卸 Hook，也不通过关闭 XeFG 伪造视觉成功。隔离分支只验证候选加载、NR Evaluate=0、退出和恢复；SDK 资源／UI Composition 到达情况标记为不可观测，`routing_passed=null`，必须另做相同场景的动态人工对照。

正常 Detour 分支的 `ReShade.log` 可用有限样本 `resource_contract` 和 `ui_composition_contract` 核对：原始／转发 Tag、Present ID、有效期、尺寸、资源是否替换、底层 SDK 返回值及实际应用状态。日志中的 SDK 成功、配置值或 `ui_composition=1` 只证明调用／状态契约，不能替代小地图透影和连续运动画质验收。

逐窗口 Evaluate 必须读取 telemetry 的 nr[evals=…]，不能取 nr_stage[evals=…] 累计值。前者用于匹配本窗口的应用帧数和 NR On／Off，后者仅为阶段累计诊断；修正解析后可用已保存的完整日志重新验证，保留修正前派生结果和原始会话记录，不必重新启动游戏。
