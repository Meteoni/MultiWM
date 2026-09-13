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
    dwindleRatios_.clear();
  } else if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    if (gridWeights_.size() != windows_.size()) gridWeights_ = DefaultWeights(windows_.size());
    stackWeights_.clear();
    dwindleRatios_.clear();
  } else if (layout_ == TileLayout::Dwindle) {
    // Keep one outgoing ratio per active slot, including a dormant ratio on the
    // final slot. The dormant value is what lets the predecessor's divider survive
    // when its successor is suspended, then become active again on restoration.
    dwindleRatios_.resize(windows_.size(), 0.5);
    stackWeights_.clear();
    gridWeights_.clear();
  } else {
    stackWeights_.clear();
    gridWeights_.clear();
    dwindleRatios_.clear();
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
  } else if (layout_ == TileLayout::Dwindle && !dwindleRatios_.empty()) {
    // Save the suspended logical slot's outgoing ratio. Because EnsureWeights
    // retains the final dormant ratio, removing the last active member does not
    // accidentally consume the divider owned by its predecessor.
    const size_t ratioIndex = index;
    slot.weight = dwindleRatios_[ratioIndex];
    slot.hasWeight = true;
    dwindleRatios_.erase(dwindleRatios_.begin() + ratioIndex);
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

static bool AdmitInitialObservedWindow(
    Workspace& workspace, HWND hwnd, bool useAutomaticAdmissionPolicy) {
  if (!hwnd || !IsWindow(hwnd) || workspace.HasRecord(hwnd) ||
      IsWindowTrackedInAnyState(hwnd)) {
    return false;
  }
  const SuspensionReason reason = GetPhysicalSuspensionReason(hwnd);
  if (reason == SuspensionReason::Hidden) return false;
  if (useAutomaticAdmissionPolicy && ShouldWindowDefaultToFloating(hwnd)) {
    WindowRecord record = MakeWindowRecord(hwnd, ManageState::Floating);
    record.pendingDefaultFloatingCenter = true;
    return workspace.AdmitFloating(std::move(record));
  }
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
  record.pendingInsertionPlacement = true;
  if (reason == SuspensionReason::None) {
    AppendTiledWithDefaultWeight(hwnd);
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
    } else if (layout_ == TileLayout::Dwindle) {
      record.savedSlot.weight = 0.5;
      record.savedSlot.hasWeight = true;
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
  record.pendingInsertionPlacement = true;
  record.suspensionReason = reason;
  record.savedSlot.index = insertionLogicalIndex;
  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    record.savedSlot.weight = 1.0;
    record.savedSlot.hasWeight = true;
  } else if (layout_ == TileLayout::MasterStack ||
             layout_ == TileLayout::MasterStackH) {
    record.savedSlot.weight = 1.0;
    record.savedSlot.hasWeight = true;
  } else if (layout_ == TileLayout::Dwindle) {
    record.savedSlot.weight = 0.5;
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
  } else if (layout_ == TileLayout::Dwindle) {
    // Reinsert the logical slot's outgoing ratio before EnsureWeights. The vector
    // deliberately includes the final dormant ratio, so a one-window intermediate
    // restore cannot discard split state needed by a later restored successor.
    const size_t ratioIndex = std::min(index, dwindleRatios_.size());
    dwindleRatios_.insert(
        dwindleRatios_.begin() + ratioIndex,
        slot.hasWeight ? ClampDouble(slot.weight, 0.1, 0.9) : 0.5);
  }

  record->state = ManageState::Tiled;
  // Preserve the saved slot as the first restore attempt, but keep the restored
  // member movable until that placement has survived conformance. If its old
  // slot became infeasible while suspended, ArrangeWorkspace can now apply the
  // same nearest-slot / Dwindle recovery used for a fresh admission instead of
  // immediately floating it.
  record->pendingInsertionPlacement = layout_ != TileLayout::Floating;
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
  record->pendingInsertionPlacement = false;
  record->pendingDefaultFloatingCenter = false;
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
  } else if (layout_ == TileLayout::Dwindle) {
    dwindleRatios_.push_back(0.5);
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
  } else if (layout_ == TileLayout::Dwindle) {
    slot.weight = 0.5;
    slot.hasWeight = true;
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
    // A destination workspace admission is newer than its existing members and
    // receives the first opportunity to find a viable destination slot.
    record.tilingGeneration = Model::NextTilingGeneration();
    record.pendingInsertionPlacement = true;
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

bool Workspace::SetDwindleRatio(size_t splitIndex, double ratio) {
  AssertWmThread(L"Workspace::SetDwindleRatio");
  EnsureWeights();
  if (layout_ != TileLayout::Dwindle ||
      dwindleRatios_.size() < 2 || splitIndex >= dwindleRatios_.size() - 1) {
    return false;
  }

  const double clamped = ClampDouble(ratio, 0.1, 0.9);
  if (std::abs(dwindleRatios_[splitIndex] - clamped) <= 1e-9) return false;
  dwindleRatios_[splitIndex] = clamped;
  DebugValidateMutation(L"Workspace::SetDwindleRatio");
  return true;
}

bool Workspace::HasSameWindowMembershipAs(const Workspace& other) const {
  if (windows_.size() != other.windows_.size() ||
      records_.size() != other.records_.size()) {
    return false;
  }
  for (const auto& [hwnd, record] : records_) {
    auto otherIt = other.records_.find(hwnd);
    if (otherIt == other.records_.end() ||
        record.pid != otherIt->second.pid ||
        record.tilingGeneration != otherIt->second.tilingGeneration ||
        record.state != otherIt->second.state ||
        record.suspensionReason != otherIt->second.suspensionReason ||
        record.hasSavedSlot != otherIt->second.hasSavedSlot) {
      return false;
    }
  }
  return true;
}

bool Workspace::HasSameLayoutStateAs(const Workspace& other) const {
  if (!HasSameWindowMembershipAs(other)) return false;
  for (const auto& [hwnd, record] : records_) {
    const auto& saved = other.records_.at(hwnd).savedSlot;
    if (record.hasSavedSlot &&
        (record.savedSlot.index != saved.index ||
         record.savedSlot.weight != saved.weight ||
         record.savedSlot.hasWeight != saved.hasWeight ||
         record.savedSlot.wasMaster != saved.wasMaster)) return false;
  }
  return windows_ == other.windows_ &&
      layout_ == other.layout_ && masterRatio_ == other.masterRatio_ &&
      stackWeights_ == other.stackWeights_ &&
      gridWeights_ == other.gridWeights_ &&
      dwindleRatios_ == other.dwindleRatios_;
}

bool Workspace::RestoreLayoutStateFrom(const Workspace& source) {
  AssertWmThread(L"Workspace::RestoreLayoutStateFrom");
  if (!HasSameWindowMembershipAs(source)) return false;

  windows_ = source.windows_;
  layout_ = source.layout_;
  masterRatio_ = source.masterRatio_;
  stackWeights_ = source.stackWeights_;
  gridWeights_ = source.gridWeights_;
  dwindleRatios_ = source.dwindleRatios_;
  for (auto& [hwnd, record] : records_) {
    const WindowRecord& sourceRecord = source.records_.find(hwnd)->second;
    if (record.state == ManageState::Suspended) {
      record.savedSlot = sourceRecord.savedSlot;
    }
  }
  DebugValidateMutation(L"Workspace::RestoreLayoutStateFrom");
  return true;
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

bool Workspace::MovePendingAdmissionToIndex(HWND hwnd, size_t targetIndex) {
  AssertWmThread(L"Workspace::MovePendingAdmissionToIndex");
  auto current = std::find(windows_.begin(), windows_.end(), hwnd);
  if (current == windows_.end()) return false;

  WindowRecord* record = FindMutable(hwnd);
  const size_t currentIndex =
      static_cast<size_t>(std::distance(windows_.begin(), current));
  if (!record || !record->pendingInsertionPlacement ||
      targetIndex >= windows_.size() || targetIndex == currentIndex) {
    return false;
  }

  EnsureWeights();
  const size_t oldCount = windows_.size();
  const size_t oldLogicalIndex = ActiveIndexToLogicalIndex(currentIndex);
  windows_.erase(current);

  if ((layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) &&
      gridWeights_.size() == oldCount) {
    gridWeights_.erase(gridWeights_.begin() + currentIndex);
  } else if ((layout_ == TileLayout::MasterStack ||
              layout_ == TileLayout::MasterStackH) &&
             oldCount > 1 && stackWeights_.size() == oldCount - 1) {
    const size_t stackIndex = currentIndex == 0 ? 0 : currentIndex - 1;
    stackWeights_.erase(stackWeights_.begin() + stackIndex);
  }
  CloseLogicalGapAfterRemoval(oldLogicalIndex, hwnd);

  const size_t insertionLogicalIndex = ActiveIndexToLogicalIndex(targetIndex);
  for (auto& kv : records_) {
    WindowRecord& existing = kv.second;
    if (existing.state == ManageState::Suspended && existing.hasSavedSlot &&
        existing.savedSlot.index >= insertionLogicalIndex) {
      ++existing.savedSlot.index;
    }
  }

  windows_.insert(windows_.begin() + targetIndex, hwnd);
  if (layout_ == TileLayout::Columns || layout_ == TileLayout::Rows) {
    gridWeights_.insert(gridWeights_.begin() + targetIndex, 1.0);
  } else if ((layout_ == TileLayout::MasterStack ||
              layout_ == TileLayout::MasterStackH) &&
             oldCount > 1) {
    const size_t stackIndex = targetIndex == 0 ? 0 : targetIndex - 1;
    stackWeights_.insert(stackWeights_.begin() + stackIndex, 1.0);
  }

  EnsureWeights();
  DebugValidateMutation(L"Workspace::MovePendingAdmissionToIndex");
  return true;
}

bool Workspace::ClearInsertionPlacement(HWND hwnd) {
  AssertWmThread(L"Workspace::ClearInsertionPlacement");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || !record->pendingInsertionPlacement) return false;
  record->pendingInsertionPlacement = false;
  DebugValidateMutation(L"Workspace::ClearInsertionPlacement");
  return true;
}

bool Workspace::ClearDefaultFloatingCenter(HWND hwnd) {
  AssertWmThread(L"Workspace::ClearDefaultFloatingCenter");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || !record->pendingDefaultFloatingCenter) return false;
  record->pendingDefaultFloatingCenter = false;
  DebugValidateMutation(L"Workspace::ClearDefaultFloatingCenter");
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

bool Workspace::LearnDwindleResize(
    const RECT& workArea, LONG gap, size_t resizedIndex,
    const MoveSizeGesture& gesture) {
  AssertWmThread(L"Workspace::LearnDwindleResize");
  EnsureWeights();
  if (layout_ != TileLayout::Dwindle || windows_.size() < 2 ||
      resizedIndex >= windows_.size()) {
    return false;
  }

  std::vector<RECT> expectedRects(windows_.size());
  std::vector<Layout::DwindleSplit> splits;
  Layout::LayoutDwindle(
      workArea, gap, 0, windows_.size(), dwindleRatios_,
      g_settings.reverseDwindle, expectedRects, &splits);
  if (splits.size() + 1 != dwindleRatios_.size()) return false;

  const RECT& resizedRect = expectedRects[resizedIndex];
  size_t selectedVerticalSplit = static_cast<size_t>(-1);
  size_t selectedHorizontalSplit = static_cast<size_t>(-1);
  LONG selectedVerticalDelta = 0;
  LONG selectedHorizontalDelta = 0;
  for (size_t splitIndex = 0;
       splitIndex < splits.size() && splitIndex <= resizedIndex;
       ++splitIndex) {
    const Layout::DwindleSplit& split = splits[splitIndex];
    LONG firstSpanDelta = 0;
    if (split.vertical) {
      if (resizedIndex == splitIndex) {
        firstSpanDelta = g_settings.reverseDwindle
            ? -(gesture.end.left - gesture.start.left)
            : gesture.end.right - gesture.start.right;
      } else if ((!g_settings.reverseDwindle &&
                  resizedRect.left == split.remaining.left) ||
                 (g_settings.reverseDwindle &&
                  resizedRect.right == split.remaining.right)) {
        firstSpanDelta = g_settings.reverseDwindle
            ? -(gesture.end.right - gesture.start.right)
            : gesture.end.left - gesture.start.left;
      }
    } else {
      if (resizedIndex == splitIndex) {
        firstSpanDelta = g_settings.reverseDwindle
            ? -(gesture.end.top - gesture.start.top)
            : gesture.end.bottom - gesture.start.bottom;
      } else if ((!g_settings.reverseDwindle &&
                  resizedRect.top == split.remaining.top) ||
                 (g_settings.reverseDwindle &&
                  resizedRect.bottom == split.remaining.bottom)) {
        firstSpanDelta = g_settings.reverseDwindle
            ? -(gesture.end.bottom - gesture.start.bottom)
            : gesture.end.top - gesture.start.top;
      }
    }
    if (!IsMeaningfulResizeDelta(firstSpanDelta) || split.usableSpan < 2) {
      continue;
    }

    // A descendant can share an edge with more than one same-axis ancestor.
    // Keep the nearest match on each axis. A corner drag may intentionally
    // update one horizontal and one vertical divider.
    if (split.vertical) {
      selectedVerticalSplit = splitIndex;
      selectedVerticalDelta = firstSpanDelta;
    } else {
      selectedHorizontalSplit = splitIndex;
      selectedHorizontalDelta = firstSpanDelta;
    }
  }

  bool changed = false;
  auto applySelectedDivider = [&](size_t splitIndex, LONG firstSpanDelta) {
    if (splitIndex >= splits.size()) return;
    const Layout::DwindleSplit& split = splits[splitIndex];
    const LONG minimumSpan = std::min<LONG>(
        kMinLearnedResizeSpan,
        std::max<LONG>(1, (split.usableSpan - 1) / 2));
    const LONG newFirstSpan = std::clamp<LONG>(
        split.firstSpan + firstSpanDelta,
        minimumSpan, split.usableSpan - minimumSpan);
    const double newRatio = ClampDouble(
        static_cast<double>(newFirstSpan) /
            static_cast<double>(split.usableSpan),
        0.1, 0.9);
    if (std::abs(newRatio - dwindleRatios_[splitIndex]) <= 1e-9) return;

    dwindleRatios_[splitIndex] = newRatio;
    changed = true;
    Wh_Log(
        L"Dwindle divider updated slot=%zu ratio=%.6f",
        splitIndex, newRatio);
    Diagnostics::RecordEvent(
        L"dwindle divider updated hwnd=%p slot=%zu ratio=%.6f",
        windows_[resizedIndex], splitIndex, newRatio);
  };
  applySelectedDivider(selectedVerticalSplit, selectedVerticalDelta);
  applySelectedDivider(selectedHorizontalSplit, selectedHorizontalDelta);
  if (!changed) return false;

  DebugValidateMutation(L"Workspace::LearnDwindleResize");
  return true;
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

    case TileLayout::Dwindle:
      Layout::LayoutDwindle(
          workArea, gap, 0, workspace.ActiveCount(),
          workspace.DwindleRatios(), g_settings.reverseDwindle, *outRects);
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

    case TileLayout::Dwindle:
      // Dwindle ratios are tree state. Constraint handling may reassign a fresh
      // admission or fit the shared shallow-divider policy, but ordinary planning
      // never reshapes the tree around its current occupants.
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

// Exchange eligibility remains separate from the ratio solver. An exchange may
// use automatic Dwindle fitting only when it is exactly one two-window swap and
// both changed slots lie inside the configured adaptive prefix.
static bool IsAdaptiveDwindleExchange(
    const Workspace& previous, const Workspace& candidate) {
  const auto& before = previous.TiledWindows();
  const auto& after = candidate.TiledWindows();
  if (previous.Layout() != TileLayout::Dwindle ||
      candidate.Layout() != TileLayout::Dwindle ||
      before.size() < 2 || before.size() != after.size()) {
    return false;
  }

  const size_t adaptiveSlotCount =
      std::min(g_settings.adaptiveDwindleSlots, before.size());
  if (adaptiveSlotCount < 2) return false;

  size_t firstChanged = static_cast<size_t>(-1);
  size_t secondChanged = static_cast<size_t>(-1);
  for (size_t i = 0; i < before.size(); ++i) {
    if (before[i] == after[i]) continue;
    if (i >= adaptiveSlotCount) return false;
    if (firstChanged == static_cast<size_t>(-1)) {
      firstChanged = i;
    } else if (secondChanged == static_cast<size_t>(-1)) {
      secondChanged = i;
    } else {
      return false;
    }
  }

  return firstChanged != static_cast<size_t>(-1) &&
      secondChanged != static_cast<size_t>(-1) &&
      before[firstChanged] == after[secondChanged] &&
      before[secondChanged] == after[firstChanged];
}

// Admission and eligible exchanges share one feasibility calculation. Leading
// split nodes may move according to Adaptive Dwindle Slots; every deeper ratio
// remains fixed. A value of zero deliberately disables automatic ratio changes.
static bool FitAdaptiveDwindleDividers(
    Workspace& candidate, const RECT& workArea, LONG gap) {
  if (candidate.Layout() != TileLayout::Dwindle || candidate.ActiveCount() < 2 ||
      g_settings.adaptiveDwindleSlots == 0) {
    return false;
  }

  const auto& windows = candidate.TiledWindows();
  const auto constraints = ActiveWindowConstraints(candidate);
  const auto& ratios = candidate.DwindleRatios();
  const size_t adaptiveSplitCount =
      std::min(g_settings.adaptiveDwindleSlots, ratios.size());
  if (adaptiveSplitCount == 0) return false;

  std::vector<SIZE> windowMinimums(windows.size(), SIZE{1, 1});
  for (size_t i = 0; i < constraints.size(); ++i) {
    const auto& constraint = constraints[i];
    if (constraint.valid && constraint.enforceMinWidth) {
      windowMinimums[i].cx = std::max<LONG>(1, constraint.minWidth);
    }
    if (constraint.valid && constraint.enforceMinHeight) {
      windowMinimums[i].cy = std::max<LONG>(1, constraint.minHeight);
    }
  }

  const LONG maximumSpan = std::max(RectWidth(workArea), RectHeight(workArea));
  auto requiredSpan = [&](LONG first, LONG tail, size_t splitIndex,
                          bool adaptive) {
    const double fixedRatio = ClampDouble(ratios[splitIndex], 0.1, 0.9);
    auto fits = [&](LONG span) {
      if (span < 2) return false;
      const LONG usable = span - std::clamp<LONG>(gap, 0, span - 2);
      if (usable < 2) return false;

      if (!adaptive) {
        const LONG allocated = std::clamp<LONG>(
            static_cast<LONG>(std::llround(usable * fixedRatio)),
            1, usable - 1);
        return allocated >= first && usable - allocated >= tail;
      }

      const LONG minimumAdaptive = std::clamp<LONG>(
          static_cast<LONG>(std::llround(usable * 0.1)), 1, usable - 1);
      const LONG maximumAdaptive = std::clamp<LONG>(
          static_cast<LONG>(std::llround(usable * 0.9)), 1, usable - 1);
      const LONG lower = std::max(first, minimumAdaptive);
      const LONG upper = std::min(usable - tail, maximumAdaptive);
      return lower <= upper;
    };

    if (!fits(maximumSpan)) return maximumSpan + 1;
    LONG low = 2;
    LONG high = maximumSpan;
    while (low < high) {
      const LONG middle = low + (high - low) / 2;
      if (fits(middle)) {
        high = middle;
      } else {
        low = middle + 1;
      }
    }
    return low;
  };

  // Minimum extent of each recursive tail. Fixed descendants preserve their
  // current ratios; adaptive descendants contribute only the space they need
  // somewhere inside the supported 10%-90% ratio range.
  std::vector<SIZE> subtreeMinimums = windowMinimums;
  for (size_t splitIndex = windows.size() - 1; splitIndex-- > 0;) {
    const SIZE first = windowMinimums[splitIndex];
    const SIZE tail = subtreeMinimums[splitIndex + 1];
    SIZE combined{
        std::max(first.cx, tail.cx),
        std::max(first.cy, tail.cy)};
    const bool adaptive = splitIndex < adaptiveSplitCount;
    if ((splitIndex % 2) == 0) {
      combined.cx = requiredSpan(first.cx, tail.cx, splitIndex, adaptive);
    } else {
      combined.cy = requiredSpan(first.cy, tail.cy, splitIndex, adaptive);
    }
    subtreeMinimums[splitIndex] = combined;
  }

  if (RectWidth(workArea) < subtreeMinimums[0].cx ||
      RectHeight(workArea) < subtreeMinimums[0].cy) {
    return false;
  }

  Workspace trial = candidate;
  RECT remaining = workArea;
  for (size_t splitIndex = 0; splitIndex < adaptiveSplitCount; ++splitIndex) {
    const bool vertical = (splitIndex % 2) == 0;
    const LONG span = vertical ? RectWidth(remaining) : RectHeight(remaining);
    if (span < 2) return false;
    const LONG usable = span - std::clamp<LONG>(gap, 0, span - 2);
    if (usable < 2) return false;

    const LONG firstMinimum = vertical
        ? windowMinimums[splitIndex].cx
        : windowMinimums[splitIndex].cy;
    const LONG tailMinimum = vertical
        ? subtreeMinimums[splitIndex + 1].cx
        : subtreeMinimums[splitIndex + 1].cy;
    const LONG minimumAdaptive = std::clamp<LONG>(
        static_cast<LONG>(std::llround(usable * 0.1)), 1, usable - 1);
    const LONG maximumAdaptive = std::clamp<LONG>(
        static_cast<LONG>(std::llround(usable * 0.9)), 1, usable - 1);
    const LONG lower = std::max(firstMinimum, minimumAdaptive);
    const LONG upper = std::min(usable - tailMinimum, maximumAdaptive);
    if (lower > upper) return false;

    const double preferred = ClampDouble(ratios[splitIndex], 0.1, 0.9);
    const LONG desired = static_cast<LONG>(std::llround(usable * preferred));
    const LONG firstSpan = std::clamp<LONG>(desired, lower, upper);
    trial.SetDwindleRatio(
        splitIndex, static_cast<double>(firstSpan) / usable);

    std::vector<RECT> rects(windows.size());
    std::vector<Layout::DwindleSplit> splits;
    Layout::LayoutDwindle(
        workArea, gap, 0, windows.size(), trial.DwindleRatios(),
        g_settings.reverseDwindle, rects, &splits);
    if (splitIndex >= splits.size()) return false;
    remaining = splits[splitIndex].remaining;
  }

  std::vector<RECT> rects;
  if (BuildWorkspaceLayoutPlan(
          trial, workArea, gap, &rects) != LayoutPlanStatus::Success) {
    return false;
  }
  candidate = std::move(trial);
  return true;
}

static void RememberPendingLayoutMutation(
    const DesktopMonitorKey& key, const Workspace& previous,
    const Workspace& candidate, HWND subject, UserLayoutMutationKind kind,
    const wchar_t* action) {
  PendingLayoutMutation mutation;
  mutation.previous = previous;
  mutation.candidate = candidate;
  mutation.subject = subject;
  mutation.kind = kind;
  mutation.action = action ? action : L"Layout mutation";
  g_pendingLayoutMutations[key] = std::move(mutation);
}

static HWND FindConstraintCulprit(
    const Workspace& workspace, const std::vector<RECT>& rects,
    HWND preferred = nullptr) {
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(workspace);
  auto violatesAssignedRect = [&](size_t index) {
    if (index >= constraints.size() || index >= rects.size()) return false;
    const WindowConstraints& constraint = constraints[index];
    return constraint.valid &&
        ((constraint.enforceMinWidth &&
          RectWidth(rects[index]) < std::max<LONG>(1, constraint.minWidth)) ||
         (constraint.enforceMinHeight &&
          RectHeight(rects[index]) < std::max<LONG>(1, constraint.minHeight)));
  };

  const size_t preferredIndex = workspace.TiledIndex(preferred);
  if (violatesAssignedRect(preferredIndex)) return preferred;
  for (size_t i = 0; i < constraints.size(); ++i) {
    if (violatesAssignedRect(i)) return workspace.TiledWindows()[i];
  }
  return nullptr;
}

// Shared constraint compatibility surface. Admission recovery and conflict
// reporting deliberately use the same fit predicate and augmenting-path primitive
// so diagnostics cannot disagree with the solver about whether a slot is usable.
static bool WindowConstraintFitsSlot(
    const WindowConstraints& constraint, const RECT& slot) {
  return !constraint.valid ||
      ((!constraint.enforceMinWidth ||
        RectWidth(slot) >= std::max<LONG>(1, constraint.minWidth)) &&
       (!constraint.enforceMinHeight ||
        RectHeight(slot) >= std::max<LONG>(1, constraint.minHeight)));
}

struct ConstraintSlotCompatibility {
  std::vector<std::vector<size_t>> slotsByWindow;
};

static ConstraintSlotCompatibility BuildConstraintSlotCompatibility(
    const std::vector<WindowConstraints>& constraints,
    const std::vector<RECT>& slots, bool preferNearbySlots) {
  ConstraintSlotCompatibility compatibility;
  compatibility.slotsByWindow.resize(constraints.size());
  for (size_t windowIndex = 0; windowIndex < constraints.size(); ++windowIndex) {
    auto& compatibleSlots = compatibility.slotsByWindow[windowIndex];
    for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
      if (WindowConstraintFitsSlot(constraints[windowIndex], slots[slotIndex])) {
        compatibleSlots.push_back(slotIndex);
      }
    }
    if (preferNearbySlots) {
      std::stable_sort(
          compatibleSlots.begin(), compatibleSlots.end(),
          [&](size_t first, size_t second) {
            const size_t firstDistance = first > windowIndex
                ? first - windowIndex
                : windowIndex - first;
            const size_t secondDistance = second > windowIndex
                ? second - windowIndex
                : windowIndex - second;
            return firstDistance != secondDistance
                ? firstDistance < secondDistance
                : first < second;
          });
    }
  }
  return compatibility;
}

static bool TryAssignConstraintWindow(
    size_t candidateWindow,
    const ConstraintSlotCompatibility& compatibility,
    std::vector<size_t>* slotOwner, const std::vector<bool>& fixedWindows,
    std::vector<bool>* visitedSlots, bool preferFreeSlots) {
  if (!slotOwner || !visitedSlots ||
      candidateWindow >= compatibility.slotsByWindow.size()) {
    return false;
  }
  constexpr size_t kUnassigned = static_cast<size_t>(-1);
  const auto& compatibleSlots = compatibility.slotsByWindow[candidateWindow];

  if (preferFreeSlots) {
    for (size_t slotIndex : compatibleSlots) {
      if (slotIndex >= slotOwner->size() || (*visitedSlots)[slotIndex] ||
          (*slotOwner)[slotIndex] != kUnassigned) {
        continue;
      }
      (*visitedSlots)[slotIndex] = true;
      (*slotOwner)[slotIndex] = candidateWindow;
      return true;
    }
  }

  for (size_t slotIndex : compatibleSlots) {
    if (slotIndex >= slotOwner->size() || (*visitedSlots)[slotIndex]) continue;
    (*visitedSlots)[slotIndex] = true;
    const size_t displacedWindow = (*slotOwner)[slotIndex];
    if (displacedWindow == kUnassigned ||
        (displacedWindow < fixedWindows.size() &&
         !fixedWindows[displacedWindow] &&
         TryAssignConstraintWindow(
             displacedWindow, compatibility, slotOwner, fixedWindows,
             visitedSlots, preferFreeSlots))) {
      (*slotOwner)[slotIndex] = candidateWindow;
      return true;
    }
  }
  return false;
}

static void ShowWindowConstraintConflict(
    const Workspace& workspace, const std::vector<RECT>& rects,
    HWND fallback, HMONITOR monitor) {
  const auto constraints = ActiveWindowConstraints(workspace);
  const ConstraintSlotCompatibility compatibility =
      BuildConstraintSlotCompatibility(constraints, rects, false);
  std::vector<bool> involved(constraints.size(), false);
  for (size_t i = 0; i < constraints.size(); ++i) {
    involved[i] = i < rects.size() &&
        !WindowConstraintFitsSlot(constraints[i], rects[i]);
  }
  // A failed matching identifies groups competing for too few slots. Follow
  // alternating paths from unmatched windows to include the other blockers,
  // even when they currently fit (e.g. Pixeval already occupies the only slot).
  if (workspace.Layout() == TileLayout::Dwindle &&
      rects.size() == constraints.size()) {
    constexpr size_t kUnassigned = static_cast<size_t>(-1);
    std::vector<size_t> owner(rects.size(), kUnassigned);
    std::vector<bool> fixed(constraints.size(), false);
    std::vector<size_t> unmatched;
    for (size_t window = 0; window < constraints.size(); ++window) {
      std::vector<bool> visited(rects.size(), false);
      if (!TryAssignConstraintWindow(
              window, compatibility, &owner, fixed, &visited, false)) {
        unmatched.push_back(window);
      }
    }
    if (!unmatched.empty()) {
      involved.assign(constraints.size(), false);
      auto include = [&](auto&& self, size_t member) -> void {
        if (member >= involved.size() || involved[member]) return;
        involved[member] = true;
        for (size_t slot : compatibility.slotsByWindow[member]) {
          if (slot < owner.size() && owner[slot] != kUnassigned) {
            self(self, owner[slot]);
          }
        }
      };
      for (size_t window : unmatched) include(include, window);
    }
  }
  std::vector<std::wstring> names;
  for (size_t i = 0; i < involved.size(); ++i) {
    if (!involved[i]) continue;
    std::wstring name = GetWindowProcessName(workspace.TiledWindows()[i]);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
      name.resize(name.size() - 4);
    if (name.empty()) name = L"A window";
    if (std::find(names.begin(), names.end(), name) == names.end())
      names.push_back(std::move(name));
  }
  if (names.empty() && fallback) {
    std::wstring name = GetWindowProcessName(fallback);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0)
      name.resize(name.size() - 4);
    if (!name.empty()) names.push_back(std::move(name));
  }
  std::wstring message;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i) message += i + 1 == names.size() ? L" and " : L", ";
    message += names[i];
  }
  if (names.empty()) message = L"A window";
  message += names.size() > 1
      ? L" need more space in this layout" : L" needs more space";
  TrayUi::ShowCommandFailureFlyout(message.c_str(), monitor);
}

