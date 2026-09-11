//=============================================================================
// Workspace repository + authoritative model
//=============================================================================

static inline long long RectAreaLL(const RECT& r) {
  long long w = (long long)r.right - (long long)r.left;
  long long h = (long long)r.bottom - (long long)r.top;
  if (w <= 0 || h <= 0) return 0;
  return w * h;
}

static inline long long WindowAreaOnWorkArea(HWND hwnd, const RECT& workArea) {
  RECT r{};
  if (!GetWindowFrameRect(hwnd, &r)) return 0;
  RECT inter{};
  if (!IntersectRect(&inter, &r, &workArea)) return 0;
  return RectAreaLL(inter);
}

// Monitor targeting policy is intentionally explicit:
// - workspace-level user commands follow the cursor, because this mod doesn't
//   maintain a separate WM "focused monitor" state;
// - window-level commands derive ownership from the focused HWND itself;
// - passive UI refreshes use the foreground window's monitor.
static HMONITOR GetCursorMonitor() {
  POINT cursorPos{};
  if (!GetCursorPos(&cursorPos)) return nullptr;
  return MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
}

static HMONITOR GetForegroundMonitor() {
  HWND foregroundWindow = GetForegroundWindow();
  return foregroundWindow ? GetWindowPhysicalMonitor(foregroundWindow) : nullptr;
}

static HMONITOR GetWorkspaceCommandMonitor() {
  HMONITOR monitor = GetCursorMonitor();
  return monitor ? monitor : GetForegroundMonitor();
}

struct WorkspaceMetrics {
  RECT workArea{};
  UINT dpi = 96;
  LONG gap = 0;
};

static bool GetWorkspaceMetrics(HMONITOR monitor, WorkspaceMetrics* out) {
  if (!monitor || !out) return false;

  RECT monitorWork{};
  if (!GetMonitorWorkArea(monitor, &monitorWork)) return false;

  const UINT dpi = GetMonitorEffectiveDpi(monitor);
  const WorkspaceInsets insets{
      ScaleDip(g_settings.insetsDip.left, dpi),
      ScaleDip(g_settings.insetsDip.top, dpi),
      ScaleDip(g_settings.insetsDip.right, dpi),
      ScaleDip(g_settings.insetsDip.bottom, dpi)};
  const RECT workArea{
      monitorWork.left + insets.left,
      monitorWork.top + insets.top,
      monitorWork.right - insets.right,
      monitorWork.bottom - insets.bottom};
  if (workArea.right <= workArea.left || workArea.bottom <= workArea.top) {
    return false;
  }

  out->workArea = workArea;
  out->dpi = dpi;
  out->gap = ScaleDip(g_settings.gapDip, dpi);
  return true;
}

static bool GetWorkspaceWorkArea(HMONITOR monitor, RECT* outWorkArea) {
  if (!outWorkArea) return false;
  WorkspaceMetrics metrics;
  if (!GetWorkspaceMetrics(monitor, &metrics)) return false;
  *outWorkArea = metrics.workArea;
  return true;
}

enum class FloatingPlacementIntent {
  PassiveRestore,
  PreserveAnchor,
  NewWindowCenter,
  NewWindowCenterMoveOnly,
};

struct FloatingPlacementHint {
  FloatingPlacementIntent intent = FloatingPlacementIntent::PassiveRestore;
  POINT anchor{};
};

static POINT RectCenter(const RECT& rect) {
  return {
      rect.left + (rect.right - rect.left) / 2,
      rect.top + (rect.bottom - rect.top) / 2};
}

static RECT CenteredRect(POINT center, LONG width, LONG height) {
  width = std::max<LONG>(1, width);
  height = std::max<LONG>(1, height);
  const LONG left = center.x - width / 2;
  const LONG top = center.y - height / 2;
  return {left, top, left + width, top + height};
}

