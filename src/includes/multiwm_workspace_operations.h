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
  return SuspensionReason::None;
}

static PresentationState GetPhysicalPresentationState(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return PresentationState::Normal;
  if (IsZoomed(hwnd)) return PresentationState::Maximized;

  // Chromium and similar apps can cover the full monitor while Windows still
  // reports the HWND as restored. Keep that as a presentation override instead
  // of removing the logical leaf from the tiling tree.
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
      return PresentationState::Fullscreen;
    }
  }
  return PresentationState::Normal;
}

// Workspace initialization needs a broader physical snapshot than normal layout
// participation. A visible top-level window can already belong to this workspace
// while minimized. Maximized/fullscreen windows remain active logical members and
// are collected normally; enumerate this broader set only for non-participating
// physical states when creating a workspace.
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
  bool alwaysFloating = false;
  const bool startsFloating =
      ShouldWindowStartFloating(hwnd, &alwaysFloating);
  if (alwaysFloating || (useAutomaticAdmissionPolicy && startsFloating)) {
    WindowRecord record = MakeWindowRecord(hwnd, ManageState::Floating);
    record.pendingDefaultFloatingCenter = true;
    record.alwaysFloating = alwaysFloating;
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

void Workspace::RememberCurrentLayoutGeometry() {
  AssertWmThread(L"Workspace::RememberCurrentLayoutGeometry");

  if (layout_ == TileLayout::MasterStack ||
      layout_ == TileLayout::MasterStackH) {
    rememberedMasterStackOrder_ = windows_;
    rememberedMasterStackWeights_ = stackWeights_;
  } else if (layout_ == TileLayout::Columns ||
             layout_ == TileLayout::Rows) {
    rememberedGridOrder_ = windows_;
    rememberedGridWeights_ = gridWeights_;
  } else if (layout_ == TileLayout::Dwindle) {
    rememberedDwindleOrder_ = windows_;
    rememberedDwindleRatios_ = dwindleRatios_;
  }
}

void Workspace::RestoreRememberedLayoutGeometry() {
  AssertWmThread(L"Workspace::RestoreRememberedLayoutGeometry");

  if (layout_ == TileLayout::MasterStack ||
      layout_ == TileLayout::MasterStackH) {
    const size_t expected = windows_.empty() ? 0 : windows_.size() - 1;
    if (rememberedMasterStackOrder_ == windows_ &&
        rememberedMasterStackWeights_.size() == expected) {
      stackWeights_ = rememberedMasterStackWeights_;
    } else {
      stackWeights_.clear();
    }
  } else if (layout_ == TileLayout::Columns ||
             layout_ == TileLayout::Rows) {
    if (rememberedGridOrder_ == windows_ &&
        rememberedGridWeights_.size() == windows_.size()) {
      gridWeights_ = rememberedGridWeights_;
    } else {
      gridWeights_.clear();
    }
  } else if (layout_ == TileLayout::Dwindle) {
    if (rememberedDwindleOrder_ == windows_ &&
        rememberedDwindleRatios_.size() == windows_.size()) {
      dwindleRatios_ = rememberedDwindleRatios_;
    } else {
      dwindleRatios_.clear();
    }
  }
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

  RememberCurrentLayoutGeometry();
  layout_ = layout;
  ResetSuspendedLayoutHints();
  RestoreRememberedLayoutGeometry();
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
        record.alwaysFloating != otherIt->second.alwaysFloating ||
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
      dwindleRatios_ == other.dwindleRatios_ &&
      rememberedMasterStackOrder_ == other.rememberedMasterStackOrder_ &&
      rememberedMasterStackWeights_ == other.rememberedMasterStackWeights_ &&
      rememberedGridOrder_ == other.rememberedGridOrder_ &&
      rememberedGridWeights_ == other.rememberedGridWeights_ &&
      rememberedDwindleOrder_ == other.rememberedDwindleOrder_ &&
      rememberedDwindleRatios_ == other.rememberedDwindleRatios_;
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
  rememberedMasterStackOrder_ = source.rememberedMasterStackOrder_;
  rememberedMasterStackWeights_ = source.rememberedMasterStackWeights_;
  rememberedGridOrder_ = source.rememberedGridOrder_;
  rememberedGridWeights_ = source.rememberedGridWeights_;
  rememberedDwindleOrder_ = source.rememberedDwindleOrder_;
  rememberedDwindleRatios_ = source.rememberedDwindleRatios_;
  for (auto& [hwnd, record] : records_) {
    const WindowRecord& sourceRecord = source.records_.find(hwnd)->second;
    if (record.state == ManageState::Suspended) {
      record.savedSlot = sourceRecord.savedSlot;
    }
  }
  DebugValidateMutation(L"Workspace::RestoreLayoutStateFrom");
  return true;
}

// Restore only the active layout/order geometry from a compatible snapshot.
// Unlike RestoreLayoutStateFrom, this intentionally tolerates extra/missing
// inactive records. It is used when a pending admission has already left active
// participation: the established tiled membership must match, while the pending
// HWND itself may now be Suspended instead of Tiled.
bool Workspace::RestoreCompatibleActiveLayoutStateFrom(
    const Workspace& source) {
  AssertWmThread(L"Workspace::RestoreCompatibleActiveLayoutStateFrom");
  if (layout_ != source.layout_ || windows_.size() != source.windows_.size()) {
    return false;
  }

  for (HWND hwnd : windows_) {
    const WindowRecord* currentRecord = Find(hwnd);
    const WindowRecord* sourceRecord = source.Find(hwnd);
    if (!currentRecord || !sourceRecord ||
        currentRecord->state != ManageState::Tiled ||
        sourceRecord->state != ManageState::Tiled ||
        currentRecord->pid != sourceRecord->pid ||
        currentRecord->tilingGeneration != sourceRecord->tilingGeneration) {
      return false;
    }
  }
  for (HWND hwnd : source.windows_) {
    if (!IsTiled(hwnd)) return false;
  }

  windows_ = source.windows_;
  masterRatio_ = source.masterRatio_;
  stackWeights_ = source.stackWeights_;
  gridWeights_ = source.gridWeights_;
  dwindleRatios_ = source.dwindleRatios_;
  rememberedMasterStackOrder_ = source.rememberedMasterStackOrder_;
  rememberedMasterStackWeights_ = source.rememberedMasterStackWeights_;
  rememberedGridOrder_ = source.rememberedGridOrder_;
  rememberedGridWeights_ = source.rememberedGridWeights_;
  rememberedDwindleOrder_ = source.rememberedDwindleOrder_;
  rememberedDwindleRatios_ = source.rememberedDwindleRatios_;

  // Undo logical-index shifts caused by the provisional admission for suspended
  // members that existed in both snapshots. Current-only inactive records are
  // deliberately preserved.
  for (auto& [hwnd, record] : records_) {
    if (record.state != ManageState::Suspended) continue;
    const WindowRecord* sourceRecord = source.Find(hwnd);
    if (!sourceRecord || sourceRecord->state != ManageState::Suspended ||
        !sourceRecord->hasSavedSlot) {
      continue;
    }
    record.savedSlot = sourceRecord->savedSlot;
    record.hasSavedSlot = true;
  }

  EnsureWeights();
  DebugValidateMutation(L"Workspace::RestoreCompatibleActiveLayoutStateFrom");
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

bool Workspace::EnforceAlwaysFloating(HWND hwnd) {
  AssertWmThread(L"Workspace::EnforceAlwaysFloating");
  WindowRecord* record = FindMutable(hwnd);
  if (!record) return false;

  bool changed = !record->alwaysFloating;
  record->alwaysFloating = true;
  if (record->state != ManageState::Floating) {
    changed |= Float(hwnd);
  }
  if (changed) DebugValidateMutation(L"Workspace::EnforceAlwaysFloating");
  return changed;
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

// Refreshes the application-owned native minimum contract without introducing a
// second solver notion of "proof". Normal arrangement probes only records that
// are missing, stale-DPI, or still resolving an admission/restore. User-authored
// mutations and explicit Tile Workspace operations can force a refresh so a
// relaxed same-DPI minimum cannot remain cached indefinitely.
static bool RefreshWorkspaceNativeConstraints(
    Workspace& workspace, bool forceAll, const wchar_t* reason) {
  bool changed = false;
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    if (!record ||
        GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
        IsWindowCloaked(hwnd)) {
      continue;
    }

    const HMONITOR windowMonitor = GetWindowPhysicalMonitor(hwnd);
    const UINT currentDpi = windowMonitor
        ? GetMonitorEffectiveDpi(windowMonitor)
        : 0;
    const bool needsProbe =
        forceAll || record->pendingInsertionPlacement ||
        !record->constraints.valid ||
        (currentDpi && currentDpi != record->constraints.dpi);
    if (!needsProbe) continue;

    const WindowConstraints before = record->constraints;
    const WindowConstraints fresh = ProbeWindowConstraints(hwnd);
    const WindowConstraints refreshed =
        MergeConstraintProbeWithCache(before, fresh);
    const bool geometryChanged =
        !SameConstraintGeometry(before, refreshed);
    changed |= workspace.UpdateConstraints(hwnd, refreshed);

    if (fresh.valid && geometryChanged) {
      Diagnostics::RecordEvent(
          L"native constraint report refreshed reason=%ls hwnd=%p min=%ldx%ld dpi=%u",
          reason ? reason : L"arrange", hwnd, refreshed.minWidth,
          refreshed.minHeight, refreshed.dpi);
    }
  }
  return changed;
}

static std::vector<WindowConstraints> ActiveWindowConstraints(
    const Workspace& workspace) {
  std::vector<WindowConstraints> constraints;
  constraints.reserve(workspace.ActiveCount());
  for (HWND hwnd : workspace.TiledWindows()) {
    const WindowRecord* record = workspace.Find(hwnd);
    constraints.push_back(record ? record->constraints : WindowConstraints{});
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
    if (c.valid &&
        (width < std::max<LONG>(1, c.minWidth) ||
         height < std::max<LONG>(1, c.minHeight))) {
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

  // Always begin from MultiWM's ordinary geometry. When the native constraints
  // already fit, this stays byte-for-byte on the established layout path.
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

struct AdaptiveDwindleRatioEnvelope {
  double minimum = 0.1;
  double maximum = 0.9;
};

static bool AutomaticDwindleAdaptationEnabled() {
  return g_settings.adaptiveDwindleSlots > 0 &&
      g_settings.adaptiveDwindlePercentage > 0;
}

static AdaptiveDwindleRatioEnvelope GetAdaptiveDwindleRatioEnvelope() {
  const double deviation =
      static_cast<double>(std::clamp<LONG>(
          g_settings.adaptiveDwindlePercentage, 0, 40)) /
      100.0;
  return {0.5 - deviation, 0.5 + deviation};
}

// Admission recovery and explicit order-mutation preflight share one geometry
// fitter. Leading split nodes may move according to Adaptive Dwindle Slots and
// Adaptive Dwindle Percentage; every deeper ratio and the complete window order
// stay fixed. A percentage of zero deliberately disables automatic ratio
// changes. A manual ratio outside the adaptive envelope is retained whenever it
// is already feasible; the envelope constrains only an automatic replacement.
static bool FitAdaptiveDwindleDividers(
    Workspace& candidate, const RECT& workArea, LONG gap) {
  if (candidate.Layout() != TileLayout::Dwindle || candidate.ActiveCount() < 2 ||
      !AutomaticDwindleAdaptationEnabled()) {
    return false;
  }

  const auto& windows = candidate.TiledWindows();
  const auto constraints = ActiveWindowConstraints(candidate);
  const auto& ratios = candidate.DwindleRatios();
  const size_t adaptiveSplitCount =
      std::min(g_settings.adaptiveDwindleSlots, ratios.size());
  if (adaptiveSplitCount == 0) return false;
  const AdaptiveDwindleRatioEnvelope adaptiveEnvelope =
      GetAdaptiveDwindleRatioEnvelope();

  std::vector<SIZE> windowMinimums(windows.size(), SIZE{1, 1});
  for (size_t i = 0; i < constraints.size(); ++i) {
    const auto& constraint = constraints[i];
    if (constraint.valid) {
      windowMinimums[i].cx = std::max<LONG>(1, constraint.minWidth);
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

      // A manually chosen ratio outside the adaptive envelope remains a legal
      // no-op if it already satisfies both sides. Only a ratio we actually need
      // to change must land inside the configured symmetric envelope.
      const LONG currentAllocated = std::clamp<LONG>(
          static_cast<LONG>(std::llround(usable * fixedRatio)),
          1, usable - 1);
      if (currentAllocated >= first && usable - currentAllocated >= tail) {
        return true;
      }

      const LONG minimumAdaptive = std::clamp<LONG>(
          static_cast<LONG>(std::llround(
              usable * adaptiveEnvelope.minimum)), 1, usable - 1);
      const LONG maximumAdaptive = std::clamp<LONG>(
          static_cast<LONG>(std::llround(
              usable * adaptiveEnvelope.maximum)), 1, usable - 1);
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
  // somewhere inside the configured adaptive ratio envelope.
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
    const double preferred = ClampDouble(ratios[splitIndex], 0.1, 0.9);
    const LONG desired = std::clamp<LONG>(
        static_cast<LONG>(std::llround(usable * preferred)),
        1, usable - 1);
    LONG firstSpan = desired;
    if (desired < firstMinimum || usable - desired < tailMinimum) {
      const LONG minimumAdaptive = std::clamp<LONG>(
          static_cast<LONG>(std::llround(
              usable * adaptiveEnvelope.minimum)), 1, usable - 1);
      const LONG maximumAdaptive = std::clamp<LONG>(
          static_cast<LONG>(std::llround(
              usable * adaptiveEnvelope.maximum)), 1, usable - 1);
      const LONG lower = std::max(firstMinimum, minimumAdaptive);
      const LONG upper = std::min(usable - tailMinimum, maximumAdaptive);
      if (lower > upper) return false;
      firstSpan = std::clamp<LONG>(desired, lower, upper);
    }
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

static void DiscardPendingAdmissionRecovery(HWND hwnd) {
  if (hwnd) g_pendingAdmissionRecoveries.erase(hwnd);
}

static void RememberPendingAdmissionRecovery(
    const DesktopMonitorKey& key, HWND hwnd, const Workspace& baseline) {
  if (!hwnd) return;
  const WindowRecord* record = baseline.Find(hwnd);
  if (!record || record->state != ManageState::Tiled ||
      !record->pendingInsertionPlacement) {
    return;
  }

  auto existing = g_pendingAdmissionRecoveries.find(hwnd);
  if (existing != g_pendingAdmissionRecoveries.end() &&
      existing->second.generation == record->tilingGeneration &&
      Model::DesktopMonitorKeyEqual{}(existing->second.key, key)) {
    // Keep the earliest pre-recovery snapshot for this unsettled admission.
    return;
  }

  PendingAdmissionRecovery recovery;
  recovery.key = key;
  recovery.baseline = baseline;
  recovery.generation = record->tilingGeneration;
  g_pendingAdmissionRecoveries[hwnd] = std::move(recovery);
}

// If an admission/restore leaves active participation before its recovery has
// settled, derive the no-longer-active state from the pre-recovery snapshot and
// restore that established order/geometry into the live workspace. This makes
// adaptive ratios and sibling reassignment provisional instead of leaking into
// the N-1 layout after a minimize/hidden participation race.
static bool RestorePendingAdmissionRecoveryAfterSuspension(
    const DesktopMonitorKey& key, HWND hwnd, Workspace* workspace) {
  if (!workspace || !hwnd) return false;
  auto recoveryIt = g_pendingAdmissionRecoveries.find(hwnd);
  if (recoveryIt == g_pendingAdmissionRecoveries.end()) return false;

  PendingAdmissionRecovery recovery = recoveryIt->second;
  g_pendingAdmissionRecoveries.erase(recoveryIt);
  if (!Model::DesktopMonitorKeyEqual{}(recovery.key, key)) return false;

  const WindowRecord* currentRecord = workspace->Find(hwnd);
  const WindowRecord* baselineRecord = recovery.baseline.Find(hwnd);
  if (!currentRecord || !baselineRecord ||
      currentRecord->state != ManageState::Suspended ||
      baselineRecord->state != ManageState::Tiled ||
      currentRecord->tilingGeneration != recovery.generation ||
      baselineRecord->tilingGeneration != recovery.generation) {
    return false;
  }

  Workspace restored = recovery.baseline;
  if (!restored.Suspend(hwnd, currentRecord->suspensionReason) ||
      !workspace->RestoreCompatibleActiveLayoutStateFrom(restored)) {
    Diagnostics::RecordEvent(
        L"pending admission recovery snapshot became incompatible hwnd=%p desktop=%08X",
        hwnd, key.desktopId.Data1);
    return false;
  }

  Diagnostics::RecordEvent(
      L"pending admission recovery rolled back after suspension hwnd=%p desktop=%08X",
      hwnd, key.desktopId.Data1);
  return true;
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
        (RectWidth(rects[index]) < std::max<LONG>(1, constraint.minWidth) ||
         RectHeight(rects[index]) < std::max<LONG>(1, constraint.minHeight));
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
      (RectWidth(slot) >= std::max<LONG>(1, constraint.minWidth) &&
       RectHeight(slot) >= std::max<LONG>(1, constraint.minHeight));
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

// Applies an already-decided active-slot permutation without changing membership.
// Suspended logical slots remain anchored; swapping active leaves changes only which
// active window occupies each currently available logical position.
static bool ApplyTiledWindowOrder(
    Workspace& workspace, const std::vector<HWND>& desiredOrder) {
  if (desiredOrder.size() != workspace.ActiveCount()) return false;
  for (size_t slotIndex = 0; slotIndex < desiredOrder.size(); ++slotIndex) {
    const HWND current = workspace.TiledWindows()[slotIndex];
    if (current != desiredOrder[slotIndex] &&
        !workspace.SwapTiled(current, desiredOrder[slotIndex])) {
      return false;
    }
  }
  return true;
}

// Layout conversion has different semantics from an explicit order mutation.
// Preserve the existing order first (the caller already tried that), but if the
// new Dwindle geometry cannot host that order, find a compatible permutation.
//
// Adaptive prefix splits are searched jointly with assignment: for a candidate
// occupant we allocate the smallest legal span that satisfies that window, which
// maximizes the recursive tail. With minimum-only constraints, any larger tail is
// never worse, so failure with this maximal tail proves that occupant cannot lead
// to a feasible continuation. Once the adaptive prefix ends, ordinary bipartite
// matching solves the remaining fixed-geometry tail in polynomial time.
static bool ReassignDwindleLayoutConversionForConstraints(
    Workspace& workspace, const RECT& workArea, LONG gap,
    size_t* outMovedWindowCount) {
  if (workspace.Layout() != TileLayout::Dwindle || workspace.ActiveCount() < 2) {
    return false;
  }

  const std::vector<HWND> windows = workspace.TiledWindows();
  const std::vector<WindowConstraints> constraints =
      ActiveWindowConstraints(workspace);
  const std::vector<double> ratios = workspace.DwindleRatios();
  const size_t count = windows.size();
  if (constraints.size() != count || ratios.size() + 1 < count) return false;

  const size_t adaptiveSplitCount = AutomaticDwindleAdaptationEnabled()
      ? std::min(
            g_settings.adaptiveDwindleSlots,
            count > 0 ? count - 1 : size_t{0})
      : 0;
  const AdaptiveDwindleRatioEnvelope adaptiveEnvelope =
      GetAdaptiveDwindleRatioEnvelope();
  std::vector<bool> used(count, false);
  std::vector<HWND> desiredOrder(count, nullptr);
  uint64_t usedMask = 0;
  size_t searchNodes = 0;
  bool searchLimitReached = false;
  constexpr size_t kMaxSearchNodes = 250000;

  struct SearchKey {
    uint64_t usedMask = 0;
    uint32_t slotIndex = 0;
    LONG width = 0;
    LONG height = 0;
    bool operator==(const SearchKey& other) const noexcept {
      return usedMask == other.usedMask && slotIndex == other.slotIndex &&
          width == other.width && height == other.height;
    }
  };
  struct SearchKeyHash {
    size_t operator()(const SearchKey& key) const noexcept {
      size_t value = std::hash<uint64_t>{}(key.usedMask);
      value ^= std::hash<uint32_t>{}(key.slotIndex) +
          0x9e3779b9u + (value << 6) + (value >> 2);
      value ^= std::hash<LONG>{}(key.width) +
          0x9e3779b9u + (value << 6) + (value >> 2);
      value ^= std::hash<LONG>{}(key.height) +
          0x9e3779b9u + (value << 6) + (value >> 2);
      return value;
    }
  };
  std::unordered_set<SearchKey, SearchKeyHash> deadStates;
  const bool canMemoize = count <= 64;

  auto makeSplit = [&](const RECT& area, size_t slotIndex, LONG firstSpan,
                       RECT* first, RECT* remaining) {
    if (!first || !remaining) return false;
    const bool vertical = (slotIndex % 2) == 0;
    const LONG span = vertical ? RectWidth(area) : RectHeight(area);
    if (span < 2) return false;
    const LONG effectiveGap = std::clamp<LONG>(gap, 0, span - 2);
    const LONG usable = span - effectiveGap;
    if (firstSpan < 1 || firstSpan >= usable) return false;

    *remaining = area;
    if (vertical) {
      if (g_settings.reverseDwindle) {
        *first = {area.right - firstSpan, area.top, area.right, area.bottom};
        remaining->right = first->left - effectiveGap;
      } else {
        *first = {area.left, area.top, area.left + firstSpan, area.bottom};
        remaining->left = first->right + effectiveGap;
      }
    } else {
      if (g_settings.reverseDwindle) {
        *first = {area.left, area.bottom - firstSpan, area.right, area.bottom};
        remaining->bottom = first->top - effectiveGap;
      } else {
        *first = {area.left, area.top, area.right, area.top + firstSpan};
        remaining->top = first->bottom + effectiveGap;
      }
    }
    return RectWidth(*remaining) > 0 && RectHeight(*remaining) > 0;
  };

  auto matchFixedTail = [&](size_t slotIndex, const RECT& area) {
    const size_t tailCount = count - slotIndex;
    if (tailCount == 0) return true;

    std::vector<size_t> remainingWindows;
    remainingWindows.reserve(tailCount);
    for (size_t windowIndex = 0; windowIndex < count; ++windowIndex) {
      if (!used[windowIndex]) remainingWindows.push_back(windowIndex);
    }
    if (remainingWindows.size() != tailCount) return false;

    std::vector<RECT> allSlots(count);
    Layout::LayoutDwindle(
        area, gap, slotIndex, tailCount, ratios,
        g_settings.reverseDwindle, allSlots);
    std::vector<RECT> tailSlots(
        allSlots.begin() + slotIndex, allSlots.end());
    std::vector<WindowConstraints> tailConstraints;
    tailConstraints.reserve(tailCount);
    for (size_t windowIndex : remainingWindows) {
      tailConstraints.push_back(constraints[windowIndex]);
    }
    const ConstraintSlotCompatibility compatibility =
        BuildConstraintSlotCompatibility(tailConstraints, tailSlots, true);
    for (const auto& compatibleSlots : compatibility.slotsByWindow) {
      if (compatibleSlots.empty()) return false;
    }

    constexpr size_t kUnassigned = static_cast<size_t>(-1);
    std::vector<size_t> slotOwner(tailCount, kUnassigned);
    std::vector<bool> fixed(tailCount, false);

    // Seed surviving exact positions first so a layout conversion disturbs as
    // little order as possible before augmenting paths are allowed to move them.
    for (size_t localWindow = 0; localWindow < tailCount; ++localWindow) {
      const size_t originalIndex = remainingWindows[localWindow];
      if (originalIndex < slotIndex) continue;
      const size_t localSlot = originalIndex - slotIndex;
      if (localSlot >= tailCount || slotOwner[localSlot] != kUnassigned) continue;
      const auto& compatibleSlots = compatibility.slotsByWindow[localWindow];
      if (std::find(
              compatibleSlots.begin(), compatibleSlots.end(), localSlot) !=
          compatibleSlots.end()) {
        slotOwner[localSlot] = localWindow;
      }
    }

    std::vector<size_t> assignmentOrder(tailCount);
    for (size_t i = 0; i < tailCount; ++i) assignmentOrder[i] = i;
    std::stable_sort(
        assignmentOrder.begin(), assignmentOrder.end(),
        [&](size_t first, size_t second) {
          const size_t firstChoices = compatibility.slotsByWindow[first].size();
          const size_t secondChoices = compatibility.slotsByWindow[second].size();
          return firstChoices != secondChoices
              ? firstChoices < secondChoices
              : remainingWindows[first] < remainingWindows[second];
        });

    for (size_t localWindow : assignmentOrder) {
      if (std::find(slotOwner.begin(), slotOwner.end(), localWindow) !=
          slotOwner.end()) {
        continue;
      }
      std::vector<bool> visitedSlots(tailCount, false);
      if (!TryAssignConstraintWindow(
              localWindow, compatibility, &slotOwner, fixed,
              &visitedSlots, true)) {
        return false;
      }
    }

    for (size_t localSlot = 0; localSlot < tailCount; ++localSlot) {
      if (slotOwner[localSlot] == kUnassigned) return false;
      desiredOrder[slotIndex + localSlot] =
          windows[remainingWindows[slotOwner[localSlot]]];
    }
    return true;
  };

  auto search = [&](auto&& self, size_t slotIndex, const RECT& area) -> bool {
    if (++searchNodes > kMaxSearchNodes) {
      searchLimitReached = true;
      return false;
    }
    if (slotIndex >= count) return true;

    SearchKey key{};
    if (canMemoize) {
      key = {usedMask, static_cast<uint32_t>(slotIndex),
             RectWidth(area), RectHeight(area)};
      if (deadStates.find(key) != deadStates.end()) return false;
    }

    if (slotIndex >= adaptiveSplitCount || slotIndex + 1 >= count) {
      const bool matched = matchFixedTail(slotIndex, area);
      if (!matched && canMemoize) deadStates.insert(key);
      return matched;
    }

    const bool vertical = (slotIndex % 2) == 0;
    const LONG span = vertical ? RectWidth(area) : RectHeight(area);
    if (span < 2) {
      if (canMemoize) deadStates.insert(key);
      return false;
    }
    const LONG effectiveGap = std::clamp<LONG>(gap, 0, span - 2);
    const LONG usable = span - effectiveGap;
    if (usable < 2) {
      if (canMemoize) deadStates.insert(key);
      return false;
    }
    const LONG minimumAdaptive = std::clamp<LONG>(
        static_cast<LONG>(std::llround(
            usable * adaptiveEnvelope.minimum)), 1, usable - 1);
    const LONG maximumAdaptive = std::clamp<LONG>(
        static_cast<LONG>(std::llround(
            usable * adaptiveEnvelope.maximum)), 1, usable - 1);
    const double currentRatio = ClampDouble(ratios[slotIndex], 0.1, 0.9);
    const LONG currentSpan = std::clamp<LONG>(
        static_cast<LONG>(std::llround(usable * currentRatio)),
        1, usable - 1);

    std::vector<size_t> candidates;
    candidates.reserve(count - slotIndex);
    for (size_t windowIndex = 0; windowIndex < count; ++windowIndex) {
      if (!used[windowIndex]) candidates.push_back(windowIndex);
    }
    std::stable_sort(
        candidates.begin(), candidates.end(),
        [&](size_t first, size_t second) {
          const size_t firstDistance = first > slotIndex
              ? first - slotIndex : slotIndex - first;
          const size_t secondDistance = second > slotIndex
              ? second - slotIndex : slotIndex - second;
          return firstDistance != secondDistance
              ? firstDistance < secondDistance : first < second;
        });

    for (size_t windowIndex : candidates) {
      const WindowConstraints& constraint = constraints[windowIndex];
      const LONG primaryMinimum = constraint.valid
          ? std::max<LONG>(
                1, vertical ? constraint.minWidth : constraint.minHeight)
          : 1;
      LONG firstSpan = usable;
      if (currentSpan >= primaryMinimum) {
        // Keeping the current manual ratio is always allowed, even when it lies
        // outside the automatic envelope.
        firstSpan = currentSpan;
      }
      const LONG adaptiveSpan = std::max(primaryMinimum, minimumAdaptive);
      if (adaptiveSpan <= maximumAdaptive) {
        // The smallest permitted automatic span maximizes the recursive tail;
        // with minimum-only constraints this is never worse for continuation.
        firstSpan = std::min(firstSpan, adaptiveSpan);
      }
      if (firstSpan >= usable) continue;

      RECT first{};
      RECT remaining{};
      if (!makeSplit(area, slotIndex, firstSpan, &first, &remaining) ||
          !WindowConstraintFitsSlot(constraint, first)) {
        continue;
      }

      used[windowIndex] = true;
      if (canMemoize) usedMask |= (uint64_t{1} << windowIndex);
      desiredOrder[slotIndex] = windows[windowIndex];
      if (self(self, slotIndex + 1, remaining)) return true;
      desiredOrder[slotIndex] = nullptr;
      if (canMemoize) usedMask &= ~(uint64_t{1} << windowIndex);
      used[windowIndex] = false;
      if (searchLimitReached) return false;
    }

    if (canMemoize) deadStates.insert(key);
    return false;
  };

  RECT normalizedArea{0, 0, RectWidth(workArea), RectHeight(workArea)};
  if (!search(search, 0, normalizedArea)) {
    if (searchLimitReached) {
      Diagnostics::RecordEvent(
          L"dwindle layout conversion search limit reached windows=%zu adaptive=%zu nodes=%zu",
          count, adaptiveSplitCount, searchNodes);
    }
    return false;
  }

  Workspace trial = workspace;
  if (!ApplyTiledWindowOrder(trial, desiredOrder)) return false;
  if (adaptiveSplitCount > 0) {
    if (!FitAdaptiveDwindleDividers(trial, workArea, gap)) return false;
  } else {
    std::vector<RECT> rects;
    ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
    if (BuildWorkspaceLayoutPlan(
            trial, workArea, gap, &rects) != LayoutPlanStatus::Success) {
      return false;
    }
  }

  size_t movedWindowCount = 0;
  for (size_t i = 0; i < count; ++i) {
    if (desiredOrder[i] != windows[i]) ++movedWindowCount;
  }
  if (outMovedWindowCount) *outMovedWindowCount = movedWindowCount;
  workspace = std::move(trial);
  return true;
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
  if (!ApplyTiledWindowOrder(workspace, desiredOrder)) return false;
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
    case PlacementResult::SuppressedByPhysicalState:
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
// refresh a native constraint or select one constraint-removal candidate.
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
  // Native reports are cached between arrangements. The first pass below probes
  // missing/stale reports and pending admissions/restores before geometry is solved.
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
      RefreshWorkspaceNativeConstraints(workspace, false, L"arrange");
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
      // An explicit order mutation has already passed a full native-constraint
      // preflight before commit. If it later becomes unsatisfiable, do not
      // reinterpret the command by reshaping/reassigning it again here. The
      // delayed transaction gets first refusal: restore the previous order and
      // report the same constraint toast. Only when that saved order is no
      // longer feasible either may ordinary global constraint recovery proceed.
      if (TryRollbackPendingLayoutMutation(
              key, monitor, &workspace, rects)) {
        continue;
      }

      // A settled Dwindle workspace should preserve membership and order when a
      // DPI/work-area/native-minimum change makes its current ratios infeasible.
      // Before considering any eviction, clamp only the configured adaptive
      // divider prefix to the nearest feasible geometry. Pending admissions have
      // their own transactional recovery below and must not use this settled path.
      bool hasPendingAdmission = false;
      for (HWND hwnd : workspace.TiledWindows()) {
        const WindowRecord* record = workspace.Find(hwnd);
        if (record && record->pendingInsertionPlacement) {
          hasPendingAdmission = true;
          break;
        }
      }
      if (!hasPendingAdmission &&
          workspace.Layout() == TileLayout::Dwindle &&
          AutomaticDwindleAdaptationEnabled()) {
        Workspace adaptive = workspace;
        const std::vector<double> beforeRatios = adaptive.DwindleRatios();
        if (FitAdaptiveDwindleDividers(adaptive, workArea, gap)) {
          const auto& afterRatios = adaptive.DwindleRatios();
          const size_t ratioCount =
              std::min(beforeRatios.size(), afterRatios.size());
          size_t changedRatios = 0;
          for (size_t splitIndex = 0; splitIndex < ratioCount; ++splitIndex) {
            if (std::abs(beforeRatios[splitIndex] -
                         afterRatios[splitIndex]) <= 1e-9) {
              continue;
            }
            ++changedRatios;
            Diagnostics::RecordEvent(
                L"settled dwindle adaptive ratio changed split=%zu from=%.6f to=%.6f desktop=%08X",
                splitIndex, beforeRatios[splitIndex], afterRatios[splitIndex],
                key.desktopId.Data1);
          }

          // BuildWorkspaceLayoutPlan was unsatisfiable immediately above, so a
          // successful fitter should necessarily alter geometry. Guard anyway to
          // prevent a pathological no-op from spinning the arrangement loop.
          if (changedRatios > 0) {
            for (HWND hwnd : workspace.TiledWindows()) {
              CancelConformanceLease(hwnd);
            }
            workspace = std::move(adaptive);
            Diagnostics::RecordEvent(
                L"settled dwindle constraints recovered adaptively changes=%zu desktop=%08X",
                changedRatios, key.desktopId.Data1);
            g_workspaces.Save(key, workspace);
            continue;
          }
        }
      }

      // Keep the exact pre-recovery state. If a pending admission is moved or
      // adaptive geometry is changed below, that state remains provisional until
      // conformance settles.
      const Workspace pendingAdmissionRecoveryBaseline = workspace;

      HWND victim = nullptr;
      bool intrinsicallyImpossible = false;
      bool pendingAdmissionRejected = false;
      bool singleRemovalMakesCurrentOrderViable = false;

      // Removing another member cannot help a window whose authoritative native
      // minimum is larger than the entire usable workspace. Float that window
      // directly; reserve candidate testing for aggregate constraint conflicts.
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
        if (!constraint.valid) continue;

        const bool impossible =
            std::max<LONG>(1, constraint.minWidth) > workspaceWidth ||
            std::max<LONG>(1, constraint.minHeight) > workspaceHeight;
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

              if (!AutomaticDwindleAdaptationEnabled()) return false;
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
                    if (!AutomaticDwindleAdaptationEnabled()) return false;
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
          RememberPendingAdmissionRecovery(
              key, repositionedAdmission, pendingAdmissionRecoveryBaseline);
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

        // A still-pending admission/restore owns the failure it introduced.
        // Once every placement/adaptive/matching recovery above has failed, reject
        // that pending member itself; never evict an already-settled sibling to
        // make room for it.
        for (const ConstraintCandidate& candidate : candidates) {
          const WindowRecord* candidateRecord = workspace.Find(candidate.hwnd);
          if (!candidateRecord || !candidateRecord->pendingInsertionPlacement) {
            continue;
          }
          victim = candidate.hwnd;
          pendingAdmissionRejected = true;
          break;
        }

        if (!victim) {
          // For a settled workspace, float a member only when that one removal is
          // proven to make the current order viable. Do not progressively delete
          // multiple settled windows from an aggregate conflict.
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
        }
      }
      const WindowRecord* victimRecord = victim ? workspace.Find(victim) : nullptr;
      if (!victim || !victimRecord) {
        const HWND culprit = FindConstraintCulprit(
            workspace, rects, preferredTiledWindow);
        if (!constraintFailureReported) {
          ShowWindowConstraintConflict(
              workspace, rects, culprit, monitor);
          constraintFailureReported = true;
        }
        Diagnostics::RecordEvent(
            L"constraint conflict left membership unchanged; no single settled removal is sufficient desktop=%08X",
            key.desktopId.Data1);
        g_workspaces.Save(key, workspace);
        return;
      }

      const uint64_t generation = victimRecord->tilingGeneration;
      const WindowConstraints constraints = victimRecord->constraints;
      if (intrinsicallyImpossible) {
        Wh_Log(
          L"Native minimum exceeds workspace; floating HWND %p (generation=%llu)",
          static_cast<void*>(victim),
          static_cast<unsigned long long>(generation));
      } else if (pendingAdmissionRejected) {
        Wh_Log(
            L"Constraint conflict; rejecting pending admission HWND %p (generation=%llu)",
            static_cast<void*>(victim),
            static_cast<unsigned long long>(generation));
        Diagnostics::RecordEvent(
            L"pending admission rejected hwnd=%p generation=%llu",
            victim, static_cast<unsigned long long>(generation));
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
          L"native constraints floated hwnd=%p generation=%llu intrinsic=%d singleRemovalCurrentOrder=%d preferred=%d min=%ldx%ld",
          victim, static_cast<unsigned long long>(generation),
          intrinsicallyImpossible ? 1 : 0,
          singleRemovalMakesCurrentOrderViable ? 1 : 0,
          victim == preferredTiledWindow ? 1 : 0,
          constraints.minWidth, constraints.minHeight);

      for (size_t i = 0;
           i < activeConstraints.size() &&
           i < workspace.TiledWindows().size();
           ++i) {
        const WindowConstraints& memberConstraint = activeConstraints[i];
        if (!memberConstraint.valid) continue;
        size_t compatibleSlotCount = 0;
        for (const RECT& slot : rects) {
          if (WindowConstraintFitsSlot(memberConstraint, slot)) {
            ++compatibleSlotCount;
          }
        }
        Diagnostics::RecordEvent(
            L"constraint conflict member hwnd=%p slot=%zu min=%ldx%ld compatibleSlots=%zu",
            workspace.TiledWindows()[i], i,
            memberConstraint.minWidth, memberConstraint.minHeight,
            compatibleSlotCount);
      }

      CancelConformanceLease(victim);
      const HWND culprit = FindConstraintCulprit(workspace, rects, victim);
      if (!constraintFailureReported) {
        ShowWindowConstraintConflict(workspace, rects, culprit ? culprit : victim, monitor);
        constraintFailureReported = true;
      }
      DiscardPendingAdmissionRecovery(victim);
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
      } else {
        const WindowRecord* record = workspace.Find(hwnd);
        if (!record) {
          action = Workspace::PlacementAction::Forget;
        } else if (
            record->presentationState == PresentationState::Normal &&
            GetPhysicalSuspensionReason(hwnd) == SuspensionReason::None &&
            GetPhysicalPresentationState(hwnd) == PresentationState::Normal &&
            !IsWindowCloaked(hwnd)) {
          PlacementObservation observation =
              PlaceWindowChecked(hwnd, record->canMove, rects[i]);
          action = workspace.ApplyPlacementObservation(hwnd, observation);

          // A native minimum report is authoritative as soon as it is available.
          // If an application still corrects a legal target, re-probe in case its
          // own WM_GETMINMAXINFO contract changed dynamically. If the report is
          // unchanged, leave the event to conformance handling rather than
          // inventing a second empirical minimum-size policy.
          if (observation.result == PlacementResult::AdjustedByWindow) {
            const WindowConstraints before = record->constraints;
            const WindowConstraints fresh = ProbeWindowConstraints(hwnd);
            const WindowConstraints observed =
                MergeConstraintProbeWithCache(before, fresh);
            const bool geometryChanged =
                !SameConstraintGeometry(before, observed);
            workspace.UpdateConstraints(hwnd, observed);

            if (fresh.valid && geometryChanged) {
              action = Workspace::PlacementAction::None;
              constraintReplanRequested = true;
              CancelConformanceLease(hwnd);
              Diagnostics::RecordEvent(
                  L"placement refreshed native constraints hwnd=%p min=%ldx%ld dpi=%u",
                  hwnd, observed.minWidth, observed.minHeight, observed.dpi);
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
                case PlacementResult::SuppressedByPhysicalState:
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

    const PresentationState physicalPresentation =
        GetPhysicalPresentationState(hwnd);

    // A maximized/fullscreen tiled member is already logically settled once the
    // workspace reflow has survived every sibling conformance lease. Its physical
    // frame is application-owned presentation geometry and must never be required
    // to equal the authoritative tile. Clear the pending-admission role only when
    // model and live presentation agree, so a transition race cannot settle stale
    // state.
    if (record->presentationState != PresentationState::Normal) {
      if (physicalPresentation == record->presentationState &&
          workspace.ClearInsertionPlacement(hwnd)) {
        changed = true;
        DiscardPendingAdmissionRecovery(hwnd);
        Diagnostics::RecordEvent(
            L"pending admission settled under presentation override hwnd=%p state=%d",
            hwnd, static_cast<int>(record->presentationState));
      }
      continue;
    }

    if (physicalPresentation != PresentationState::Normal) continue;

    RECT authoritative{};
    RECT current{};
    if (GetCurrentAuthoritativeTiledRect(
            key, workspace, hwnd, &authoritative) &&
        GetWindowFrameRect(hwnd, &current) &&
        RectsNear(current, authoritative)) {
      if (workspace.ClearInsertionPlacement(hwnd)) {
        changed = true;
        DiscardPendingAdmissionRecovery(hwnd);
      }
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
      GetPhysicalPresentationState(hwnd) != PresentationState::Normal ||
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
  if (!record ||
      record->presentationState != PresentationState::Normal ||
      !ConformanceWindowMatchesRecordIdentity(hwnd, *record)) {
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
  // A window that entered maximize/fullscreen meanwhile owns its presentation
  // geometry; a stale lease must not convert that tiled member to Floating.
  if (GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      GetPhysicalPresentationState(hwnd) != PresentationState::Normal) {
    stale();
    return;
  }

  // The deadline is the last chance to catch a dynamic native minimum report
  // that changed after planning. If the app's own report is unchanged, remaining
  // nonconformance is not reinterpreted as a second empirical minimum policy.
  const WindowConstraints beforeConstraints = record->constraints;
  const WindowConstraints freshConstraints = ProbeWindowConstraints(hwnd);
  const WindowConstraints observed = MergeConstraintProbeWithCache(
      beforeConstraints, freshConstraints);
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, *record) ||
      GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      GetPhysicalPresentationState(hwnd) != PresentationState::Normal ||
      IsWindowCloaked(hwnd)) {
    stale();
    return;
  }
  const bool geometryChanged =
      !SameConstraintGeometry(beforeConstraints, observed);
  workspace.UpdateConstraints(hwnd, observed);
  if (freshConstraints.valid && geometryChanged) {
    Diagnostics::RecordEvent(
        L"conformance deadline refreshed native constraints hwnd=%p min=%ldx%ld dpi=%u",
        hwnd, observed.minWidth, observed.minHeight, observed.dpi);
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
      GetPhysicalPresentationState(hwnd) != PresentationState::Normal ||
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
      record->presentationState != PresentationState::Normal ||
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
      GetPhysicalPresentationState(hwnd) != PresentationState::Normal ||
      IsWindowCloaked(hwnd)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  // The incoming LOCATIONCHANGE says the application did not remain on its
  // authoritative tile. Re-probe its own native minimum contract before another
  // SetWindowPos, in case that contract changed dynamically.
  const WindowConstraints beforeConstraints = record->constraints;
  const WindowConstraints freshConstraints = ProbeWindowConstraints(hwnd);
  const WindowConstraints observedConstraints = MergeConstraintProbeWithCache(
      beforeConstraints, freshConstraints);
  // WM_GETMINMAXINFO executes target window code. Revalidate physical state
  // after that bounded call before either learning policy or reinforcing a tile.
  if (!ConformanceWindowMatchesRecordIdentity(hwnd, *record) ||
      GetPhysicalSuspensionReason(hwnd) != SuspensionReason::None ||
      GetPhysicalPresentationState(hwnd) != PresentationState::Normal ||
      IsWindowCloaked(hwnd)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  const bool locationConstraintChanged =
      !SameConstraintGeometry(beforeConstraints, observedConstraints);
  workspace.UpdateConstraints(hwnd, observedConstraints);
  if (freshConstraints.valid && locationConstraintChanged) {
    CancelConformanceLease(hwnd, lease.generation);
    Diagnostics::RecordEvent(
        L"conformance location refreshed native constraints hwnd=%p min=%ldx%ld dpi=%u",
        hwnd, observedConstraints.minWidth, observedConstraints.minHeight,
        observedConstraints.dpi);
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

