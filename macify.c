// Macify: Mac-style three finger drag, keyboard shortcuts and Ctrl/Alt-click for Windows.
//
// No settings UI: all preferences are the constants below; edit and rebuild.
// A tray icon (three dots) has a Quit item.
// Before use, in Windows Settings > Bluetooth & devices > Touchpad:
//   - Three-finger gestures: Swipes = Nothing, Taps = Nothing
//   - Turn off "Tap twice and drag to multi-select"
// To stop it: tray icon > Quit (or taskkill /im macify.exe)
// Installing: run macify.exe from anywhere (e.g. Downloads). After one UAC prompt it copies
// itself to C:\Program Files\Macify, registers the startup task for that copy and starts
// it. Running a newer macify.exe the same way updates it: the running copy is closed and
// replaced. It also keeps a "Macify" Start menu shortcut.
// Start with Windows as administrator: on first run (one UAC prompt) it registers a
// scheduled task "Macify" that starts it elevated at logon. Whenever it is
// started without admin rights while that task exists, it starts the task instead, so
// there is never another prompt. Turn it on/off from the tray menu.
//
// How the drag is computed (the part that differs from ThreeFingerDragOnWindows):
//   - Every contact is tracked by its contact ID between frames. The cursor moves by
//     the AVERAGE movement of the fingers that were on the pad in both frames, i.e. the
//     movement of the group's centre. Moving one finger alone moves the cursor 1/3 as far.
//   - Fingers are paired between frames by position, not by the touchpad's contact IDs.
//   - When a finger lands or lifts, movement is ignored for SETTLE_MS: many touchpads
//     report fingers side by side as one blob and then split it, which would otherwise
//     look like a huge, instant movement.
//   - A finger that suddenly moves very differently from the others in one frame is
//     left out (sensor artefact). A real drag moves all fingers together, so this never
//     limits speed.
//   - Some touchpads briefly report several times the real movement for all fingers
//     (seen when three fingers start moving sideways). A real hand can't change speed
//     that abruptly, so such a jump is held back for up to SPIKE_HOLD_MS: if it snaps
//     back it was an artefact and is discarded; if it persists it is applied in full.
//   - Movement is timed with the touchpad's own scan timestamps, not message arrival.
//   - The cursor position is kept as a float and never re-read mid-drag, so there is
//     no race with Windows' own input queue.
//
// Keyboard and mouse remaps (replace an AutoHotkey script; see "remaps" below):
//   Left Win acts as Ctrl. Ctrl+D / Ctrl+Backspace = Delete. Ctrl+2/3/4/7/8/9/0 and
//   Ctrl+< / Ctrl+´ = the AltGr symbol on that key. Ctrl+H = Win+M. Ctrl+I = Ctrl+Shift+C.
//   Ctrl+Shift+7 = Alt+Shift+A. Physical Ctrl or Alt + click = plain right-click, and a
//   three-finger drag started with Ctrl or Alt held is a right-button drag.
//
// Earlier versions were called three-finger-drag: their settings, startup task and
// Start menu shortcut are taken over automatically.
//
// Settings file: %APPDATA%\Macify\settings.json (tray menu > Edit settings), created
// with the defaults, picked up as soon as it is saved:
//   "runAtStartup": true                start Macify when you log in
//   "runAsAdministrator": true          run with admin rights (so it also works in admin
//                                       windows) via a scheduled task, without prompts
//   "threeFingerDragAsOneFinger": true  true: the fingers move the cursor together, as
//                                       one (their centre, like macOS); false: the cursor
//                                       follows whichever finger moves the most, like
//                                       ThreeFingerDragOnWindows
//   "threeFingerDragSpeed": 1.0         multiplies the three-finger drag speed
//
// Pointer speed is learned, not configured: while you use one finger normally, the app
// compares how far (and how fast) your finger moved with how far Windows moved the
// cursor, and builds Windows' speed curve from that. The drag then uses that curve, so
// it matches your touchpad settings. The curve is saved in
// %APPDATA%\Macify\speed-curve.txt and relearned if pointer settings change.

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <winternl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <shlobj.h>
#include <objbase.h>
#include <hidusage.h>
#include <hidpi.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "version.h"

// ---------------------------------------------------------------- preferences

#define DRAG_FINGERS              3      // fingers that start a drag
#define START_THRESHOLD_MM        1.0    // group movement before the button goes down
#define RELEASE_DELAY_MS          450    // grace period to lift and re-place fingers
#define CANCEL_MOVE_PX            2      // cursor movement during the grace period that drops immediately
// Speed curve used only until enough has been learned:
#define MIN_GAIN_PX_PER_MM        7.0    // cursor speed when moving slowly
#define MAX_GAIN_PX_PER_MM        22.0   // cursor speed when moving fast
#define ACCEL_LOW_MM_PER_S        20.0   // below this speed: MIN_GAIN
#define ACCEL_HIGH_MM_PER_S       260.0  // above this speed: MAX_GAIN
#define SETTLE_MS                 30.0   // ignore movement this long after a finger lands or lifts
#define OUTLIER_MM                3.0    // per-frame disagreement with the other fingers that marks an artefact
#define MAX_ACCEL_MM_PER_MS2      0.04   // speed change no real hand makes within one frame (40 m/s^2)
#define SPIKE_HOLD_MS             80.0   // how long a sudden jump is held back before it is trusted
#define USE_PALM_REJECTION        true   // ignore contacts the touchpad marks as not confident
#define FALLBACK_PAD_WIDTH_MM     105.0  // used only if the touchpad reports no physical size
#define FALLBACK_PAD_HEIGHT_MM    68.0

// ---------------------------------------------------------------- HID usages

#define PAGE_GENERIC        0x01
#define PAGE_DIGITIZER      0x0D
#define USAGE_X             0x30
#define USAGE_Y             0x31
#define USAGE_TOUCHPAD      0x05
#define USAGE_TIP_SWITCH    0x42
#define USAGE_CONFIDENCE    0x47
#define USAGE_CONTACT_ID    0x51
#define USAGE_CONTACT_COUNT 0x54
#define USAGE_SCAN_TIME     0x56

#define MAX_CONTACTS 10
#define MAX_DEVICES  8
#define TIMER_RELEASE 1
#define TIMER_TRAY_RETRY 2
#define WM_TRAY (WM_APP + 1)
#define WM_CANCEL_DRAG (WM_APP + 2)
#define TIMER_SAVE 3
#define TIMER_SETTINGS 4
#define TASK_NAME L"Macify"
#define OLD_NAME L"three-finger-drag"  // previous name: data folder, task, mutex
#define ID_QUIT 100
#define WM_HOOK_LIFT (WM_APP + 3)
#define INPUT_MAGIC 0x54464421  // dwExtraInfo on everything we inject, so our hooks ignore it
#define ID_RELEARN 101
#define ID_STARTUP 102
#define ID_ADMIN 106
#define ID_DEBUG 103
#define ID_SETTINGS 104
#define ID_UPDATE 105
#define WM_UPDATE_RESULT (WM_APP + 5)
#define UPDATE_API L"https://api.github.com/repos/adamakesson/macify/releases/latest"

// speed-curve learning
#define LEARN_BINS          16
#define LEARN_MIN_SPEED     4.0     // mm/s, lowest bin
#define LEARN_MAX_SPEED     600.0   // mm/s, highest bin
#define LEARN_WINDOW_MS     50.0    // measure finger vs cursor over windows this long
#define LEARN_BIN_CAP_MM    400.0   // older data fades once a bin has seen this much movement
#define LEARN_BIN_READY_MM  25.0    // movement a bin needs before it is trusted
#define SPEED_SMOOTH_MS     25.0    // smoothing of finger speed while dragging
#define SAVE_INTERVAL_MS    30000

// Debug log: off unless switched on from the tray menu; then lines are collected in
// memory and saved to the desktop on "Finish debug log". Costs one check while off.
static volatile bool gDebugOn;
static void debugLog(const char *fmt, ...);
#define LOG(...) do { if (gDebugOn) debugLog(__VA_ARGS__); } while (0)
static void debugStart(void);
static void debugFinish(void);
static void settingsEdit(void);
static void settingsLoad(bool userEdit);
static void settingsWrite(void);
static void quit(void);
static void updateCheck(void);
static volatile bool gUpdateBusy;

typedef struct {
  ULONG id;
  bool tip;
  bool confident;
  double x, y;  // millimetres
} Contact;

typedef struct {
  HANDLE handle;
  PHIDP_PREPARSED_DATA pp;
  USHORT slots[MAX_CONTACTS];
  bool slotHasConfidence[MAX_CONTACTS];
  int slotCount;
  bool hasCount;
  USHORT countLink;
  double mmPerUnitX, mmPerUnitY;
  bool usable;
  // hybrid-mode frame assembly: one frame may span several reports
  int expected;
  int collected;
  int scanTime;  // 100 us units, -1 if the touchpad doesn't report it
  Contact frame[MAX_CONTACTS];
} Device;

typedef enum { IDLE, ARMED, DRAGGING } State;

static HWND gWnd;
static Device gDevices[MAX_DEVICES];
static int gDeviceCount;

static State gState = IDLE;
static bool gReleasePending;
static bool gResync;
static Contact gPrev[MAX_CONTACTS];
static int gPrevCount;
static double gArmX, gArmY;
static double gCurX, gCurY;
static LONG gSentX, gSentY;
static LARGE_INTEGER gFreq, gLastFrame;
static HHOOK gMouseHook;
static bool gCancelPosted;
static UINT gTaskbarCreated;
static NOTIFYICONDATAW gNid;
static HICON gIcon;
static volatile HWND gHookWnd;          // remap thread's window
static volatile bool gPhysCtrl, gPhysAlt;  // physical Ctrl / Alt held (set by the remap thread)
static bool gDragRight;                 // current three-finger drag uses the right button
static HANDLE gMutex;
static UINT gQuitMsg;  // "MacifyQuit": a newer copy asks the running one to close
static void toggleSetting(bool *value);

static double gBinMm[LEARN_BINS], gBinPx[LEARN_BINS];
static bool gCurveDirty;
static int gSettingsSig[3];
static double gDragSpeed;
static double gSettleUntilMs;
static bool gAsOneFinger = true;   // settings.json: threeFingerDragAsOneFinger
static bool gRunAtStartup = true;  // settings.json: runAtStartup
static bool gRunAsAdmin = true;    // settings.json: runAsAdministrator
static double gSpeedMul = 1.0;     // settings.json: threeFingerDragSpeed
static int gLastScan = -1;
static bool gVelValid, gHolding;
static double gVx, gVy;  // trusted finger velocity, mm/ms
static double gHoldMs, gPendX, gPendY, gEntryJump;
static struct {
  bool active, warm;
  double fx, fy, ms;
  POINT start;
} gWin;

// ---------------------------------------------------------------- device setup

static USAGE capUsage(const HIDP_VALUE_CAPS *c) {
  return c->IsRange ? c->Range.UsageMin : c->NotRange.Usage;
}

static double mmPerUnit(const HIDP_VALUE_CAPS *c, double fallbackMm) {
  double logical = (double)c->LogicalMax - (double)c->LogicalMin;
  if (logical <= 0) return 0.01;
  double physical = (double)c->PhysicalMax - (double)c->PhysicalMin;
  int exp = (int)(c->UnitsExp & 0xF);
  if (exp > 7) exp -= 16;
  double unitMm = 0;
  if ((c->Units & 0xFF) == 0x11) unitMm = 10.0;       // centimetre
  else if ((c->Units & 0xFF) == 0x13) unitMm = 25.4;  // inch
  double result = fallbackMm / logical;
  if (physical > 0 && unitMm > 0) {
    double r = physical * pow(10.0, exp) * unitMm / logical;
    if (r > 0.0005 && r < 1.0) result = r;
  }
  return result;
}

static void addSlot(Device *d, USHORT link) {
  for (int i = 0; i < d->slotCount; i++) if (d->slots[i] == link) return;
  if (d->slotCount >= MAX_CONTACTS) return;
  int i = d->slotCount++;
  while (i > 0 && d->slots[i - 1] > link) { d->slots[i] = d->slots[i - 1]; i--; }
  d->slots[i] = link;
}

