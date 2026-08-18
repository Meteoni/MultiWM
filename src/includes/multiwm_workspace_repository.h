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
  HMONITOR monitor = foregroundWindow ? MonitorFromWindow(foregroundWindow, MONITOR_DEFAULTTONULL) : nullptr;
  if (!monitor) {
    POINT cursorPos{};
    if (GetCursorPos(&cursorPos)) {
      monitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTONEAREST);
    }
  }
  return monitor;
}

static bool GetWorkspaceWorkArea(HMONITOR monitor, RECT* outWorkArea) {
  if (!monitor || !outWorkArea) return false;
  RECT monitorWork{};
  if (!GetMonitorWorkArea(monitor, &monitorWork)) return false;

  *outWorkArea = {
      monitorWork.left + g_settings.insets.left,
      monitorWork.top + g_settings.insets.top,
      monitorWork.right - g_settings.insets.right,
      monitorWork.bottom - g_settings.insets.bottom};
  return outWorkArea->right > outWorkArea->left &&
         outWorkArea->bottom > outWorkArea->top;
}

// Resolves a monitor on the current virtual desktop to its workspace key and,
// when requested, returns the configured workspace-inset work area.
static bool GetCurrentWorkspaceKey(HMONITOR monitor, DesktopMonitorKey* outKey, RECT* outWorkArea = nullptr) {
  if (!monitor || !outKey) return false;
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
    const DesktopMonitorKey& key, const wchar_t* context) const {
  bool valid = true;
  auto fail = [&](const wchar_t* message, HWND hwnd = nullptr) {
    Wh_Log(
        L"Workspace invariant violation (%s, desktop=%08X monitor=%p hwnd=%p): %s",
        context ? context : L"workspace", key.desktopId.Data1, key.monitor, hwnd,
        message);
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
  workspace.Validate(key, L"repository save");
  AcquireSRWLockExclusive(&lock_);
  states_[key] = workspace;
  ReleaseSRWLockExclusive(&lock_);
}

bool WorkspaceRepository::IsTracked(HWND hwnd) const {
  if (!hwnd) return false;
  bool tracked = false;
  AcquireSRWLockShared(&lock_);
  for (const auto& kv : states_) {
    if (kv.second.HasRecord(hwnd)) {
      tracked = true;
      break;
    }
  }
  ReleaseSRWLockShared(&lock_);
  return tracked;
}

bool WorkspaceRepository::IsSuspendedMaximized(HWND hwnd) const {
  if (!hwnd) return false;
  bool found = false;
  AcquireSRWLockShared(&lock_);
  for (const auto& kv : states_) {
    const WindowRecord* record = kv.second.Find(hwnd);
    if (record && record->state == ManageState::Suspended &&
        record->suspensionReason == SuspensionReason::Maximized) {
      found = true;
      break;
    }
  }
  ReleaseSRWLockShared(&lock_);
  return found;
}

std::vector<DesktopMonitorKey> WorkspaceRepository::OwnersOf(HWND hwnd) const {
  std::vector<DesktopMonitorKey> keys;
  if (!hwnd) return keys;
  AcquireSRWLockShared(&lock_);
  for (const auto& kv : states_) {
    if (kv.second.HasRecord(hwnd)) keys.push_back(kv.first);
  }
  ReleaseSRWLockShared(&lock_);
  return keys;
}

std::vector<HWND> WorkspaceRepository::KnownWindows() const {
  std::vector<HWND> known;
  std::unordered_set<HWND> seen;
  AcquireSRWLockShared(&lock_);
  for (const auto& kv : states_) {
    for (const auto& record : kv.second.Records()) {
      if (seen.insert(record.first).second) known.push_back(record.first);
    }
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

void WorkspaceRepository::ClearAfterWmStopped() {
  // This operation is intentionally permitted only when no WM actor exists.
  assert(!g_wm.thread || g_wm.threadId == 0);
  AcquireSRWLockExclusive(&lock_);
  states_.clear();
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



