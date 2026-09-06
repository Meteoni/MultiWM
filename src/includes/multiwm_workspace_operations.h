//=============================================================================
// Workspace model operations
//=============================================================================
//
// The Workspace class is the only owner of logical participation/order/weight
// invariants. Callers may inspect const views, but cannot edit the backing
// containers directly.

void Workspace::EnsureWeights() {
  if (layout_ == TileLayout::MasterStack || layout_ == TileLayout::MasterStackH) {
    const size_t stackCount = windows_.empty() ? 0 : windows_.size() - 1;
    if (stackWeights_.size() != stackCount) stackWeights_ = DefaultWeights(stackCount);
    gridWeights_.clear();
  } else if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    if (gridWeights_.size() != windows_.size()) gridWeights_ = DefaultWeights(windows_.size());
    stackWeights_.clear();
  } else {
    stackWeights_.clear();
    gridWeights_.clear();
  }
}

void Workspace::RemoveActiveAt(size_t index, SuspendedSlot* saved) {
  AssertWmThread(L"Workspace::RemoveActiveAt");
  if (index >= windows_.size()) return;

  SuspendedSlot slot{};
  slot.index = index;
  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    if (index < gridWeights_.size()) {
      slot.weight = gridWeights_[index];
      slot.hasWeight = true;
      gridWeights_.erase(gridWeights_.begin() + index);
    }
  } else if (layout_ == TileLayout::MasterStack || layout_ == TileLayout::MasterStackH) {
    if (index == 0) {
      slot.wasMaster = true;
      if (!stackWeights_.empty()) {
        slot.weight = stackWeights_.front();
        slot.hasWeight = true;
        stackWeights_.erase(stackWeights_.begin());
      }
    } else if (index - 1 < stackWeights_.size()) {
      slot.weight = stackWeights_[index - 1];
      slot.hasWeight = true;
      stackWeights_.erase(stackWeights_.begin() + (index - 1));
    }
  }

  windows_.erase(windows_.begin() + index);
  if (saved) *saved = slot;
  EnsureWeights();
}

size_t Workspace::ActiveIndexToLogicalIndex(size_t activeIndex) const {
  std::vector<size_t> suspendedIndices;
  for (const auto& kv : records_) {
    const WindowRecord& record = kv.second;
    if (record.state == ManageState::Suspended && record.hasSavedSlot) {
      suspendedIndices.push_back(record.savedSlot.index);
    }
  }
  std::sort(suspendedIndices.begin(), suspendedIndices.end());

  size_t logicalIndex = activeIndex;
  for (size_t suspendedIndex : suspendedIndices) {
    if (suspendedIndex <= logicalIndex) ++logicalIndex;
    else break;
  }
  return logicalIndex;
}

size_t Workspace::LogicalIndexToActiveIndex(
    size_t logicalIndex, HWND restoringHwnd) const {
  size_t suspendedBefore = 0;
  for (const auto& kv : records_) {
    const WindowRecord& record = kv.second;
    if (kv.first != restoringHwnd &&
        record.state == ManageState::Suspended &&
        record.hasSavedSlot &&
        record.savedSlot.index < logicalIndex) {
      ++suspendedBefore;
    }
  }
  return logicalIndex >= suspendedBefore ? logicalIndex - suspendedBefore : 0;
}

void Workspace::CloseLogicalGapAfterRemoval(
    size_t removedLogicalIndex, HWND exceptHwnd) {
  AssertWmThread(L"Workspace::CloseLogicalGapAfterRemoval");
  for (auto& kv : records_) {
    WindowRecord& record = kv.second;
    if (kv.first != exceptHwnd &&
        record.state == ManageState::Suspended &&
        record.hasSavedSlot &&
        record.savedSlot.index > removedLogicalIndex) {
      --record.savedSlot.index;
    }
  }
}

bool Workspace::Suspend(HWND hwnd, SuspensionReason reason) {
  AssertWmThread(L"Workspace::Suspend");
  WindowRecord* existing = FindMutable(hwnd);
  if (existing && existing->state == ManageState::Suspended) {
    if (existing->suspensionReason == reason) return false;
    existing->suspensionReason = reason;
    DebugValidateMutation(L"Workspace::Suspend(reason update)");
    return true;
  }

  auto it = std::find(windows_.begin(), windows_.end(), hwnd);
  if (it == windows_.end()) return false;

  WindowRecord* record = FindMutable(hwnd);
  if (!record) return false;
  SuspendedSlot slot{};
  const size_t activeIndex = static_cast<size_t>(it - windows_.begin());
  const size_t logicalIndex = ActiveIndexToLogicalIndex(activeIndex);
  RemoveActiveAt(activeIndex, &slot);
  slot.index = logicalIndex;

  record->state = ManageState::Suspended;
  record->suspensionReason = reason;
  record->savedSlot = slot;
  record->hasSavedSlot = true;
  DebugValidateMutation(L"Workspace::Suspend");
  return true;
}

static SuspensionReason GetPhysicalSuspensionReason(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return SuspensionReason::None;
  if (IsIconic(hwnd)) return SuspensionReason::Minimized;
  if (!IsWindowVisible(hwnd)) return SuspensionReason::Hidden;
  if (IsZoomed(hwnd)) return SuspensionReason::Maximized;

  // Match FancyWM's native-fullscreen fallback: Chromium and similar apps can
  // cover a complete display while Windows still reports the HWND as Restored.
  // Maximized is the right model reason because both states have identical
  // suspend/save-slot/restore semantics here.
  RECT windowRect{};
  if (GetWindowRect(hwnd, &windowRect)) {
    HMONITOR monitor = MonitorFromRect(&windowRect, MONITOR_DEFAULTTONULL);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    constexpr LONG kFullscreenTolerancePx = 2;
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo) &&
        !Differs(windowRect.left, monitorInfo.rcMonitor.left,
                 kFullscreenTolerancePx) &&
        !Differs(windowRect.top, monitorInfo.rcMonitor.top,
                 kFullscreenTolerancePx) &&
        !Differs(windowRect.right, monitorInfo.rcMonitor.right,
                 kFullscreenTolerancePx) &&
        !Differs(windowRect.bottom, monitorInfo.rcMonitor.bottom,
                 kFullscreenTolerancePx)) {
      return SuspensionReason::Maximized;
    }
  }
  return SuspensionReason::None;
}

// Workspace initialization needs a broader physical snapshot than normal layout
// participation. A visible top-level window can already belong to this workspace
// while minimized or maximized, even though CollectTileWindows correctly excludes
// it from placement. Enumerate that broader set only when creating a workspace.
static std::vector<HWND> CollectWorkspaceWindowsForInitialization(
    HMONITOR monitor, const GUID& desktopId) {
  std::vector<HWND> windows;
  if (!monitor) return windows;

  struct EnumContext {
    HMONITOR targetMonitor;
    GUID desktopId;
    std::vector<HWND>* windowList;
  } context = {monitor, desktopId, &windows};

  ++Diagnostics::g_runtime.counters.enumWindowsPasses;
  EnumWindows(
      [](HWND hwnd, LPARAM lParam) WINAPI -> BOOL {
        ++Diagnostics::g_runtime.counters.enumWindowsVisited;
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (!WindowCanBeManaged(hwnd)) return TRUE;
        if (!IsWindowTrackedInAnyState(hwnd) && !IsWindowEnabled(hwnd)) return TRUE;

        GUID windowDesktop{};
        if (!GetWindowDesktopIdSafe(hwnd, &windowDesktop) ||
            !IsEqualGUID(windowDesktop, ctx->desktopId)) {
          return TRUE;
        }

        if (GetWindowPhysicalMonitor(hwnd) != ctx->targetMonitor) {
          return TRUE;
        }

        ctx->windowList->push_back(hwnd);
        ++Diagnostics::g_runtime.counters.tileCandidatesCollected;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context));

  return windows;
}

static bool AdmitInitialObservedWindow(Workspace& workspace, HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd) || workspace.HasRecord(hwnd) ||
      IsWindowTrackedInAnyState(hwnd)) {
    return false;
  }
  const SuspensionReason reason = GetPhysicalSuspensionReason(hwnd);
  if (reason == SuspensionReason::Hidden) return false;
  return workspace.AdmitInitial(MakeWindowRecord(hwnd), reason);
}

size_t Workspace::LogicalWindowCount() const {
  size_t count = windows_.size();
  for (const auto& kv : records_) {
    if (kv.second.state == ManageState::Suspended && kv.second.hasSavedSlot) ++count;
  }
  return count;
}