static void ReportRejectedLayoutMutation(
    const DesktopMonitorKey& key, HMONITOR monitor,
    UserLayoutMutationKind kind, const wchar_t* action, HWND subject,
    const Workspace& rejectedWorkspace, const std::vector<RECT>& rejectedRects,
    HWND culpritHint = nullptr) {
  switch (kind) {
    case UserLayoutMutationKind::Resize:
      ++Diagnostics::g_runtime.counters.rejectedResizeGestures;
      break;
    case UserLayoutMutationKind::Order:
      ++Diagnostics::g_runtime.counters.rejectedOrderCommands;
      break;
    case UserLayoutMutationKind::LayoutChange:
      ++Diagnostics::g_runtime.counters.rejectedLayoutChanges;
      break;
  }

  HWND culprit = FindConstraintCulprit(
      rejectedWorkspace, rejectedRects, subject);
  if (!culprit) culprit = culpritHint;
  ShowWindowConstraintConflict(
      rejectedWorkspace, rejectedRects, culprit, monitor);
  Wh_Log(
      L"%ls rejected by layout constraints subject=%p culprit=%p",
      action, subject, culprit);
  Diagnostics::RecordEvent(
      L"layout mutation rejected action=%ls subject=%p culprit=%p desktop=%08X",
      action, subject, culprit, key.desktopId.Data1);
}

