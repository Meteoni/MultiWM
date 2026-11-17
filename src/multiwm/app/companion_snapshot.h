// Publishes the read-only snapshot that the MultiWM Companion mod reads before a
// new window is first shown (layout in shared/companion_snapshot.h).
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.
//
// One-way by design: MultiWM writes, the companion reads, and nothing MultiWM
// does depends on the companion. If the section cannot be created, MultiWM runs
// exactly as without it.

namespace CompanionSnapshot {

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
};

// Process-wide: the section outlives WM-thread restarts on settings reload.
static Publisher g_publisher;

// --- Building one body ---------------------------------------------------------

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

    const Placement::AdmissionPrediction prediction = Placement::PredictAutomaticAdmission(
        workspace, metrics, Placement::AutomaticWindowLimitForMonitor(key.monitor), insertAfterFocused);

    CompanionAbi::WorkspaceHint& hint = body->workspaces[body->workspaceCount++];
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
  if (RuntimeLifecycle::IsAutomaticMode()) body->flags |= CompanionAbi::kFlagAutomaticMode;

  AppendWorkspaceHints(desktop, body);
  AppendTrackedWindows(body);

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
  return true;
}

// Tells readers the WM thread stopped (settings reload or shutdown), so they stop
// hinting from stale state. WM thread cleanup, or process shutdown.
static void PublishStopped() {
  if (!g_publisher.view) return;
  std::memset(g_publisher.staging.get(), 0, sizeof(CompanionAbi::SnapshotBody));
  WriteBody(*g_publisher.staging);
  g_publisher.published = false;
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
  const bool inputsChanged = !g_publisher.published || g_publisher.displayChanged ||
                             revision != g_publisher.repositoryRevision || mode != g_publisher.mode ||
                             !IsEqualGUID(desktop, g_publisher.desktop);
  if (!inputsChanged) return;

  g_publisher.repositoryRevision = revision;
  g_publisher.mode = mode;
  g_publisher.desktop = desktop;
  g_publisher.displayChanged = false;

  CompanionAbi::SnapshotBody& next = *g_publisher.staging;
  BuildBody(desktop, &next);
  // Only this thread writes the section, so reading it back needs no seqlock.
  const size_t compared = offsetof(CompanionAbi::SnapshotBody, desktopId);
  if (g_publisher.published &&
      std::memcmp(reinterpret_cast<const uint8_t*>(&next) + compared,
                  reinterpret_cast<const uint8_t*>(&g_publisher.view->body) + compared,
                  sizeof(next) - compared) == 0) {
    return;
  }
  WriteBody(next);
  g_publisher.published = true;
}

}  // namespace CompanionSnapshot