bool Workspace::AdmitInitial(
    WindowRecord record, SuspensionReason reason) {
  AssertWmThread(L"Workspace::AdmitInitial");
  const HWND hwnd = record.hwnd;
  if (!hwnd || HasRecord(hwnd) || reason == SuspensionReason::Hidden) return false;
  if (!record.tilingGeneration) {
    record.tilingGeneration = Model::NextTilingGeneration();
  }

  record.state = reason == SuspensionReason::None
                     ? ManageState::Tiled
                     : ManageState::Suspended;
  if (reason == SuspensionReason::None) {
    windows_.push_back(hwnd);
  } else {
    const size_t logicalIndex = LogicalWindowCount();
    record.suspensionReason = reason;
    record.savedSlot.index = logicalIndex;
    if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
      record.savedSlot.weight = 1.0;
      record.savedSlot.hasWeight = true;
    } else if (layout_ == TileLayout::MasterStack || layout_ == TileLayout::MasterStackH) {
      if (logicalIndex == 0) record.savedSlot.wasMaster = true;
      else {
        record.savedSlot.weight = 1.0;
        record.savedSlot.hasWeight = true;
      }
    }
    record.hasSavedSlot = true;
  }

  records_.emplace(hwnd, std::move(record));
  EnsureWeights();
  DebugValidateMutation(L"Workspace::AdmitInitial");
  return true;
}

bool Workspace::AdmitInitialAfter(
    WindowRecord record, SuspensionReason reason, HWND anchor) {
  AssertWmThread(L"Workspace::AdmitInitialAfter");
  if (reason == SuspensionReason::None) {
    return AdmitTiledAfter(std::move(record), anchor);
  }

  const HWND hwnd = record.hwnd;
  if (!hwnd || HasRecord(hwnd) || reason == SuspensionReason::Hidden ||
      !IsTiled(anchor)) {
    return AdmitInitial(std::move(record), reason);
  }

  const size_t anchorActiveIndex = TiledIndex(anchor);
  if (anchorActiveIndex == static_cast<size_t>(-1)) {
    return AdmitInitial(std::move(record), reason);
  }

  const size_t insertionLogicalIndex =
      ActiveIndexToLogicalIndex(anchorActiveIndex) + 1;
  for (auto& kv : records_) {
    WindowRecord& existing = kv.second;
    if (existing.state == ManageState::Suspended &&
        existing.hasSavedSlot &&
        existing.savedSlot.index >= insertionLogicalIndex) {
      ++existing.savedSlot.index;
    }
  }

  if (!record.tilingGeneration) {
    record.tilingGeneration = Model::NextTilingGeneration();
  }
  record.state = ManageState::Suspended;
  record.suspensionReason = reason;
  record.savedSlot.index = insertionLogicalIndex;
  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    record.savedSlot.weight = 1.0;
    record.savedSlot.hasWeight = true;
  } else if (layout_ == TileLayout::MasterStack ||
             layout_ == TileLayout::MasterStackH) {
    record.savedSlot.weight = 1.0;
    record.savedSlot.hasWeight = true;
  }
  record.hasSavedSlot = true;

  records_.emplace(hwnd, std::move(record));
  EnsureWeights();
  DebugValidateMutation(L"Workspace::AdmitInitialAfter");
  return true;
}

static bool IsWindowSuspendedMaximized(HWND hwnd) {
  return g_workspaces.IsSuspendedMaximized(hwnd);
}

bool Workspace::Restore(HWND hwnd) {
  AssertWmThread(L"Workspace::Restore");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || record->state != ManageState::Suspended || !record->hasSavedSlot) {
    return false;
  }

  const SuspendedSlot slot = record->savedSlot;
  if (!record->tilingGeneration) {
    record->tilingGeneration = Model::NextTilingGeneration();
  }
  const size_t index = std::min(
      LogicalIndexToActiveIndex(slot.index, hwnd), windows_.size());
  windows_.insert(windows_.begin() + index, hwnd);

  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    if (gridWeights_.size() + 1 == windows_.size()) {
      gridWeights_.insert(
          gridWeights_.begin() + std::min(index, gridWeights_.size()),
          slot.hasWeight ? slot.weight : 1.0);
    }
  } else if (layout_ == TileLayout::MasterStack || layout_ == TileLayout::MasterStackH) {
    if (slot.wasMaster) {
      if (stackWeights_.size() + 1 == windows_.size() - 1) {
        stackWeights_.insert(
            stackWeights_.begin(), slot.hasWeight ? slot.weight : 1.0);
      }
    } else if (index > 0 && stackWeights_.size() + 1 == windows_.size() - 1) {
      const size_t stackIndex = std::min(index - 1, stackWeights_.size());
      stackWeights_.insert(
          stackWeights_.begin() + stackIndex,
          slot.hasWeight ? slot.weight : 1.0);
    }
  }

  record->state = ManageState::Tiled;
  record->suspensionReason = SuspensionReason::None;
  record->savedSlot = {};
  record->hasSavedSlot = false;
  EnsureWeights();
  DebugValidateMutation(L"Workspace::Restore");
  return true;
}

bool Workspace::Float(HWND hwnd) {
  AssertWmThread(L"Workspace::Float");
  WindowRecord* existing = FindMutable(hwnd);
  if (existing && existing->state == ManageState::Floating) return false;

  auto activeIt = std::find(windows_.begin(), windows_.end(), hwnd);
  if (activeIt != windows_.end()) {
    const size_t activeIndex = static_cast<size_t>(activeIt - windows_.begin());
    const size_t logicalIndex = ActiveIndexToLogicalIndex(activeIndex);
    RemoveActiveAt(activeIndex);
    CloseLogicalGapAfterRemoval(logicalIndex);
  } else if (existing && existing->state == ManageState::Suspended && existing->hasSavedSlot) {
    CloseLogicalGapAfterRemoval(existing->savedSlot.index, hwnd);
  }

  WindowRecord* record = FindMutable(hwnd);
  if (!record) return false;
  record->state = ManageState::Floating;
  record->suspensionReason = SuspensionReason::None;
  record->savedSlot = {};
  record->hasSavedSlot = false;
  EnsureWeights();
  DebugValidateMutation(L"Workspace::Float");
  return true;
}

bool Workspace::Forget(HWND hwnd) {
  AssertWmThread(L"Workspace::Forget");
  if (lastFocusedWindow_ == hwnd) lastFocusedWindow_ = nullptr;
  auto activeIt = std::find(windows_.begin(), windows_.end(), hwnd);
  if (activeIt != windows_.end()) {
    const size_t activeIndex = static_cast<size_t>(activeIt - windows_.begin());
    const size_t logicalIndex = ActiveIndexToLogicalIndex(activeIndex);
    RemoveActiveAt(activeIndex);
    CloseLogicalGapAfterRemoval(logicalIndex);
    records_.erase(hwnd);
    DebugValidateMutation(L"Workspace::Forget(active)");
    return true;
  }

  auto recordIt = records_.find(hwnd);
  if (recordIt == records_.end()) return false;
  if (recordIt->second.state == ManageState::Suspended && recordIt->second.hasSavedSlot) {
    const size_t logicalIndex = recordIt->second.savedSlot.index;
    records_.erase(recordIt);
    CloseLogicalGapAfterRemoval(logicalIndex);
  } else {
    records_.erase(recordIt);
  }
  EnsureWeights();
  DebugValidateMutation(L"Workspace::Forget");
  return true;
}

void Workspace::AppendTiledWithDefaultWeight(HWND hwnd) {
  AssertWmThread(L"Workspace::AppendTiledWithDefaultWeight");
  EnsureWeights();
  const size_t oldCount = windows_.size();
  windows_.push_back(hwnd);
  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    gridWeights_.push_back(1.0);
  } else if ((layout_ == TileLayout::MasterStack || layout_ == TileLayout::MasterStackH) &&
             oldCount > 0) {
    stackWeights_.push_back(1.0);
  }
}

SuspendedSlot Workspace::MakeAppendedSuspendedSlot() const {
  SuspendedSlot slot{};
  slot.index = LogicalWindowCount();
  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    slot.weight = 1.0;
    slot.hasWeight = true;
  } else if (layout_ == TileLayout::MasterStack || layout_ == TileLayout::MasterStackH) {
    if (slot.index == 0) slot.wasMaster = true;
    else {
      slot.weight = 1.0;
      slot.hasWeight = true;
    }
  }
  return slot;
}

bool Workspace::ExtractForMigration(HWND hwnd, WindowRecord* outRecord) {
  AssertWmThread(L"Workspace::ExtractForMigration");
  const WindowRecord* record = Find(hwnd);
  if (!record) return false;
  if (outRecord) *outRecord = *record;
  return Forget(hwnd);
}

void Workspace::AdmitMigrated(WindowRecord record, HWND anchor) {
  AssertWmThread(L"Workspace::AdmitMigrated");
  const HWND hwnd = record.hwnd;
  if (!hwnd) return;
  if (record.state == ManageState::Tiled ||
      record.state == ManageState::Suspended) {
    // A destination workspace admission is newer than its existing members,
    // matching FancyWM's generation-based overflow victim policy.
    record.tilingGeneration = Model::NextTilingGeneration();
  }
  Forget(hwnd);

  // Automatic cross-workspace insertion follows the same destination-anchor
  // policy as fresh admission. The caller supplies no anchor for Last slot or
  // for Manual-mode migration, preserving the previous append behavior.
  if (anchor && anchor != hwnd && IsTiled(anchor)) {
    if (record.state == ManageState::Tiled) {
      AdmitTiledAfter(std::move(record), anchor);
      return;
    }

    if (record.state == ManageState::Suspended &&
        record.suspensionReason != SuspensionReason::None &&
        record.suspensionReason != SuspensionReason::Hidden) {
      const SuspensionReason reason = record.suspensionReason;
      AdmitInitialAfter(std::move(record), reason, anchor);
      return;
    }
  }

  if (record.state == ManageState::Tiled) {
    record.suspensionReason = SuspensionReason::None;
    record.savedSlot = {};
    record.hasSavedSlot = false;
    AppendTiledWithDefaultWeight(hwnd);
  } else if (record.state == ManageState::Suspended) {
    record.savedSlot = MakeAppendedSuspendedSlot();
    record.hasSavedSlot = true;
  }
  records_[hwnd] = std::move(record);
  EnsureWeights();
  DebugValidateMutation(L"Workspace::AdmitMigrated");
}