static void setupDevice(Device *d) {
  UINT size = 0;
  if (GetRawInputDeviceInfoW(d->handle, RIDI_PREPARSEDDATA, NULL, &size) != 0 || size == 0) return;
  d->pp = (PHIDP_PREPARSED_DATA)malloc(size);
  if (!d->pp || GetRawInputDeviceInfoW(d->handle, RIDI_PREPARSEDDATA, d->pp, &size) == (UINT)-1) return;

  HIDP_CAPS caps;
  if (HidP_GetCaps(d->pp, &caps) != HIDP_STATUS_SUCCESS) return;

  USHORT nv = caps.NumberInputValueCaps;
  HIDP_VALUE_CAPS *vc = calloc(nv ? nv : 1, sizeof *vc);
  if (nv && HidP_GetValueCaps(HidP_Input, vc, &nv, d->pp) == HIDP_STATUS_SUCCESS) {
    bool gotX = false, gotY = false;
    for (int i = 0; i < nv; i++) {
      USAGE u = capUsage(&vc[i]);
      if (vc[i].UsagePage == PAGE_GENERIC && u == USAGE_X) {
        addSlot(d, vc[i].LinkCollection);
        if (!gotX) { d->mmPerUnitX = mmPerUnit(&vc[i], FALLBACK_PAD_WIDTH_MM); gotX = true; }
      } else if (vc[i].UsagePage == PAGE_GENERIC && u == USAGE_Y) {
        if (!gotY) { d->mmPerUnitY = mmPerUnit(&vc[i], FALLBACK_PAD_HEIGHT_MM); gotY = true; }
      } else if (vc[i].UsagePage == PAGE_DIGITIZER && u == USAGE_CONTACT_COUNT) {
        d->hasCount = true;
        d->countLink = vc[i].LinkCollection;
      }
    }
    if (!gotY) d->mmPerUnitY = d->mmPerUnitX;
  }
  free(vc);

  USHORT nb = caps.NumberInputButtonCaps;
  HIDP_BUTTON_CAPS *bc = calloc(nb ? nb : 1, sizeof *bc);
  if (nb && HidP_GetButtonCaps(HidP_Input, bc, &nb, d->pp) == HIDP_STATUS_SUCCESS) {
    for (int i = 0; i < nb; i++) {
      if (bc[i].UsagePage != PAGE_DIGITIZER) continue;
      USAGE lo = bc[i].IsRange ? bc[i].Range.UsageMin : bc[i].NotRange.Usage;
      USAGE hi = bc[i].IsRange ? bc[i].Range.UsageMax : bc[i].NotRange.Usage;
      if (USAGE_CONFIDENCE < lo || USAGE_CONFIDENCE > hi) continue;
      for (int s = 0; s < d->slotCount; s++)
        if (d->slots[s] == bc[i].LinkCollection) d->slotHasConfidence[s] = true;
    }
  }
  free(bc);

  d->usable = d->slotCount > 0 && d->hasCount;
  LOG("device %p: %d contact slots, count=%d, %.4f x %.4f mm/unit\n",
      d->handle, d->slotCount, d->hasCount, d->mmPerUnitX, d->mmPerUnitY);
}

static Device *deviceFor(HANDLE h) {
  for (int i = 0; i < gDeviceCount; i++) if (gDevices[i].handle == h) return &gDevices[i];
  if (gDeviceCount >= MAX_DEVICES) return NULL;
  Device *d = &gDevices[gDeviceCount++];
  memset(d, 0, sizeof *d);
  d->handle = h;
  setupDevice(d);
  return d;
}

// ---------------------------------------------------------------- mouse output

static void sendButton(DWORD flag) {
  INPUT in = { 0 };
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flag;
  in.mi.dwExtraInfo = INPUT_MAGIC;
  SendInput(1, &in, sizeof in);
}

static void clampToMonitors(double *x, double *y) {
  POINT p = { (LONG)lround(*x), (LONG)lround(*y) };
  if (MonitorFromPoint(p, MONITOR_DEFAULTTONULL)) return;
  MONITORINFO mi = { .cbSize = sizeof mi };
  if (!GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) return;
  if (*x < mi.rcMonitor.left) *x = mi.rcMonitor.left;
  if (*x > mi.rcMonitor.right - 1) *x = mi.rcMonitor.right - 1;
  if (*y < mi.rcMonitor.top) *y = mi.rcMonitor.top;
  if (*y > mi.rcMonitor.bottom - 1) *y = mi.rcMonitor.bottom - 1;
}

static void syncCursor(void) {
  POINT p;
  GetCursorPos(&p);
  gCurX = p.x; gCurY = p.y;
  gSentX = p.x; gSentY = p.y;
}

// ---------------------------------------------------------------- speed curve

static int binFor(double speed) {
  int b = (int)floor(log(speed / LEARN_MIN_SPEED) / log(LEARN_MAX_SPEED / LEARN_MIN_SPEED) * LEARN_BINS);
  return b < 0 ? 0 : b >= LEARN_BINS ? LEARN_BINS - 1 : b;
}

static double binSpeed(int b) {
  return LEARN_MIN_SPEED * pow(LEARN_MAX_SPEED / LEARN_MIN_SPEED, (b + 0.5) / LEARN_BINS);
}

static int learnedBins(void) {
  int n = 0;
  for (int b = 0; b < LEARN_BINS; b++) if (gBinMm[b] >= LEARN_BIN_READY_MM) n++;
  return n;
}

static double fallbackGain(double speed) {
  double t = (speed - ACCEL_LOW_MM_PER_S) / (ACCEL_HIGH_MM_PER_S - ACCEL_LOW_MM_PER_S);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  t = t * t * (3 - 2 * t);
  return MIN_GAIN_PX_PER_MM + (MAX_GAIN_PX_PER_MM - MIN_GAIN_PX_PER_MM) * t;
}

// px per mm at a finger speed: interpolated between learned bins (on a log speed axis),
// forced non-decreasing since Windows' acceleration only ever increases with speed.
static double gainAt(double speed) {
  double ls[LEARN_BINS], g[LEARN_BINS];
  int n = 0;
  for (int b = 0; b < LEARN_BINS; b++) {
    if (gBinMm[b] < LEARN_BIN_READY_MM) continue;
    double gain = gBinPx[b] / gBinMm[b];
    if (n > 0 && gain < g[n - 1]) gain = g[n - 1];
    ls[n] = log(binSpeed(b));
    g[n++] = gain;
  }
  if (n == 0) return fallbackGain(speed);
  double l = log(speed > 0.01 ? speed : 0.01);
  if (l <= ls[0]) return g[0];
  if (l >= ls[n - 1]) return g[n - 1];
  for (int i = 1; i < n; i++)
    if (l <= ls[i]) return g[i - 1] + (g[i] - g[i - 1]) * (l - ls[i - 1]) / (ls[i] - ls[i - 1]);
  return g[n - 1];
}

static bool nearMonitorEdge(POINT p) {
  MONITORINFO mi = { .cbSize = sizeof mi };
  if (!GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) return true;
  return p.x <= mi.rcMonitor.left + 2 || p.x >= mi.rcMonitor.right - 3 ||
         p.y <= mi.rcMonitor.top + 2 || p.y >= mi.rcMonitor.bottom - 3;
}

static void learnReset(void) { gWin.active = gWin.warm = false; }

// Called for each frame where exactly one finger moves the cursor the normal way.
static void learnObserve(double dx, double dy, double dtMs) {
  if (!gWin.active) {
    gWin.active = true;
    gWin.fx = gWin.fy = gWin.ms = 0;
    GetCursorPos(&gWin.start);
  }
  gWin.fx += dx;
  gWin.fy += dy;
  gWin.ms += dtMs;
  if (gWin.ms < LEARN_WINDOW_MS) return;

  POINT now;
  GetCursorPos(&now);
  double cx = now.x - gWin.start.x, cy = now.y - gWin.start.y;
  double fd = hypot(gWin.fx, gWin.fy), cd = hypot(cx, cy);
  double speed = fd / (gWin.ms / 1000.0);
  // Skip the first window of each stroke (Windows lags at the start), clamped cursors,
  // and anything where the cursor didn't move the way the finger did (e.g. a mouse).
  bool good = gWin.warm && fd > 0.3 && cd > 0 && !nearMonitorEdge(now) &&
              (gWin.fx * cx + gWin.fy * cy) / (fd * cd) > 0.94;
  if (good) {
    int b = binFor(speed);
    gBinMm[b] += fd;
    gBinPx[b] += cd;
    if (gBinMm[b] > LEARN_BIN_CAP_MM) {
      double k = LEARN_BIN_CAP_MM / gBinMm[b];
      gBinMm[b] *= k;
      gBinPx[b] *= k;
    }
    gCurveDirty = true;
    LOG("learn %6.1f mm/s -> %5.2f px/mm (bin %d now %5.2f)\n", speed, cd / fd, b, gBinPx[b] / gBinMm[b]);
  }
  gWin.warm = true;
  gWin.start = now;
  gWin.fx = gWin.fy = gWin.ms = 0;
}

// Pointer-related Windows settings. If any change, the learned curve is thrown away.
static void readSettingsSig(int sig[3]) {
  DWORD v = 0, size = sizeof v;
  sig[0] = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad",
                        L"CursorSpeed", RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS ? (int)v : -1;
  int speed = 0, mouse[3] = { 0 };
  SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0);
  SystemParametersInfoW(SPI_GETMOUSE, 0, mouse, 0);
  sig[1] = speed;
  sig[2] = mouse[2];
}

static bool curvePath(wchar_t *path, size_t n, bool createDir) {
  const wchar_t *appData = _wgetenv(L"APPDATA");
  if (!appData) return false;
  _snwprintf(path, n, L"%ls\\Macify", appData);
  if (createDir) CreateDirectoryW(path, NULL);
  _snwprintf(path, n, L"%ls\\Macify\\speed-curve.txt", appData);
  return true;
}

static void curveSave(void) {
  wchar_t path[MAX_PATH], tmp[MAX_PATH + 8];
  if (!curvePath(path, MAX_PATH, true)) return;
  _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", path);
  FILE *f = _wfopen(tmp, L"w");
  if (!f) return;
  fprintf(f, "# Macify learned speed curve: bin speed_mm_per_s finger_mm cursor_px\n");
  fprintf(f, "settings %d %d %d\n", gSettingsSig[0], gSettingsSig[1], gSettingsSig[2]);
  for (int b = 0; b < LEARN_BINS; b++)
    fprintf(f, "%d %.1f %.3f %.3f\n", b, binSpeed(b), gBinMm[b], gBinPx[b]);
  fclose(f);
  MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
  gCurveDirty = false;
  LOG("saved speed curve (%d/%d ranges learned)\n", learnedBins(), LEARN_BINS);
}

static void curveClear(void) {
  memset(gBinMm, 0, sizeof gBinMm);
  memset(gBinPx, 0, sizeof gBinPx);
  learnReset();
  gCurveDirty = true;
}

static void curveLoad(void) {
  readSettingsSig(gSettingsSig);
  wchar_t path[MAX_PATH];
  if (!curvePath(path, MAX_PATH, false)) return;
  FILE *f = _wfopen(path, L"r");
  if (!f) return;
  char line[256];
  int sig[3];
  bool sameSettings = false;
  while (fgets(line, sizeof line, f)) {
    int b;
    double speed, mm, px;
    if (sscanf(line, "settings %d %d %d", &sig[0], &sig[1], &sig[2]) == 3)
      sameSettings = memcmp(sig, gSettingsSig, sizeof sig) == 0;
    else if (sscanf(line, "%d %lf %lf %lf", &b, &speed, &mm, &px) == 4 && b >= 0 && b < LEARN_BINS && mm >= 0 && px >= 0) {
      gBinMm[b] = mm;
      gBinPx[b] = px;
    }
  }
  fclose(f);
  if (!sameSettings) { curveClear(); LOG("pointer settings changed since last run: relearning\n"); }
  LOG("loaded speed curve (%d/%d ranges learned)\n", learnedBins(), LEARN_BINS);
}

static void checkSettings(void) {
  int sig[3];
  readSettingsSig(sig);
  if (memcmp(sig, gSettingsSig, sizeof sig) == 0) return;
  memcpy(gSettingsSig, sig, sizeof sig);
  curveClear();
  curveSave();
  LOG("pointer settings changed: relearning\n");
}

// ---------------------------------------------------------------- mouse output (drag)

static void moveCursor(double dxMm, double dyMm, double dtMs) {
  double dist = hypot(dxMm, dyMm);
  double inst = dist / (dtMs / 1000.0);
  gDragSpeed += (inst - gDragSpeed) * (1.0 - exp(-dtMs / SPEED_SMOOTH_MS));
  if (dist <= 0) return;
  double gain = gainAt(gDragSpeed) * gSpeedMul;

  gCurX += dxMm * gain;
  gCurY += dyMm * gain;
  clampToMonitors(&gCurX, &gCurY);

  LONG ix = (LONG)lround(gCurX), iy = (LONG)lround(gCurY);
  if (ix == gSentX && iy == gSentY) return;
  LARGE_INTEGER t0, t1;
  if (gDebugOn) {
    POINT actual;  // where Windows has the cursor vs. where we last put it
    GetCursorPos(&actual);
    if (labs(actual.x - gSentX) + labs(actual.y - gSentY) > 2)
      LOG("  cursor is %ld,%ld px behind where we put it\n", gSentX - actual.x, gSentY - actual.y);
    QueryPerformanceCounter(&t0);
  }

  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  INPUT in = { 0 };
  in.type = INPUT_MOUSE;
  in.mi.dx = (LONG)((((LONGLONG)(ix - vx)) * 65536 + vw - 1) / vw);
  in.mi.dy = (LONG)((((LONGLONG)(iy - vy)) * 65536 + vh - 1) / vh);
  in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  in.mi.dwExtraInfo = INPUT_MAGIC;
  SendInput(1, &in, sizeof in);
  if (gDebugOn) {
    QueryPerformanceCounter(&t1);
    double sendMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)gFreq.QuadPart;
    if (sendMs > 2) LOG("  moving the cursor took %.1f ms\n", sendMs);
  }
  gSentX = ix; gSentY = iy;
}

// ---------------------------------------------------------------- gesture logic