static bool TryRollbackPendingLayoutMutation(
    const DesktopMonitorKey& key, HMONITOR monitor, Workspace* workspace,
    const std::vector<RECT>& rejectedRects, HWND culpritHint = nullptr) {
  if (!workspace) return false;
  auto mutationIt = g_pendingLayoutMutations.find(key);
  if (mutationIt == g_pendingLayoutMutations.end()) return false;
  // Constraint learning and admission recovery are allowed to adjust ratios or
  // reorder pending admissions downstream of the user's command. Those are part
  // of the same delayed transaction, not evidence that the transaction is stale.
  // Only a membership/participation change makes the saved rollback snapshot
  // unsafe to apply.
  if (!workspace->HasSameWindowMembershipAs(mutationIt->second.candidate)) {
    Diagnostics::RecordEvent(
        L"discarded stale layout mutation rollback after membership change action=%ls desktop=%08X",
        mutationIt->second.action, key.desktopId.Data1);
    g_pendingLayoutMutations.erase(mutationIt);
    return false;
  }

  PendingLayoutMutation mutation = mutationIt->second;
  Workspace restored = *workspace;
  if (!restored.RestoreLayoutStateFrom(mutation.previous)) {
    g_pendingLayoutMutations.erase(mutationIt);
    return false;
  }

  WorkspaceMetrics rollbackMetrics;
  if (!GetWorkspaceMetrics(monitor, &rollbackMetrics)) {
    g_pendingLayoutMutations.erase(mutationIt);
    return false;
  }
  std::vector<RECT> restoredRects;
  ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
  const LayoutPlanStatus restoredStatus = BuildWorkspaceLayoutPlan(
      restored, rollbackMetrics.workArea, rollbackMetrics.gap, &restoredRects);
  if (restoredStatus != LayoutPlanStatus::Success &&
      restoredStatus != LayoutPlanStatus::NotApplicable) {
    Diagnostics::RecordEvent(
        L"layout mutation rollback no longer feasible action=%ls desktop=%08X",
        mutation.action, key.desktopId.Data1);
    g_pendingLayoutMutations.erase(mutationIt);
    return false;
  }

  for (HWND hwnd : workspace->TiledWindows()) CancelConformanceLease(hwnd);
  if (restored.Layout() == TileLayout::Floating) {
    RestoreWorkspaceFloatingGeometry(key, restored);
  }
  ReportRejectedLayoutMutation(
      key, monitor, mutation.kind, mutation.action, mutation.subject,
      *workspace, rejectedRects, culpritHint);
  g_workspaces.Save(key, restored);
  *workspace = std::move(restored);
  g_pendingLayoutMutations.erase(mutationIt);
  TrayUi::UpdateIcon(workspace->Layout(), monitor);
  return true;
}