void Workspace::MergeRememberedFloatingGeometryFrom(const Workspace& source) {
  AssertWmThread(L"Workspace::MergeRememberedFloatingGeometryFrom");
  for (const auto& kv : source.records_) {
    const WindowRecord& sourceRecord = kv.second;
    if (!sourceRecord.hasFloatingRect) continue;
    WindowRecord* targetRecord = FindMutable(kv.first);
    if (!targetRecord) continue;
    targetRecord->floatingRect = sourceRecord.floatingRect;
    targetRecord->floatingMonitor = sourceRecord.floatingMonitor;
    targetRecord->floatingDpi = sourceRecord.floatingDpi;
    targetRecord->hasFloatingRect = true;
  }
  DebugValidateMutation(L"Workspace::MergeRememberedFloatingGeometryFrom");
}

void Workspace::MergeNonTiledRecordsFrom(const Workspace& source) {
  AssertWmThread(L"Workspace::MergeNonTiledRecordsFrom");
  for (const auto& kv : source.records_) {
    if (kv.second.state != ManageState::Tiled) records_[kv.first] = kv.second;
  }
  DebugValidateMutation(L"Workspace::MergeNonTiledRecordsFrom");
}

void Workspace::ResetSuspendedLayoutHints() {
  AssertWmThread(L"Workspace::ResetSuspendedLayoutHints");
  for (auto& kv : records_) {
    WindowRecord& record = kv.second;
    if (record.state == ManageState::Suspended && record.hasSavedSlot) {
      record.savedSlot.weight = 1.0;
      record.savedSlot.hasWeight = false;
      record.savedSlot.wasMaster = false;
    }
  }
}

void Workspace::SetLayout(TileLayout layout) {
  AssertWmThread(L"Workspace::SetLayout");
  if (layout_ == layout) return;
  layout_ = layout;
  ResetSuspendedLayoutHints();
  EnsureWeights();
  DebugValidateMutation(L"Workspace::SetLayout");
}

bool Workspace::CycleLayout(const std::vector<TileLayout>& cycle) {
  AssertWmThread(L"Workspace::CycleLayout");
  if (cycle.empty()) return false;

  auto it = std::find(cycle.begin(), cycle.end(), layout_);
  TileLayout nextLayout = cycle.front();
  if (it != cycle.end()) {
    ++it;
    if (it != cycle.end()) nextLayout = *it;
  }
  if (nextLayout == layout_) return false;

  SetLayout(nextLayout);
  return true;
}

void Workspace::SetMasterRatio(double ratio) {
  AssertWmThread(L"Workspace::SetMasterRatio");
  masterRatio_ = ClampDouble(ratio, 0.1, 0.9);
  DebugValidateMutation(L"Workspace::SetMasterRatio");
}

bool Workspace::MakeMaster(HWND hwnd, HWND* oldMaster) {
  AssertWmThread(L"Workspace::MakeMaster");
  if (layout_ == TileLayout::Floating || windows_.size() < 2) return false;
  auto it = std::find(windows_.begin(), windows_.end(), hwnd);
  if (it == windows_.end() || it == windows_.begin()) return false;
  if (oldMaster) *oldMaster = windows_.front();
  std::iter_swap(windows_.begin(), it);
  DebugValidateMutation(L"Workspace::MakeMaster");
  return true;
}

bool Workspace::SwapTiled(HWND first, HWND second) {
  AssertWmThread(L"Workspace::SwapTiled");
  auto firstIt = std::find(windows_.begin(), windows_.end(), first);
  auto secondIt = std::find(windows_.begin(), windows_.end(), second);
  if (firstIt == windows_.end() || secondIt == windows_.end() || firstIt == secondIt) {
    return false;
  }
  std::iter_swap(firstIt, secondIt);
  DebugValidateMutation(L"Workspace::SwapTiled");
  return true;
}

bool Workspace::PromoteTiledToIndex(HWND hwnd, size_t targetIndex) {
  AssertWmThread(L"Workspace::PromoteTiledToIndex");
  auto it = std::find(windows_.begin(), windows_.end(), hwnd);
  if (it == windows_.end()) return false;

  const size_t currentIndex =
      static_cast<size_t>(std::distance(windows_.begin(), it));
  if (targetIndex >= currentIndex) return false;

  std::rotate(windows_.begin() + targetIndex, it, it + 1);
  DebugValidateMutation(L"Workspace::PromoteTiledToIndex");
  return true;
}

void Workspace::RepairForArrangement(
    const std::vector<HWND>& invalidWindows,
    const std::vector<WindowRecord>& missingActiveRecords) {
  AssertWmThread(L"Workspace::RepairForArrangement");
  for (HWND hwnd : invalidWindows) Forget(hwnd);

  for (WindowRecord record : missingActiveRecords) {
    if (!record.hwnd || !ContainsWindow(windows_, record.hwnd)) continue;
    if (!record.tilingGeneration) {
      record.tilingGeneration = Model::NextTilingGeneration();
    }
    record.state = ManageState::Tiled;
    record.suspensionReason = SuspensionReason::None;
    record.savedSlot = {};
    record.hasSavedSlot = false;
    UpsertRecord(std::move(record));
  }
  EnsureWeights();
  DebugValidateMutation(L"Workspace::RepairForArrangement");
}




static bool IsWorkspaceOnActiveDesktop(const DesktopMonitorKey& key) {
  GUID zeroGuid{};
  if (IsEqualGUID(key.desktopId, zeroGuid)) return false;

  // The reconciled desktop is actor-owned and authoritative between transition
  // signals. Avoid re-querying Explorer from high-frequency placement and
  // LOCATIONCHANGE paths; a dirty transition deliberately retains the old GUID
  // for comparison while falling through to the existing live query.
  if (g_wm.reconciledDesktopState == ReconciledDesktopState::Settled) {
    return IsEqualGUID(g_wm.reconciledDesktop, key.desktopId);
  }

  GUID currentDesktop{};
  if (!GetCurrentDesktopId(&currentDesktop)) return false;
  return IsEqualGUID(currentDesktop, key.desktopId);
}

// Repairs only model-local arrangement preconditions. OS lifecycle/ownership
// reconciliation belongs elsewhere; this function handles missed destruction and
// legacy states whose active-order record marker drifted.
static void ObserveAndRepairWorkspaceForArrangement(Workspace& workspace) {
  std::vector<HWND> invalidWindows;
  std::vector<WindowRecord> missingActiveRecords;

  for (const auto& kv : workspace.Records()) {
    if (!IsWindow(kv.first)) invalidWindows.push_back(kv.first);
  }
  for (HWND hwnd : workspace.TiledWindows()) {
    if (!workspace.HasRecord(hwnd) && IsWindow(hwnd)) {
      missingActiveRecords.push_back(MakeWindowRecord(hwnd, ManageState::Tiled));
    }
  }
  workspace.RepairForArrangement(invalidWindows, missingActiveRecords);
}

//-----------------------------------------------------------------------------
// Workspace resize-learning operations
//-----------------------------------------------------------------------------

static std::vector<WindowConstraints> ActiveWindowConstraints(
    const Workspace& workspace) {
  std::vector<WindowConstraints> constraints;
  constraints.reserve(workspace.ActiveCount());
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    WindowConstraints constraint =
        record ? record->constraints : WindowConstraints{};

    // Proven pixel bounds are DPI-specific observations. If a window crossed to
    // another DPI before a new correction is observed, optimistically ignore the
    // stale proof; a real bound will immediately prove itself again on placement.
    if (constraint.valid &&
        (constraint.enforceMinWidth || constraint.enforceMinHeight)) {
      const HMONITOR monitor = GetWindowPhysicalMonitor(hwnd);
      const UINT currentDpi = monitor ? GetMonitorEffectiveDpi(monitor) : 0;
      if (currentDpi && currentDpi != constraint.dpi) {
        constraint.enforceMinWidth = false;
        constraint.enforceMinHeight = false;
      }
    }
    constraints.push_back(constraint);
  }
  return constraints;
}

static bool RectsSatisfyWindowConstraints(
    const std::vector<RECT>& rects,
    const std::vector<WindowConstraints>& constraints) {
  if (rects.size() != constraints.size()) return false;
  for (size_t i = 0; i < rects.size(); ++i) {
    const LONG width = rects[i].right - rects[i].left;
    const LONG height = rects[i].bottom - rects[i].top;
    const WindowConstraints& c = constraints[i];
    if ((c.valid && c.enforceMinWidth && width < std::max<LONG>(1, c.minWidth)) ||
        (c.valid && c.enforceMinHeight && height < std::max<LONG>(1, c.minHeight))) {
      return false;
    }
  }
  return true;
}

