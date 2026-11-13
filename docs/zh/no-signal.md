# 无信号画面

[English](../en/no-signal.md) | 简体中文

`onekvm-ui` 把 PNG 放进语言包 `/usr/share/onekvm/l10n/<语言>/no-signal/`。
`onekvm-no-signal-cache` 把这些 PNG 转成 NV21，写到
`/data/onekvm/cache/no-signal/<语言>/`，并把 `current` 指到当前目录。
`/data/onekvm/personalization/no-signal.png` 存在时，画面改用留黑边后的
`custom/`，移除该图片后回到当前语言。

`soph-mmf.so` 读取 `/data/onekvm/cache/no-signal/current/<宽>x<高>.nv21`。
文件不存在时输出黑帧。其他输出尺寸选用最接近的缓存并留黑边。`current`
指向变化后，后端重新读取，并以 IDR 开始下一帧。

同一 NV21 内存缓冲区也可能被新图片覆盖。缓存身份变化时必须同时使两块
VPSS 用户帧 DMA 副本失效，分别重新转换并刷新后再复用；只比较源指针会继续显示旧图。

占位画面没有采集时钟，按编码器输出帧率送帧（默认 60 fps）。soph-v4l2.so 原先每次
上层调用都送一帧，编码器一返回就送下一帧；107 1080p H.264 无信号实测（各 2 轮）：

| soph-v4l2.so | 送帧节拍 | VENC 帧率 | 码率 | Core CPU |
| --- | --- | --- | --- | --- |
| 自动码率 | 不限 → 输出帧率 | 102–103 → 60 fps | 8.1–8.2 → 4.9–5.0 Mbps | 32% → 19–20% |
| 上限 500 kbps（开跳帧） | 不限 → 输出帧率 | 314–320 → 59–60 fps | 2.13–2.15 → 0.31–0.34 Mbps | 47% → 16% |

码控按输出帧率分配每帧比特，送帧更快时码率上限守不住。soph-mmf.so 由厂商绑定路径
定节拍，本来约 55 fps，改动前后不变。

`assets/no-signal/` 和 `src/no_signal_frames.inc` 不再进入运行时。
