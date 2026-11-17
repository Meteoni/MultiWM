// Publishes the read-only snapshot that the MultiWM Companion mod reads before a
// new window is first shown (layout in shared/companion_snapshot.h).
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.
//
// One-way by design: MultiWM writes, the companion reads, and nothing MultiWM
// does depends on the companion. If the section cannot be created, MultiWM runs
// exactly as without it.

namespace CompanionSnapshot {

// One window's invisible resize borders, measured while it was tiled and restored.
// They are valid only while the window's own DPI and frame styles are unchanged:
// both decide the system-drawn borders.
struct MeasuredFrameMargins {
  DWORD processId = 0;  // Guards against a reused HWND value.
  RECT margins{};       // GetWindowRect minus the DWM frame, per side, physical pixels.
  UINT dpi = 0;         // Monitor DPI at measurement; published for the companion's check.
  UINT windowDpi = 0;   // GetDpiForWindow at measurement.
  LONG_PTR style = 0;   // GWL_STYLE and GWL_EXSTYLE at measurement.
  LONG_PTR exStyle = 0;
};

// The window properties a measurement depends on, read now. Cheap user-mode
// reads; no message is sent to the window.
static bool FrameInputsUnchanged(HWND hwnd, const MeasuredFrameMargins& measured) {
  return GetDpiForWindow(hwnd) == measured.windowDpi &&
         GetWindowLongPtrW(hwnd, GWL_STYLE) == measured.style &&
         GetWindowLongPtrW(hwnd, GWL_EXSTYLE) == measured.exStyle;
}

struct Publisher {
  HANDLE section = nullptr;
  HANDLE aliveEvent = nullptr;
  CompanionAbi::SnapshotSection* view = nullptr;
  // The next body is built here, compared with the published one, and copied in
  // only when it differs.
  std::unique_ptr<CompanionAbi::SnapshotBody> staging;
  uint64_t generation = 0;

  // Inputs of the last build; a change triggers a rebuild. Model changes all go
  // through WorkspaceRepository::Save (revision). Work-area, DPI and monitor
  // changes don't, so the tray window's display notifications set
  // `displayChanged`. Settings reloads restart the WM thread, which clears
  // `published` through PublishStopped.
  bool published = false;
  uint64_t repositoryRevision = 0;
  ManagementMode mode = ManagementMode::Manual;
  GUID desktop{};
  bool displayChanged = false;
  bool overflowRoutes = false;  // AutomaticOverflowRoutes: settings, hook health and monitor count.

  // Borders of tracked windows, kept while MultiWM knows them, so a window that
  // is minimized or maximized now still has the values from its restored state.
  std::unordered_map<HWND, MeasuredFrameMargins> frameMargins;
};

// Process-wide: the section outlives WM-thread restarts on settings reload.
static Publisher g_publisher;

// --- Building one body ---------------------------------------------------------

// Measures the resize borders of the workspace's tiled windows that are restored
// and visible. The companion needs them to restore a window into its slot
// exactly. Nothing a minimized or maximized window reports matches them.
// Measured at 192 DPI on a captioned window with 11 px borders: 0 px at the
// bottom while minimized, 13 px on every side while maximized, and 13 px from
// SM_CXSIZEFRAME + SM_CXPADDEDBORDER. Errors of that size fall inside
// kPlacementTolerancePx, so MultiWM would never correct them.
//
// A window is measured again when its own DPI or frame styles change, not only
// when the monitor's DPI does: after a move to another monitor, the window's DPI
// (and with it the borders) changes only when the app handles WM_DPICHANGED.
// A measurement taken while the window's DPI changed is discarded.
// Three cheap reads per cached window per rebuild; two native queries per new
// measurement.
static void RememberFrameMargins(const Workspace& workspace, UINT dpi) {
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (!record) continue;
    const auto cached = g_publisher.frameMargins.find(hwnd);
    if (cached != g_publisher.frameMargins.end() && cached->second.processId == record->pid &&
        cached->second.dpi == dpi && FrameInputsUnchanged(hwnd, cached->second)) {
      continue;
    }
    MeasuredFrameMargins measured;
    measured.processId = record->pid;
    measured.dpi = dpi;
    measured.windowDpi = GetDpiForWindow(hwnd);
    measured.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    measured.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    RECT windowRect{};
    RECT frame{};
    if (!measured.windowDpi || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd) ||
        !GetWindowRect(hwnd, &windowRect) ||
        FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) ||
        frame.right <= frame.left || frame.bottom <= frame.top || !FrameInputsUnchanged(hwnd, measured)) {
      // Unmeasurable now (or changing under us): drop a stale entry rather than
      // keep borders that no longer belong to this window.
      if (cached != g_publisher.frameMargins.end()) g_publisher.frameMargins.erase(cached);
      continue;
    }
    measured.margins = {frame.left - windowRect.left, frame.top - windowRect.top,
                        windowRect.right - frame.right, windowRect.bottom - frame.bottom};
    const RECT& margins = measured.margins;
    if (margins.left < 0 || margins.top < 0 || margins.right < 0 || margins.bottom < 0) {
      if (cached != g_publisher.frameMargins.end()) g_publisher.frameMargins.erase(cached);
      continue;
    }
    g_publisher.frameMargins[hwnd] = measured;
  }
}