// While the fingers are lifted (grace period), any other cursor movement means the
// user is done: swallow that movement, then drop at the position the drag ended.
static LRESULT CALLBACK mouseHook(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && wp == WM_MOUSEMOVE && gReleasePending &&
      ((const MSLLHOOKSTRUCT *)lp)->dwExtraInfo != INPUT_MAGIC) {
    const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
    if (gCancelPosted) return 1;
    if (labs(m->pt.x - gSentX) > CANCEL_MOVE_PX || labs(m->pt.y - gSentY) > CANCEL_MOVE_PX) {
      gCancelPosted = true;
      PostMessageW(gWnd, WM_CANCEL_DRAG, 0, 0);
      return 1;
    }
  }
  return CallNextHookEx(gMouseHook, code, wp, lp);
}

static void beginGracePeriod(void) {
  gReleasePending = true;
  gCancelPosted = false;
  SetTimer(gWnd, TIMER_RELEASE, RELEASE_DELAY_MS, NULL);
  if (!gMouseHook) gMouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(NULL), 0);
}

static void stopGracePeriod(void) {
  KillTimer(gWnd, TIMER_RELEASE);
  if (gMouseHook) { UnhookWindowsHookEx(gMouseHook); gMouseHook = NULL; }
  gReleasePending = false;
  gCancelPosted = false;
}