static bool HasOutstandingConformanceLease(const Workspace& workspace) {
  for (HWND hwnd : workspace.TiledWindows()) {
    if (ReadConformanceLease(hwnd, nullptr) !=
        ConformanceLeaseReadResult::Missing) {
      return true;
    }
  }
  return false;
}

static void ClearSettledLayoutMutation(
    const DesktopMonitorKey& key, const Workspace& workspace) {
  auto mutationIt = g_pendingLayoutMutations.find(key);
  if (mutationIt == g_pendingLayoutMutations.end()) return;
  if (!workspace.HasSameWindowMembershipAs(mutationIt->second.candidate)) {
    Diagnostics::RecordEvent(
        L"discarded stale layout mutation settle after membership change action=%ls desktop=%08X",
        mutationIt->second.action, key.desktopId.Data1);
    g_pendingLayoutMutations.erase(mutationIt);
    return;
  }
  if (HasOutstandingConformanceLease(workspace)) return;
  Diagnostics::RecordEvent(
      L"layout mutation settled action=%ls desktop=%08X",
      mutationIt->second.action, key.desktopId.Data1);
  g_pendingLayoutMutations.erase(mutationIt);
}


// Dwindle geometry belongs to slots, not applications. During admission only,
// assign every active window to a compatible existing slot before concluding
// that one must float. The new window stays in the candidate slot, and an
// explicit user-mutation subject stays put.
static bool ReassignPendingDwindleAdmissionForConstraints(
    Workspace& workspace, const std::vector<RECT>& slots,
    HWND pendingAdmission, HWND fixedWindow, size_t* outMovedWindowCount) {
  if (workspace.Layout() != TileLayout::Dwindle ||
      slots.size() != workspace.ActiveCount()) {
    return false;
  }

  const WindowRecord* pendingRecord = workspace.Find(pendingAdmission);
  const size_t pendingWindowIndex = workspace.TiledIndex(pendingAdmission);
  if (!pendingRecord || !pendingRecord->pendingInsertionPlacement ||
      pendingWindowIndex >= workspace.ActiveCount()) {
    return false;
  }

  const std::vector<HWND> windows = workspace.TiledWindows();
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(workspace);
  const size_t count = windows.size();
  const ConstraintSlotCompatibility compatibility =
      BuildConstraintSlotCompatibility(constraints, slots, true);
  for (const auto& compatibleSlots : compatibility.slotsByWindow) {
    if (compatibleSlots.empty()) return false;
  }

  constexpr size_t kUnassigned = static_cast<size_t>(-1);
  std::vector<size_t> slotOwner(count, kUnassigned);
  std::vector<bool> fixed(count, false);
  auto pinWindow = [&](size_t windowIndex, size_t slotIndex) {
    if (windowIndex >= count || slotIndex >= count ||
        slotOwner[slotIndex] != kUnassigned) {
      return false;
    }
    const auto& compatibleSlots = compatibility.slotsByWindow[windowIndex];
    if (std::find(
            compatibleSlots.begin(), compatibleSlots.end(), slotIndex) ==
        compatibleSlots.end()) {
      return false;
    }
    slotOwner[slotIndex] = windowIndex;
    fixed[windowIndex] = true;
    return true;
  };

  if (!pinWindow(pendingWindowIndex, pendingWindowIndex)) return false;
  const size_t fixedWindowIndex = workspace.TiledIndex(fixedWindow);
  if (fixedWindow && fixedWindow != pendingAdmission &&
      !pinWindow(fixedWindowIndex, fixedWindowIndex)) {
    return false;
  }

  // Seed every compatible current placement. Starting with empty slots allowed
  // later flexible windows to evict earlier ones and shuffle the whole order.
  for (size_t i = 0; i < count; ++i) {
    const auto& compatibleSlots = compatibility.slotsByWindow[i];
    if (!fixed[i] && slotOwner[i] == kUnassigned &&
        std::find(compatibleSlots.begin(), compatibleSlots.end(), i) !=
            compatibleSlots.end()) {
      slotOwner[i] = i;
    }
  }
  std::vector<size_t> assignmentOrder;
  assignmentOrder.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (!fixed[i]) assignmentOrder.push_back(i);
  }
  std::stable_sort(
      assignmentOrder.begin(), assignmentOrder.end(),
      [&](size_t first, size_t second) {
        const size_t firstChoices = compatibility.slotsByWindow[first].size();
        const size_t secondChoices = compatibility.slotsByWindow[second].size();
        return firstChoices != secondChoices
            ? firstChoices < secondChoices
            : first < second;
      });

  for (size_t windowIndex : assignmentOrder) {
    if (std::find(slotOwner.begin(), slotOwner.end(), windowIndex) != slotOwner.end())
      continue;
    std::vector<bool> visitedSlots(count, false);
    if (!TryAssignConstraintWindow(
            windowIndex, compatibility, &slotOwner, fixed,
            &visitedSlots, true)) {
      return false;
    }
  }

  std::vector<HWND> desiredOrder(count);
  size_t movedWindowCount = 0;
  for (size_t slotIndex = 0; slotIndex < count; ++slotIndex) {
    if (slotOwner[slotIndex] == kUnassigned) return false;
    desiredOrder[slotIndex] = windows[slotOwner[slotIndex]];
    if (desiredOrder[slotIndex] != windows[slotIndex]) ++movedWindowCount;
  }
  for (size_t slotIndex = 0; slotIndex < count; ++slotIndex) {
    const HWND current = workspace.TiledWindows()[slotIndex];
    if (current != desiredOrder[slotIndex] &&
        !workspace.SwapTiled(current, desiredOrder[slotIndex])) {
      return false;
    }
  }
  if (outMovedWindowCount) *outMovedWindowCount = movedWindowCount;
  return movedWindowCount > 0;
}