static RECT ClampFloatingRectToWorkArea(RECT rect, const RECT& workArea) {
  const LONG areaWidth = std::max<LONG>(1, workArea.right - workArea.left);
  const LONG areaHeight = std::max<LONG>(1, workArea.bottom - workArea.top);
  const LONG width = std::min<LONG>(
      std::max<LONG>(1, rect.right - rect.left), areaWidth);
  const LONG height = std::min<LONG>(
      std::max<LONG>(1, rect.bottom - rect.top), areaHeight);

  LONG left = rect.left;
  LONG top = rect.top;
  if (left < workArea.left) left = workArea.left;
  if (top < workArea.top) top = workArea.top;
  if (left + width > workArea.right) left = workArea.right - width;
  if (top + height > workArea.bottom) top = workArea.bottom - height;
  return {left, top, left + width, top + height};
}

static FloatingPlacementHint MakeFloatingPlacementHintFromGesture(
    const MoveSizeGesture& gesture) {
  FloatingPlacementHint hint;
  if (gesture.intent != MoveSizeIntent::Move) return hint;

  hint.intent = FloatingPlacementIntent::PreserveAnchor;
  // Preserve the moved window's final center rather than the mouse pointer. The
  // pointer is usually on the title bar, so centering a repaired window on it
  // would visibly jump away from the user's chosen placement.
  if (gesture.hasRects) {
    hint.anchor = RectCenter(gesture.end);
  } else if (gesture.hasDropPoint) {
    hint.anchor = gesture.dropPoint;
  }
  return hint;
}

static HMONITOR ResolveRememberedFloatingMonitor(
    const WindowRecord& record) {
  if (!record.hasFloatingRect) return nullptr;
  if (IsLiveMonitorHandle(record.floatingMonitor)) {
    return record.floatingMonitor;
  }

  // HMONITOR can be invalidated by a live display-topology rebuild. The remembered
  // physical rectangle is still a useful best-effort hint inside the same session;
  // if it maps to a current monitor, treat that as the remembered boundary.
  return MonitorFromRect(
      &record.floatingRect, MONITOR_DEFAULTTONULL);
}

// Converts remembered floating geometry into a destination-monitor rectangle.
// Passive migration restores the full rectangle on the same monitor and centers
// remembered/default size on a different monitor. Explicit drag/hotkey intent
// always preserves the user's anchor while repairing only the size.
static bool RepairFloatingGeometry(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd,
    const FloatingPlacementHint& hint) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || !record->canMove || !IsWindow(hwnd) ||
      GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    return false;
  }

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return false;

  WorkspaceMetrics metrics;
  if (!GetWorkspaceMetrics(monitor, &metrics)) return false;
  const RECT& workArea = metrics.workArea;
  const UINT targetDpi = metrics.dpi;

  RECT current{};
  const bool haveCurrent = GetWindowFrameRect(hwnd, &current) &&
                           current.right > current.left &&
                           current.bottom > current.top;

  const bool centerWithoutResize =
      hint.intent == FloatingPlacementIntent::NewWindowCenterMoveOnly;
  const bool centerNewWindow =
      hint.intent == FloatingPlacementIntent::NewWindowCenter ||
      centerWithoutResize;
  if (centerWithoutResize && !haveCurrent) return false;

  const bool needsFallbackSize = centerNewWindow || !record->hasFloatingRect;
  const WindowRule* placementOverride =
      IsAutomaticMode() && needsFallbackSize && !centerWithoutResize
          ? FindMatchingWindowRule(
                hwnd, WindowRuleTreatment::FloatingPlacementOverride)
          : nullptr;
  const bool preserveCurrentSize =
      placementOverride && placementOverride->preserveFloatingSize;
  const bool useCustomSize =
      placementOverride && !placementOverride->preserveFloatingSize;

  LONG width = 0;
  LONG height = 0;
  if (centerWithoutResize || (preserveCurrentSize && haveCurrent)) {
    width = current.right - current.left;
    height = current.bottom - current.top;
  } else if (useCustomSize) {
    if (placementOverride->useWorkspaceSize) {
      width = workArea.right - workArea.left;
      height = workArea.bottom - workArea.top;
    } else {
      width = ScaleDip(placementOverride->floatingSizeDip.width, targetDpi);
      height = ScaleDip(placementOverride->floatingSizeDip.height, targetDpi);
    }
  } else if (!centerNewWindow && record->hasFloatingRect) {
    const UINT sourceDpi = record->floatingDpi ? record->floatingDpi : 96;
    width = MulDiv(
        record->floatingRect.right - record->floatingRect.left,
        static_cast<int>(targetDpi), static_cast<int>(sourceDpi));
    height = MulDiv(
        record->floatingRect.bottom - record->floatingRect.top,
        static_cast<int>(targetDpi), static_cast<int>(sourceDpi));
  } else {
    width = ScaleDip(g_settings.floatingDefaultSizeDip.width, targetDpi);
    height = ScaleDip(g_settings.floatingDefaultSizeDip.height, targetDpi);
  }

  if (!record->canResize && haveCurrent) {
    width = current.right - current.left;
    height = current.bottom - current.top;
  }

  POINT center{};
  if (hint.intent == FloatingPlacementIntent::PreserveAnchor) {
    center = hint.anchor;
  } else if (!centerNewWindow && record->hasFloatingRect &&
             ResolveRememberedFloatingMonitor(*record) == monitor) {
    center = RectCenter(record->floatingRect);
  } else {
    center = RectCenter(workArea);
  }

  RECT target = CenteredRect(center, width, height);
  if (!centerWithoutResize) {
    target = ClampFloatingRectToWorkArea(target, workArea);
  }
  // The window can enter a suspended physical state while the placement target
  // is being calculated. Never overwrite that newer application-owned state.
  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) return false;
  PlacementObservation observation =
      PlaceWindowChecked(hwnd, record->canMove, target, centerWithoutResize);
  if (observation.result != PlacementResult::Success &&
      observation.result != PlacementResult::AdjustedByWindow) {
    return false;
  }

  const RECT remembered =
      observation.observed.right > observation.observed.left &&
              observation.observed.bottom > observation.observed.top
          ? observation.observed
          : target;
  workspace.RememberFloatingGeometry(hwnd, remembered, monitor, targetDpi);
  ++Diagnostics::g_runtime.counters.floatingGeometryRepairs;
  return true;
}