static void endDrag(void) {
  stopGracePeriod();
  if (gState == DRAGGING) {
    sendButton(gDragRight ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
    LOG("drag end\n");
  }
  gState = IDLE;
}

// Pairs each contact in a[] with one in b[] (same count) so the total squared distance is
// smallest. Brute force is fine: there are at most a handful of fingers.
static double gBestCost;
static int gBest[MAX_CONTACTS], gCurPerm[MAX_CONTACTS];
static bool gUsedSlot[MAX_CONTACTS];

static void assignRec(const Contact *a, const Contact *b, int n, int i, double cost) {
  if (cost >= gBestCost) return;
  if (i == n) { gBestCost = cost; memcpy(gBest, gCurPerm, sizeof(int) * n); return; }
  for (int j = 0; j < n; j++) {
    if (gUsedSlot[j]) continue;
    double ex = a[i].x - b[j].x, ey = a[i].y - b[j].y;
    gUsedSlot[j] = true;
    gCurPerm[i] = j;
    assignRec(a, b, n, i + 1, cost + ex * ex + ey * ey);
    gUsedSlot[j] = false;
  }
}

static void pairContacts(const Contact *a, const Contact *b, int n, int *perm) {
  if (n <= 6) {
    gBestCost = INFINITY;
    memset(gUsedSlot, 0, sizeof gUsedSlot);
    assignRec(a, b, n, 0, 0);
    memcpy(perm, gBest, sizeof(int) * n);
    return;
  }
  for (int i = 0; i < n; i++) {  // unlikely: fall back to contact IDs
    perm[i] = i;
    for (int j = 0; j < n; j++) if (a[i].id == b[j].id) { perm[i] = j; break; }
  }
}

static double median(double *v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

static bool sameFingers(const Contact *a, int na, const Contact *b, int nb) {
  if (na != nb) return false;
  for (int i = 0; i < na; i++) {
    bool found = false;
    for (int j = 0; j < nb && !found; j++) found = a[i].id == b[j].id;
    if (!found) return false;
  }
  return true;
}

static void resetMotionFilter(void) { gVelValid = gHolding = false; }

// Holds back physically impossible jumps; see the notes at the top of the file.
static void filterMotion(double *dx, double *dy, double dtMs) {
  double vx = *dx / dtMs, vy = *dy / dtMs;
  if (!gVelValid) { gVx = vx; gVy = vy; gVelValid = true; return; }
  double dev = hypot(vx - gVx, vy - gVy);
  if (!gHolding) {
    if (dev <= MAX_ACCEL_MM_PER_MS2 * dtMs) { gVx = vx; gVy = vy; return; }
    gHolding = true;
    gHoldMs = 0;
    gPendX = gPendY = 0;
    gEntryJump = dev;
    LOG("sudden jump of %.1f mm/frame, holding back\n", dev * dtMs);
  } else if (dev < 0.5 * gEntryJump) {
    LOG("jump snapped back after %.0f ms: discarded %.1f mm\n", gHoldMs, hypot(gPendX, gPendY));
    gHolding = false;
    gVx = vx; gVy = vy;
    return;
  }
  double ex = gVx * dtMs, ey = gVy * dtMs;  // continue at the trusted speed meanwhile
  gHoldMs += dtMs;
  gPendX += *dx - ex;
  gPendY += *dy - ey;
  if (gHoldMs >= SPIKE_HOLD_MS) {
    LOG("jump persisted: catching up %.1f mm\n", hypot(gPendX, gPendY));
    *dx = ex + gPendX;
    *dy = ey + gPendY;
    gHolding = false;
    gVx = vx; gVy = vy;
    return;
  }
  *dx = ex;
  *dy = ey;
}

static void processFrame(const Contact *all, int n, int scanTime) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  double dtMs = (double)(now.QuadPart - gLastFrame.QuadPart) * 1000.0 / (double)gFreq.QuadPart;
  gLastFrame = now;
  if (scanTime >= 0 && gLastScan >= 0) {
    int d = (scanTime - gLastScan) & 0xFFFF;
    if (d > 0 && d < 2000) dtMs = d / 10.0;
  }
  gLastScan = scanTime;
  if (dtMs < 1) dtMs = 1;
  if (dtMs > 50) dtMs = 50;

  Contact active[MAX_CONTACTS];
  int count = 0;
  for (int i = 0; i < n; i++)
    if (all[i].tip && all[i].confident) active[count++] = all[i];

  double nowMs = (double)now.QuadPart * 1000.0 / (double)gFreq.QuadPart;
  if (!sameFingers(active, count, gPrev, gPrevCount)) {
    gSettleUntilMs = nowMs + SETTLE_MS;
    if (gState != IDLE) LOG("fingers %d -> %d, settling\n", gPrevCount, count);
  }
  bool settling = nowMs < gSettleUntilMs;

  // threeFingerDragAsOneFinger: movement of the group's centre (average of each finger's
  // movement). Otherwise the movement of the finger that moved most.
  double dx = 0, dy = 0;
  int used = 0;
  if (!settling && count > 0 && count == gPrevCount) {
    int perm[MAX_CONTACTS];
    double fx[MAX_CONTACTS], fy[MAX_CONTACTS], sx[MAX_CONTACTS], sy[MAX_CONTACTS];
    pairContacts(active, gPrev, count, perm);
    for (int i = 0; i < count; i++) {
      fx[i] = sx[i] = active[i].x - gPrev[perm[i]].x;
      fy[i] = sy[i] = active[i].y - gPrev[perm[i]].y;
    }
    if (!gAsOneFinger) {
      double best = -1;
      for (int i = 0; i < count; i++)
        if (hypot(fx[i], fy[i]) > best) { best = hypot(fx[i], fy[i]); dx = fx[i]; dy = fy[i]; }
      used = count;
    }
    double mx = 0, my = 0, limit = INFINITY;
    if (gAsOneFinger && count >= 3) {  // with three or more fingers the median tells us what the hand did
      mx = median(sx, count);
      my = median(sy, count);
      limit = OUTLIER_MM + 0.5 * hypot(mx, my);
    }
    for (int i = 0; i < count && gAsOneFinger; i++) {
      if (hypot(fx[i] - mx, fy[i] - my) > limit) {
        LOG("ignored finger moving %.1fmm against the others\n", hypot(fx[i] - mx, fy[i] - my));
        continue;
      }
      dx += fx[i];
      dy += fy[i];
      used++;
    }
    if (used && gAsOneFinger) { dx /= used; dy /= used; }
  }
  if (used && count == DRAG_FINGERS && (gState == ARMED || gState == DRAGGING)) {
    double rx = dx, ry = dy;
    (void)rx; (void)ry;  // only used by the debug log
    filterMotion(&dx, &dy, dtMs);
    if (gState == DRAGGING && !gReleasePending)
      LOG("move %6.2f,%6.2f mm in %4.1f ms (raw %6.2f,%6.2f) gain %.1f px/mm\n", dx, dy, dtMs, rx, ry, gainAt(gDragSpeed));
  } else {
    resetMotionFilter();
  }

  // One finger, nothing of ours going on: Windows is moving the cursor. Learn from it.
  if (count == 1 && gPrevCount == 1 && used == 1 && gState == IDLE && !gReleasePending)
    learnObserve(dx, dy, dtMs);
  else
    learnReset();

  if (count == DRAG_FINGERS) {
    switch (gState) {
      case IDLE:
        gState = ARMED;
        gArmX = gArmY = 0;
        break;
      case ARMED:
        gArmX += dx; gArmY += dy;
        if (hypot(gArmX, gArmY) >= START_THRESHOLD_MM) {
          syncCursor();
          gDragSpeed = 0;
          // Ctrl or Alt held as the drag starts: right-button drag, Mac style (the app
          // sees a plain right-button drag, without the modifier).
          gDragRight = gPhysCtrl || gPhysAlt;
          if (gDragRight && gHookWnd) SendMessageW(gHookWnd, WM_HOOK_LIFT, 0, 0);
          sendButton(gDragRight ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
          gState = DRAGGING;
          LOG("drag start%s\n", gDragRight ? " (right button)" : "");
        }
        break;
      case DRAGGING:
        if (gReleasePending) {
          stopGracePeriod();
          gResync = true;
        }
        if (gResync) { syncCursor(); gDragSpeed = 0; gResync = false; }
        moveCursor(dx, dy, dtMs);
        break;
    }
  } else {
    if (gState == ARMED) gState = IDLE;
    else if (gState == DRAGGING && !gReleasePending) beginGracePeriod();
  }

  memcpy(gPrev, active, sizeof(Contact) * count);
  gPrevCount = count;
}

static void handleReport(Device *d, PCHAR rep, ULONG len) {
  ULONG cc = 0;
  if (HidP_GetUsageValue(HidP_Input, PAGE_DIGITIZER, d->countLink, USAGE_CONTACT_COUNT,
                         &cc, d->pp, rep, len) == HIDP_STATUS_SUCCESS && cc > 0) {
    d->expected = cc > MAX_CONTACTS ? MAX_CONTACTS : (int)cc;
    d->collected = 0;
    ULONG st;
    d->scanTime = HidP_GetUsageValue(HidP_Input, PAGE_DIGITIZER, d->countLink, USAGE_SCAN_TIME,
                                     &st, d->pp, rep, len) == HIDP_STATUS_SUCCESS ? (int)(st & 0xFFFF) : -1;
  }
  if (d->expected == 0) return;

  for (int s = 0; s < d->slotCount && d->collected < d->expected; s++) {
    USHORT link = d->slots[s];
    ULONG id, x, y;
    if (HidP_GetUsageValue(HidP_Input, PAGE_DIGITIZER, link, USAGE_CONTACT_ID, &id, d->pp, rep, len) != HIDP_STATUS_SUCCESS) continue;
    if (HidP_GetUsageValue(HidP_Input, PAGE_GENERIC, link, USAGE_X, &x, d->pp, rep, len) != HIDP_STATUS_SUCCESS) continue;
    if (HidP_GetUsageValue(HidP_Input, PAGE_GENERIC, link, USAGE_Y, &y, d->pp, rep, len) != HIDP_STATUS_SUCCESS) continue;

    USAGE usages[32];
    ULONG nu = 32;
    bool tip = false, conf = false;
    if (HidP_GetUsages(HidP_Input, PAGE_DIGITIZER, link, usages, &nu, d->pp, rep, len) == HIDP_STATUS_SUCCESS)
      for (ULONG k = 0; k < nu; k++) {
        if (usages[k] == USAGE_TIP_SWITCH) tip = true;
        if (usages[k] == USAGE_CONFIDENCE) conf = true;
      }
    if (!USE_PALM_REJECTION || !d->slotHasConfidence[s]) conf = true;

    Contact *c = &d->frame[d->collected++];
    c->id = id;
    c->tip = tip;
    c->confident = conf;
    c->x = x * d->mmPerUnitX;
    c->y = y * d->mmPerUnitY;
  }

  if (d->collected >= d->expected) {
    processFrame(d->frame, d->collected, d->scanTime);
    d->expected = 0;
    d->collected = 0;
  }
}

static void onRawInput(HRAWINPUT h) {
  static BYTE *buf;
  static UINT bufSize;
  UINT size = 0;
  if (GetRawInputData(h, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) return;
  if (size > bufSize) {
    free(buf);
    buf = malloc(size);
    bufSize = buf ? size : 0;
    if (!buf) return;
  }
  if (GetRawInputData(h, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;

  RAWINPUT *ri = (RAWINPUT *)buf;
  if (ri->header.dwType != RIM_TYPEHID) return;
  Device *d = deviceFor(ri->header.hDevice);
  if (!d || !d->usable) return;

  BYTE *rep = ri->data.hid.bRawData;
  for (DWORD i = 0; i < ri->data.hid.dwCount; i++)
    handleReport(d, (PCHAR)(rep + i * ri->data.hid.dwSizeHid), ri->data.hid.dwSizeHid);
}

// ---------------------------------------------------------------- remaps (own thread)
//
// Works like AutoHotkey: low-level hooks see every key and click before apps do, can
// swallow it, and send something else instead. Kept on its own high-priority thread so
// touchpad processing can never delay typing.

static const UINT kMods[] = { VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN };
#define N_MODS (sizeof kMods / sizeof *kMods)

static bool kPhys[256];      // keys physically held
static bool kLog[256];       // modifier state as Windows currently sees it
static bool kSup[256];       // modifiers hidden from apps for a remapped click, until released
static bool kSwallowUp[256]; // hotkey keys whose release must be swallowed too
static bool gClickRemapped;

static bool isMod(UINT vk) {
  for (size_t i = 0; i < N_MODS; i++) if (kMods[i] == vk) return true;
  return false;
}

static bool isExtendedVk(UINT vk) {
  switch (vk) {
    case VK_DELETE: case VK_INSERT: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_LWIN: case VK_RWIN: case VK_RCONTROL: case VK_RMENU:
      return true;
  }
  return false;
}

static void sendKey(UINT vk, bool down) {
  INPUT in = { 0 };
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = (WORD)vk;
  in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
  in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (isExtendedVk(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
  in.ki.dwExtraInfo = INPUT_MAGIC;
  SendInput(1, &in, sizeof in);
  if (vk < 256) kLog[vk] = down;
}

// What Windows should currently see for a modifier, given the physical keys.
static bool wantDown(UINT vk) {
  if (kSup[vk]) return false;
  if (vk == VK_LWIN) return false;                                  // left Win is Ctrl
  if (vk == VK_LCONTROL) return kPhys[VK_LCONTROL] || kPhys[VK_LWIN];
  return kPhys[vk];
}

static void updatePhysFlags(void) {
  gPhysCtrl = kPhys[VK_LCONTROL] || kPhys[VK_RCONTROL];
  gPhysAlt = kPhys[VK_LMENU] || kPhys[VK_RMENU];
}

// Hide Ctrl and Alt from apps (for a remapped click). They stay hidden until released,
// or until another key needs them (then they are pressed again first).
static void liftForRightClick(void) {
  const UINT lift[] = { VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU };
  // Releasing Alt right after pressing it would open the window's menu bar. A dummy key
  // in between (unassigned vk E8, as AutoHotkey does) prevents that.
  if (kLog[VK_LMENU] || kLog[VK_RMENU]) { sendKey(0xE8, true); sendKey(0xE8, false); }
  for (int i = 0; i < 4; i++) {
    if (kLog[lift[i]]) sendKey(lift[i], false);
    if (wantDown(lift[i])) kSup[lift[i]] = true;
  }
}

// Returns true if anything had to be pressed again (the triggering event must then be
// swallowed and replayed so it arrives after the modifiers).
static bool restoreHidden(void) {
  bool any = false;
  for (size_t i = 0; i < N_MODS; i++) {
    UINT vk = kMods[i];
    if (!kSup[vk]) continue;
    kSup[vk] = false;
    if (wantDown(vk) && !kLog[vk]) { sendKey(vk, true); any = true; }
  }
  return any;
}

// AutoHotkey-style Send: modifiers that aren't part of the combination are released for
// the moment, missing ones pressed, then everything is put back as it was.
static void sendCombo(bool ctrl, bool alt, bool shift, bool win, UINT vk) {
  const bool want[N_MODS] = { ctrl, ctrl, alt, alt, shift, shift, win, win };
  bool lifted[N_MODS] = { 0 }, pressed[N_MODS] = { 0 };
  for (size_t i = 0; i < N_MODS; i++)
    if (kLog[kMods[i]] && !want[i]) { sendKey(kMods[i], false); lifted[i] = true; }
  for (size_t i = 0; i < N_MODS; i += 2)  // pairs: left, right
    if (want[i] && !kLog[kMods[i]] && !kLog[kMods[i + 1]]) { sendKey(kMods[i], true); pressed[i] = true; }
  sendKey(vk, true);
  sendKey(vk, false);
  for (size_t i = 0; i < N_MODS; i++) if (pressed[i]) sendKey(kMods[i], false);
  for (size_t i = 0; i < N_MODS; i++) if (lifted[i] && wantDown(kMods[i])) sendKey(kMods[i], true);
}

// The key that types a character on the current keyboard layout (like AutoHotkey's ^<).
static UINT charVk(wchar_t ch, UINT fallback) {
  HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));
  SHORT r = VkKeyScanExW(ch, hkl);
  if (r == -1 || (r & 0xFF00)) return fallback;  // needs Shift/AltGr: not a plain key
  return (UINT)(r & 0xFF);
}

// The hotkeys. Returns true if the key was handled (and must be swallowed).
static bool runHotkey(UINT vk) {
  bool ctrl = kLog[VK_LCONTROL] || kLog[VK_RCONTROL];
  bool alt = kLog[VK_LMENU] || kLog[VK_RMENU];
  bool shift = kLog[VK_LSHIFT] || kLog[VK_RSHIFT];
  bool win = kLog[VK_LWIN] || kLog[VK_RWIN];
  if (!ctrl || alt || win) return false;  // like AutoHotkey: exactly these modifiers

  UINT lessVk = charVk(L'<', VK_OEM_102), acuteVk = charVk(L'´', VK_OEM_4);
  if (shift) {
    if (vk == '7') { sendCombo(false, true, true, false, 'A'); return true; }     // ^+7 -> Alt+Shift+A
    return false;
  }
  switch (vk) {
    case 'D': case VK_BACK: sendCombo(false, false, false, false, VK_DELETE); return true;
    case '2': case '3': case '4': case '7': case '8': case '9': case '0':
      sendCombo(true, true, false, false, vk); return true;                     // AltGr symbol
    case 'H': sendCombo(false, false, false, true, 'M'); return true;            // Win+M
    case 'I': sendCombo(true, false, true, false, 'C'); return true;             // Ctrl+Shift+C
  }
  if (vk == lessVk || vk == acuteVk) { sendCombo(true, true, false, false, lessVk); return true; }  // |
  return false;
}

static void replayKey(const KBDLLHOOKSTRUCT *kb) {
  INPUT in = { 0 };
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = (WORD)kb->vkCode;
  in.ki.wScan = (WORD)kb->scanCode;
  in.ki.dwFlags = (kb->flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
  in.ki.dwExtraInfo = INPUT_MAGIC;
  SendInput(1, &in, sizeof in);
}

static LRESULT CALLBACK keyboardHook(int code, WPARAM wp, LPARAM lp) {
  if (code != HC_ACTION) return CallNextHookEx(NULL, code, wp, lp);
  const KBDLLHOOKSTRUCT *kb = (const KBDLLHOOKSTRUCT *)lp;
  UINT vk = kb->vkCode;
  bool down = !(kb->flags & LLKHF_UP);
  if (kb->dwExtraInfo == INPUT_MAGIC || vk >= 256) return CallNextHookEx(NULL, code, wp, lp);
  // AltGr sends a fake left Ctrl along with right Alt: leave it alone.
  if (vk == VK_LCONTROL && (kb->scanCode & 0x200)) return CallNextHookEx(NULL, code, wp, lp);

  if (isMod(vk)) {
    kPhys[vk] = down;
    UINT target = vk == VK_LWIN ? VK_LCONTROL : vk;
    if (!down && !kPhys[VK_LCONTROL] && !kPhys[VK_LWIN]) kSup[VK_LCONTROL] = false;
    if (!down && vk != VK_LWIN && vk != VK_LCONTROL) kSup[vk] = false;
    updatePhysFlags();
    bool want = wantDown(target);
    // Pass the real event through only if it is what Windows should see (a release only
    // if Windows thinks the key is down: a hidden modifier's release is swallowed).
    if (vk == target && want == down && (down || kLog[target])) {
      kLog[target] = down;
      return CallNextHookEx(NULL, code, wp, lp);
    }
    if (want != kLog[target]) sendKey(target, want);
    return 1;  // swallowed: left Win, a hidden modifier, or no change for Windows
  }

  if (!down) {
    kPhys[vk] = false;
    if (kSwallowUp[vk]) { kSwallowUp[vk] = false; return 1; }
    return CallNextHookEx(NULL, code, wp, lp);
  }
  kPhys[vk] = true;
  bool restored = restoreHidden();
  if (runHotkey(vk)) { kSwallowUp[vk] = true; LOG("hotkey vk %02X\n", vk); return 1; }
  if (restored) { replayKey(kb); return 1; }
  return CallNextHookEx(NULL, code, wp, lp);
}

static void sendMouse(DWORD flags, DWORD data) {
  INPUT in = { 0 };
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flags;
  in.mi.mouseData = data;
  in.mi.dwExtraInfo = INPUT_MAGIC;
  SendInput(1, &in, sizeof in);
}

static LRESULT CALLBACK remapMouseHook(int code, WPARAM wp, LPARAM lp) {
  if (code != HC_ACTION) return CallNextHookEx(NULL, code, wp, lp);
  const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
  if (m->dwExtraInfo == INPUT_MAGIC || wp == WM_MOUSEMOVE) return CallNextHookEx(NULL, code, wp, lp);

  if (wp == WM_LBUTTONDOWN && (gPhysCtrl || gPhysAlt)) {  // Mac style: plain right-click
    liftForRightClick();
    sendMouse(MOUSEEVENTF_RIGHTDOWN, 0);
    gClickRemapped = true;
    LOG("click -> right click\n");
    return 1;
  }
  if (wp == WM_LBUTTONUP && gClickRemapped) {
    gClickRemapped = false;
    sendMouse(MOUSEEVENTF_RIGHTUP, 0);
    return 1;
  }
  // Anything else that may need the hidden modifiers (Ctrl+scroll to zoom, etc.)
  if (wp != WM_LBUTTONDOWN && wp != WM_LBUTTONUP && restoreHidden()) {
    DWORD data = HIWORD(m->mouseData);
    switch (wp) {
      case WM_MOUSEWHEEL:  sendMouse(MOUSEEVENTF_WHEEL, (DWORD)(LONG)(SHORT)data); break;
      case WM_MOUSEHWHEEL: sendMouse(MOUSEEVENTF_HWHEEL, (DWORD)(LONG)(SHORT)data); break;
      case WM_RBUTTONDOWN: sendMouse(MOUSEEVENTF_RIGHTDOWN, 0); break;
      case WM_RBUTTONUP:   sendMouse(MOUSEEVENTF_RIGHTUP, 0); break;
      case WM_MBUTTONDOWN: sendMouse(MOUSEEVENTF_MIDDLEDOWN, 0); break;
      case WM_MBUTTONUP:   sendMouse(MOUSEEVENTF_MIDDLEUP, 0); break;
      case WM_XBUTTONDOWN: sendMouse(MOUSEEVENTF_XDOWN, data); break;
      case WM_XBUTTONUP:   sendMouse(MOUSEEVENTF_XUP, data); break;
      default: return CallNextHookEx(NULL, code, wp, lp);
    }
    return 1;
  }
  return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK hookWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_HOOK_LIFT) { liftForRightClick(); return 0; }
  return DefWindowProcW(w, msg, wp, lp);
}

static DWORD WINAPI remapThread(LPVOID unused) {
  (void)unused;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSW wc = { 0 };
  wc.lpfnWndProc = hookWndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"MacifyRemaps";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
  HHOOK kh = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHook, inst, 0);
  HHOOK mh = SetWindowsHookExW(WH_MOUSE_LL, remapMouseHook, inst, 0);
  LOG("remaps %s\n", kh && mh ? "active" : "FAILED to install");
  gHookWnd = w;

  MSG m;
  while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);

  gHookWnd = NULL;
  if (kh) UnhookWindowsHookEx(kh);
  if (mh) UnhookWindowsHookEx(mh);
  for (size_t i = 0; i < N_MODS; i++)  // never leave a key we pressed stuck down
    if (kLog[kMods[i]] && !kPhys[kMods[i]]) sendKey(kMods[i], false);
  if (kLog[VK_LCONTROL] && !kPhys[VK_LCONTROL]) sendKey(VK_LCONTROL, false);
  if (gClickRemapped) sendMouse(MOUSEEVENTF_RIGHTUP, 0);
  return 0;
}

// ---------------------------------------------------------------- tray icon

static bool lightTaskbar(void) {
  DWORD v = 0, size = sizeof v;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &v, &size) != ERROR_SUCCESS) return false;
  return v != 0;
}

// Keyboard-and-mouse glyph from the exe's resources: white or black to suit the taskbar.
static HICON makeIcon(void) {
  int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
  HICON icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(lightTaskbar() ? 3 : 2),
                                 IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
  return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

static void trayAdd(void) {
  gNid.cbSize = sizeof gNid;
  gNid.hWnd = gWnd;
  gNid.uID = 1;
  gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  gNid.uCallbackMessage = WM_TRAY;
  gNid.hIcon = gIcon;
  wcscpy(gNid.szTip, L"Macify");
  // Explorer may not be up yet when started at logon: keep retrying.
  if (Shell_NotifyIconW(NIM_ADD, &gNid)) KillTimer(gWnd, TIMER_TRAY_RETRY);
  else SetTimer(gWnd, TIMER_TRAY_RETRY, 2000, NULL);
}

static void trayRefreshIcon(void) {
  HICON old = gIcon;
  gIcon = makeIcon();
  gNid.hIcon = gIcon;
  Shell_NotifyIconW(NIM_MODIFY, &gNid);
  if (old) DestroyIcon(old);
}

static void trayNotify(const wchar_t *title, const wchar_t *text) {
  gNid.uFlags |= NIF_INFO;
  wcsncpy(gNid.szInfoTitle, title, 63);
  wcsncpy(gNid.szInfo, text, 255);
  gNid.dwInfoFlags = NIIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &gNid);
  gNid.uFlags &= ~NIF_INFO;
}

static void quit(void) {
  if (gDebugOn) debugFinish();  // don't lose a log in progress
  endDrag();
  if (gCurveDirty) curveSave();
  Shell_NotifyIconW(NIM_DELETE, &gNid);
  DestroyWindow(gWnd);
}

static void showTrayMenu(void) {
  POINT p;
  GetCursorPos(&p);
  HMENU menu = CreatePopupMenu();
  wchar_t status[96];
  int learned = learnedBins();
  if (learned == 0) _snwprintf(status, 96, L"Pointer speed: learning, move one finger normally");
  else _snwprintf(status, 96, L"Pointer speed: %d of %d speed ranges learned", learned, LEARN_BINS);
  wchar_t title[48];
  _snwprintf(title, 48, L"Macify %hs", MACIFY_VERSION_STR);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status);
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, ID_RELEARN, L"Relearn pointer speed");
  AppendMenuW(menu, MF_STRING | (gRunAtStartup ? MF_CHECKED : 0), ID_STARTUP, L"Run at startup");
  AppendMenuW(menu, MF_STRING | (gRunAsAdmin ? MF_CHECKED : 0), ID_ADMIN, L"Run as administrator");
  AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"Edit settings");
  AppendMenuW(menu, MF_STRING | (gUpdateBusy ? MF_GRAYED : 0), ID_UPDATE,
              gUpdateBusy ? L"Checking for updates..." : L"Check for updates");
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, ID_DEBUG, gDebugOn ? L"Finish debug log (save to desktop)" : L"Start debug log");
  AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit");
  SetForegroundWindow(gWnd);
  int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, gWnd, NULL);
  PostMessageW(gWnd, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (cmd == ID_RELEARN) { curveClear(); curveSave(); }
  if (cmd == ID_STARTUP) toggleSetting(&gRunAtStartup);
  if (cmd == ID_ADMIN) toggleSetting(&gRunAsAdmin);
  if (cmd == ID_SETTINGS) settingsEdit();
  if (cmd == ID_UPDATE) updateCheck();
  if (cmd == ID_DEBUG) { if (gDebugOn) debugFinish(); else debugStart(); }
  if (cmd == ID_QUIT) quit();
}