static bool FloatWindowAfterFailure(
    const DesktopMonitorKey& key, Workspace& workspace, HWND hwnd) {
  if (!workspace.Float(hwnd)) return false;
  FloatingPlacementHint hint;
  hint.intent = FloatingPlacementIntent::FailureCenter;
  if (!RepairFloatingGeometry(key, workspace, hwnd, hint)) {
    Diagnostics::RecordEvent(
        L"failure float geometry reset failed hwnd=%p desktop=%08X",
        hwnd, key.desktopId.Data1);
  }
  return true;
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
// learn a new enforced constraint or select one constraint-removal candidate.
static void ArrangeWorkspace(
    const DesktopMonitorKey& key, HWND preferredTiledWindow) {
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
  auto pendingMutationIt = g_pendingLayoutMutations.find(key);
  if (pendingMutationIt != g_pendingLayoutMutations.end() &&
      !workspace.HasSameWindowMembershipAs(
          pendingMutationIt->second.candidate)) {
    Diagnostics::RecordEvent(
        L"discarded stale layout mutation before arrangement after membership change action=%ls desktop=%08X",
        pendingMutationIt->second.action, key.desktopId.Data1);
    g_pendingLayoutMutations.erase(pendingMutationIt);
  }
  // Constraint reports are intentionally lazy. Until a window actually corrects
  // one of our placements, ordinary MultiWM geometry remains authoritative.
  const size_t maxPasses = workspace.ActiveCount() * 5 + 8;
  bool constraintFailureReported = false;
  for (size_t pass = 0; pass < maxPasses; ++pass) {
    ++Diagnostics::g_runtime.counters.arrangePasses;
    if (workspace.Layout() == TileLayout::Floating || workspace.Empty()) {
      if (workspace.Layout() == TileLayout::Floating) {
        for (HWND hwnd : workspace.TiledWindows()) CancelConformanceLease(hwnd);
      }
      g_workspaces.Save(key, workspace);
      ClearSettledLayoutMutation(key, workspace);
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

    std::vector<RECT> rects;
    ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
    LayoutPlanStatus planStatus =
        BuildWorkspaceLayoutPlan(workspace, workArea, gap, &rects);
    if (planStatus == LayoutPlanStatus::NotApplicable) {
      g_workspaces.Save(key, workspace);
      return;
    }
    if (planStatus == LayoutPlanStatus::Unsatisfiable) {
      ++Diagnostics::g_runtime.counters.constraintUnsatisfiablePlans;
      auto mutation = g_pendingLayoutMutations.find(key);
      if (mutation != g_pendingLayoutMutations.end() &&
          mutation->second.kind == UserLayoutMutationKind::Order &&
          workspace.HasSameLayoutStateAs(mutation->second.candidate) &&
          IsAdaptiveDwindleExchange(mutation->second.previous, workspace) &&
          FitAdaptiveDwindleDividers(workspace, workArea, gap)) {
        mutation->second.candidate = workspace;
        g_workspaces.Save(key, workspace);
        Diagnostics::RecordEvent(L"adaptive dwindle exchange accommodated newly proven constraints");
        continue;
      }
      if (TryRollbackPendingLayoutMutation(
              key, monitor, &workspace, rects)) {
        continue;
      }
      HWND victim = nullptr;
      bool intrinsicallyImpossible = false;
      bool singleRemovalMakesCurrentOrderViable = false;

      // Removing another member cannot help a window whose proven minimum is
      // larger than the entire usable workspace. Float that window directly;
      // reserve candidate testing for aggregate constraint conflicts.
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
        struct ConstraintCandidate {
          HWND hwnd;
          uint64_t generation;
        };

        std::vector<ConstraintCandidate> candidates;
        candidates.reserve(workspace.ActiveCount());
        for (HWND candidate : workspace.TiledWindows()) {
          const WindowRecord* candidateRecord = workspace.Find(candidate);
          candidates.push_back({
              candidate,
              candidateRecord ? candidateRecord->tilingGeneration : 0});
        }
        std::stable_sort(
            candidates.begin(), candidates.end(),
            [&](const ConstraintCandidate& first,
                const ConstraintCandidate& second) {
              const bool firstPreferred = first.hwnd == preferredTiledWindow;
              const bool secondPreferred = second.hwnd == preferredTiledWindow;
              if (firstPreferred != secondPreferred) return !firstPreferred;
              return first.generation > second.generation;
            });

        // A fresh admission's configured position is a preference, not a reason
        // to discard it. Once placement has proven a conflict, preserve it when
        // adaptive Dwindle ratios can make it viable, then try nearby slots before
        // allowing broader sibling reassignment.
        const TileLayout layout = workspace.Layout();
        const bool searchAlternateInsertion =
            layout == TileLayout::MasterStack ||
            layout == TileLayout::MasterStackH ||
            layout == TileLayout::Columns || layout == TileLayout::Rows ||
            layout == TileLayout::Dwindle;
        const size_t preferredTiledIndex =
            workspace.TiledIndex(preferredTiledWindow);
        Workspace viableInsertion;
        HWND repositionedAdmission = nullptr;
        size_t previousInsertionIndex = 0;
        size_t viableInsertionIndex = 0;
        bool matchedDwindleConstraints = false;
        size_t reassignedDwindleWindows = 0;
        struct DwindleRatioAdjustment {
          size_t splitIndex;
          double before;
          double after;
        };
        std::vector<DwindleRatioAdjustment> adjustedDwindleRatios;

        auto rememberDwindleRatioAdjustments = [&](const std::vector<double>& before,
                                                   const Workspace& after) {
          adjustedDwindleRatios.clear();
          const auto& afterRatios = after.DwindleRatios();
          const size_t count = std::min(before.size(), afterRatios.size());
          for (size_t splitIndex = 0; splitIndex < count; ++splitIndex) {
            if (std::abs(before[splitIndex] - afterRatios[splitIndex]) <= 1e-9) {
              continue;
            }
            adjustedDwindleRatios.push_back(
                {splitIndex, before[splitIndex], afterRatios[splitIndex]});
          }
        };

        auto preservePreferredWindow = [&](const Workspace& trial) {
          return preferredTiledIndex >= workspace.ActiveCount() ||
              trial.TiledIndex(preferredTiledWindow) == preferredTiledIndex;
        };

        auto acceptInsertion = [&](Workspace&& trial, HWND hwnd,
                                   size_t currentIndex, size_t targetIndex,
                                   size_t movedWindowCount) {
          viableInsertion = std::move(trial);
          repositionedAdmission = hwnd;
          previousInsertionIndex = currentIndex;
          viableInsertionIndex = targetIndex;
          matchedDwindleConstraints = movedWindowCount > 0;
          reassignedDwindleWindows = movedWindowCount;
        };

        if (searchAlternateInsertion && layout == TileLayout::Dwindle) {
          // Adaptive Dwindle Slots controls which leading split ratios may move,
          // not where the pending window may be placed. Preserve its saved/current
          // slot first; then walk every nearby slot before reassigning siblings.
          for (const ConstraintCandidate& candidate : candidates) {
            const WindowRecord* candidateRecord = workspace.Find(candidate.hwnd);
            if (!candidateRecord ||
                !candidateRecord->pendingInsertionPlacement ||
                candidate.hwnd == preferredTiledWindow) {
              continue;
            }

            const size_t currentIndex = workspace.TiledIndex(candidate.hwnd);
            if (currentIndex >= workspace.ActiveCount()) continue;

            auto tryAdaptiveIndex = [&](size_t targetIndex,
                                        bool tryFixedGeometryFirst) {
              Workspace trial = workspace;
              if (targetIndex != currentIndex &&
                  !trial.MovePendingAdmissionToIndex(
                      candidate.hwnd, targetIndex)) {
                return false;
              }
              if (!preservePreferredWindow(trial)) return false;

              if (tryFixedGeometryFirst) {
                std::vector<RECT> trialRects;
                ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
                if (BuildWorkspaceLayoutPlan(
                        trial, workArea, gap, &trialRects) ==
                    LayoutPlanStatus::Success) {
                  adjustedDwindleRatios.clear();
                  acceptInsertion(
                      std::move(trial), candidate.hwnd, currentIndex,
                      targetIndex, 0);
                  return true;
                }
              }

              if (g_settings.adaptiveDwindleSlots == 0) return false;
              const std::vector<double> beforeRatios = trial.DwindleRatios();
              if (!FitAdaptiveDwindleDividers(trial, workArea, gap)) {
                return false;
              }
              rememberDwindleRatioAdjustments(beforeRatios, trial);
              acceptInsertion(
                  std::move(trial), candidate.hwnd, currentIndex,
                  targetIndex, 0);
              return true;
            };

            // The current assignment is already known to fail at fixed geometry,
            // so test whether changing only eligible ratios can preserve it.
            if (tryAdaptiveIndex(currentIndex, false)) break;

            for (size_t distance = 1;
                 distance < workspace.ActiveCount() && !repositionedAdmission;
                 ++distance) {
              if (currentIndex + distance < workspace.ActiveCount() &&
                  tryAdaptiveIndex(currentIndex + distance, true)) {
                break;
              }
              if (currentIndex >= distance &&
                  tryAdaptiveIndex(currentIndex - distance, true)) {
                break;
              }
            }
            if (repositionedAdmission) break;
          }

          // Only after preserving/moving the pending member fails do we allow
          // matching to move established siblings. Nearby target slots are tried
          // before matching around the current slot to minimize disruption.
          if (!repositionedAdmission) {
            for (const ConstraintCandidate& candidate : candidates) {
              const WindowRecord* candidateRecord = workspace.Find(candidate.hwnd);
              if (!candidateRecord ||
                  !candidateRecord->pendingInsertionPlacement ||
                  candidate.hwnd == preferredTiledWindow) {
                continue;
              }

              const size_t currentIndex = workspace.TiledIndex(candidate.hwnd);
              if (currentIndex >= workspace.ActiveCount()) continue;

              auto tryMatchedIndex = [&](size_t targetIndex) {
                Workspace trial = workspace;
                if (targetIndex != currentIndex &&
                    !trial.MovePendingAdmissionToIndex(
                        candidate.hwnd, targetIndex)) {
                  return false;
                }
                if (!preservePreferredWindow(trial)) return false;

                std::vector<RECT> trialRects;
                ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
                LayoutPlanStatus trialStatus = BuildWorkspaceLayoutPlan(
                    trial, workArea, gap, &trialRects);
                size_t movedWindowCount = 0;
                if (trialStatus != LayoutPlanStatus::Success) {
                  if (!ReassignPendingDwindleAdmissionForConstraints(
                          trial, trialRects, candidate.hwnd,
                          preferredTiledWindow, &movedWindowCount)) {
                    return false;
                  }
                  ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
                  trialStatus = BuildWorkspaceLayoutPlan(
                      trial, workArea, gap, &trialRects);
                  if (trialStatus != LayoutPlanStatus::Success) {
                    if (g_settings.adaptiveDwindleSlots == 0) return false;
                    const std::vector<double> beforeRatios =
                        trial.DwindleRatios();
                    if (!FitAdaptiveDwindleDividers(
                            trial, workArea, gap)) {
                      return false;
                    }
                    rememberDwindleRatioAdjustments(beforeRatios, trial);
                  } else {
                    adjustedDwindleRatios.clear();
                  }
                } else {
                  adjustedDwindleRatios.clear();
                }

                acceptInsertion(
                    std::move(trial), candidate.hwnd, currentIndex,
                    targetIndex, movedWindowCount);
                return true;
              };

              for (size_t distance = 1;
                   distance < workspace.ActiveCount() && !repositionedAdmission;
                   ++distance) {
                if (currentIndex + distance < workspace.ActiveCount() &&
                    tryMatchedIndex(currentIndex + distance)) {
                  break;
                }
                if (currentIndex >= distance &&
                    tryMatchedIndex(currentIndex - distance)) {
                  break;
                }
              }
              if (!repositionedAdmission && tryMatchedIndex(currentIndex)) {
                break;
              }
              if (repositionedAdmission) break;
            }
          }
        } else if (searchAlternateInsertion) {
          // Other layouts have no automatic divider adaptation or matching.
          for (const ConstraintCandidate& candidate : candidates) {
            const WindowRecord* candidateRecord = workspace.Find(candidate.hwnd);
            if (!candidateRecord ||
                !candidateRecord->pendingInsertionPlacement ||
                candidate.hwnd == preferredTiledWindow) {
              continue;
            }

            const size_t currentIndex = workspace.TiledIndex(candidate.hwnd);
            if (currentIndex >= workspace.ActiveCount()) continue;

            auto tryInsertionIndex = [&](size_t targetIndex) {
              Workspace trial = workspace;
              if (!trial.MovePendingAdmissionToIndex(
                      candidate.hwnd, targetIndex) ||
                  !preservePreferredWindow(trial)) {
                return false;
              }
              std::vector<RECT> trialRects;
              ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
              if (BuildWorkspaceLayoutPlan(
                      trial, workArea, gap, &trialRects) !=
                  LayoutPlanStatus::Success) {
                return false;
              }
              adjustedDwindleRatios.clear();
              acceptInsertion(
                  std::move(trial), candidate.hwnd, currentIndex,
                  targetIndex, 0);
              return true;
            };

            for (size_t distance = 1;
                 distance < workspace.ActiveCount() && !repositionedAdmission;
                 ++distance) {
              if (currentIndex + distance < workspace.ActiveCount() &&
                  tryInsertionIndex(currentIndex + distance)) {
                break;
              }
              if (currentIndex >= distance &&
                  tryInsertionIndex(currentIndex - distance)) {
                break;
              }
            }
            if (repositionedAdmission) break;
          }
        }

        if (repositionedAdmission) {
          workspace = std::move(viableInsertion);
          if (matchedDwindleConstraints) {
            Wh_Log(
                L"Dwindle admission reassigned %zu windows across compatible slots for HWND %p",
                reassignedDwindleWindows,
                static_cast<void*>(repositionedAdmission));
            Diagnostics::RecordEvent(
                L"dwindle admission matched all constraints hwnd=%p slot=%zu reassigned=%zu",
                repositionedAdmission, viableInsertionIndex,
                reassignedDwindleWindows);
          }
          if (!adjustedDwindleRatios.empty()) {
            Wh_Log(
                L"Dwindle admission adapted %zu split ratio(s) for HWND %p at slot %zu",
                adjustedDwindleRatios.size(),
                static_cast<void*>(repositionedAdmission),
                viableInsertionIndex);
            Diagnostics::RecordEvent(
                L"dwindle admission adapted ratios hwnd=%p slot=%zu changes=%zu",
                repositionedAdmission, viableInsertionIndex,
                adjustedDwindleRatios.size());
            for (const DwindleRatioAdjustment& adjustment :
                 adjustedDwindleRatios) {
              Wh_Log(
                  L"Dwindle adaptive split %zu changed %.6f -> %.6f for HWND %p",
                  adjustment.splitIndex, adjustment.before, adjustment.after,
                  static_cast<void*>(repositionedAdmission));
              Diagnostics::RecordEvent(
                  L"dwindle adaptive ratio changed hwnd=%p split=%zu from=%.6f to=%.6f",
                  repositionedAdmission, adjustment.splitIndex,
                  adjustment.before, adjustment.after);
            }
          }
          if (previousInsertionIndex != viableInsertionIndex) {
            Wh_Log(
                L"Constraint-aware admission repositioned HWND %p from slot %zu to %zu",
                static_cast<void*>(repositionedAdmission),
                previousInsertionIndex, viableInsertionIndex);
            Diagnostics::RecordEvent(
                L"constraint-aware admission repositioned hwnd=%p from=%zu to=%zu",
                repositionedAdmission, previousInsertionIndex,
                viableInsertionIndex);
          }
          g_workspaces.Save(key, workspace);
          continue;
        }

        // Test one removal at a time against the already-swapped model. This
        // avoids permanently floating several newer windows before reaching the
        // one member whose removal actually makes the layout viable. The window
        // explicitly moved by the user is considered last so promotion,
        // demotion, make-master, and drag swaps preserve their requested subject.
        for (const ConstraintCandidate& candidate : candidates) {
          Workspace trial = workspace;
          if (!trial.Float(candidate.hwnd)) continue;

          std::vector<RECT> trialRects;
          ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
          if (BuildWorkspaceLayoutPlan(
                  trial, workArea, gap, &trialRects) !=
              LayoutPlanStatus::Success) {
            continue;
          }

          victim = candidate.hwnd;
          singleRemovalMakesCurrentOrderViable = true;
          break;
        }

        // Some aggregate conflicts require more than one removal. Retain the
        // bounded iterative fallback, but use the same ordering so the user's
        // moved window remains the final candidate rather than the first newest
        // admission discarded.
        if (!victim && !candidates.empty()) victim = candidates.front().hwnd;
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
      } else if (singleRemovalMakesCurrentOrderViable) {
        Wh_Log(
            L"Current-order constraint conflict; floating single-removal candidate HWND %p (generation=%llu)",
            static_cast<void*>(victim),
            static_cast<unsigned long long>(generation));
      } else {
        Wh_Log(
            L"Constraint conflict; floating fallback HWND %p (generation=%llu)",
            static_cast<void*>(victim),
            static_cast<unsigned long long>(generation));
      }
      Diagnostics::RecordEvent(
          L"proven constraints floated hwnd=%p generation=%llu intrinsic=%d singleRemovalCurrentOrder=%d preferred=%d enforcedMin=%d%d reportedMin=%ldx%ld",
          victim, static_cast<unsigned long long>(generation),
          intrinsicallyImpossible ? 1 : 0,
          singleRemovalMakesCurrentOrderViable ? 1 : 0,
          victim == preferredTiledWindow ? 1 : 0,
          constraints.enforceMinWidth ? 1 : 0,
          constraints.enforceMinHeight ? 1 : 0,
          constraints.minWidth, constraints.minHeight);

      for (size_t i = 0;
           i < activeConstraints.size() &&
           i < workspace.TiledWindows().size();
           ++i) {
        const WindowConstraints& memberConstraint = activeConstraints[i];
        if (!memberConstraint.valid ||
            (!memberConstraint.enforceMinWidth &&
             !memberConstraint.enforceMinHeight)) {
          continue;
        }
        size_t compatibleSlotCount = 0;
        for (const RECT& slot : rects) {
          if ((!memberConstraint.enforceMinWidth ||
               RectWidth(slot) >=
                   std::max<LONG>(1, memberConstraint.minWidth)) &&
              (!memberConstraint.enforceMinHeight ||
               RectHeight(slot) >=
                   std::max<LONG>(1, memberConstraint.minHeight))) {
            ++compatibleSlotCount;
          }
        }
        Diagnostics::RecordEvent(
            L"constraint conflict member hwnd=%p slot=%zu min=%ldx%ld enforcedMin=%d%d compatibleSlots=%zu",
            workspace.TiledWindows()[i], i,
            memberConstraint.minWidth, memberConstraint.minHeight,
            memberConstraint.enforceMinWidth ? 1 : 0,
            memberConstraint.enforceMinHeight ? 1 : 0,
            compatibleSlotCount);
      }

      CancelConformanceLease(victim);
      const HWND culprit = FindConstraintCulprit(workspace, rects, victim);
      if (!constraintFailureReported) {
        ShowWindowConstraintConflict(workspace, rects, culprit ? culprit : victim, monitor);
        constraintFailureReported = true;
      }
      if (!FloatWindowAfterFailure(key, workspace, victim)) {
        g_workspaces.Save(key, workspace);
        return;
      }
      ++Diagnostics::g_runtime.counters.constraintFloats;
      g_workspaces.Save(key, workspace);
      if (workspace.Empty()) return;
      continue;
    }

    std::vector<HWND> toFloat;
    std::vector<HWND> toForget;
    bool constraintReplanRequested = false;
    bool layoutMutationRollbackRequested = false;
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
                    BeginConformanceLease(
                        hwnd, rects[i], false, preferredTiledWindow);
                  }
                  break;
                case PlacementResult::AdjustedByWindow:
                case PlacementResult::Refused:
                  BeginConformanceLease(
                      hwnd, rects[i], true, preferredTiledWindow);
                  if (g_settings.conformanceLeaseMs == 0 &&
                      TryRollbackPendingLayoutMutation(
                          key, monitor, &workspace, rects, hwnd)) {
                    layoutMutationRollbackRequested = true;
                  }
                  break;
                case PlacementResult::AccessDenied:
                case PlacementResult::Dead:
                  break;
              }
            }
          }
        }
      }

      if (layoutMutationRollbackRequested) break;
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

    if (layoutMutationRollbackRequested) continue;
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
      participationChanged |= FloatWindowAfterFailure(key, workspace, hwnd);
    }

    if (!participationChanged) {
      ClearSettledInsertionPlacements(key, workspace);
    }
    g_workspaces.Save(key, workspace);
    if (!participationChanged || workspace.Empty()) {
      ClearSettledLayoutMutation(key, workspace);
      return;
    }
  }

  Diagnostics::RecordEvent(
      L"arrangement safety bound exhausted desktop=%08X active=%zu",
      key.desktopId.Data1, workspace.ActiveCount());
  g_workspaces.Save(key, workspace);
  ClearSettledLayoutMutation(key, workspace);
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

