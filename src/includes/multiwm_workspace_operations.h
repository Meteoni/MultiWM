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

void Workspace::AdmitMigrated(WindowRecord record) {
  AssertWmThread(L"Workspace::AdmitMigrated");
  const HWND hwnd = record.hwnd;
  if (!hwnd) return;
  Forget(hwnd);

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

void Workspace::RepairForArrangement(
    const std::vector<HWND>& invalidWindows,
    const std::vector<WindowRecord>& missingActiveRecords) {
  AssertWmThread(L"Workspace::RepairForArrangement");
  for (HWND hwnd : invalidWindows) Forget(hwnd);

  for (WindowRecord record : missingActiveRecords) {
    if (!record.hwnd || !ContainsWindow(windows_, record.hwnd)) continue;
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
  std::vector<RECT> expectedRects;
  Layout::LayoutMasterStackWeighted(
      workArea, gap, windows_.size(), expectedRects, horizontal,
      ClampDouble(masterRatio_, 0.1, 0.9), stackWeights_);
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
  std::vector<RECT> expectedRects;
  Layout::LayoutGridWeighted(
      workArea, gap, windows_.size(), expectedRects, horizontal, gridWeights_);
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

static bool BuildWorkspaceLayoutPlan(
    const Workspace& workspace, const RECT& workArea, LONG gap,
    std::vector<RECT>* outRects) {
  if (!outRects) return false;
  outRects->assign(workspace.ActiveCount(), RECT{});

  switch (workspace.Layout()) {
    case TileLayout::MasterStack:
      Layout::LayoutMasterStackWeighted(
          workArea, gap, workspace.ActiveCount(), *outRects, false,
          ClampDouble(workspace.MasterRatio(), 0.1, 0.9),
          workspace.StackWeights());
      return true;

    case TileLayout::MasterStackH:
      Layout::LayoutMasterStackWeighted(
          workArea, gap, workspace.ActiveCount(), *outRects, true,
          ClampDouble(workspace.MasterRatio(), 0.1, 0.9),
          workspace.StackWeights());
      return true;

    case TileLayout::Columns:
      Layout::LayoutGridWeighted(
          workArea, gap, workspace.ActiveCount(), *outRects, false,
          workspace.GridWeights());
      return true;

    case TileLayout::Rows:
      Layout::LayoutGridWeighted(
          workArea, gap, workspace.ActiveCount(), *outRects, true,
          workspace.GridWeights());
      return true;

    case TileLayout::BSP:
      Layout::LayoutBSP(workArea, gap, 0, workspace.ActiveCount(), 0, *outRects);
      return true;

    case TileLayout::Monocle:
      outRects->assign(workspace.ActiveCount(), workArea);
      return true;

    case TileLayout::Floating:
    case TileLayout::COUNT:
      outRects->clear();
      return false;
  }
  return false;
}

Workspace::PlacementAction Workspace::ApplyPlacementObservation(
    HWND hwnd, const PlacementObservation& observation) {
  AssertWmThread(L"Workspace::ApplyPlacementObservation");
  WindowRecord* record = FindMutable(hwnd);
  if (!record) return PlacementAction::Forget;

  // placementFailures is a consecutive-failure streak for materially the same
  // authoritative target, not a lifetime bad-window score. Compare against the
  // fixed streak anchor rather than lastRequestedRect so repeated small target
  // changes cannot walk the streak beyond the placement tolerance.
  const bool sameFailureTarget =
      record->placementFailures > 0 &&
      RectsNear(record->placementFailureTarget, observation.requested);

  record->lastRequestedRect = observation.requested;
  record->lastObservedRect = observation.observed;
  record->lastPlacementResult = observation.result;

  switch (observation.result) {
    case PlacementResult::Success:
      record->placementFailures = 0;
      record->placementFailureTarget = {};
      DebugValidateMutation(L"Workspace::ApplyPlacementObservation");
      return PlacementAction::None;

    case PlacementResult::AdjustedByWindow:
    case PlacementResult::AccessDenied:
    case PlacementResult::Refused:
      if (sameFailureTarget) {
        ++record->placementFailures;
      } else {
        record->placementFailures = 1;
        record->placementFailureTarget = observation.requested;
      }
      DebugValidateMutation(L"Workspace::ApplyPlacementObservation");
      return record->placementFailures >= Model::kPlacementFailureLimit
                 ? PlacementAction::Float
                 : PlacementAction::None;

    case PlacementResult::Dead:
      return PlacementAction::Forget;
  }
  return PlacementAction::None;
}

// Renders one workspace from authoritative model state. Arrangement itself is a
// small pipeline: repair local invariants -> compute targets -> apply placement ->
// fold placement outcomes back into the model. Reflow is iterative rather than
// recursive; each continuing pass removes at least one active tile.
static void ArrangeWorkspace(const DesktopMonitorKey& key) {
  ++Diagnostics::g_runtime.counters.arrangeCalls;
  if (!IsWorkspaceOnActiveDesktop(key)) return;

  Workspace workspace;
  if (!g_workspaces.Load(key, &workspace)) return;

  HMONITOR monitor = key.ResolveMonitor();
  if (!monitor) return;  // Detached monitor: keep workspace state dormant.

  RECT workArea{};
  if (!GetWorkspaceWorkArea(monitor, &workArea)) return;

  ObserveAndRepairWorkspaceForArrangement(workspace);
  const LONG gap = GetWorkspaceGapPixels(monitor);
  const size_t maxPasses = workspace.ActiveCount() + 1;
  for (size_t pass = 0; pass < maxPasses; ++pass) {
    ++Diagnostics::g_runtime.counters.arrangePasses;
    if (workspace.Layout() == TileLayout::Floating || workspace.Empty()) {
      if (workspace.Layout() == TileLayout::Floating) {
        for (HWND hwnd : workspace.TiledWindows()) CancelConformanceLease(hwnd);
      }
      g_workspaces.Save(key, workspace);
      return;
    }

    std::vector<RECT> rects;
    ++Diagnostics::g_runtime.counters.layoutPlansBuilt;
    if (!BuildWorkspaceLayoutPlan(workspace, workArea, gap, &rects)) {
      g_workspaces.Save(key, workspace);
      return;
    }

    std::vector<HWND> toFloat;
    std::vector<HWND> toForget;
    const std::vector<HWND> active = workspace.TiledWindows();
    const size_t count = std::min(active.size(), rects.size());
    for (size_t i = 0; i < count; ++i) {
      const HWND hwnd = active[i];
      Workspace::PlacementAction action = Workspace::PlacementAction::None;

      if (!IsWindow(hwnd)) {
        action = Workspace::PlacementAction::Forget;
      } else if (!IsIconic(hwnd) && IsWindowVisible(hwnd) && !IsWindowCloaked(hwnd) &&
                 !IsZoomed(hwnd)) {
        const WindowRecord* record = workspace.Find(hwnd);
        if (!record) {
          action = Workspace::PlacementAction::Forget;
        } else {
          PlacementObservation observation =
              PlaceWindowChecked(hwnd, record->canMove, rects[i]);
          action = workspace.ApplyPlacementObservation(hwnd, observation);
          if (observation.result == PlacementResult::Success &&
              observation.placementIssued &&
              action == Workspace::PlacementAction::None) {
            BeginConformanceLease(hwnd, rects[i]);
          } else if (observation.result != PlacementResult::Success ||
                     action != Workspace::PlacementAction::None) {
            CancelConformanceLease(hwnd);
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
          L"Window %p failed checked placement %d times (last result=%d); switching it to Floating",
          hwnd, record->placementFailures,
          static_cast<int>(record->lastPlacementResult));
      Diagnostics::RecordEvent(
          L"placement failures forced hwnd=%p to per-window Floating (failures=%d result=%d)",
          hwnd, record->placementFailures, static_cast<int>(record->lastPlacementResult));
      CancelConformanceLease(hwnd);
      participationChanged |= workspace.Float(hwnd);
    }

    g_workspaces.Save(key, workspace);
    if (!participationChanged || workspace.Empty()) return;
  }
}

// During a short post-placement lease, an application may accept our rectangle
// synchronously and then restore its own startup/session geometry later. Repair
// only that unexpected late movement; explicit user gestures cancel the lease at
// MOVESIZESTART and normal tiled placement remains the authoritative source.
static bool HandleConformanceLeaseLocationChange(HWND hwnd) {
  ConformanceLease lease;
  if (!ReadActiveConformanceLease(hwnd, &lease)) return false;

  if (!IsWindow(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd) ||
      !IsWindowVisible(hwnd) || IsWindowCloaked(hwnd)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  const std::vector<DesktopMonitorKey> owners = g_workspaces.OwnersOf(hwnd);
  if (owners.size() != 1 || !IsWorkspaceOnActiveDesktop(owners.front())) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  Workspace workspace;
  if (!g_workspaces.Load(owners.front(), &workspace)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  const WindowRecord* record = workspace.Find(hwnd);
  if (!record || record->state != ManageState::Tiled ||
      workspace.Layout() == TileLayout::Floating) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }

  RECT current{};
  if (!GetWindowFrameRect(hwnd, &current)) {
    CancelConformanceLease(hwnd, lease.generation);
    return false;
  }
  if (RectsNear(current, lease.expectedRect)) {
    return true;  // Our own placement/event echo; nothing to repair.
  }

  const ULONGLONG now = GetTickCount64();
  if (lease.lastRepairTickMs &&
      now - lease.lastRepairTickMs < kConformanceRepairMinIntervalMs) {
    const ULONGLONG due =
        lease.lastRepairTickMs + kConformanceRepairMinIntervalMs;
    // Do not simply drop a bursty snap-back event: schedule one delayed re-check
    // so a final app move inside the rate window cannot leave the tile displaced.
    if (DeferConformanceLeaseRepair(hwnd, lease.generation, due)) {
      ++Diagnostics::g_runtime.counters.conformanceLeaseDeferredRepairs;
      return true;
    }
    return false;
  }

  // A copied observation must still refer to the same active lease immediately
  // before we act. MOVESIZESTART/topology/lifecycle cancellation can happen from
  // another thread after the initial read; a generation check prevents stale work
  // from deliberately reviving or accounting against a replacement lease.
  if (!IsCurrentConformanceLease(hwnd, lease.generation)) return false;

  PlacementObservation observation =
      PlaceWindowChecked(hwnd, record->canMove, lease.expectedRect);

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
    // checked attempt at the normal cadence. The existing placement-failure policy
    // remains the authority that eventually Floats a persistently nonconforming
    // window; the lease itself still ends only at its original deadline.
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG due = std::min(
        lease.expiresAtTickMs, now + kConformanceRepairMinIntervalMs);
    DeferConformanceLeaseRepair(hwnd, lease.generation, due);
  }
  return true;
}

static void ProcessPendingConformanceRepairs() {
  AssertWmThread(L"ProcessPendingConformanceRepairs");
  if (g_wm.conformanceTimer) {
    KillTimer(nullptr, g_wm.conformanceTimer);
    g_wm.conformanceTimer = 0;
  }

  const ULONGLONG now = GetTickCount64();
  std::vector<HWND> dueWindows;
  AcquireSRWLockExclusive(&g_conformanceLeases.lock);
  for (auto it = g_conformanceLeases.leases.begin();
       it != g_conformanceLeases.leases.end();) {
    if (now >= it->second.expiresAtTickMs) {
      it = g_conformanceLeases.leases.erase(it);
      continue;
    }
    if (it->second.repairDueTickMs && it->second.repairDueTickMs <= now) {
      dueWindows.push_back(it->first);
      it->second.repairDueTickMs = 0;
    }
    ++it;
  }
  ReleaseSRWLockExclusive(&g_conformanceLeases.lock);

  for (HWND hwnd : dueWindows) {
    HandleConformanceLeaseLocationChange(hwnd);
  }
  ScheduleNextConformanceRepairTimer();
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