// Builds the restore hint for one suspended member, with its measured borders
// when MultiWM has them for this process and the window's DPI and frame styles
// still match the measurement. A suspended window can't be measured again, so a
// mismatch publishes no borders, and the companion then gives no restore hint.
static CompanionAbi::RestoreWindowHint MakeRestoreWindowHint(
    const WindowRecord& record, uint32_t workspaceIndex, const std::optional<RECT>& restoredFrame) {
  CompanionAbi::RestoreWindowHint hint{};
  hint.hwnd = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(record.hwnd));
  hint.processId = record.pid;
  hint.workspaceIndex = workspaceIndex;
  if (restoredFrame) {
    hint.frame = *restoredFrame;
    hint.hasFrame = 1;
  }
  const auto measured = g_publisher.frameMargins.find(record.hwnd);
  if (measured != g_publisher.frameMargins.end() && measured->second.processId == record.pid &&
      FrameInputsUnchanged(record.hwnd, measured->second)) {
    hint.frameMargins = measured->second.margins;
    hint.marginsDpi = measured->second.dpi;
  }
  return hint;
}

// Adds one entry per current monitor that has a workspace on `desktop`. Monitors
// without a workspace yet are left out, so the companion gives no hint there.
static void AppendWorkspaceHints(const GUID& desktop, CompanionAbi::SnapshotBody* body) {
  std::vector<HMONITOR> monitors;
  Reconcile::EnumerateCurrentMonitors(monitors);
  const bool insertAfterFocused =
      g_settings.automaticNewWindowPosition == AutomaticNewWindowPosition::AfterFocused;

  for (HMONITOR monitor : monitors) {
    if (body->workspaceCount == CompanionAbi::kMaxWorkspaces) break;
    DesktopMonitorKey key{};
    Workspace workspace;
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    Platform::Monitors::WorkspaceMetrics metrics;
    if (!DesktopMonitorKey::FromHMonitor(desktop, monitor, &key) ||
        !g_workspaces.Load(key, &workspace) || !GetMonitorInfoW(monitor, &monitorInfo) ||
        !Platform::Monitors::GetWorkspaceMetrics(monitor, &metrics)) {
      continue;
    }

    // Same count as enforcement, so a hint is never given for a window that
    // the limit will release. Free when no limit is configured.
    const bool limitReached = Placement::WouldExceedAutomaticWindowLimit(key, workspace, monitor);
    const Placement::AdmissionPrediction prediction =
        Placement::PredictAutomaticAdmission(workspace, metrics, limitReached, insertAfterFocused);
    RememberFrameMargins(workspace, metrics.dpi);

    const uint32_t workspaceIndex = body->workspaceCount++;
    CompanionAbi::WorkspaceHint& hint = body->workspaces[workspaceIndex];
    hint.monitorRect = monitorInfo.rcMonitor;
    hint.workArea = metrics.workArea;
    hint.dpi = metrics.dpi;
    hint.floatingLayout = workspace.Layout() == TileLayout::Floating ? 1 : 0;
    if (prediction.tiledFrame) {
      hint.tiledFrame = *prediction.tiledFrame;
      hint.hasTiledFrame = 1;
    }
    if (prediction.floatingFrame) {
      hint.floatingFrame = *prediction.floatingFrame;
      hint.hasFloatingFrame = 1;
    }
    hint.newWindowFloats = prediction.newWindowFloats ? 1 : 0;
    if (workspace.Layout() == TileLayout::Floating) continue;

    // One exact restore frame per suspended member: a dry run of the restore
    // MultiWM will perform. About a microsecond per window per rebuild.
    // Records are visited in HWND order so equal models publish equal bytes.
    std::vector<const WindowRecord*> suspended;
    for (const auto& member : workspace.Records()) {
      if (member.second.state == ManageState::Suspended && member.second.hasSavedSlot) {
        suspended.push_back(&member.second);
      }
    }
    std::sort(suspended.begin(), suspended.end(), [](const auto* left, const auto* right) {
      return reinterpret_cast<uintptr_t>(left->hwnd) < reinterpret_cast<uintptr_t>(right->hwnd);
    });
    for (const WindowRecord* record : suspended) {
      if (body->restoreWindowCount == CompanionAbi::kMaxRestoreWindows) break;
      body->restoreWindows[body->restoreWindowCount++] = MakeRestoreWindowHint(
          *record, workspaceIndex, Placement::PredictRestoredFrame(workspace, record->hwnd, metrics));
    }
  }
}

