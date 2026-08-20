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

static HMONITOR GetTargetMonitor() {
  HWND foregroundWindow = GetForegroundWindow();
  HMONITOR monitor = foregroundWindow ? GetWindowPhysicalMonitor(foregroundWindow) : nullptr;
  if (!monitor) {
    POINT cursorPos{};
    if (GetCursorPos(&cursorPos)) {
      monitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
    }
  }
  return monitor;
}

static LONG GetWorkspaceGapPixels(HMONITOR monitor) {
  return ScaleDip(g_settings.gapDip, GetMonitorEffectiveDpi(monitor));
}

static WorkspaceInsets GetWorkspaceInsetsPixels(HMONITOR monitor) {
  const UINT dpi = GetMonitorEffectiveDpi(monitor);
  return {
      ScaleDip(g_settings.insetsDip.left, dpi),
      ScaleDip(g_settings.insetsDip.top, dpi),
      ScaleDip(g_settings.insetsDip.right, dpi),
      ScaleDip(g_settings.insetsDip.bottom, dpi)};
}

static bool GetWorkspaceWorkArea(HMONITOR monitor, RECT* outWorkArea) {
  if (!monitor || !outWorkArea) return false;
  RECT monitorWork{};
  if (!GetMonitorWorkArea(monitor, &monitorWork)) return false;
  const WorkspaceInsets insets = GetWorkspaceInsetsPixels(monitor);

  *outWorkArea = {
      monitorWork.left + insets.left,
      monitorWork.top + insets.top,
      monitorWork.right - insets.right,
      monitorWork.bottom - insets.bottom};
  return outWorkArea->right > outWorkArea->left &&
         outWorkArea->bottom > outWorkArea->top;
}

enum class FloatingPlacementIntent {
  PassiveRestore,
  PreserveAnchor,
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

  RECT workArea{};
  if (!GetWorkspaceWorkArea(key.monitor, &workArea)) return false;

  RECT current{};
  const bool haveCurrent = GetWindowFrameRect(hwnd, &current) &&
                           current.right > current.left &&
                           current.bottom > current.top;
  const UINT targetDpi = GetMonitorEffectiveDpi(key.monitor);

  LONG width = 0;
  LONG height = 0;
  if (record->hasFloatingRect) {
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
  } else if (record->hasFloatingRect &&
             ResolveRememberedFloatingMonitor(*record) == key.monitor) {
    center = RectCenter(record->floatingRect);
  } else {
    center = RectCenter(workArea);
  }

  RECT target = ClampFloatingRectToWorkArea(
      CenteredRect(center, width, height), workArea);
  PlacementObservation observation =
      PlaceWindowChecked(hwnd, record->canMove, target);
  if (observation.result != PlacementResult::Success &&
      observation.result != PlacementResult::AdjustedByWindow) {
    return false;
  }

  const RECT remembered =
      observation.observed.right > observation.observed.left &&
              observation.observed.bottom > observation.observed.top
          ? observation.observed
          : target;
  workspace.RememberFloatingGeometry(hwnd, remembered, key.monitor, targetDpi);
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

  RECT frame{};
  if (!GetWindowFrameRect(hwnd, &frame) || frame.right <= frame.left ||
      frame.bottom <= frame.top) {
    return false;
  }
  return workspace.RememberFloatingGeometry(
      hwnd, frame, key.monitor, GetMonitorEffectiveDpi(key.monitor));
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

// Resolves a monitor on the current virtual desktop to its workspace key and,
// when requested, returns the configured workspace-inset work area.
static bool GetCurrentWorkspaceKey(HMONITOR monitor, DesktopMonitorKey* outKey, RECT* outWorkArea = nullptr) {
  if (!outKey || !IsLiveMonitorHandle(monitor)) return false;
  GUID desktopId{};
  // Never collapse separate desktops into a zero-GUID workspace when the
  // private API is temporarily unavailable; let the caller retry after recovery.
  if (!InitializeVirtualDesktopAPI() || !GetCurrentDesktopId(&desktopId)) {
    return false;
  }
  outKey->desktopId = desktopId;
  outKey->monitor = monitor;

  if (outWorkArea && !GetWorkspaceWorkArea(monitor, outWorkArea)) return false;
  return true;
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
          L"Workspace invariant violation (%s, desktop=%08X monitor=%p hwnd=%p): %s",
          context ? context : L"workspace", key.desktopId.Data1, key.monitor, hwnd,
          message);
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
#ifdef WH_EDITING
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
  std::vector<HMONITOR> monitors;
  AcquireSRWLockShared(&lock_);
  for (const auto& kv : states_) {
    if (!IsEqualGUID(kv.first.desktopId, desktopId)) continue;
    if (std::find(monitors.begin(), monitors.end(), kv.first.monitor) == monitors.end()) {
      monitors.push_back(kv.first.monitor);
    }
  }
  ReleaseSRWLockShared(&lock_);

  // HMONITOR is a live-session routing token, not a durable display identity.
  // After a topology rebuild, old workspace keys can legitimately remain in the
  // repository until their records migrate. Never feed invalid handles back into
  // ordinary current-desktop discovery/arrangement.
  monitors.erase(
      std::remove_if(
          monitors.begin(), monitors.end(),
          [](HMONITOR monitor) {
            return !IsLiveMonitorHandle(monitor);
          }),
      monitors.end());
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

void WorkspaceRepository::ClearAfterWmStopped() {
  // This operation is intentionally permitted only when no WM actor exists.
  assert(!g_wm.thread || g_wm.threadId.load(std::memory_order_acquire) == 0);
  AcquireSRWLockExclusive(&lock_);
  states_.clear();
  ownersByWindow_.clear();
  ReleaseSRWLockExclusive(&lock_);
}

// UI is intentionally implemented later so the complete workspace model and
// reconciliation pipeline can be read contiguously. Only this narrow surface is
// visible to the WM core.
namespace TrayUi {
static void LoadSettings();
static bool Initialize();
static void Shutdown();
static void UpdateIcon(TileLayout layout);
static void ShowLayoutFlyout(TileLayout layout);
static void ShowManagementModeFlyout(bool automatic);
static void RefreshForCurrentWorkspace();
}  // namespace TrayUi



