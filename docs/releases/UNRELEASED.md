# NR Nexus — 后续更新 / Upcoming changes

[简体中文](#简体中文) · [English](#english)

## 简体中文

以下改动仅在开发版中可用，尚未发布。

### 修复内容

- 修复使用 Present 时，开启 XeFG 后 NR 可能持续显示没有提交的问题，并恢复关闭 XeFG 后的普通 Present 处理。
- 修复载档或关闭 XeFG、回到普通 Present 时可能出现的崩溃。

### 验证情况

普通 Release 构建及 WARP 软件 GPU／模型替身定向验证通过。Mafia DX11、巫师 3 DX12 的游戏接入与实际画质仍待验证。

## English

These changes are available in development builds and have not been released.

### Fixes

- Fixed an issue where enabling XeFG with Present processing could leave NR without submissions, and restored ordinary Present processing after XeFG is disabled.
- Fixed a possible crash while loading a save or disabling XeFG as processing returned to ordinary Present.

### Validation

The ordinary Release build and targeted WARP software-GPU/model-substitute checks passed. In-game integration and image quality in Mafia DX11 and The Witcher 3 DX12 still need validation.