// Lists every window MultiWM has a record for, so the companion leaves a known
// window alone when it is shown again (it returns to its own slot, not the next).
static void AppendTrackedWindows(CompanionAbi::SnapshotBody* body) {
  const std::vector<HWND> known = g_workspaces.KnownWindows();
  std::vector<uint32_t> values;
  values.reserve(known.size());
  for (HWND hwnd : known) {
    values.push_back(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd)));
  }
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());

  const size_t count = std::min<size_t>(values.size(), CompanionAbi::kMaxTrackedWindows);
  std::copy_n(values.begin(), count, body->trackedWindows);
  body->trackedCount = static_cast<uint32_t>(count);
  if (count == values.size()) body->flags |= CompanionAbi::kFlagTrackedComplete;
}

// Fills `*body` from current model state. Zeroes it first so unused entries
// compare equal between builds. Reads only; WM thread.
static void BuildBody(const GUID& desktop, CompanionAbi::SnapshotBody* body) {
  std::memset(body, 0, sizeof(*body));
  body->desktopId = desktop;
  body->flags = CompanionAbi::kFlagRunning;
  if (RuntimeLifecycle::IsAutomaticMode()) {
    body->flags |= CompanionAbi::kFlagAutomaticMode;
    if (!Placement::AutomaticOverflowRoutes()) body->flags |= CompanionAbi::kFlagOverflowCentersFloat;
  }

  AppendWorkspaceHints(desktop, body);
  AppendTrackedWindows(body);
  // Forget borders of windows MultiWM no longer knows; a reused HWND value is
  // also caught by the process check.
  std::erase_if(g_publisher.frameMargins,
                [](const auto& entry) { return !g_workspaces.IsTracked(entry.first); });

  bool rulesComplete = false;
  body->rulesBytes = CompanionAbi::SerializeRules(
      g_settings.windowRules, body->rules, CompanionAbi::kRulesCapacity, &rulesComplete);
  if (rulesComplete) body->flags |= CompanionAbi::kFlagRulesComplete;
}

static void WriteBody(const CompanionAbi::SnapshotBody& body) {
  CompanionAbi::BeginWrite(g_publisher.view);
  std::memcpy(&g_publisher.view->body, &body, sizeof(body));
  g_publisher.view->body.generation = ++g_publisher.generation;
  CompanionAbi::EndWrite(g_publisher.view);
}