// BSP geometry remains canonical. Reassign the existing slots only when every
// active window has a compatible destination; no alternate divider model is
// introduced for constrained windows.
static bool PromoteBspWindowForConstraints(
    Workspace& workspace, const RECT& workArea, LONG gap) {
  if (workspace.Layout() != TileLayout::BSP || workspace.ActiveCount() < 2) {
    return false;
  }

  std::vector<RECT> slots(workspace.ActiveCount());
  Layout::LayoutBSP(
      workArea, gap, 0, workspace.ActiveCount(), 0, slots);
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(workspace);
  if (RectsSatisfyWindowConstraints(slots, constraints)) return false;

  struct BspWindow {
    HWND hwnd;
    WindowConstraints constraints;
    uint64_t tilingGeneration;
    std::vector<size_t> compatibleSlots;
  };

  std::vector<BspWindow> windows;
  windows.reserve(constraints.size());
  for (size_t windowIndex = 0; windowIndex < constraints.size(); ++windowIndex) {
    const WindowConstraints& constraint = constraints[windowIndex];
    BspWindow window{
        workspace.TiledWindows()[windowIndex], constraint, 0, {}};
    const WindowRecord* record = workspace.Find(window.hwnd);
    window.tilingGeneration = record ? record->tilingGeneration : 0;

    auto addSlotIfCompatible = [&](size_t slotIndex) {
      const LONG width = slots[slotIndex].right - slots[slotIndex].left;
      const LONG height = slots[slotIndex].bottom - slots[slotIndex].top;
      if ((!constraint.valid || !constraint.enforceMinWidth ||
           width >= std::max<LONG>(1, constraint.minWidth)) &&
          (!constraint.valid || !constraint.enforceMinHeight ||
           height >= std::max<LONG>(1, constraint.minHeight))) {
        window.compatibleSlots.push_back(slotIndex);
      }
    };

    // Prefer retaining the current slot. Remaining choices run from later to
    // earlier so a displaced flexible window consumes the smallest slot it can.
    addSlotIfCompatible(windowIndex);
    for (size_t slotIndex = slots.size(); slotIndex-- > 0;) {
      if (slotIndex != windowIndex) addSlotIfCompatible(slotIndex);
    }
    if (window.compatibleSlots.empty()) return false;
    windows.push_back(std::move(window));
  }

  // Match every window to one compatible canonical slot. Processing newer
  // admissions last gives them the same tie-breaking priority as the previous
  // one-window promotion policy, while augmenting paths allow several constrained
  // windows to move before deciding that the layout is genuinely unsatisfiable.
  std::vector<size_t> matchingOrder(windows.size());
  for (size_t i = 0; i < matchingOrder.size(); ++i) matchingOrder[i] = i;
  std::stable_sort(
      matchingOrder.begin(), matchingOrder.end(),
      [&](size_t first, size_t second) {
        return windows[first].tilingGeneration <
               windows[second].tilingGeneration;
      });

  const size_t unmatched = windows.size();
  std::vector<size_t> slotToWindow(slots.size(), unmatched);
  auto assignWindow = [&](auto&& self, size_t windowIndex,
                          std::vector<bool>& visitedSlots) -> bool {
    for (size_t slotIndex : windows[windowIndex].compatibleSlots) {
      if (visitedSlots[slotIndex]) continue;
      visitedSlots[slotIndex] = true;
      if (slotToWindow[slotIndex] == unmatched ||
          self(self, slotToWindow[slotIndex], visitedSlots)) {
        slotToWindow[slotIndex] = windowIndex;
        return true;
      }
    }
    return false;
  };

  for (size_t windowIndex : matchingOrder) {
    std::vector<bool> visitedSlots(slots.size(), false);
    if (!assignWindow(assignWindow, windowIndex, visitedSlots)) return false;
  }

  std::vector<HWND> orderedWindows(slots.size());
  std::vector<WindowConstraints> reorderedConstraints;
  reorderedConstraints.reserve(slots.size());
  for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
    if (slotToWindow[slotIndex] == unmatched) return false;
    const BspWindow& window = windows[slotToWindow[slotIndex]];
    orderedWindows[slotIndex] = window.hwnd;
    reorderedConstraints.push_back(window.constraints);
  }
  if (!RectsSatisfyWindowConstraints(slots, reorderedConstraints)) {
    return false;
  }

  bool changed = false;
  for (size_t targetIndex = 0;
       targetIndex < orderedWindows.size(); ++targetIndex) {
    const auto& active = workspace.TiledWindows();
    auto current = std::find(
        active.begin() + targetIndex, active.end(), orderedWindows[targetIndex]);
    if (current == active.end()) return changed;

    const size_t sourceIndex =
        static_cast<size_t>(std::distance(active.begin(), current));
    if (sourceIndex == targetIndex) continue;
    if (!workspace.PromoteTiledToIndex(
            orderedWindows[targetIndex], targetIndex)) {
      return changed;
    }

    changed = true;
    Wh_Log(
        L"BSP constraint promotion hwnd=%p slot=%zu -> %zu",
        orderedWindows[targetIndex], sourceIndex, targetIndex);
    Diagnostics::RecordEvent(
        L"BSP constraint promotion hwnd=%p slot=%zu -> %zu",
        orderedWindows[targetIndex], sourceIndex, targetIndex);
  }
  return changed;
}

constexpr LONG kResizeEdgeDeltaTolerance = 1;
constexpr LONG kMinLearnedResizeSpan = 80;

static bool IsMeaningfulResizeDelta(LONG delta) {
  return std::abs(delta) > kResizeEdgeDeltaTolerance;
}

// Transfers one divider delta between adjacent logical weights while preserving
// their combined weight. Expected layout rectangles provide the pre-drag spans;
// live sibling geometry is intentionally never learned into durable state.
static bool TransferPairResizeDelta(
    std::vector<double>& weights, size_t firstWeight,
    const RECT& firstRect, const RECT& secondRect,
    bool verticalAxis, LONG delta) {
  if (!IsMeaningfulResizeDelta(delta) || firstWeight + 1 >= weights.size()) {
    return false;
  }

  const LONG firstSpan = verticalAxis ? firstRect.bottom - firstRect.top
                                      : firstRect.right - firstRect.left;
  const LONG secondSpan = verticalAxis ? secondRect.bottom - secondRect.top
                                       : secondRect.right - secondRect.left;
  const LONG pairSpan = firstSpan + secondSpan;
  if (firstSpan < 1 || secondSpan < 1 || pairSpan <= 1) return false;

  const LONG minSpan = std::min<LONG>(
      kMinLearnedResizeSpan, std::max<LONG>(1, (pairSpan - 1) / 2));
  const LONG maxFirstSpan = pairSpan - minSpan;
  const LONG newFirstSpan =
      std::clamp<LONG>(firstSpan + delta, minSpan, maxFirstSpan);
  if (newFirstSpan == firstSpan) return false;

  double pairWeight = weights[firstWeight] + weights[firstWeight + 1];
  if (!(pairWeight > 0.0) || !std::isfinite(pairWeight)) pairWeight = 2.0;

  const double firstFraction =
      static_cast<double>(newFirstSpan) / static_cast<double>(pairSpan);
  weights[firstWeight] = pairWeight * firstFraction;
  weights[firstWeight + 1] = pairWeight - weights[firstWeight];
  return true;
}

