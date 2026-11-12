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
- **The Windows key acts as Ctrl**, in the place of a Mac's Cmd key (see
  [Windows key = Ctrl key](#windows-key--ctrl-key)).
- **Key remaps**: Mac-style shortcuts for a Swedish keyboard layout, which you can change
  in the settings file (see [Key remaps](#key-remaps)).
- **Ctrl+click and Alt+click** give a plain right-click, and a three-finger drag started
  with Ctrl or Alt held is a right-button drag.
- **Dark title bars** for apps that don't follow Windows' dark mode (see
  [Dark title bars](#dark-title-bars)).

## Three-finger drag

- The cursor follows **whichever finger moves the most**, like ThreeFingerDragOnWindows.
- **Pointer speed matches Windows exactly.** While you use one finger normally, Macify
  compares your finger's movement with how far Windows moves the cursor and learns
  Windows' speed curve from that. The curve is saved in `%APPDATA%\Macify\speed-curve.txt`
  and relearned automatically if you change the touchpad or mouse pointer settings.
- **Lift and re-place** your fingers within 450 ms to continue a drag. Moving the cursor in
  that time drops immediately, where the drag ended.
- **No jumps**: movement is ignored for a moment while fingers land or lift (many touchpads
  briefly report fingers side by side as one), fingers are paired by position rather than
  by contact ID, and an impossibly sudden jump is held back until it is clear whether it is
  real (some touchpads briefly report several times the real movement when a drag starts
  fast; see `threeFingerDragJumpThreshold` in [Settings](#settings)). None of this limits
  how fast you can drag.

**Credit:** Macify's three-finger drag is inspired by
[ThreeFingerDragOnWindows](https://github.com/ClementGre/ThreeFingerDragOnWindows) by
Clément Grennerat, which I used before writing this. Macify is a separate implementation
and contains none of its code.

## Windows key = Ctrl key

- The **left Windows key acts as Ctrl**, sitting where a Mac's Cmd key is: Win+C copies,
  Win+W closes a tab, Win+click opens a link in a new tab, and so on.
- Since that takes the Windows key's own role, **left Alt+W acts as the Windows key**,
  held for as long as W is held: alone it opens Start, and with other keys it makes
  Windows-key shortcuts (Alt+W+E opens File Explorer, Alt+W+Shift+S takes a screenshot).
  The one exception is Alt+W+L: Windows only lets a physical Windows key lock the PC.

This is always on and can't be changed in the settings file.

## Key remaps

Shortcuts that send other keys instead. These are the defaults:

| Keys | Result |
|---|---|
| Ctrl+D, Ctrl+Backspace | Delete |
| Ctrl+Shift+3 | Win+PrintScreen (screenshot, saved to Pictures) |
| Ctrl+Shift+4 | Win+Shift+S (snip part of the screen) |
| Ctrl+1, 2, 3, 4, 7, 8, 9, 0, `+`, `<` | The AltGr character on that key (such as `@ £ $ { [ ] } \ \|` on a Swedish layout) |
| Ctrl+`´` | `\|` |
| Ctrl+I | Ctrl+Shift+C |
| Ctrl+`-` | Win+`-` (Magnifier: zoom out) |
| Alt+1 … 9, 0, `+`, `-` | Ctrl+the same key (switch browser tabs, zoom, …) |

"Ctrl" includes the left Windows key. A remap only fires with exactly those modifiers
held, the way AutoHotkey hotkeys do: Ctrl+D doesn't fire for Ctrl+Shift+D. Modifiers that
aren't part of the keys sent are released for that moment, so Ctrl+D sends a plain Delete.

### Changing them

The remaps are the `keyRemaps` part of the [settings file](#settings), one indented line
each, `keys you press: keys sent instead`:

```yaml
keyRemaps:
  Ctrl+D: Delete
  Ctrl+H: Win+M
```

- Keys are written as modifiers joined with `+`, then one key: `Ctrl+Shift+7`.
- Modifiers: `Ctrl`, `Alt`, `Shift`, `Win`.
- Keys: `A`–`Z`, `0`–`9`, `F1`–`F24`, `Delete`, `Backspace`, `Enter`, `Tab`, `Esc`,
  `Space`, `Left`, `Right`, `Up`, `Down`, `Home`, `End`, `PageUp`, `PageDown`, `Insert`,
  `PrintScreen`, or a character that has its own key on your keyboard layout, like `<`
  or `´` (the `+` key is written `Ctrl++`). Keys containing `#` or `:` need quotes:
  `"Ctrl+#": Delete`.
- In the keys you press, `Ctrl` also means the left Windows key, and `Win` can't be used.
  `Alt+W` is reserved for the Windows key. `Alt` is the plain Alt key: AltGr always keeps
  typing its characters, even when Alt with the same key is remapped.
- An entry with a mistake is skipped, with a notification saying why; the others still
  work. Remove `keyRemaps` to get the defaults back; `keyRemaps: {}` turns them all off.

## Ctrl+click and Alt+click

Holding the **physical Ctrl key** or **Alt** while clicking gives apps a plain right-click,
like Control+click on a Mac (Win+click stays Ctrl+click). A three-finger drag started with
Ctrl or Alt held is a right-button drag. This is always on.

## Dark title bars

Some apps keep a light title bar even when Windows is in dark mode. List them under
`darkTitleBarApps` in the [settings file](#settings) and Macify gives their windows a dark
title bar with white text and buttons, in the color set by `darkTitleBarColor`
(`"#202020"` by default), the way Mica For Everyone does:

```yaml
darkTitleBarApps:
  - Plex.exe
darkTitleBarColor: "#202020"
```

- Write each app's program file name, as shown in Task Manager → Details.
- The color is `"#RRGGBB"`, with the quotes (otherwise YAML reads `#` as a comment). The
  color only works on Windows 11; Windows 10 uses its own dark color.
- It only works for apps with the normal Windows title bar. Windows of an app you remove
  from the list keep the dark title bar until they're closed.

The list is empty by default.

## Setup

1. In **Settings → Bluetooth & devices → Touchpad**, set three-finger swipes and taps to
   **Nothing**, and turn off **Tap twice and drag to multi-select**.
2. Run `macify.exe` from wherever you downloaded it and accept the UAC prompt. Macify
   copies itself to `C:\Program Files\Macify` (where only admins can replace it, which
   matters since it can run with admin rights), sets itself up to start at login with admin
   rights (see [Settings](#settings)), adds a **Macify** Start menu shortcut, and starts.
   A notification confirms it's running.

If you decline the UAC prompt, Macify runs from where it is, without admin rights, and
doesn't ask again; tray icon → **Run as administrator** installs it later.

Built for Windows 10/11 with a Precision Touchpad.

## Settings

Tray icon → **Edit settings** opens `%APPDATA%\Macify\settings.yaml`. Changes apply as
soon as you save; a notification confirms them, or says what's wrong with the file. The
startup and update options can also be switched from the tray menu, which updates the file.

```yaml
runAtStartup: true
runAsAdministrator: true
checkForUpdatesOnStart: true
threeFingerDragSpeed: 1.0
threeFingerDragJumpThreshold: 0
darkTitleBarApps: []
darkTitleBarColor: "#202020"
keyRemaps:
  Ctrl+D: Delete
  # ... the other key remaps
```

The file starts with a comment explaining every setting. (Versions up to 1.8 used
`settings.json`; it's converted to `settings.yaml` automatically, keeping its values.)

| Setting | Meaning |
|---|---|
| `runAtStartup` | Start Macify when you log in |
| `runAsAdministrator` | Run with admin rights, so it also works in admin windows. Uses a scheduled task (`Macify`), so there's no prompt at login or when starting it from the Start menu. Turning it on asks for admin rights once |
| `checkForUpdatesOnStart` | Shortly after Macify starts, check here for a newer version. If there is one, it's downloaded in the background and a notification offers it: click it, or choose **Install Macify …** in the tray menu. Silent when there's nothing new |
| `threeFingerDragSpeed` | Multiplies the three-finger drag speed: `1.0` matches the normal pointer, `1.5` is 50 % faster |
| `threeFingerDragJumpThreshold` | Protection against touchpads that briefly report a much bigger movement than the real one (mostly when a drag starts fast), which makes the cursor jump. A sudden change of more than this many mm between two touchpad reports is held back until it's clear whether it was real. It never limits how fast you can drag. `0` (the default) turns it off; if your cursor jumps when you start a drag fast, try `3` |
| `darkTitleBarApps` | Apps that get a dark title bar, see [Dark title bars](#dark-title-bars). `[]` (the default) means none |
| `darkTitleBarColor` | Background color of those title bars, like `"#202020"` (the default) |
| `keyRemaps` | Key remaps, see [Key remaps](#key-remaps) |

## Debug mode

Tray icon → **Start debug mode**, reproduce the problem, then press **Esc** (or choose
**Finish debug mode**): a `Macify debug log <date>.txt` appears on the desktop. While debug
mode is on, Esc only finishes it and isn't passed on to apps. It starts with the app's state
(Windows version, touchpad details, learned speed curve) followed by events: drags,
filtered touchpad artefacts, cursor lag, learning, and remapped shortcuts and clicks.
It never records what you type. Logging costs nothing while off.

## Updating

With `checkForUpdatesOnStart` on (the default), Macify looks for updates shortly after it
starts and offers a new version in a notification. Or check yourself any time: tray icon →
**Check for updates**. If a newer release is published here, Macify downloads
it and asks before installing: the update closes the running copy, replaces the one in
Program Files, keeps your settings and learned pointer speed, and starts. (Running a newer
`macify.exe` by hand does the same, after a UAC prompt.)

### Publishing a release

Bump the version in [`version.h`](version.h) and push to `main`. A GitHub Actions
workflow ([`release.yml`](.github/workflows/release.yml)) then builds `macify.exe` from
the source and publishes it as release `v<version>`, with the commits since the previous
release as release notes. Installed copies pick it up with **Check for updates**.

## Uninstall

**Settings → Apps → Installed apps → Macify → Uninstall**, like any other app. After a
confirmation and a UAC prompt, Macify closes and removes everything it added: the program
in `C:\Program Files\Macify`, the `Macify` scheduled task, the startup entry, the Start
menu shortcut, and your settings and learned pointer speed in `%APPDATA%\Macify`.

(Running `"C:\Program Files\Macify\macify.exe" --uninstall` does the same. Copies
installed before 1.18 show up in Installed apps once they've updated to 1.18.)

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