// Keeps a summary of what was just written for the diagnostic report.
static void RecordPublishedState(const CompanionAbi::SnapshotBody& body) {
  auto& state = Diagnostics::g_runtime.companion;
  state.sectionOpen = g_publisher.view != nullptr;
  state.aliveEventOpen = g_publisher.aliveEvent != nullptr;
  state.published = g_publisher.published;
  state.generation = g_publisher.generation;
  state.flags = body.flags;
  state.trackedCount = body.trackedCount;
  state.restoreWindowCount = body.restoreWindowCount;
  state.rulesBytes = body.rulesBytes;
  state.measuredMarginWindows = g_publisher.frameMargins.size();
  const uint32_t count = std::min(body.workspaceCount, CompanionAbi::kMaxWorkspaces);
  state.workspaces.assign(body.workspaces, body.workspaces + count);
}

// --- Lifetime ----------------------------------------------------------------------

static HANDLE CreateNamedObject(const wchar_t* sddl, const auto& create) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
    Wh_Log(L"Companion snapshot: security descriptor failed: %lu", GetLastError());
    return nullptr;
  }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
  const HANDLE handle = create(&attributes);
  const DWORD error = GetLastError();
  LocalFree(descriptor);
  SetLastError(error);
  return handle;
}

// Creates (or, after a restart, reopens) the section and the liveness event and
// marks the snapshot as not running yet. Process start, before the WM thread.
// Returns false and leaves nothing open on failure; MultiWM works without it.
static bool Open() {
  if (g_publisher.view) return true;

  g_publisher.section = CreateNamedObject(CompanionAbi::kSectionSddl, [](SECURITY_ATTRIBUTES* attributes) {
    return CreateFileMappingW(INVALID_HANDLE_VALUE, attributes, PAGE_READWRITE, 0,
                              sizeof(CompanionAbi::SnapshotSection), CompanionAbi::kSectionName);
  });
  if (!g_publisher.section) {
    Wh_Log(L"Companion snapshot: CreateFileMapping failed: %lu", GetLastError());
    return false;
  }

  auto* view = static_cast<CompanionAbi::SnapshotSection*>(
      MapViewOfFile(g_publisher.section, FILE_MAP_WRITE, 0, 0, 0));
  MEMORY_BASIC_INFORMATION region{};
  // A section left by a different build with the same name could be smaller.
  if (!view || !VirtualQuery(view, &region, sizeof(region)) ||
      region.RegionSize < sizeof(CompanionAbi::SnapshotSection)) {
    Wh_Log(L"Companion snapshot: unusable section (%lu)", GetLastError());
    if (view) UnmapViewOfFile(view);
    CloseHandle(g_publisher.section);
    g_publisher.section = nullptr;
    return false;
  }

  g_publisher.aliveEvent = CreateNamedObject(CompanionAbi::kAliveEventSddl, [](SECURITY_ATTRIBUTES* attributes) {
    return CreateEventW(attributes, TRUE, FALSE, CompanionAbi::kAliveEventName);
  });
  if (!g_publisher.aliveEvent) {
    Wh_Log(L"Companion snapshot: liveness event failed: %lu", GetLastError());
  }

  g_publisher.view = view;
  g_publisher.staging = std::make_unique<CompanionAbi::SnapshotBody>();
  CompanionAbi::BeginWrite(view);
  view->magic = CompanionAbi::kMagic;
  view->abiMajor = CompanionAbi::kAbiMajor;
  view->abiMinor = CompanionAbi::kAbiMinor;
  view->sectionSize = sizeof(CompanionAbi::SnapshotSection);
  view->writerPid = GetCurrentProcessId();
  std::memset(&view->body, 0, sizeof(view->body));
  CompanionAbi::EndWrite(view);
  Diagnostics::g_runtime.companion.sectionOpen = true;
  Diagnostics::g_runtime.companion.aliveEventOpen = g_publisher.aliveEvent != nullptr;
  return true;
}

// Tells readers the WM thread stopped (settings reload or shutdown), so they stop
// hinting from stale state. WM thread cleanup, or process shutdown.
static void PublishStopped() {
  if (!g_publisher.view) return;
  std::memset(g_publisher.staging.get(), 0, sizeof(CompanionAbi::SnapshotBody));
  WriteBody(*g_publisher.staging);
  g_publisher.published = false;
  RecordPublishedState(*g_publisher.staging);
}