// ---------------------------------------------------------------- start menu & elevation

static void ensureStartMenuShortcut(void) {
  wchar_t exe[MAX_PATH], dir[MAX_PATH], lnk[MAX_PATH];
  if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
  const wchar_t *appData = _wgetenv(L"APPDATA");
  if (!appData) return;
  _snwprintf(lnk, MAX_PATH, L"%ls\\Microsoft\\Windows\\Start Menu\\Programs\\Macify.lnk", appData);
  wchar_t oldLnk[MAX_PATH];  // shortcut from before the rename
  _snwprintf(oldLnk, MAX_PATH, L"%ls\\Microsoft\\Windows\\Start Menu\\Programs\\Three-finger drag.lnk", appData);
  DeleteFileW(oldLnk);
  wcscpy(dir, exe);
  wchar_t *slash = wcsrchr(dir, L'\\');
  if (slash) *slash = 0;

  if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return;
  IShellLinkW *link;
  if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
    IPersistFile *file;
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
      wchar_t current[MAX_PATH] = L"";
      if (SUCCEEDED(IPersistFile_Load(file, lnk, STGM_READ)))
        IShellLinkW_GetPath(link, current, MAX_PATH, NULL, SLGP_RAWPATH);
      if (_wcsicmp(current, exe) != 0) {  // missing, or the exe has moved
        IShellLinkW_SetPath(link, exe);
        IShellLinkW_SetWorkingDirectory(link, dir);
        IShellLinkW_SetDescription(link, L"Mac-style three finger drag, keyboard shortcuts and Ctrl-click");
        IShellLinkW_SetIconLocation(link, exe, 0);
        IPersistFile_Save(file, lnk, TRUE);
      }
      IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
  }
  CoUninitialize();
}

static bool isElevated(void) {
  HANDLE token;
  TOKEN_ELEVATION e = { 0 };
  DWORD size;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  GetTokenInformation(token, TokenElevation, &e, sizeof e, &size);
  CloseHandle(token);
  return e.TokenIsElevated != 0;
}

static DWORD runHidden(const wchar_t *args) {
  wchar_t cmd[1024];
  _snwprintf(cmd, 1024, L"schtasks.exe %ls", args);
  STARTUPINFOW si = { .cb = sizeof si };
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 1;
  WaitForSingleObject(pi.hProcess, 15000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return code;
}

static bool exePath(wchar_t *exe, wchar_t *dir) {
  if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return false;
  wcscpy(dir, exe);
  wchar_t *slash = wcsrchr(dir, L'\\');
  if (slash) *slash = 0;
  return true;
}

// Small state files in %APPDATA%\Macify
static bool dataFile(wchar_t *path, const wchar_t *name) {
  const wchar_t *appData = _wgetenv(L"APPDATA");
  if (!appData) return false;
  _snwprintf(path, MAX_PATH, L"%ls\\Macify", appData);
  CreateDirectoryW(path, NULL);
  _snwprintf(path, MAX_PATH, L"%ls\\Macify\\%ls", appData, name);
  return true;
}

static bool writeUtf16(const wchar_t *path, const wchar_t *text) {
  FILE *f = _wfopen(path, L"wb");
  if (!f) return false;
  fwrite("\xFF\xFE", 1, 2, f);
  size_t n = wcslen(text);
  bool ok = fwrite(text, sizeof(wchar_t), n, f) == n;
  fclose(f);
  return ok;
}

// task.txt records what the startup task was registered for ("<exe>|<at startup 0/1>"),
// so it is only re-registered (which needs admin rights) when something changed.
static bool taskMarkerMatches(const wchar_t *exe, bool startup) {
  wchar_t path[MAX_PATH], buf[MAX_PATH + 8] = { 0 }, want[MAX_PATH + 8];
  if (!dataFile(path, L"task.txt")) return false;
  FILE *f = _wfopen(path, L"rb");
  if (!f) return false;
  BYTE bom[2];
  size_t ok = fread(bom, 1, 2, f);
  size_t n = ok == 2 ? fread(buf, sizeof(wchar_t), MAX_PATH + 7, f) : 0;
  fclose(f);
  buf[n] = 0;
  _snwprintf(want, MAX_PATH + 8, L"%ls|%d", exe, startup ? 1 : 0);
  return _wcsicmp(buf, want) == 0;
}

static void setTaskMarker(const wchar_t *exe, bool startup) {
  wchar_t path[MAX_PATH], text[MAX_PATH + 8];
  if (!dataFile(path, L"task.txt")) return;
  if (!exe) { DeleteFileW(path); return; }
  _snwprintf(text, MAX_PATH + 8, L"%ls|%d", exe, startup ? 1 : 0);
  writeUtf16(path, text);
}

// install-declined: the UAC prompt to install was answered No; don't ask on every start.
static bool installDeclined(void) {
  wchar_t path[MAX_PATH];
  return dataFile(path, L"install-declined") && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void setInstallDeclined(bool declined) {
  wchar_t path[MAX_PATH];
  if (!dataFile(path, L"install-declined")) return;
  if (declined) writeUtf16(path, L"Delete this file to be offered the install again.");
  else DeleteFileW(path);
}

// Starting at login without admin rights: the usual per-user "Run" registry entry.
static void setRunEntry(const wchar_t *exe) {
  const wchar_t *key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
  if (!exe) { RegDeleteKeyValueW(HKEY_CURRENT_USER, key, L"Macify"); return; }
  wchar_t cmd[MAX_PATH + 4];
  _snwprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"Macify", REG_SZ, cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
}

static void xmlEscape(const wchar_t *in, wchar_t *out, size_t n) {
  size_t o = 0;
  for (; *in && o + 6 < n; in++) {
    const wchar_t *r = *in == L'&' ? L"&amp;" : *in == L'<' ? L"&lt;" : *in == L'>' ? L"&gt;" : NULL;
    if (r) { wcscpy(out + o, r); o += wcslen(r); }
    else out[o++] = *in;
  }
  out[o] = 0;
}

// Where the installed copy lives: C:\Program Files\Macify\macify.exe
static bool installedPath(wchar_t *path, wchar_t *dir) {
  const wchar_t *pf = _wgetenv(L"ProgramW6432");
  if (!pf) pf = _wgetenv(L"ProgramFiles");
  if (!pf) return false;
  _snwprintf(dir, MAX_PATH, L"%ls\\Macify", pf);
  _snwprintf(path, MAX_PATH, L"%ls\\Macify\\macify.exe", pf);
  return true;
}

static bool isInstalledCopy(void) {
  wchar_t exe[MAX_PATH], dir[MAX_PATH], target[MAX_PATH], targetDir[MAX_PATH];
  return exePath(exe, dir) && installedPath(target, targetDir) && _wcsicmp(exe, target) == 0;
}

// Closes running copies of Macify except the one that asked for the install (skipPid).
// They may be elevated: their windows let this message through.
static HWND otherInstance(DWORD skipPid) {
  for (HWND w = NULL; (w = FindWindowExW(NULL, w, L"MacifyWindow", NULL)) != NULL;) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != skipPid) return w;
  }
  return NULL;
}

static bool stopRunningInstance(DWORD skipPid) {
  HWND w = otherInstance(skipPid);
  if (!w) return true;
  if (!gQuitMsg) gQuitMsg = RegisterWindowMessageW(L"MacifyQuit");
  PostMessageW(w, gQuitMsg, 0, 0);
  for (int i = 0; i < 50; i++) {
    Sleep(100);
    if (!otherInstance(skipPid)) return true;
  }
  return false;
}

static bool taskExists(void) { return runHidden(L"/query /tn " TASK_NAME) == 0; }

static int deleteTask(void) {
  runHidden(L"/delete /tn " OLD_NAME L" /f");  // startup task from before the rename
  if (!taskExists()) return 0;
  return runHidden(L"/delete /tn " TASK_NAME L" /f") == 0 ? 0 : 1;
}

// Needs admin rights. Registers the "Macify" task that starts exe with admin rights: at
// login if startup, otherwise only when started on demand (from the Start menu). Unlike a
// plain schtasks /create, the XML also lets it run on battery and without a time limit.
static int registerTask(const wchar_t *exe, const wchar_t *dir, bool startup, const wchar_t *user) {
  wchar_t eExe[MAX_PATH * 2], eDir[MAX_PATH * 2], eUser[512], fallback[512], trigger[700];
  if (!user || !*user) {
    const wchar_t *d = _wgetenv(L"USERDOMAIN"), *u = _wgetenv(L"USERNAME");
    if (!d || !u) return 1;
    _snwprintf(fallback, 512, L"%ls\\%ls", d, u);
    user = fallback;
  }
  xmlEscape(exe, eExe, MAX_PATH * 2);
  xmlEscape(dir, eDir, MAX_PATH * 2);
  xmlEscape(user, eUser, 512);
  if (startup) _snwprintf(trigger, 700, L"<LogonTrigger><Enabled>true</Enabled><UserId>%ls</UserId></LogonTrigger>", eUser);
  else trigger[0] = 0;

  static wchar_t xml[8192];
  _snwprintf(xml, 8192,
    L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
    L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
    L"  <RegistrationInfo><Description>Starts Macify with admin rights, so it also works in admin windows.</Description></RegistrationInfo>\r\n"
    L"  <Triggers>%ls</Triggers>\r\n"
    L"  <Principals><Principal id=\"Author\"><UserId>%ls</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
    L"  <Settings>\r\n"
    L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
    L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
    L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
    L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
    L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
    L"    <Enabled>true</Enabled>\r\n"
    L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
    L"    <Priority>4</Priority>\r\n"
    L"  </Settings>\r\n"
    L"  <Actions Context=\"Author\"><Exec><Command>%ls</Command><WorkingDirectory>%ls</WorkingDirectory></Exec></Actions>\r\n"
    L"</Task>\r\n", trigger, eUser, eExe, eDir);

  wchar_t tmp[MAX_PATH], file[MAX_PATH], args[MAX_PATH + 64];
  GetTempPathW(MAX_PATH, tmp);
  _snwprintf(file, MAX_PATH, L"%lsmacify-task.xml", tmp);
  if (!writeUtf16(file, xml)) return 1;
  _snwprintf(args, MAX_PATH + 64, L"/create /tn " TASK_NAME L" /xml \"%ls\" /f", file);
  DWORD code = runHidden(args);
  DeleteFileW(file);
  if (code == 0) runHidden(L"/delete /tn " OLD_NAME L" /f");
  return code == 0 ? 0 : 1;
}

// Needs admin rights: makes the task match settings.json for the installed copy.
static int configureTask(const wchar_t *user) {
  wchar_t target[MAX_PATH], targetDir[MAX_PATH];
  if (!installedPath(target, targetDir)) return 1;
  settingsLoad(false);
  return gRunAsAdmin ? registerTask(target, targetDir, gRunAtStartup, user) : deleteTask();
}