bool Workspace::LearnMasterStackResize(
    const RECT& workArea, LONG gap, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  AssertWmThread(L"Workspace::LearnMasterStackResize");
  EnsureWeights();
  const bool horizontal = layout_ == TileLayout::MasterStackH;
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(*this);

  std::vector<RECT> expectedRects;
  Layout::LayoutMasterStackWeighted(
      workArea, gap, windows_.size(), expectedRects, horizontal,
      ClampDouble(masterRatio_, 0.1, 0.9), stackWeights_);
  if (!RectsSatisfyWindowConstraints(expectedRects, constraints)) {
    std::vector<RECT> constrainedRects;
    if (Layout::LayoutMasterStackConstrained(
            workArea, gap, horizontal,
            ClampDouble(masterRatio_, 0.1, 0.9), stackWeights_,
            constraints, &constrainedRects)) {
      expectedRects = std::move(constrainedRects);
    }
  }
  if (expectedRects.size() != windows_.size()) return false;

  bool changed = false;

  // Main master/stack divider.
  const LONG mainDelta = horizontal
      ? (resizedIndex == 0 ? gesture.end.bottom - gesture.start.bottom
                           : gesture.end.top - gesture.start.top)
      : (resizedIndex == 0 ? gesture.end.right - gesture.start.right
                           : gesture.end.left - gesture.start.left);

  if (windows_.size() > 1 && IsMeaningfulResizeDelta(mainDelta)) {
    const LONG totalSize = horizontal ? workArea.bottom - workArea.top
                                      : workArea.right - workArea.left;
    const LONG usableSize = totalSize - gap;
    if (usableSize > 1) {
      const LONG oldMasterSpan = horizontal
          ? expectedRects[0].bottom - expectedRects[0].top
          : expectedRects[0].right - expectedRects[0].left;
      const LONG newMasterSpan = std::clamp<LONG>(
          oldMasterSpan + mainDelta, 1, usableSize - 1);
      const double newRatio = ClampDouble(
          static_cast<double>(newMasterSpan) / static_cast<double>(usableSize),
          0.1, 0.9);
      if (std::abs(newRatio - masterRatio_) > 1e-9) {
        masterRatio_ = newRatio;
        changed = true;
      }
    }
  }

  // Stack-axis dividers.
  if (resizedIndex > 0 && stackWeights_.size() + 1 == windows_.size()) {
    const size_t stackIndex = resizedIndex - 1;
    const LONG leadingDelta = horizontal
        ? gesture.end.left - gesture.start.left
        : gesture.end.top - gesture.start.top;
    const LONG trailingDelta = horizontal
        ? gesture.end.right - gesture.start.right
        : gesture.end.bottom - gesture.start.bottom;

    if (stackIndex > 0) {
      changed |= TransferPairResizeDelta(
          stackWeights_, stackIndex - 1,
          expectedRects[resizedIndex - 1], expectedRects[resizedIndex],
          !horizontal, leadingDelta);
    }
    if (stackIndex + 1 < stackWeights_.size()) {
      changed |= TransferPairResizeDelta(
          stackWeights_, stackIndex,
          expectedRects[resizedIndex], expectedRects[resizedIndex + 1],
          !horizontal, trailingDelta);
    }
  }

  if (changed) DebugValidateMutation(L"Workspace::LearnMasterStackResize");
  return changed;
}

bool Workspace::LearnGridResize(
    const RECT& workArea, LONG gap, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  AssertWmThread(L"Workspace::LearnGridResize");
  EnsureWeights();
  const bool horizontal = layout_ == TileLayout::Rows;
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(*this);

  std::vector<RECT> expectedRects;
  Layout::LayoutGridWeighted(
      workArea, gap, windows_.size(), expectedRects,
      horizontal, gridWeights_);
  if (!RectsSatisfyWindowConstraints(expectedRects, constraints)) {
    std::vector<RECT> constrainedRects;
    if (Layout::LayoutGridConstrained(
            workArea, gap, gridWeights_, constraints,
            horizontal, &constrainedRects)) {
      expectedRects = std::move(constrainedRects);
    }
  }
  if (expectedRects.size() != windows_.size()) return false;

  bool changed = false;
  const LONG leadingDelta = horizontal
      ? gesture.end.top - gesture.start.top
      : gesture.end.left - gesture.start.left;
  const LONG trailingDelta = horizontal
      ? gesture.end.bottom - gesture.start.bottom
      : gesture.end.right - gesture.start.right;

  if (resizedIndex > 0) {
    changed |= TransferPairResizeDelta(
        gridWeights_, resizedIndex - 1,
        expectedRects[resizedIndex - 1], expectedRects[resizedIndex],
        horizontal, leadingDelta);
  }
  if (resizedIndex + 1 < windows_.size()) {
    changed |= TransferPairResizeDelta(
        gridWeights_, resizedIndex,
        expectedRects[resizedIndex], expectedRects[resizedIndex + 1],
        horizontal, trailingDelta);
  }
  if (changed) DebugValidateMutation(L"Workspace::LearnGridResize");
  return changed;
}

enum class LayoutPlanStatus {
  Success,
  NotApplicable,
  Unsatisfiable,
};

static LayoutPlanStatus BuildWorkspaceLayoutPlan(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects) {
  if (!outRects) return LayoutPlanStatus::Unsatisfiable;
  outRects->assign(workspace.ActiveCount(), RECT{});

  // Always begin from MultiWM's ordinary geometry. With no empirically proven
  // constraints this is byte-for-byte the established layout path.
  switch (workspace.Layout()) {
    case TileLayout::MasterStack:
      Layout::LayoutMasterStackWeighted(
          workArea, gap, workspace.ActiveCount(), *outRects, false,
          ClampDouble(workspace.MasterRatio(), 0.1, 0.9),
          workspace.StackWeights());
      break;

    case TileLayout::MasterStackH:
      Layout::LayoutMasterStackWeighted(
          workArea, gap, workspace.ActiveCount(), *outRects, true,
          ClampDouble(workspace.MasterRatio(), 0.1, 0.9),
          workspace.StackWeights());
      break;

    case TileLayout::Columns:
      Layout::LayoutGridWeighted(
          workArea, gap, workspace.ActiveCount(), *outRects, false,
          workspace.GridWeights());
      break;

    case TileLayout::Rows:
      Layout::LayoutGridWeighted(
          workArea, gap, workspace.ActiveCount(), *outRects, true,
          workspace.GridWeights());
      break;

    case TileLayout::BSP:
      Layout::LayoutBSP(
          workArea, gap, 0, workspace.ActiveCount(), 0, *outRects);
      break;

    case TileLayout::Monocle:
      outRects->assign(workspace.ActiveCount(), workArea);
      break;

    case TileLayout::Floating:
    case TileLayout::COUNT:
      outRects->clear();
      return LayoutPlanStatus::NotApplicable;
  }

  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(workspace);
  if (RectsSatisfyWindowConstraints(*outRects, constraints)) {
    return LayoutPlanStatus::Success;
  }

  bool constrainedSuccess = false;
  switch (workspace.Layout()) {
    case TileLayout::MasterStack:
      constrainedSuccess = Layout::LayoutMasterStackConstrained(
          workArea, gap, false,
          ClampDouble(workspace.MasterRatio(), 0.1, 0.9),
          workspace.StackWeights(), constraints, outRects);
      break;

    case TileLayout::MasterStackH:
      constrainedSuccess = Layout::LayoutMasterStackConstrained(
          workArea, gap, true,
          ClampDouble(workspace.MasterRatio(), 0.1, 0.9),
          workspace.StackWeights(), constraints, outRects);
      break;

    case TileLayout::Columns:
      constrainedSuccess = Layout::LayoutGridConstrained(
          workArea, gap, workspace.GridWeights(),
          constraints, false, outRects);
      break;

    case TileLayout::Rows:
      constrainedSuccess = Layout::LayoutGridConstrained(
          workArea, gap, workspace.GridWeights(),
          constraints, true, outRects);
      break;

    // BSP keeps its ordinary 50/50 geometry. ArrangeWorkspace first tries to
    // promote a constrained window into an earlier canonical slot.
    case TileLayout::BSP:
      return LayoutPlanStatus::Unsatisfiable;

    case TileLayout::Monocle:
      constrainedSuccess = Layout::LayoutMonocleConstrained(
          workArea, constraints, outRects);
      break;

    case TileLayout::Floating:
    case TileLayout::COUNT:
      return LayoutPlanStatus::NotApplicable;
  }

  if (constrainedSuccess) return LayoutPlanStatus::Success;
  return LayoutPlanStatus::Unsatisfiable;
}


Workspace::PlacementAction Workspace::ApplyPlacementObservation(
    HWND hwnd, const PlacementObservation& observation) {
  AssertWmThread(L"Workspace::ApplyPlacementObservation");
  WindowRecord* record = FindMutable(hwnd);
  if (!record) return PlacementAction::Forget;

  record->lastRequestedRect = observation.requested;
  record->lastObservedRect = observation.observed;
  record->lastPlacementResult = observation.result;
  DebugValidateMutation(L"Workspace::ApplyPlacementObservation");

  switch (observation.result) {
    case PlacementResult::Success:
    case PlacementResult::AdjustedByWindow:
    case PlacementResult::Refused:
      return PlacementAction::None;

    case PlacementResult::AccessDenied:
      return PlacementAction::Float;

    case PlacementResult::Dead:
      return PlacementAction::Forget;
  }
  return PlacementAction::None;
}

