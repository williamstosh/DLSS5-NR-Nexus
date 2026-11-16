# NR Nexus — 后续更新 / Upcoming changes

[简体中文](#简体中文) · [English](#english)

## 简体中文

以下改动仅在开发版中可用，尚未发布。

### 修复内容

- 修复在 Present 模式下开启 XeFG 或载档后 NR 可能持续没有提交、需打开菜单才恢复的问题，并恢复关闭 XeFG 后的普通 Present 处理。
- 修复载档或关闭 XeFG、回到普通 Present 时可能出现的崩溃。
- 消除启用 XeFG 时一次不必要的 NR 交接等待。

### 验证情况

普通 Release 构建及 WARP 软件 GPU／模型替身定向验证通过。Mafia DX11、巫师 3 DX12 的游戏接入与实际画质仍待验证。

## English

These changes are available in development builds and have not been released.

### Fixes

- Fixed an issue where enabling XeFG or loading a save with Present processing could leave NR without submissions until a menu was opened, and restored ordinary Present processing after XeFG is disabled.
- Fixed a possible crash while loading a save or disabling XeFG as processing returned to ordinary Present.
- Removed an unnecessary NR handoff delay when enabling XeFG.

### Validation

The ordinary Release build and targeted WARP software-GPU/model-substitute checks passed. In-game integration and image quality in Mafia DX11 and The Witcher 3 DX12 still need validation.