static bool RememberCurrentFloatingGeometry(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd) {
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record) return false;
  if (workspace.Layout() != TileLayout::Floating &&
      record->state != ManageState::Floating) {
    return false;
  }

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return false;

  RECT frame{};
  if (!GetWindowFrameRect(hwnd, &frame) || frame.right <= frame.left ||
      frame.bottom <= frame.top) {
    return false;
  }
  return workspace.RememberFloatingGeometry(
      hwnd, frame, monitor, GetMonitorEffectiveDpi(monitor));
}

// Entering the workspace-level Floating layout is a state restoration, not a
// per-window drag/float command. Restore each active tiled member's complete
// remembered floating rectangle; windows without memory use the configured
// centered fallback. Per-window Floating records are already user-positioned and
// deliberately remain untouched.
static void RestoreWorkspaceFloatingGeometry(
    const DesktopMonitorKey& key, Workspace& workspace) {
  if (workspace.Layout() != TileLayout::Floating) return;

  const std::vector<HWND> active = workspace.TiledWindows();
  const FloatingPlacementHint passiveRestore{};
  for (HWND hwnd : active) {
    RepairFloatingGeometry(key, workspace, hwnd, passiveRestore);
  }
}

// Resolves a monitor on the current virtual desktop to its workspace key.
static bool GetCurrentWorkspaceKey(
    HMONITOR monitor, DesktopMonitorKey* outKey) {
  if (!outKey || !IsLiveMonitorHandle(monitor)) return false;
  GUID desktopId{};
  if (g_wm.reconciledDesktopState == ReconciledDesktopState::Settled) {
    desktopId = g_wm.reconciledDesktop;
  } else {
    // Never collapse separate desktops into a zero-GUID workspace when the
    // private API is temporarily unavailable; let the caller retry after recovery.
    if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&desktopId)) {
      return false;
    }
  }
  return DesktopMonitorKey::FromHMonitor(desktopId, monitor, outKey);
}