// Runs elevated (--install): copies this exe to Program Files (closing the running copy,
// which keeps it locked) and sets up the task as settings.json says.
static int installTask(const wchar_t *user, DWORD callerPid) {
  wchar_t exe[MAX_PATH], dir[MAX_PATH], target[MAX_PATH], targetDir[MAX_PATH], stream[MAX_PATH + 20];
  if (!exePath(exe, dir) || !installedPath(target, targetDir)) return 1;
  CreateDirectoryW(targetDir, NULL);
  if (_wcsicmp(exe, target) != 0) {
    stopRunningInstance(callerPid);
    bool copied = false;
    for (int i = 0; i < 40 && !copied; i++) {  // the previous copy may take a moment to exit
      copied = CopyFileW(exe, target, FALSE);
      if (!copied) Sleep(250);
    }
    if (!copied) return 2;
    // Drop the "downloaded from the internet" mark, so starting it never shows a warning.
    _snwprintf(stream, MAX_PATH + 20, L"%ls:Zone.Identifier", target);
    DeleteFileW(stream);
  }
  return configureTask(user);
}

// Runs this exe with admin rights (UAC prompt unless already elevated) and waits for it.
// Returns its exit code, or -1 if it couldn't start (e.g. the UAC prompt was declined).
static int runElevated(const wchar_t *params) {
  wchar_t exe[MAX_PATH], dir[MAX_PATH];
  if (!exePath(exe, dir)) return -1;
  SHELLEXECUTEINFOW sei = { 0 };
  sei.cbSize = sizeof sei;
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  sei.lpVerb = L"runas";
  sei.lpFile = exe;
  sei.lpParameters = params;
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
  WaitForSingleObject(sei.hProcess, 30000);
  DWORD code = 1;
  GetExitCodeProcess(sei.hProcess, &code);
  CloseHandle(sei.hProcess);
  return (int)code;
}

static void userParam(wchar_t *out, size_t n) {
  const wchar_t *d = _wgetenv(L"USERDOMAIN"), *u = _wgetenv(L"USERNAME");
  // Passed along in case the UAC prompt is answered with another admin account.
  if (d && u) _snwprintf(out, n, L"\"%ls\\%ls\"", d, u);
  else _snwprintf(out, n, L"\"\"");
}

// 0: done, -1: UAC prompt declined, other: failed
static int installViaUac(void) {
  wchar_t user[300], params[400], target[MAX_PATH], targetDir[MAX_PATH];
  userParam(user, 300);
  _snwprintf(params, 400, L"--install %ls %lu", user, GetCurrentProcessId());
  int code = runElevated(params);
  if (code != 0) return code;
  if (installedPath(target, targetDir)) setTaskMarker(gRunAsAdmin ? target : NULL, gRunAtStartup);
  setInstallDeclined(false);
  return 0;
}

static int configureViaUac(void) {
  wchar_t user[300], params[400], target[MAX_PATH], targetDir[MAX_PATH];
  userParam(user, 300);
  _snwprintf(params, 400, L"--configure %ls", user);
  int code = isElevated() ? configureTask(NULL) : runElevated(params);
  if (code == 0 && installedPath(target, targetDir)) setTaskMarker(gRunAsAdmin ? target : NULL, gRunAtStartup);
  return code;
}

// Shown by the installed copy right after installing, since nothing else is visible.
static void markJustInstalled(bool set) {
  wchar_t path[MAX_PATH];
  if (!dataFile(path, L"just-installed")) return;
  if (set) writeUtf16(path, L"");
  else DeleteFileW(path);
}

static bool justInstalled(void) {
  wchar_t path[MAX_PATH];
  return dataFile(path, L"just-installed") && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static bool startTask(void) { return runHidden(L"/run /tn " TASK_NAME) == 0; }

// Starts exe without admin rights, even from an elevated process: Explorer (which runs as
// the user) starts it on our behalf.
static void launchAsUser(const wchar_t *exe) {
  if (!isElevated()) { ShellExecuteW(NULL, L"open", exe, NULL, NULL, SW_SHOWNORMAL); return; }
  wchar_t args[MAX_PATH + 4];
  _snwprintf(args, MAX_PATH + 4, L"\"%ls\"", exe);
  ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

// Starts the installed copy the way settings.json says: through the task (admin rights,
// no prompt) or as the user.
static bool launchInstalled(void) {
  wchar_t target[MAX_PATH], targetDir[MAX_PATH];
  if (gRunAsAdmin && startTask()) return true;
  if (!installedPath(target, targetDir)) return false;
  launchAsUser(target);
  return true;
}

// Hands over to another instance of Macify: this one quits once it has been started.
static void switchInstance(bool toTask, const wchar_t *exe) {
  if (gCurveDirty) curveSave();
  if (gMutex) { CloseHandle(gMutex); gMutex = NULL; }  // let the new instance start
  if (toTask) { if (!startTask()) return; }
  else launchAsUser(exe);
  quit();
}

// Makes Windows match runAtStartup / runAsAdministrator, then makes sure this instance
// runs the way they say (switching to an admin or a non-admin instance if needed).
static void applyStartup(void) {
  wchar_t exe[MAX_PATH], dir[MAX_PATH], target[MAX_PATH], targetDir[MAX_PATH];
  if (!exePath(exe, dir) || !installedPath(target, targetDir)) return;
  bool installed = _wcsicmp(exe, target) == 0;
  LOG("startup settings: at startup %d, as admin %d (running %s, from %ls)\n", gRunAtStartup, gRunAsAdmin,
      isElevated() ? "as admin" : "without admin", exe);

  if (gRunAsAdmin) {
    setRunEntry(NULL);
    if (!installed || !taskMarkerMatches(target, gRunAtStartup) || !taskExists()) {
      int code = installed ? configureViaUac() : installViaUac();  // admin copies live in Program Files
      if (code == -1) {  // UAC prompt declined
        gRunAsAdmin = false;
        settingsWrite();
        setRunEntry(gRunAtStartup ? exe : NULL);
        trayNotify(L"Run as administrator is off",
                   L"The admin prompt was declined, so Macify runs without admin rights. Turn it back on in the tray menu.");
        return;
      }
      if (code != 0) {
        trayNotify(L"Macify setup failed", L"The admin startup task couldn't be set up.");
        return;
      }
    }
    if (!isElevated() || !installed) switchInstance(true, NULL);
  } else {
    setRunEntry(gRunAtStartup ? exe : NULL);
    if (taskExists()) {
      int code = configureViaUac();  // removes the task
      if (code != 0 && taskExists()) {
        gRunAsAdmin = true;
        settingsWrite();
        trayNotify(L"Run as administrator is still on", L"The admin task couldn't be removed without admin rights.");
        return;
      }
    }
    setTaskMarker(NULL, false);
    if (isElevated()) switchInstance(false, exe);
  }
}

static void toggleSetting(bool *value) {
  *value = !*value;
  settingsWrite();
  applyStartup();
}

// ---------------------------------------------------------------- debug log
//
// Records what Macify does (touchpad detection, drags, filtered jumps, learning, remapped
// shortcuts and clicks), never what you type: only remapped shortcuts are logged.

#define LOG_MAX_BYTES (16u << 20)
static CRITICAL_SECTION gLogLock;
static char *gLog;
static size_t gLogLen, gLogCap;
static bool gLogFull;
static LARGE_INTEGER gLogStart;

static void debugLog(const char *fmt, ...) {  // may be called from both threads
  char line[512];
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  int n = snprintf(line, sizeof line, "[%9.3f] ",
                   (double)(now.QuadPart - gLogStart.QuadPart) / (double)gFreq.QuadPart);
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof line - n, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  size_t len = (size_t)n + (size_t)m;
  if (len > sizeof line - 1) len = sizeof line - 1;

  EnterCriticalSection(&gLogLock);
  if (gDebugOn && !gLogFull) {
    if (gLogLen + len + 64 > LOG_MAX_BYTES) {
      const char *full = "... log full, nothing further recorded\n";
      memcpy(gLog + gLogLen, full, strlen(full));
      gLogLen += strlen(full);
      gLogFull = true;
    } else {
      if (gLogLen + len > gLogCap) {
        size_t cap = gLogCap ? gLogCap * 2 : (256u << 10);
        while (cap < gLogLen + len) cap *= 2;
        if (cap > LOG_MAX_BYTES) cap = LOG_MAX_BYTES;
        char *grown = realloc(gLog, cap);
        if (!grown) { LeaveCriticalSection(&gLogLock); return; }
        gLog = grown;
        gLogCap = cap;
      }
      memcpy(gLog + gLogLen, line, len);
      gLogLen += len;
    }
  }
  LeaveCriticalSection(&gLogLock);
}

static void logRegString(const char *label, const wchar_t *name) {
  wchar_t v[128];
  DWORD size = sizeof v;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name,
                   RRF_RT_REG_SZ, NULL, v, &size) == ERROR_SUCCESS)
    LOG("%s: %ls\n", label, v);
}

// Everything worth knowing at the start of a log.
static void logState(void) {
  SYSTEMTIME t;
  GetLocalTime(&t);
  LOG("Macify %s debug log, started %04d-%02d-%02d %02d:%02d:%02d\n", MACIFY_VERSION_STR,
      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  logRegString("Windows", L"ProductName");
  logRegString("Windows version", L"DisplayVersion");
  logRegString("Windows build", L"CurrentBuild");
  LOG("admin rights: %s, installed copy: %s, run at startup: %s\n", isElevated() ? "yes" : "no",
      isInstalledCopy() ? "yes" : "no", gRunAtStartup ? "on" : "off");
  LOG("pointer settings: touchpad cursor speed %d, mouse speed %d, enhance precision %d\n",
      gSettingsSig[0], gSettingsSig[1], gSettingsSig[2]);
  HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));
  LOG("keyboard layout: %08lx\n", (unsigned long)(ULONG_PTR)hkl);
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  LOG("screens: %d monitor(s), desktop %d,%d %dx%d px, system DPI %u\n", GetSystemMetrics(SM_CMONITORS),
      vx, vy, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), GetDpiForSystem());
  if (gDeviceCount == 0) LOG("touchpads: none seen yet (touch the touchpad)\n");
  for (int i = 0; i < gDeviceCount; i++) {
    Device *d = &gDevices[i];
    int conf = 0;
    for (int k = 0; k < d->slotCount; k++) conf += d->slotHasConfidence[k];
    LOG("touchpad %d: %s, %d contact slot(s), contact count %s, palm flags on %d slot(s), %.4f x %.4f mm/unit\n",
        i, d->usable ? "usable" : "NOT usable", d->slotCount, d->hasCount ? "yes" : "no", conf,
        d->mmPerUnitX, d->mmPerUnitY);
  }
  LOG("speed curve (%d/%d ranges learned):\n", learnedBins(), LEARN_BINS);
  for (int b = 0; b < LEARN_BINS; b++)
    if (gBinMm[b] > 0)
      LOG("  %6.1f mm/s: %6.2f px/mm from %.0f mm of movement\n", binSpeed(b), gBinPx[b] / gBinMm[b], gBinMm[b]);
  LOG("settings: runAtStartup %d, runAsAdministrator %d, threeFingerDragAsOneFinger %d, threeFingerDragSpeed %.2f\n",
      gRunAtStartup, gRunAsAdmin, gAsOneFinger, gSpeedMul);
  LOG("--- events ---\n");
}

static void debugStart(void) {
  EnterCriticalSection(&gLogLock);
  free(gLog);
  gLog = NULL;
  gLogLen = gLogCap = 0;
  gLogFull = false;
  QueryPerformanceCounter(&gLogStart);
  gDebugOn = true;
  LeaveCriticalSection(&gLogLock);
  logState();
  trayNotify(L"Macify debug log started",
             L"Reproduce the problem, then choose \"Finish debug log\" in this menu to save it to your desktop.");
}

static void debugFinish(void) {
  LOG("--- finished ---\n");
  EnterCriticalSection(&gLogLock);
  gDebugOn = false;
  char *log = gLog;
  size_t len = gLogLen;
  gLog = NULL;
  gLogLen = gLogCap = 0;
  LeaveCriticalSection(&gLogLock);

  wchar_t *desktop = NULL, path[MAX_PATH], name[64];
  SYSTEMTIME t;
  GetLocalTime(&t);
  _snwprintf(name, 64, L"Macify debug log %04d-%02d-%02d %02d.%02d.%02d.txt",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  bool saved = false;
  if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Desktop, 0, NULL, &desktop))) {
    _snwprintf(path, MAX_PATH, L"%ls\\%ls", desktop, name);
    FILE *f = _wfopen(path, L"wb");
    if (f) {
      saved = fwrite(log ? log : "", 1, len, f) == len;
      fclose(f);
    }
    CoTaskMemFree(desktop);
  }
  free(log);
  wchar_t text[256];
  if (saved) _snwprintf(text, 256, L"Saved to your desktop as \"%ls\".", name);
  else _snwprintf(text, 256, L"The log couldn't be saved to your desktop.");
  trayNotify(saved ? L"Macify debug log saved" : L"Macify debug log not saved", text);
}

// ---------------------------------------------------------------- settings file

#define FOUND_STARTUP 1
#define FOUND_ADMIN   2
#define FOUND_ASONE   4
#define FOUND_SPEED   8
#define FOUND_ALL     15

typedef struct {
  bool runAtStartup, runAsAdmin, asOneFinger;
  double speed;
  int found;  // which settings the file had: FOUND_* bits
} Settings;

static FILETIME gSettingsTime;
static wchar_t gSettingsError[256];  // a problem found before the tray icon existed
static HANDLE gSettingsWatch = INVALID_HANDLE_VALUE;

// A small parser for a flat JSON object of booleans, numbers and strings. Comments
// (// and /* */) are allowed; unknown keys are ignored. On a syntax error, *err says
// where and false is returned.
typedef struct { const char *p, *start; } Json;