// Renders one workspace from authoritative model state. Arrangement itself is a
// small pipeline: repair local invariants -> compute targets -> apply placement ->
// fold placement outcomes back into the model. Reflow is iterative: a pass may
// learn a new enforced constraint or permanently float one newest admission.
static void ArrangeWorkspace(const DesktopMonitorKey& key) {
  ++Diagnostics::g_runtime.counters.arrangeCalls;
  if (!IsWorkspaceOnActiveDesktop(key)) return;

  Workspace workspace;
  if (!g_workspaces.Load(key, &workspace)) return;

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return;  // Detached monitor: keep workspace state dormant.

  WorkspaceMetrics metrics;
  if (!GetWorkspaceMetrics(monitor, &metrics)) return;
  const RECT& workArea = metrics.workArea;
  const LONG gap = metrics.gap;

  ObserveAndRepairWorkspaceForArrangement(workspace);
  // Constraint reports are intentionally lazy. Until a window actually corrects
  // one of our placements, ordinary MultiWM geometry remains authoritative.
  const size_t maxPasses = workspace.ActiveCount() * 5 + 8;
  for (size_t pass = 0; pass < maxPasses; ++pass) {
    ++Diagnostics::g_runtime.counters.arrangePasses;
    if (workspace.Layout() == TileLayout::Floating || workspace.Empty()) {
      if (workspace.Layout() == TileLayout::Floating) {
        for (HWND hwnd : workspace.TiledWindows()) CancelConformanceLease(hwnd);
      }
      g_workspaces.Save(key, workspace);
      return;
    }

    if (pass == 0) {
      // Proven native minimums are mutable window state, not permanent HWND
      // identity. Refresh only the uncommon records that already constrain the
      // solver. A failed query preserves the last proof; a changed successful
      // report clears it until the HWND demonstrates the new bound.
      for (HWND hwnd : workspace.TiledWindows()) {
        const WindowRecord* record = workspace.Find(hwnd);
        if (!record || !record->constraints.valid ||
            (!record->constraints.enforceMinWidth &&
             !record->constraints.enforceMinHeight) ||
            GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
            IsWindowCloaked(hwnd)) {
          continue;
        }

        const WindowConstraints before = record->constraints;
        const WindowConstraints fresh = ProbeWindowConstraints(hwnd);
        WindowConstraints refreshed =
            MergeConstraintProbeWithCache(before, fresh);
        if (workspace.UpdateConstraints(hwnd, refreshed) && fresh.valid &&
            !SameEnforcedConstraintGeometry(before, refreshed)) {
          Diagnostics::RecordEvent(
              L"constraint report changed; proof cleared hwnd=%p min=%ldx%ld dpi=%u",
              hwnd, refreshed.minWidth, refreshed.minHeight, refreshed.dpi);
        }
      }
    }

    PromoteBspWindowForConstraints(workspace, workArea, gap);

    std::vector<RECT> rects;
    ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
    const LayoutPlanStatus planStatus =
        BuildWorkspaceLayoutPlan(workspace, workArea, gap, &rects);
    if (planStatus == LayoutPlanStatus::NotApplicable) {
      g_workspaces.Save(key, workspace);
      return;
    }
    if (planStatus == LayoutPlanStatus::Unsatisfiable) {
      ++Diagnostics::g_runtime.counters.constraintUnsatisfiablePlans;
      HWND victim = nullptr;
      bool intrinsicallyImpossible = false;

      // Removing another member cannot help a window whose proven minimum is
      // larger than the entire usable workspace. Float that window directly;
      // reserve the newest-admission policy for aggregate constraint conflicts.
      const LONG workspaceWidth = RectWidth(workArea);
      const LONG workspaceHeight = RectHeight(workArea);
      const std::vector<WindowConstraints> activeConstraints =
          ActiveWindowConstraints(workspace);
      uint64_t newestImpossibleGeneration = 0;
      for (size_t i = 0;
           i < workspace.TiledWindows().size() &&
           i < activeConstraints.size();
           ++i) {
        const WindowConstraints& constraint = activeConstraints[i];
        if (!constraint.valid ||
            (!constraint.enforceMinWidth && !constraint.enforceMinHeight)) {
          continue;
        }

        const bool impossible =
            (constraint.enforceMinWidth &&
             std::max<LONG>(1, constraint.minWidth) > workspaceWidth) ||
            (constraint.enforceMinHeight &&
             std::max<LONG>(1, constraint.minHeight) > workspaceHeight);
        if (!impossible) continue;

        const HWND candidate = workspace.TiledWindows()[i];
        const WindowRecord* candidateRecord = workspace.Find(candidate);
        const uint64_t generation =
            candidateRecord ? candidateRecord->tilingGeneration : 0;
        if (!victim || generation >= newestImpossibleGeneration) {
          victim = candidate;
          newestImpossibleGeneration = generation;
        }
      }

      if (victim) {
        intrinsicallyImpossible = true;
      } else {
        victim = workspace.NewestTiledWindow();
      }
      const WindowRecord* victimRecord = victim ? workspace.Find(victim) : nullptr;
      if (!victim || !victimRecord) {
        g_workspaces.Save(key, workspace);
        return;
      }

      const uint64_t generation = victimRecord->tilingGeneration;
      const WindowConstraints constraints = victimRecord->constraints;
      if (intrinsicallyImpossible) {
        Wh_Log(
          L"Proven minimum exceeds workspace; floating HWND %p (generation=%llu)",
          static_cast<void*>(victim),
          static_cast<unsigned long long>(generation));
      } else {
        Wh_Log(
          L"Proven constraint layout unsatisfiable; floating newest tiled HWND %p (generation=%llu)",
          static_cast<void*>(victim),
          static_cast<unsigned long long>(generation));
      }
      Diagnostics::RecordEvent(
          L"proven constraints floated hwnd=%p generation=%llu intrinsic=%d enforcedMin=%d%d reportedMin=%ldx%ld",
          victim, static_cast<unsigned long long>(generation),
          intrinsicallyImpossible ? 1 : 0,
          constraints.enforceMinWidth ? 1 : 0,
          constraints.enforceMinHeight ? 1 : 0,
          constraints.minWidth, constraints.minHeight);

      CancelConformanceLease(victim);
      if (!workspace.Float(victim)) {
        g_workspaces.Save(key, workspace);
        return;
      }
      ++Diagnostics::g_runtime.counters.constraintFloats;
      RepairFloatingGeometry(key, workspace, victim, FloatingPlacementHint{});
      g_workspaces.Save(key, workspace);
      if (workspace.Empty()) return;
      continue;
    }

    std::vector<HWND> toFloat;
    std::vector<HWND> toForget;
    bool constraintReplanRequested = false;
    const std::vector<HWND> active = workspace.TiledWindows();
    const size_t count = std::min(active.size(), rects.size());
    for (size_t i = 0; i < count; ++i) {
      const HWND hwnd = active[i];
      Workspace::PlacementAction action = Workspace::PlacementAction::None;

      if (!IsWindow(hwnd)) {
        action = Workspace::PlacementAction::Forget;
      } else if (GetPhysicalSuspensionReason(hwnd) == SuspensionReason::None &&
                 !IsWindowCloaked(hwnd)) {
        const WindowRecord* record = workspace.Find(hwnd);
        if (!record) {
          action = Workspace::PlacementAction::Forget;
        } else {
          PlacementObservation observation =
              PlaceWindowChecked(hwnd, record->canMove, rects[i]);
          action = workspace.ApplyPlacementObservation(hwnd, observation);

          // Treat WM_GETMINMAXINFO as evidence, not authority. Query only after
          // the HWND has actually corrected our target; promote individual axes
          // when the observed frame lands on the reported bound.
          if (observation.result == PlacementResult::AdjustedByWindow) {
            const WindowConstraints before = record->constraints;
            const WindowConstraints fresh = ProbeWindowConstraints(hwnd);
            WindowConstraints observed = MergeConstraintProbeWithCache(
                before, fresh);
            const bool reportChanged = !SameConstraintReport(before, observed);
            if (fresh.valid && IsWindow(hwnd) &&
                GetPhysicalSuspensionReason(hwnd) == SuspensionReason::None &&
                !IsWindowCloaked(hwnd)) {
              PromoteEnforcedConstraintEvidence(observation, &observed);
            }
            const bool enforcedChanged =
                !SameEnforcedConstraintGeometry(before, observed);
            workspace.UpdateConstraints(hwnd, observed);

            if (enforcedChanged) {
              action = Workspace::PlacementAction::None;
              constraintReplanRequested = true;
              CancelConformanceLease(hwnd);
              Diagnostics::RecordEvent(
                  L"placement updated constraints hwnd=%p min=%ldx%ld enforcedMin=%d%d",
                  hwnd, observed.minWidth, observed.minHeight,
                  observed.enforceMinWidth ? 1 : 0,
                  observed.enforceMinHeight ? 1 : 0);
            } else if (reportChanged) {
              Diagnostics::RecordEvent(
                  L"placement updated unproven constraint report hwnd=%p min=%ldx%ld",
                  hwnd, observed.minWidth, observed.minHeight);
            }
          } else if (observation.result == PlacementResult::Success) {
            // Empirical evidence works both ways. If a previously proven minimum
            // is crossed successfully (for example after a framework/state change),
            // stop constraining future plans on that stale proof.
            WindowConstraints relaxed = record->constraints;
            const LONG width = RectWidth(observation.requested);
            const LONG height = RectHeight(observation.requested);
            bool relaxedAny = false;
            if (relaxed.enforceMinWidth &&
                width + Model::kPlacementTolerancePx < relaxed.minWidth) {
              relaxed.enforceMinWidth = false;
              relaxedAny = true;
            }
            if (relaxed.enforceMinHeight &&
                height + Model::kPlacementTolerancePx < relaxed.minHeight) {
              relaxed.enforceMinHeight = false;
              relaxedAny = true;
            }
            if (relaxedAny) {
              workspace.UpdateConstraints(hwnd, relaxed);
              Diagnostics::RecordEvent(
                  L"successful placement cleared stale constraint proof hwnd=%p",
                  hwnd);
            }
          }

          if (!constraintReplanRequested) {
            if (action != Workspace::PlacementAction::None) {
              CancelConformanceLease(hwnd);
            } else {
              switch (observation.result) {
                case PlacementResult::Success:
                  if (observation.placementIssued) {
                    BeginConformanceLease(hwnd, rects[i]);
                  }
                  break;
                case PlacementResult::AdjustedByWindow:
                case PlacementResult::Refused:
                  BeginConformanceLease(hwnd, rects[i], true);
                  break;
                case PlacementResult::AccessDenied:
                case PlacementResult::Dead:
                  break;
              }
            }
          }
        }
      }

      switch (action) {
        case Workspace::PlacementAction::None:
          break;
        case Workspace::PlacementAction::Float:
          toFloat.push_back(hwnd);
          break;
        case Workspace::PlacementAction::Forget:
          toForget.push_back(hwnd);
          break;
      }
      if (constraintReplanRequested) break;
    }

    if (constraintReplanRequested) {
      g_workspaces.Save(key, workspace);
      continue;
    }

    bool participationChanged = false;
    for (HWND hwnd : toForget) {
      CancelConformanceLease(hwnd);
      participationChanged |= workspace.Forget(hwnd);
    }
    for (HWND hwnd : toFloat) {
      const WindowRecord* record = workspace.Find(hwnd);
      if (!record || record->state != ManageState::Tiled) continue;
      Wh_Log(
          L"Window %p cannot be placed (result=%d); switching it to Floating",
          hwnd, static_cast<int>(record->lastPlacementResult));
      Diagnostics::RecordEvent(
          L"placement failure forced hwnd=%p to per-window Floating (result=%d)",
          hwnd, static_cast<int>(record->lastPlacementResult));
      CancelConformanceLease(hwnd);
      participationChanged |= workspace.Float(hwnd);
    }

    g_workspaces.Save(key, workspace);
    if (!participationChanged || workspace.Empty()) return;
  }

  Diagnostics::RecordEvent(
      L"arrangement safety bound exhausted desktop=%08X active=%zu",
      key.desktopId.Data1, workspace.ActiveCount());
  g_workspaces.Save(key, workspace);
}

