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

`assets/no-signal/` 和 `src/no_signal_frames.inc` 不再进入运行时。