static void jsonSkip(Json *j) {
  for (;;) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n') j->p++;
    if (j->p[0] == '/' && j->p[1] == '/') { while (*j->p && *j->p != '\n') j->p++; continue; }
    if (j->p[0] == '/' && j->p[1] == '*') {
      const char *e = strstr(j->p + 2, "*/");
      j->p = e ? e + 2 : j->p + strlen(j->p);
      continue;
    }
    if ((unsigned char)j->p[0] == 0xEF && (unsigned char)j->p[1] == 0xBB && (unsigned char)j->p[2] == 0xBF) { j->p += 3; continue; }
    return;
  }
}

static int jsonLine(const Json *j) {
  int line = 1;
  for (const char *c = j->start; c < j->p; c++) if (*c == '\n') line++;
  return line;
}

static bool jsonString(Json *j, char *out, size_t n) {
  if (*j->p != '"') return false;
  size_t o = 0;
  for (j->p++; *j->p && *j->p != '"'; j->p++) {
    if (*j->p == '\\' && j->p[1]) j->p++;
    if (o + 1 < n) out[o++] = *j->p;
  }
  if (*j->p != '"') return false;
  j->p++;
  out[o] = 0;
  return true;
}

static bool jsonBool(Json *j, bool *out) {
  if (strncmp(j->p, "true", 4) == 0) { *out = true; j->p += 4; return true; }
  if (strncmp(j->p, "false", 5) == 0) { *out = false; j->p += 5; return true; }
  return false;
}

static bool parseSettings(const char *text, Settings *st, char *err, size_t errLen) {
  Json j = { text, text };
  jsonSkip(&j);
  if (*j.p != '{') { snprintf(err, errLen, "expected { at line %d", jsonLine(&j)); return false; }
  j.p++;
  for (;;) {
    jsonSkip(&j);
    if (*j.p == '}') return true;
    char key[64];
    if (!jsonString(&j, key, sizeof key)) { snprintf(err, errLen, "expected a \"name\" at line %d", jsonLine(&j)); return false; }
    jsonSkip(&j);
    if (*j.p != ':') { snprintf(err, errLen, "expected : after \"%s\" at line %d", key, jsonLine(&j)); return false; }
    j.p++;
    jsonSkip(&j);
    int line = jsonLine(&j);
    int bit = strcmp(key, "runAtStartup") == 0 ? FOUND_STARTUP
            : strcmp(key, "runAsAdministrator") == 0 ? FOUND_ADMIN
            : strcmp(key, "threeFingerDragAsOneFinger") == 0 ? FOUND_ASONE : 0;
    bool *flag = bit == FOUND_STARTUP ? &st->runAtStartup : bit == FOUND_ADMIN ? &st->runAsAdmin
               : bit == FOUND_ASONE ? &st->asOneFinger : NULL;
    if (flag) {
      if (!jsonBool(&j, flag)) { snprintf(err, errLen, "\"%s\" must be true or false (line %d)", key, line); return false; }
      st->found |= bit;
    } else if (strcmp(key, "threeFingerDragSpeed") == 0) {
      char *end;
      double v = strtod(j.p, &end);
      if (end == j.p || !(v > 0) || v > 20) {
        snprintf(err, errLen, "\"%s\" must be a number above 0, like 1.0 or 1.5 (line %d)", key, line);
        return false;
      }
      st->speed = v;
      st->found |= FOUND_SPEED;
      j.p = end;
    } else {  // unknown key: skip its value
      char tmp[256];
      if (*j.p == '"') { if (!jsonString(&j, tmp, sizeof tmp)) { snprintf(err, errLen, "unfinished text at line %d", line); return false; } }
      else while (*j.p && *j.p != ',' && *j.p != '}' && *j.p != '\n') j.p++;
      LOG("settings: ignoring unknown \"%s\"\n", key);
    }
    jsonSkip(&j);
    if (*j.p == ',') { j.p++; continue; }
    if (*j.p == '}') return true;
    snprintf(err, errLen, "expected , or } at line %d", jsonLine(&j));
    return false;
  }
}

static bool settingsPath(wchar_t *path) { return dataFile(path, L"settings.json"); }

// Writes the current settings to settings.json (used by the tray menu, and to add
// settings a file from an older version doesn't have yet).
static void settingsWrite(void) {
  wchar_t path[MAX_PATH];
  if (!settingsPath(path)) return;
  char speed[32];
  snprintf(speed, sizeof speed, gSpeedMul == floor(gSpeedMul) ? "%.1f" : "%g", gSpeedMul);
  FILE *f = _wfopen(path, L"wb");
  if (!f) return;
  fprintf(f, "{\n"
             "  \"runAtStartup\": %s,\n"
             "  \"runAsAdministrator\": %s,\n"
             "  \"threeFingerDragAsOneFinger\": %s,\n"
             "  \"threeFingerDragSpeed\": %s\n"
             "}\n",
          gRunAtStartup ? "true" : "false", gRunAsAdmin ? "true" : "false", gAsOneFinger ? "true" : "false", speed);
  fclose(f);
  WIN32_FILE_ATTRIBUTE_DATA a;  // our own write: not a change to react to
  if (GetFileAttributesExW(path, GetFileExInfoStandard, &a)) gSettingsTime = a.ftLastWriteTime;
}

// Loads settings.json if it changed since last time (creating it with the defaults if
// it doesn't exist). userEdit: the user saved it, so apply and confirm the changes.
static void settingsLoad(bool userEdit) {
  wchar_t path[MAX_PATH];
  if (!settingsPath(path)) return;
  WIN32_FILE_ATTRIBUTE_DATA a;
  if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) { settingsWrite(); return; }
  if (CompareFileTime(&a.ftLastWriteTime, &gSettingsTime) == 0) return;  // unchanged
  gSettingsTime = a.ftLastWriteTime;

  FILE *f = _wfopen(path, L"rb");
  if (!f) return;
  char text[8192];
  size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = 0;
  if (n == 0 && userEdit) return;  // editors sometimes truncate before writing: wait for the real save

  Settings st = { true, true, true, 1.0, 0 };  // defaults for anything the file leaves out
  char err[160];
  if (!parseSettings(text, &st, err, sizeof err)) {
    LOG("settings.json not applied: %s\n", err);
    _snwprintf(gSettingsError, 256, L"settings.json has a mistake: %hs. Keeping the previous settings.", err);
    if (gWnd) { trayNotify(L"Macify settings not applied", gSettingsError); gSettingsError[0] = 0; }
    return;
  }
  // Until 1.5, threeFingerDragAsOneFinger meant the opposite (true: follow the finger that
  // moves most). Those files don't have the startup settings yet: keep their behaviour.
  if ((st.found & FOUND_ASONE) && !(st.found & (FOUND_STARTUP | FOUND_ADMIN))) st.asOneFinger = !st.asOneFinger;
  bool dragChanged = st.asOneFinger != gAsOneFinger || st.speed != gSpeedMul;
  bool startupChanged = st.runAtStartup != gRunAtStartup || st.runAsAdmin != gRunAsAdmin;
  gRunAtStartup = st.runAtStartup;
  gRunAsAdmin = st.runAsAdmin;
  gAsOneFinger = st.asOneFinger;
  gSpeedMul = st.speed;
  LOG("settings: runAtStartup %d, runAsAdministrator %d, threeFingerDragAsOneFinger %d, threeFingerDragSpeed %.2f\n",
      gRunAtStartup, gRunAsAdmin, gAsOneFinger, gSpeedMul);
  if (st.found != FOUND_ALL) settingsWrite();  // from an older version: add the missing settings
  if (userEdit && dragChanged) {
    wchar_t msg[160];
    _snwprintf(msg, 160, L"Three-finger drag follows %ls, speed %.2fx.",
               gAsOneFinger ? L"the centre of the fingers" : L"the finger that moves most", gSpeedMul);
    trayNotify(L"Macify settings applied", msg);
  }
  if (userEdit && startupChanged && gWnd) applyStartup();
}

// Watches %APPDATA%\Macify so saved changes apply right away.
static void settingsWatchStart(void) {
  wchar_t path[MAX_PATH];
  if (!settingsPath(path)) return;
  wchar_t *slash = wcsrchr(path, L'\\');
  if (slash) *slash = 0;
  gSettingsWatch = FindFirstChangeNotificationW(path, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
}

// Opens settings.json in the default editor, without admin rights: Explorer (which runs
// as the user) opens it on our behalf.
static void settingsEdit(void) {
  wchar_t path[MAX_PATH], args[MAX_PATH + 4];
  if (!settingsPath(path)) return;
  settingsLoad(false);  // make sure it exists
  _snwprintf(args, MAX_PATH + 4, L"\"%ls\"", path);
  ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------- updates
//
// "Check for updates" asks GitHub for the latest release of adamakesson/macify. If its
// version is higher and it has a macify.exe attached, that is downloaded and, after you
// confirm, started with "--update": it waits for this copy to exit, installs itself in
// Program Files the same way a first install does, and starts.

typedef struct {
  int status;  // 0: up to date, 1: newer version downloaded, 2: no releases, -1: error
  char tag[32];
  wchar_t file[MAX_PATH];
  char error[160];
} UpdateResult;

// GET a URL into memory (redirects are followed). Returns false on network errors.
static bool httpGet(const wchar_t *url, bool api, char **out, size_t *outLen, DWORD *status) {
  wchar_t host[256], path[2048], agent[32];
  URL_COMPONENTS uc = { 0 };
  uc.dwStructSize = sizeof uc;
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(url, 0, 0, &uc)) return false;
  _snwprintf(agent, 32, L"Macify/%hs", MACIFY_VERSION_STR);  // GitHub requires a User-Agent

  bool ok = false;
  char *buf = NULL;
  size_t len = 0, cap = 0;
  HINTERNET s = WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
  HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : NULL;
  if (r && api) WinHttpAddRequestHeaders(r, L"Accept: application/vnd.github+json", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
  if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(r, NULL)) {
    DWORD code = 0, size = sizeof code;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &size, WINHTTP_NO_HEADER_INDEX);
    *status = code;
    ok = true;
    for (DWORD avail = 0; WinHttpQueryDataAvailable(r, &avail) && avail > 0;) {
      if (len + avail + 1 > cap) {
        cap = (len + avail + 1) * 2;
        if (cap > (64u << 20)) { ok = false; break; }  // nothing of ours is this big
        char *grown = realloc(buf, cap);
        if (!grown) { ok = false; break; }
        buf = grown;
      }
      DWORD got = 0;
      if (!WinHttpReadData(r, buf + len, avail, &got)) { ok = false; break; }
      len += got;
    }
  }
  if (r) WinHttpCloseHandle(r);
  if (c) WinHttpCloseHandle(c);
  if (s) WinHttpCloseHandle(s);
  if (!ok) { free(buf); return false; }
  if (!buf) buf = calloc(1, 1);
  else buf[len] = 0;
  *out = buf;
  *outLen = len;
  return true;
}

// The string value after "key": at or after *from (advanced past it). JSON escapes are
// only unescaped for "\/", which is all GitHub uses in these fields.
static bool jsonValue(const char **from, const char *key, char *out, size_t n) {
  char pattern[64];
  snprintf(pattern, sizeof pattern, "\"%s\"", key);
  const char *k = strstr(*from, pattern);
  if (!k) return false;
  const char *v = strchr(k + strlen(pattern), '"');
  if (!v) return false;
  size_t o = 0;
  for (v++; *v && *v != '"'; v++) {
    if (*v == '\\' && v[1]) v++;
    if (o + 1 < n) out[o++] = *v;
  }
  out[o] = 0;
  *from = v;
  return true;
}

// Compares "v1.10" with "1.9" etc.: negative, 0 or positive like strcmp.
static int compareVersions(const char *a, const char *b) {
  while (*a && !(*a >= '0' && *a <= '9')) a++;
  while (*b && !(*b >= '0' && *b <= '9')) b++;
  while (*a || *b) {
    long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
    if (x != y) return x < y ? -1 : 1;
    if (*a == '.') a++;
    if (*b == '.') b++;
    if (!(*a >= '0' && *a <= '9') && !(*b >= '0' && *b <= '9')) break;
  }
  return 0;
}