// Consumes one expired generation. A user gesture, migration, or new
// authoritative placement that cancelled/replaced it wins if it happens first.
static bool TakeExpiredConformanceLease(
    HWND hwnd, uint64_t generation, ConformanceLease* outLease) {
  AssertWmThread(L"TakeExpiredConformanceLease");
  if (!hwnd || generation == 0 || !outLease) return false;

  const ULONGLONG now = GetTickCount64();
  bool taken = false;
  auto it = g_conformanceLeases.leases.find(hwnd);
  if (it != g_conformanceLeases.leases.end() &&
      it->second.generation == generation &&
      now >= it->second.expiresAtTickMs) {
    *outLease = it->second;
    g_conformanceLeases.leases.erase(it);
    taken = true;
  }
  return taken;
}

// Recomputes the target from the current workspace rather than trusting a copied
// lease blindly. This makes expiry harmless if a layout/work-area change replaced
// the authoritative tile without issuing another SetWindowPos for this HWND.
static bool GetCurrentAuthoritativeTiledRect(
    const DesktopMonitorKey& key, const Workspace& workspace, HWND hwnd,
    RECT* outRect) {
  if (!outRect || !hwnd || workspace.Layout() == TileLayout::Floating) return false;
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || record->state != ManageState::Tiled) return false;

  const size_t tiledIndex = workspace.TiledIndex(hwnd);
  if (tiledIndex == static_cast<size_t>(-1)) return false;

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return false;
  WorkspaceMetrics metrics;
  if (!GetWorkspaceMetrics(monitor, &metrics)) return false;

  std::vector<RECT> rects;
  if (BuildWorkspaceLayoutPlan(
          workspace, metrics.workArea, metrics.gap, &rects) !=
          LayoutPlanStatus::Success ||
      tiledIndex >= rects.size()) {
    return false;
  }
  *outRect = rects[tiledIndex];
  return true;
}

// HWND values can be reused. Conformance is allowed to act only on the same
// process identity that owns the authoritative WindowRecord; normal settled
// reconciliation remains responsible for forgetting a stale reused handle.
static bool ConformanceWindowMatchesRecordIdentity(
    HWND hwnd, const WindowRecord& record) {
  if (!hwnd || !record.pid) return false;
  DWORD livePid = 0;
  return GetWindowThreadProcessId(hwnd, &livePid) != 0 &&
         livePid != 0 && livePid == record.pid;
}

// The deadline is a final passive verdict, not one more fight with the app. If
// this exact lease still describes the current tile and the HWND is materially
// elsewhere, concede that it is unsuitable for tiling: float it and close the
// remaining layout around it. Stale/replaced state is simply discarded.
static void FinalizeExpiredConformanceLease(
    HWND hwnd, const ConformanceLease& lease) {
  AssertWmThread(L"FinalizeExpiredConformanceLease");

  auto stale = [&] {
    ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredStale;
  };

  if (!IsWindow(hwnd) ||
      GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      IsWindowCloaked(hwnd) ||
      IsMoveSizeGestureInProgress(hwnd)) {
    stale();
    return;
  }

  const std::vector<DesktopMonitorKey> owners = g_workspaces.OwnersOf(hwnd);
  if (owners.size() != 1 || !IsWorkspaceOnActiveDesktop(owners.front())) {
    stale();
    return;
  }

  Workspace workspace;
  if (!g_workspaces.Load(owners.front(), &workspace)) {
    stale();
    return;
  }

  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || !ConformanceWindowMatchesRecordIdentity(hwnd, *record)) {
    stale();
    return;
  }

  RECT authoritative{};
  if (!GetCurrentAuthoritativeTiledRect(
          owners.front(), workspace, hwnd, &authoritative) ||
      !RectsNear(authoritative, lease.expectedRect)) {
    stale();
    return;
  }

  RECT current{};
  if (!GetWindowFrameRect(hwnd, &current)) {
    stale();
    return;
  }
  if (RectsNear(current, authoritative)) {
    ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredCompliant;
    Diagnostics::RecordEvent(
        L"conformance lease completed compliant hwnd=%p attempts=%u",
        hwnd, lease.attempts);
    return;
  }

  // Expiry processing performs several live queries after its initial preflight.
  // A window that entered fullscreen/maximized meanwhile must remain suspended,
  // not be permanently converted to Floating by this stale lease verdict.
  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None) {
    stale();
    return;
  }

  // The deadline is the last chance to distinguish a genuine native constraint
  // from generic nonconformance. This also covers a missed LOCATIONCHANGE event.
  PlacementObservation evidence;
  evidence.result = PlacementResult::AdjustedByWindow;
  evidence.requested = authoritative;
  evidence.observed = current;
  const WindowConstraints beforeConstraints = record->constraints;
  const WindowConstraints freshConstraints = ProbeWindowConstraints(hwnd);
  WindowConstraints observed = MergeConstraintProbeWithCache(
      beforeConstraints, freshConstraints);
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, *record) ||
      GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      IsWindowCloaked(hwnd)) {
    stale();
    return;
  }
  if (freshConstraints.valid) {
    PromoteEnforcedConstraintEvidence(evidence, &observed);
  }
  if (!SameEnforcedConstraintGeometry(beforeConstraints, observed)) {
    workspace.UpdateConstraints(hwnd, observed);
    Diagnostics::RecordEvent(
        L"conformance deadline updated constraints hwnd=%p min=%ldx%ld enforcedMin=%d%d",
        hwnd, observed.minWidth, observed.minHeight,
        observed.enforceMinWidth ? 1 : 0,
        observed.enforceMinHeight ? 1 : 0);
    g_workspaces.Save(owners.front(), workspace);
    ArrangeWorkspace(owners.front());
    return;
  }

  if (!workspace.Float(hwnd)) {
    stale();
    return;
  }

  ++Diagnostics::g_runtime.counters.conformanceLeaseExpiredFloats;
  Diagnostics::RecordEvent(
      L"conformance lease expired nonconforming; floated hwnd=%p attempts=%u",
      hwnd, lease.attempts);
  Wh_Log(
      L"Window %p remained outside its assigned tile at conformance deadline; switching it to Floating",
      hwnd);
  g_workspaces.Save(owners.front(), workspace);
  ArrangeWorkspace(owners.front());
}