bool Workspace::Validate(
    const DesktopMonitorKey& key, const wchar_t* context,
    std::vector<std::wstring>* errors, bool logFailures) const {
  bool valid = true;
  auto fail = [&](const wchar_t* message, HWND hwnd = nullptr) {
    if (errors) {
      if (hwnd) {
        wchar_t buffer[512]{};
        swprintf(
            buffer, ARRAYSIZE(buffer), L"%ls (hwnd=%p)", message,
            reinterpret_cast<void*>(hwnd));
        errors->push_back(buffer);
      } else {
        errors->push_back(message);
      }
    }
    if (logFailures) {
      Wh_Log(
          L"Workspace invariant violation (%s, desktop=%08X monitorId=%ls hwnd=%p): %s",
          context ? context : L"workspace", key.desktopId.Data1,
          key.monitor.deviceId.c_str(), hwnd, message);
    }
    valid = false;
  };

  std::unordered_set<HWND> active;
  for (HWND hwnd : windows_) {
    if (!hwnd) fail(L"null HWND in active tiled order");
    if (!active.insert(hwnd).second) fail(L"duplicate HWND in active tiled order", hwnd);

    auto recordIt = records_.find(hwnd);
    if (recordIt == records_.end()) {
      fail(L"active tiled HWND has no WindowRecord", hwnd);
    } else if (recordIt->second.state != ManageState::Tiled) {
      fail(L"active tiled HWND does not have Tiled state", hwnd);
    }
  }

  size_t suspendedCount = 0;
  std::unordered_set<size_t> suspendedLogicalSlots;
  for (const auto& kv : records_) {
    HWND hwnd = kv.first;
    const WindowRecord& record = kv.second;
    const bool isActive = active.find(hwnd) != active.end();

    if (record.hwnd && record.hwnd != hwnd) {
      fail(L"WindowRecord hwnd field disagrees with record-map key", hwnd);
    }
    if (record.hasFloatingRect &&
        (record.floatingRect.right <= record.floatingRect.left ||
         record.floatingRect.bottom <= record.floatingRect.top ||
         !record.floatingMonitor || !record.floatingDpi)) {
      fail(L"remembered floating geometry is invalid", hwnd);
    }

    switch (record.state) {
      case ManageState::Tiled:
        if (!isActive) fail(L"Tiled record is absent from active tiled order", hwnd);
        if (record.hasSavedSlot) fail(L"Tiled record retains a suspended slot", hwnd);
        break;

      case ManageState::Suspended:
        ++suspendedCount;
        if (isActive) fail(L"Suspended record appears in active tiled order", hwnd);
        if (!record.hasSavedSlot) {
          fail(L"Suspended record has no saved logical slot", hwnd);
        } else if (!suspendedLogicalSlots.insert(record.savedSlot.index).second) {
          fail(L"two suspended records share the same logical slot", hwnd);
        }
        break;

      case ManageState::Floating:
      case ManageState::Ignored:
        if (isActive) fail(L"non-tiled record appears in active tiled order", hwnd);
        if (record.hasSavedSlot) fail(L"non-suspended record retains a saved slot", hwnd);
        break;
    }
  }

  const size_t logicalCount = windows_.size() + suspendedCount;
  for (const auto& kv : records_) {
    const WindowRecord& record = kv.second;
    if (record.state == ManageState::Suspended && record.hasSavedSlot &&
        record.savedSlot.index >= logicalCount) {
      fail(L"suspended logical slot is outside workspace logical order", kv.first);
    }
  }

  if (!std::isfinite(masterRatio_) || masterRatio_ < 0.1 || masterRatio_ > 0.9) {
    fail(L"master ratio is outside the supported range");
  }

  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    if (gridWeights_.size() != windows_.size()) {
      fail(L"grid weight count does not match active tiled count");
    }
    if (!stackWeights_.empty()) fail(L"grid layout retains stack weights");
  } else if (layout_ == TileLayout::MasterStack ||
             layout_ == TileLayout::MasterStackH) {
    const size_t expected = windows_.empty() ? 0 : windows_.size() - 1;
    if (stackWeights_.size() != expected) {
      fail(L"stack weight count does not match non-master tiled count");
    }
    if (!gridWeights_.empty()) fail(L"master/stack layout retains grid weights");
  } else if (!stackWeights_.empty() || !gridWeights_.empty()) {
    fail(L"weightless layout retains layout weights");
  }

  return valid;
}