// An admission remains movable until every placement produced by its reflow has
// survived conformance. This matters because an existing sibling, not only the
// new HWND, can reveal the constraint that requires an alternate insertion slot.
static bool ClearSettledInsertionPlacements(
    const DesktopMonitorKey& key, Workspace& workspace) {
  if (HasOutstandingConformanceLease(workspace)) return false;

  bool changed = false;
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (!record || !record->pendingInsertionPlacement ||
        GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
        IsWindowCloaked(hwnd)) {
      continue;
    }

    RECT authoritative{};
    RECT current{};
    if (GetCurrentAuthoritativeTiledRect(
            key, workspace, hwnd, &authoritative) &&
        GetWindowFrameRect(hwnd, &current) &&
        RectsNear(current, authoritative)) {
      changed |= workspace.ClearInsertionPlacement(hwnd);
    }
  }
  return changed;
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
    if (ClearSettledInsertionPlacements(owners.front(), workspace)) {
      g_workspaces.Save(owners.front(), workspace);
    }
    ClearSettledLayoutMutation(owners.front(), workspace);
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
    ArrangeWorkspace(owners.front(), lease.preferredTiledWindow);
    return;
  }

  const std::vector<RECT> noRejectedRects;
  if (TryRollbackPendingLayoutMutation(
          owners.front(), owners.front().ResolveMonitor(), &workspace,
          noRejectedRects, hwnd)) {
    ArrangeWorkspace(owners.front());
    return;
  }

  if (!FloatWindowAfterFailure(owners.front(), workspace, hwnd)) {
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
  ArrangeWorkspace(owners.front(), lease.preferredTiledWindow);
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
    ArrangeWorkspace(owners.front(), lease.preferredTiledWindow);
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
        ArrangeWorkspace(owners.front(), lease.preferredTiledWindow);
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
    ArrangeWorkspace(owners.front(), lease.preferredTiledWindow);
    return true;
  }
  if (action == Workspace::PlacementAction::Float) {
    CancelConformanceLease(hwnd, lease.generation);
    FloatWindowAfterFailure(owners.front(), workspace, hwnd);
    g_workspaces.Save(owners.front(), workspace);
    ArrangeWorkspace(owners.front(), lease.preferredTiledWindow);
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