// Process shutdown, after the WM thread has stopped.
static void Close() {
  if (!g_publisher.view) return;
  PublishStopped();
  UnmapViewOfFile(g_publisher.view);
  g_publisher.view = nullptr;
  CloseHandle(g_publisher.section);
  g_publisher.section = nullptr;
  if (g_publisher.aliveEvent) CloseHandle(g_publisher.aliveEvent);
  g_publisher.aliveEvent = nullptr;
  g_publisher.staging.reset();
  g_publisher.frameMargins.clear();
  Diagnostics::g_runtime.companion = {};
}

// --- Publishing ---------------------------------------------------------------------

// Monitors, work areas or DPI changed: rebuild at the next PublishIfChanged.
// Called from RuntimeLifecycle::NotifyDisplayTopologyChanged on the WM thread.
static void NotifyDisplayChanged() {
  g_publisher.displayChanged = true;
}

// Rebuilds the snapshot when one of its inputs changed and writes it when the
// content differs from what is published. Event-driven; never polls.
// WM thread only, between messages: never from a callback that can run while
// another WM operation is waiting on a native call.
//
// Keeps the previous snapshot while the current desktop is not settled; a hint
// from it may be wrong for a moment, and MultiWM corrects such a window as usual.
static void PublishIfChanged() {
  RuntimeLifecycle::AssertWmThread(L"CompanionSnapshot::PublishIfChanged");
  if (!g_publisher.view || g_wm.desktop.state != ReconciledDesktopState::Settled) return;

  const uint64_t revision = g_workspaces.Revision();
  const ManagementMode mode = g_wm.managementMode;
  const GUID desktop = g_wm.desktop.reconciled;
  const bool overflowRoutes = Placement::AutomaticOverflowRoutes();
  const bool inputsChanged = !g_publisher.published || g_publisher.displayChanged ||
                             revision != g_publisher.repositoryRevision || mode != g_publisher.mode ||
                             !IsEqualGUID(desktop, g_publisher.desktop) ||
                             overflowRoutes != g_publisher.overflowRoutes;
  if (!inputsChanged) return;

  g_publisher.repositoryRevision = revision;
  g_publisher.mode = mode;
  g_publisher.desktop = desktop;
  g_publisher.displayChanged = false;
  g_publisher.overflowRoutes = overflowRoutes;

  // Measured for the diagnostic report: two QueryPerformanceCounter reads and a
  // tick read per rebuild.
  auto& counters = Diagnostics::g_runtime.counters;
  auto& state = Diagnostics::g_runtime.companion;
  const ULONGLONG now = GetTickCount64();
  ++counters.companionRebuilds;
  if (state.lastRebuildTickMs && now - state.lastRebuildTickMs < Diagnostics::kCompanionBurstWindowMs) {
    ++counters.companionBurstRebuilds;
  }
  state.lastRebuildTickMs = now;
  LARGE_INTEGER frequency{}, start{}, end{};
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);

  CompanionAbi::SnapshotBody& next = *g_publisher.staging;
  BuildBody(desktop, &next);
  // Only this thread writes the section, so reading it back needs no seqlock.
  const size_t compared = offsetof(CompanionAbi::SnapshotBody, desktopId);
  const bool unchanged =
      g_publisher.published &&
      std::memcmp(reinterpret_cast<const uint8_t*>(&next) + compared,
                  reinterpret_cast<const uint8_t*>(&g_publisher.view->body) + compared,
                  sizeof(next) - compared) == 0;

  QueryPerformanceCounter(&end);
  const uint64_t microseconds =
      frequency.QuadPart ? static_cast<uint64_t>((end.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart) : 0;
  counters.companionRebuildMicroseconds += microseconds;
  state.slowestRebuildMicroseconds = std::max(state.slowestRebuildMicroseconds, microseconds);
  if (unchanged) {
    ++counters.companionUnchangedRebuilds;
    return;
  }
  WriteBody(next);
  ++counters.companionWrites;
  g_publisher.published = true;
  RecordPublishedState(next);
}

}  // namespace CompanionSnapshot