void Workspace::DebugValidateMutation(const wchar_t* operation) const {
#ifdef MULTIWM_DEBUG_VALIDATE
  DesktopMonitorKey unknownKey{};
  const bool valid = Validate(unknownKey, operation ? operation : L"mutation");
  assert(valid);
#else
  (void)operation;
#endif
}

bool WorkspaceRepository::Load(
    const DesktopMonitorKey& key, Workspace* workspace) const {
  if (!workspace) return false;
  bool found = false;
  AcquireSRWLockShared(&lock_);
  auto it = states_.find(key);
  if (it != states_.end()) {
    *workspace = it->second;
    found = true;
  }
  ReleaseSRWLockShared(&lock_);
  return found;
}

void WorkspaceRepository::Save(
    const DesktopMonitorKey& key, const Workspace& workspace) {
  AssertWmThread(L"WorkspaceRepository::Save");
  ++Diagnostics::g_runtime.counters.workspaceSaves;
  workspace.Validate(key, L"repository save");

  AcquireSRWLockExclusive(&lock_);

  // Remove this workspace key from the reverse index for the previous snapshot.
  // Re-indexing the whole small workspace keeps the code simple and guarantees the
  // index follows every semantic mutation made through Workspace.
  auto oldStateIt = states_.find(key);
  if (oldStateIt != states_.end()) {
    for (const auto& record : oldStateIt->second.Records()) {
      auto ownersIt = ownersByWindow_.find(record.first);
      if (ownersIt == ownersByWindow_.end()) continue;

      auto& owners = ownersIt->second;
      owners.erase(
          std::remove_if(
              owners.begin(), owners.end(),
              [&](const DesktopMonitorKey& ownerKey) {
                return DesktopMonitorKeyEqual{}(ownerKey, key);
              }),
          owners.end());
      if (owners.empty()) ownersByWindow_.erase(ownersIt);
    }
  }

  states_[key] = workspace;

  // Add the replacement snapshot to the reverse index. Preserve multiple owner
  // keys if they exist so EnsureUniqueWindowOwnership can still repair old bugs.
  for (const auto& record : workspace.Records()) {
    auto& owners = ownersByWindow_[record.first];
    if (std::none_of(
            owners.begin(), owners.end(),
            [&](const DesktopMonitorKey& ownerKey) {
              return DesktopMonitorKeyEqual{}(ownerKey, key);
            })) {
      owners.push_back(key);
    }
  }

  ReleaseSRWLockExclusive(&lock_);
}

bool WorkspaceRepository::IsTracked(HWND hwnd) const {
  if (!hwnd) return false;
  AcquireSRWLockShared(&lock_);
  auto it = ownersByWindow_.find(hwnd);
  const bool tracked = it != ownersByWindow_.end() && !it->second.empty();
  ReleaseSRWLockShared(&lock_);
  return tracked;
}

bool WorkspaceRepository::IsTiled(HWND hwnd) const {
  if (!hwnd) return false;
  bool found = false;
  AcquireSRWLockShared(&lock_);

  auto ownersIt = ownersByWindow_.find(hwnd);
  if (ownersIt != ownersByWindow_.end()) {
    for (const auto& key : ownersIt->second) {
      auto stateIt = states_.find(key);
      if (stateIt != states_.end() && stateIt->second.IsTiled(hwnd)) {
        found = true;
        break;
      }
    }
  }

  ReleaseSRWLockShared(&lock_);
  return found;
}