static DWORD WINAPI updateThread(LPVOID unused) {
  (void)unused;
  UpdateResult *res = calloc(1, sizeof *res);
  if (!res) { gUpdateBusy = false; return 0; }
  char *body = NULL;
  size_t len = 0;
  DWORD status = 0;
  if (!httpGet(UPDATE_API, true, &body, &len, &status)) {
    res->status = -1;
    snprintf(res->error, sizeof res->error, "couldn't reach GitHub (are you online?)");
  } else if (status == 404) {
    res->status = 2;
  } else if (status != 200) {
    res->status = -1;
    snprintf(res->error, sizeof res->error, "GitHub answered with error %lu", (unsigned long)status);
  } else {
    const char *p = body;
    char url[1024] = "", candidate[1024];
    if (!jsonValue(&p, "tag_name", res->tag, sizeof res->tag)) {
      res->status = -1;
      snprintf(res->error, sizeof res->error, "unexpected answer from GitHub");
    } else if (compareVersions(res->tag, MACIFY_VERSION_STR) <= 0) {
      res->status = 0;
    } else {
      p = body;
      while (jsonValue(&p, "browser_download_url", candidate, sizeof candidate)) {
        size_t n = strlen(candidate);
        if (n >= 11 && _stricmp(candidate + n - 11, "/macify.exe") == 0) { strcpy(url, candidate); break; }
      }
      if (!*url) {
        res->status = -1;
        snprintf(res->error, sizeof res->error, "release %s has no macify.exe attached", res->tag);
      }
    }
    LOG("update check: latest %s, running %s\n", res->tag, MACIFY_VERSION_STR);
    if (*url) {
      wchar_t wurl[1024];
      MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
      char *exe = NULL;
      size_t exeLen = 0;
      if (!httpGet(wurl, false, &exe, &exeLen, &status) || status != 200) {
        res->status = -1;
        snprintf(res->error, sizeof res->error, "the download of %s failed", res->tag);
      } else if (exeLen < 20000 || exe[0] != 'M' || exe[1] != 'Z') {  // not a Windows program
        res->status = -1;
        snprintf(res->error, sizeof res->error, "the download of %s isn't a valid macify.exe", res->tag);
      } else {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        _snwprintf(res->file, MAX_PATH, L"%lsmacify-update-%hs.exe", tmp, res->tag);
        FILE *f = _wfopen(res->file, L"wb");
        bool written = f && fwrite(exe, 1, exeLen, f) == exeLen;
        if (f) fclose(f);
        res->status = written ? 1 : -1;
        if (!written) snprintf(res->error, sizeof res->error, "the update couldn't be saved");
      }
      free(exe);
    }
  }
  free(body);
  PostMessageW(gWnd, WM_UPDATE_RESULT, 0, (LPARAM)res);
  return 0;
}

static void updateCheck(void) {
  if (gUpdateBusy) return;
  gUpdateBusy = true;
  HANDLE t = CreateThread(NULL, 0, updateThread, NULL, 0, NULL);
  if (t) CloseHandle(t);
  else gUpdateBusy = false;
}

static void updateResult(UpdateResult *res) {
  gUpdateBusy = false;
  wchar_t msg[256];
  if (res->status == 0) {
    _snwprintf(msg, 256, L"You have the latest version, %hs.", MACIFY_VERSION_STR);
    trayNotify(L"Macify is up to date", msg);
  } else if (res->status == 2) {
    trayNotify(L"No Macify releases yet", L"There are no published releases on GitHub to update from.");
  } else if (res->status < 0) {
    _snwprintf(msg, 256, L"The update check failed: %hs.", res->error);
    trayNotify(L"Macify update failed", msg);
  } else {
    _snwprintf(msg, 256, L"Macify %hs is available (you have %hs).\n\nInstall it now?", res->tag, MACIFY_VERSION_STR);
    if (MessageBoxW(gWnd, msg, L"Macify update", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES) {
      wchar_t params[32];
      _snwprintf(params, 32, L"--update %lu", GetCurrentProcessId());
      SHELLEXECUTEINFOW sei = { 0 };
      sei.cbSize = sizeof sei;
      sei.lpVerb = L"runas";  // admin rights for Program Files (no prompt if already elevated)
      sei.lpFile = res->file;
      sei.lpParameters = params;
      sei.nShow = SW_HIDE;
      if (ShellExecuteExW(&sei)) quit();  // the update takes over once we're gone
    } else {
      DeleteFileW(res->file);
    }
  }
  free(res);
}

static void flagFile(const wchar_t *name, bool set) {
  wchar_t path[MAX_PATH];
  if (!dataFile(path, name)) return;
  if (set) writeUtf16(path, L"");
  else DeleteFileW(path);
}

static bool takeFlag(const wchar_t *name) {
  wchar_t path[MAX_PATH];
  if (!dataFile(path, name) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
  DeleteFileW(path);
  return true;
}

// Runs from the downloaded exe ("--update <pid>"), with admin rights.
static int applyUpdate(DWORD oldPid) {
  HANDLE old = oldPid ? OpenProcess(SYNCHRONIZE, FALSE, oldPid) : NULL;
  if (old) { WaitForSingleObject(old, 15000); CloseHandle(old); }
  int code = installTask(NULL, 0);  // also reads settings.json
  if (code == 0) {
    wchar_t target[MAX_PATH], targetDir[MAX_PATH];
    if (installedPath(target, targetDir)) setTaskMarker(gRunAsAdmin ? target : NULL, gRunAtStartup);
    setInstallDeclined(false);
    flagFile(L"just-updated", true);
  } else {
    flagFile(L"update-failed", true);
  }
  launchInstalled();
  return code;
}

// Earlier downloads in %TEMP% (an update can't delete itself while running).
static void cleanUpdateDownloads(void) {
  wchar_t tmp[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH], self[MAX_PATH];
  GetModuleFileNameW(NULL, self, MAX_PATH);
  GetTempPathW(MAX_PATH, tmp);
  _snwprintf(pattern, MAX_PATH, L"%lsmacify-update-*.exe", tmp);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    _snwprintf(path, MAX_PATH, L"%ls%ls", tmp, fd.cFileName);
    if (_wcsicmp(path, self) != 0) DeleteFileW(path);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

// ---------------------------------------------------------------- window plumbing

static LRESULT CALLBACK wndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INPUT:
      onRawInput((HRAWINPUT)lp);
      break;  // DefWindowProc must still see WM_INPUT
    case WM_TIMER:
      if (wp == TIMER_RELEASE) { endDrag(); return 0; }
      if (wp == TIMER_SETTINGS) { KillTimer(w, TIMER_SETTINGS); settingsLoad(true); return 0; }
      if (wp == TIMER_TRAY_RETRY) { trayAdd(); return 0; }
      if (wp == TIMER_SAVE) {
        checkSettings();
        if (gSettingsWatch == INVALID_HANDLE_VALUE) settingsLoad(true);
        if (gCurveDirty) curveSave();
        return 0;
      }
      break;
    case WM_UPDATE_RESULT:
      updateResult((UpdateResult *)lp);
      return 0;
    case WM_CANCEL_DRAG:
      if (gReleasePending) { LOG("cursor moved during grace period\n"); endDrag(); }
      return 0;
    case WM_TRAY:
      if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) showTrayMenu();
      return 0;
    case WM_SETTINGCHANGE:
      if (lp && wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") == 0) trayRefreshIcon();
      checkSettings();
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      if (msg == gTaskbarCreated && gTaskbarCreated) { trayAdd(); return 0; }  // Explorer restarted
      if (msg == gQuitMsg && gQuitMsg) {  // a newer copy is being installed
        quit();
        return 0;
      }
  }
  return DefWindowProcW(w, msg, wp, lp);
}

// First run after the rename: move %APPDATA%\three-finger-drag to %APPDATA%\Macify.
static void migrateOldData(void) {
  const wchar_t *appData = _wgetenv(L"APPDATA");
  if (!appData) return;
  wchar_t from[MAX_PATH], to[MAX_PATH];
  _snwprintf(from, MAX_PATH, L"%ls\\" OLD_NAME, appData);
  _snwprintf(to, MAX_PATH, L"%ls\\Macify", appData);
  if (GetFileAttributesW(to) == INVALID_FILE_ATTRIBUTES) MoveFileW(from, to);
}

static int run(void) {
  InitializeCriticalSection(&gLogLock);
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && argc >= 2 && wcscmp(argv[1], L"--install") == 0)
    return installTask(argc >= 3 ? argv[2] : NULL, argc >= 4 ? wcstoul(argv[3], NULL, 10) : 0);
  if (argv && argc >= 2 && wcscmp(argv[1], L"--configure") == 0) return configureTask(argc >= 3 ? argv[2] : NULL);
  if (argv && argc >= 2 && wcscmp(argv[1], L"--update") == 0)
    return applyUpdate(argc >= 3 ? wcstoul(argv[2], NULL, 10) : 0);

  // The old version (before the rename) still running would fight over the same input.
  HANDLE old = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\" OLD_NAME);
  if (old || GetLastError() == ERROR_ACCESS_DENIED) {
    if (old) CloseHandle(old);
    MessageBoxW(NULL, L"The old three-finger-drag app is still running.\n\n"
                      L"Quit it from its tray icon (three dots), then start Macify again.",
                L"Macify", MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  migrateOldData();
  gQuitMsg = RegisterWindowMessageW(L"MacifyQuit");
  settingsLoad(false);
  wchar_t oldMarker[MAX_PATH];  // replaced by task.txt
  if (dataFile(oldMarker, L"autostart.txt")) DeleteFileW(oldMarker);
  if (dataFile(oldMarker, L"autostart-off")) DeleteFileW(oldMarker);

  // Started from anywhere but Program Files (a download, a fresh build): install or update.
  if (!isElevated() && !isInstalledCopy() && !installDeclined()) {
    int result = installViaUac();
    if (result == 0) {
      markJustInstalled(true);
      if (launchInstalled()) return 0;
      markJustInstalled(false);
    } else if (result == -1) {  // UAC declined: run from here, without admin rights, and don't ask again
      setInstallDeclined(true);
      gRunAsAdmin = false;
      settingsWrite();
    } else {
      MessageBoxW(NULL, L"Macify couldn't be installed in Program Files.\n\n"
                        L"If it is running, quit it from its tray icon, then run this again.",
                  L"Macify", MB_OK | MB_ICONWARNING);
      return 0;
    }
  }

  HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\Macify");
  gMutex = mutex;
  DWORD err = GetLastError();
  // ACCESS_DENIED: an elevated instance already holds it.
  if ((mutex && err == ERROR_ALREADY_EXISTS) || (!mutex && err == ERROR_ACCESS_DENIED)) {
    return 0;
  }

  // Started without admin rights (e.g. from the Start menu) while it should have them:
  // the task starts the admin instance, without a prompt.
  if (!isElevated() && gRunAsAdmin && isInstalledCopy() && taskExists()) {
    if (gMutex) { CloseHandle(gMutex); gMutex = NULL; }
    if (startTask()) return 0;
    // couldn't start the task: carry on, applyStartup below sorts it out
  }
  ensureStartMenuShortcut();

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  QueryPerformanceFrequency(&gFreq);
  QueryPerformanceCounter(&gLastFrame);
  gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSW wc = { 0 };
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"MacifyWindow";
  if (!RegisterClassW(&wc)) return 1;
  // Hidden top-level window (never shown): receives touchpad input, tray and Explorer messages.
  gWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Macify", WS_POPUP,
                         0, 0, 0, 0, NULL, NULL, inst, NULL);
  if (!gWnd) return 1;

  // When running elevated, let the (non-elevated) taskbar reach us.
  ChangeWindowMessageFilterEx(gWnd, WM_TRAY, MSGFLT_ALLOW, NULL);
  ChangeWindowMessageFilterEx(gWnd, gTaskbarCreated, MSGFLT_ALLOW, NULL);
  ChangeWindowMessageFilterEx(gWnd, WM_COMMAND, MSGFLT_ALLOW, NULL);
  ChangeWindowMessageFilterEx(gWnd, gQuitMsg, MSGFLT_ALLOW, NULL);

  RAWINPUTDEVICE rid = { PAGE_DIGITIZER, USAGE_TOUCHPAD, RIDEV_INPUTSINK, gWnd };
  if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) {
    LOG("RegisterRawInputDevices failed: %lu\n", GetLastError());
    return 1;
  }
  LOG("listening for touchpad input\n");

  DWORD remapThreadId = 0;
  HANDLE remap = CreateThread(NULL, 0, remapThread, NULL, 0, &remapThreadId);

  curveLoad();
  settingsLoad(false);
  settingsWatchStart();
  SetTimer(gWnd, TIMER_SAVE, SAVE_INTERVAL_MS, NULL);
  gIcon = makeIcon();
  trayAdd();
  cleanUpdateDownloads();
  if (gSettingsError[0]) { trayNotify(L"Macify settings not applied", gSettingsError); gSettingsError[0] = 0; }
  applyStartup();
  if (takeFlag(L"just-updated")) {
    wchar_t msg[96];
    _snwprintf(msg, 96, L"You're now running Macify %hs.", MACIFY_VERSION_STR);
    trayNotify(L"Macify updated", msg);
  }
  if (takeFlag(L"update-failed"))
    trayNotify(L"Macify update failed", L"The new version couldn't be installed in Program Files, so the previous one is still running.");
  if (justInstalled()) {
    markJustInstalled(false);
    trayNotify(L"Macify is installed and running",
               L"It starts with Windows from now on. Its menu is behind this icon in the system tray.");
  }

  MSG m;
  for (bool running = true; running;) {
    DWORD handles = gSettingsWatch != INVALID_HANDLE_VALUE ? 1 : 0;
    DWORD r = MsgWaitForMultipleObjects(handles, &gSettingsWatch, FALSE, INFINITE, QS_ALLINPUT);
    if (handles && r == WAIT_OBJECT_0) {  // something in %APPDATA%\Macify changed:
      SetTimer(gWnd, TIMER_SETTINGS, 150, NULL);  // look once the editor has finished writing
      FindNextChangeNotification(gSettingsWatch);
      continue;
    }
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
      if (m.message == WM_QUIT) { running = false; break; }
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  if (gSettingsWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(gSettingsWatch);
  endDrag();
  if (gCurveDirty) curveSave();
  if (remap) {
    PostThreadMessageW(remapThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(remap, 2000);
  }
  return 0;
}

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, PWSTR c, int d) {
  (void)a; (void)b; (void)c; (void)d;
  return run();
}