// A tracked tiled HWND either confirms its authoritative rectangle or opens/reuses
// one bounded conformance lease. Native user gestures remain authoritative and
// are handled separately after MOVESIZEEND.
static bool HandleTiledWindowLocationChange(HWND hwnd) {
  if (IsMoveSizeGestureInProgress(hwnd)) return true;

  ConformanceLease lease;
  const ConformanceLeaseReadResult leaseState =
      ReadConformanceLease(hwnd, &lease);
  if (leaseState == ConformanceLeaseReadResult::Expired) {
    ScheduleNextConformanceTimer();
    return true;
  }
  const bool hasLease = leaseState == ConformanceLeaseReadResult::Active;

  if (!IsWindow(hwnd) ||
      GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      IsWindowCloaked(hwnd)) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  const std::vector<DesktopMonitorKey> owners = g_workspaces.OwnersOf(hwnd);
  if (owners.size() != 1 || !IsWorkspaceOnActiveDesktop(owners.front())) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  Workspace workspace;
  if (!g_workspaces.Load(owners.front(), &workspace)) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || record->state != ManageState::Tiled ||
      workspace.Layout() == TileLayout::Floating ||
      !ConformanceWindowMatchesRecordIdentity(hwnd, *record)) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  // A lease is target-specific. Layout/order/weight changes can alter a tile
  // without necessarily issuing SetWindowPos for this HWND, so never reinforce
  // the copied target until it is checked against the current layout plan.
  RECT authoritative{};
  if (!GetCurrentAuthoritativeTiledRect(
          owners.front(), workspace, hwnd, &authoritative)) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  RECT current{};
  if (!GetWindowFrameRect(hwnd, &current)) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  if (RectsNear(current, authoritative)) {
    return true;  // Our own placement/event echo; nothing to repair.
  }

  if (!hasLease || !RectsNear(authoritative, lease.expectedRect)) {
    if (hasLease) CancelConformanceLease(hwnd, lease.generation);
    BeginConformanceLease(hwnd, authoritative, true);
    return true;
  }

  const ULONGLONG now = GetTickCount64();
  if (lease.repairDueTickMs && now < lease.repairDueTickMs) {
    return true;
  }
  if (lease.lastRepairTickMs &&
      now - lease.lastRepairTickMs < g_settings.conformanceRepairIntervalMs) {
    const ULONGLONG due =
        lease.lastRepairTickMs + g_settings.conformanceRepairIntervalMs;
    // Do not simply drop a bursty snap-back event: schedule one delayed re-check
    // so a final app move inside the rate window cannot leave the tile displaced.
    if (DeferConformanceLeaseRepair(hwnd, lease.generation, due)) {
      ++Diagnostics::g_runtime.counters.conformanceLeaseDeferredRepairs;
      return true;
    }
    return false;
  }

  // A copied observation must still refer to the same active lease immediately
  // before we act. Earlier actor work can have cancelled or replaced it; the
  // generation check prevents stale work from accounting against the replacement.
  if (!IsCurrentConformanceLease(hwnd, lease.generation)) return false;

  // Re-check at the mutation boundary: Chromium can finish entering fullscreen
  // while the ownership/layout queries above are in progress.
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, *record) ||
      GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      IsWindowCloaked(hwnd)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  // The incoming LOCATIONCHANGE describes where the application settled after
  // our previous placement. Preserve that passive evidence before another
  // SetWindowPos can temporarily move the HWND back onto its requested tile.
  PlacementObservation settledEvidence;
  settledEvidence.result = PlacementResult::AdjustedByWindow;
  settledEvidence.requested = authoritative;
  settledEvidence.observed = current;
  const WindowConstraints beforeConstraints = record->constraints;
  const WindowConstraints freshConstraints = ProbeWindowConstraints(hwnd);
  WindowConstraints observedConstraints = MergeConstraintProbeWithCache(
      beforeConstraints, freshConstraints);
  // WM_GETMINMAXINFO executes target window code. Revalidate physical state
  // after that bounded call before either learning policy or reinforcing a tile.
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, *record) ||
      GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      IsWindowCloaked(hwnd)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  if (freshConstraints.valid) {
    PromoteEnforcedConstraintEvidence(settledEvidence, &observedConstraints);
  }
  if (!SameEnforcedConstraintGeometry(
          beforeConstraints, observedConstraints)) {
    workspace.UpdateConstraints(hwnd, observedConstraints);
    CancelConformanceLease(hwnd, lease.generation);
    Diagnostics::RecordEvent(
        L"conformance location updated constraints hwnd=%p min=%ldx%ld enforcedMin=%d%d",
        hwnd, observedConstraints.minWidth, observedConstraints.minHeight,
        observedConstraints.enforceMinWidth ? 1 : 0,
        observedConstraints.enforceMinHeight ? 1 : 0);
    g_workspaces.Save(owners.front(), workspace);
    ArrangeWorkspace(owners.front());
    return true;
  }

  PlacementObservation observation =
      PlaceWindowChecked(hwnd, record->canMove, authoritative);

  // Count every issued reinforcement attempt, not only successful ones. A failed
  // checked placement is still part of the lease's cadence and must not look like
  // repeated "repair=1" telemetry. This does not extend the original deadline.
  unsigned attemptNumber = lease.attempts + 1;
  const bool leaseRetained =
      RecordConformanceLeaseAttempt(hwnd, lease.generation, &attemptNumber);

  Workspace::PlacementAction action =
      workspace.ApplyPlacementObservation(hwnd, observation);

  if (observation.result == PlacementResult::AdjustedByWindow) {
    const WindowRecord* updatedRecord = workspace.Find(hwnd);
    if (updatedRecord) {
      if (freshConstraints.valid) {
        PromoteEnforcedConstraintEvidence(observation, &observedConstraints);
      }
      const bool enforcedChanged =
          !SameEnforcedConstraintGeometry(
              updatedRecord->constraints, observedConstraints);
      workspace.UpdateConstraints(hwnd, observedConstraints);
      if (enforcedChanged) {
        CancelConformanceLease(hwnd, lease.generation);
        Diagnostics::RecordEvent(
            L"conformance repair updated constraints hwnd=%p min=%ldx%ld enforcedMin=%d%d",
            hwnd, observedConstraints.minWidth, observedConstraints.minHeight,
            observedConstraints.enforceMinWidth ? 1 : 0,
            observedConstraints.enforceMinHeight ? 1 : 0);
        g_workspaces.Save(owners.front(), workspace);
        ArrangeWorkspace(owners.front());
        return true;
      }
    }
  }

  ++Diagnostics::g_runtime.counters.conformanceLeaseRepairs;
  Diagnostics::RecordEvent(
      L"conformance lease reinforced hwnd=%p attempt=%u result=%d",
      hwnd, attemptNumber, static_cast<int>(observation.result));

  if (action == Workspace::PlacementAction::Forget) {
    CancelConformanceLease(hwnd, lease.generation);
    workspace.Forget(hwnd);
    g_workspaces.Save(owners.front(), workspace);
    ArrangeWorkspace(owners.front());
    return true;
  }
  if (action == Workspace::PlacementAction::Float) {
    CancelConformanceLease(hwnd, lease.generation);
    workspace.Float(hwnd);
    g_workspaces.Save(owners.front(), workspace);
    ArrangeWorkspace(owners.front());
    return true;
  }

  g_workspaces.Save(owners.front(), workspace);
  if (observation.result != PlacementResult::Success && leaseRetained) {
    // Adjusted/refused placements are exactly the cases where relying on another
    // LOCATIONCHANGE is unsafe: the application may already be sitting at its
    // final self-selected rectangle. Keep the lease alive and schedule one more
    // checked attempt at the configured cadence. The hard deadline performs the
    // passive final verdict and Floats only if the HWND is still materially off
    // its current tile. AccessDenied remains an immediate terminal result above.
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG due = std::min(
        lease.expiresAtTickMs,
        now + g_settings.conformanceRepairIntervalMs);
    DeferConformanceLeaseRepair(hwnd, lease.generation, due);
  }
  return true;
}

static void ProcessConformanceTimer() {
  AssertWmThread(L"ProcessConformanceTimer");
  if (g_wm.conformanceTimer) {
    KillTimer(nullptr, g_wm.conformanceTimer);
    g_wm.conformanceTimer = 0;
  }

  struct ExpiredCandidate {
    HWND hwnd = nullptr;
    uint64_t generation = 0;
  };

  const ULONGLONG now = GetTickCount64();
  std::vector<ExpiredCandidate> expired;
  std::vector<HWND> dueWindows;
  for (auto& kv : g_conformanceLeases.leases) {
    ConformanceLease& lease = kv.second;
    if (now >= lease.expiresAtTickMs) {
      expired.push_back({kv.first, lease.generation});
      continue;
    }
    if (lease.repairDueTickMs && lease.repairDueTickMs <= now) {
      dueWindows.push_back(kv.first);
      lease.repairDueTickMs = 0;
    }
  }
  for (const ExpiredCandidate& candidate : expired) {
    ConformanceLease lease;
    if (TakeExpiredConformanceLease(
            candidate.hwnd, candidate.generation, &lease)) {
      FinalizeExpiredConformanceLease(candidate.hwnd, lease);
    }
  }
  for (HWND hwnd : dueWindows) {
    HandleTiledWindowLocationChange(hwnd);
  }
  ScheduleNextConformanceTimer();
}

// Uses aggregate work-area coverage as a coarse guard against learning arbitrary
// floating geometry as an authoritative tiled layout.
static bool SnapshotLooksTiled(const std::vector<HWND>& windows, const RECT& workArea) {
  if (windows.empty()) return false;
  const long long workAreaArea = RectAreaLL(workArea);
  long long sumArea = 0;
  for (HWND w : windows) sumArea += WindowAreaOnWorkArea(w, workArea);
  const long long lo = (workAreaArea * 85) / 100;
  const long long hi = (windows.size() <= 2) ? (workAreaArea * 115) / 100 : (workAreaArea * 105) / 100;
  return sumArea >= lo && sumArea <= hi;
}