bool WorkspaceRepository::IsSuspendedMaximized(HWND hwnd) const {
  if (!hwnd) return false;
  bool found = false;
  AcquireSRWLockShared(&lock_);

  auto ownersIt = ownersByWindow_.find(hwnd);
  if (ownersIt != ownersByWindow_.end()) {
    for (const auto& key : ownersIt->second) {
      auto stateIt = states_.find(key);
      if (stateIt == states_.end()) continue;
      const WindowRecord* record = stateIt->second.Find(hwnd);
      if (record && record->state == ManageState::Suspended &&
          record->suspensionReason == SuspensionReason::Maximized) {
        found = true;
        break;
      }
    }
  }

  ReleaseSRWLockShared(&lock_);
  return found;
}

std::vector<DesktopMonitorKey> WorkspaceRepository::OwnersOf(HWND hwnd) const {
  std::vector<DesktopMonitorKey> keys;
  if (!hwnd) return keys;
  AcquireSRWLockShared(&lock_);
  auto it = ownersByWindow_.find(hwnd);
  if (it != ownersByWindow_.end()) keys = it->second;
  ReleaseSRWLockShared(&lock_);
  return keys;
}

std::vector<HWND> WorkspaceRepository::KnownWindows() const {
  std::vector<HWND> known;
  AcquireSRWLockShared(&lock_);
  known.reserve(ownersByWindow_.size());
  for (const auto& entry : ownersByWindow_) {
    if (!entry.second.empty()) known.push_back(entry.first);
  }
  ReleaseSRWLockShared(&lock_);
  return known;
}

std::vector<HMONITOR> WorkspaceRepository::MonitorsOnDesktop(
    const GUID& desktopId) const {
  std::vector<Model::MonitorId> monitorIds;
  AcquireSRWLockShared(&lock_);
  for (const auto& kv : states_) {
    if (!IsEqualGUID(kv.first.desktopId, desktopId)) continue;
    if (std::find(monitorIds.begin(), monitorIds.end(), kv.first.monitor) ==
        monitorIds.end()) {
      monitorIds.push_back(kv.first.monitor);
    }
  }
  ReleaseSRWLockShared(&lock_);

  // Repository identity survives topology churn. Only return identities that
  // currently resolve to a live HMONITOR; detached workspaces remain dormant.
  std::vector<HMONITOR> monitors;
  for (const auto& monitorId : monitorIds) {
    HMONITOR monitor = monitorId.Resolve();
    if (monitor && std::find(monitors.begin(), monitors.end(), monitor) == monitors.end()) {
      monitors.push_back(monitor);
    }
  }
  return monitors;
}

std::vector<std::pair<DesktopMonitorKey, Workspace>>
WorkspaceRepository::Snapshot() const {
  std::vector<std::pair<DesktopMonitorKey, Workspace>> snapshot;
  AcquireSRWLockShared(&lock_);
  snapshot.reserve(states_.size());
  for (const auto& kv : states_) snapshot.push_back(kv);
  ReleaseSRWLockShared(&lock_);
  return snapshot;
}

std::vector<std::pair<HWND, std::vector<DesktopMonitorKey>>>
WorkspaceRepository::OwnershipIndexSnapshot() const {
  std::vector<std::pair<HWND, std::vector<DesktopMonitorKey>>> snapshot;
  AcquireSRWLockShared(&lock_);
  snapshot.reserve(ownersByWindow_.size());
  for (const auto& kv : ownersByWindow_) snapshot.push_back(kv);
  ReleaseSRWLockShared(&lock_);
  return snapshot;
}

// UI is intentionally implemented later so the complete workspace model and
// reconciliation pipeline can be read contiguously. Only this narrow surface is
// visible to the WM core.
namespace TrayUi {
static void LoadSettings();
static bool Initialize();
static void Shutdown();
static void UpdateIcon(TileLayout layout, HMONITOR monitor = nullptr);
static void ShowLayoutFlyout(TileLayout layout, HMONITOR monitor = nullptr);
static void ShowManagementModeFlyout(bool automatic);
static void ShowDesktopSwitchFlyouts(const GUID& desktopId);
static void RefreshForMonitor(HMONITOR monitor);
static void RefreshDisplayedMonitor();
static void RefreshForCurrentWorkspace();
}  // namespace TrayUi



