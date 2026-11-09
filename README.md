# Macify

> [!WARNING]
> **This app was written with AI (Claude) for my own private use.**
>
> It's shared as-is, with no support, no guarantees and no promise of updates. It's
> built for one specific laptop setup (a Precision Touchpad and a Swedish keyboard
> layout) and may not work, or may behave unexpectedly, on yours.
>
> Macify runs with administrator rights and intercepts your keyboard, mouse and
> touchpad input. **Read the code before you run it**, and use it entirely at your own
> risk.

Small Windows background app that makes a Windows laptop behave more like a Mac:

- **Three-finger drag** on Precision Touchpads, like macOS: put three fingers down and move
  to drag windows or select text, as if holding the left button.
- **Mac-style shortcuts**: the left Windows key acts as Ctrl (Cmd), plus a handful of
  shortcuts for a Swedish keyboard layout (see [Keyboard](#keyboard)).
- **Ctrl+click and Alt+click** give a plain right-click, and a three-finger drag started
  with Ctrl or Alt held is a right-button drag.

No settings window: a few settings live in a small file (see [Settings](#settings)), the
rest are constants at the top of [`macify.c`](macify.c). A tray icon has a menu to edit
the settings, relearn the pointer speed, turn start-with-Windows on or off, record a debug
log and quit.

## Three-finger drag

- The cursor follows the **centre of the three fingers**: every finger is tracked, and the
  cursor moves by their average movement.
- **Pointer speed matches Windows exactly.** While you use one finger normally, Macify
  compares your finger's movement with how far Windows moves the cursor and learns
  Windows' speed curve from that. The curve is saved in `%APPDATA%\Macify\speed-curve.txt`
  and relearned automatically if you change the touchpad or mouse pointer settings.
- **Lift and re-place** your fingers within 450 ms to continue a drag. Moving the cursor in
  that time drops immediately, where the drag ended.
- **No jumps**: movement is ignored for a moment while fingers land or lift (many touchpads
  briefly report fingers side by side as one), fingers are paired by position rather than
  by contact ID, and an impossibly sudden jump is held back until it is clear whether it is
  real (some touchpads briefly report several times the real movement when a sideways drag
  starts fast). None of this limits how fast you can drag.

## Keyboard

| Keys | Result |
|---|---|
| Left Win | Ctrl |
| Ctrl+D, Ctrl+Backspace | Delete |
| Ctrl+2, 3, 4, 7, 8, 9, 0 | AltGr symbol on that key (`@ £ $ { [ ] }`) |
| Ctrl+`<`, Ctrl+`´` | `\|` |
| Ctrl+H | Win+M (minimise all) |
| Ctrl+I | Ctrl+Shift+C |
| Ctrl+Shift+7 | Alt+Shift+A |
| Ctrl+click or Alt+click | right-click (physical Ctrl key only; Win+click stays Ctrl+click) |

"Ctrl" includes the left Win key. Each shortcut fires only with exactly those modifiers
held, the way AutoHotkey hotkeys do.

## Setup

1. In **Settings → Bluetooth & devices → Touchpad**, set three-finger swipes and taps to
   **Nothing**, and turn off **Tap twice and drag to multi-select**.
2. Run `macify.exe` from wherever you downloaded it and accept the UAC prompt. Macify
   copies itself to `C:\Program Files\Macify` (it runs with admin rights at login, so it
   lives where only admins can replace it), registers a scheduled task (`Macify`) that
   starts it with admin rights at every login without further prompts, adds a **Macify**
   Start menu shortcut, and starts. A notification confirms it's running.

If you decline the UAC prompt, Macify runs from where it is, without admin rights, and
doesn't ask again; tray icon → **Start with Windows as administrator** installs it later.

Built for Windows 10/11 with a Precision Touchpad.

## Settings

Tray icon → **Edit settings** opens `%APPDATA%\Macify\settings.json`. Changes apply as
soon as you save; a notification confirms them, or says what's wrong with the file.

```json
{
  "threeFingerDragAsOneFinger": false,
  "threeFingerDragSpeed": 1.0
}
```

| Setting | Meaning |
|---|---|
| `threeFingerDragAsOneFinger` | `false` (default): the cursor follows the centre of the fingers, like macOS. `true`: it follows whichever finger moves most, like ThreeFingerDragOnWindows |
| `threeFingerDragSpeed` | Multiplies the three-finger drag speed: `1.0` matches the normal pointer, `1.5` is 50 % faster |

## Debug log

Tray icon → **Start debug log**, reproduce the problem, then **Finish debug log**: a
`Macify debug log <date>.txt` appears on the desktop. It starts with the app's state
(Windows version, touchpad details, learned speed curve) followed by events: drags,
filtered touchpad artefacts, cursor lag, learning, and remapped shortcuts and clicks.
It never records what you type. Logging costs nothing while off.

## Updating

Tray icon → **Check for updates**. If a newer release is published here, Macify downloads
it and asks before installing: the update closes the running copy, replaces the one in
Program Files, keeps your settings and learned pointer speed, and starts. (Running a newer
`macify.exe` by hand does the same, after a UAC prompt.)

### Publishing a release

Bump the version in [`version.h`](version.h) and push to `main`. A GitHub Actions
workflow ([`release.yml`](.github/workflows/release.yml)) then builds `macify.exe` from
the source and publishes it as release `v<version>`, with the commits since the previous
release as release notes. Installed copies pick it up with **Check for updates**.

## Uninstall

1. Tray icon → untick **Start with Windows as administrator**, then **Quit**.
2. Delete `C:\Program Files\Macify`, `%APPDATA%\Macify` and the Start menu shortcut
   (`%APPDATA%\Microsoft\Windows\Start Menu\Programs\Macify.lnk`).

## Building

Cross-compiled from Linux with MinGW-w64 (`gcc-mingw-w64-x86-64`):

```sh
sh build.sh
```

This produces `macify.exe`.

The icons are generated by `make_icon.py` (needs Pillow); the generated `.ico` files are
committed so building doesn't need Python.

## License

[MIT](LICENSE).
