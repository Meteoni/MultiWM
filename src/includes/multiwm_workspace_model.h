// Workspace value-model operations and validation.
// Implementation/interface fragment assembled by src/multiwm.cpp in dependency order.

namespace Model {

static void NormalizeMonitorIdentityToken(std::wstring* value) {
  if (!value) return;
  std::transform(
      value->begin(), value->end(), value->begin(),
      [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
}



bool ContainsWindow(const std::vector<HWND>& windows, HWND hwnd) {
  for (HWND w : windows) {
    if (w == hwnd) return true;
  }
  return false;
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
    if (record.alwaysFloating && record.state != ManageState::Floating) {
      fail(L"Always-floating record is not Floating", hwnd);
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

  if (IsGridLayout(layout_)) {
    if (gridWeights_.size() != windows_.size()) {
      fail(L"grid weight count does not match active tiled count");
    }
    if (!stackWeights_.empty()) fail(L"grid layout retains stack weights");
    if (!dwindleRatios_.empty()) fail(L"grid layout retains Dwindle ratios");
  } else if (IsMasterStackLayout(layout_)) {
    const size_t expected = windows_.empty() ? 0 : windows_.size() - 1;
    if (stackWeights_.size() != expected) {
      fail(L"stack weight count does not match non-master tiled count");
    }
    if (!gridWeights_.empty()) fail(L"master/stack layout retains grid weights");
    if (!dwindleRatios_.empty()) fail(L"master/stack layout retains Dwindle ratios");
  } else if (layout_ == TileLayout::Dwindle) {
    const size_t expected = windows_.size();
    if (dwindleRatios_.size() != expected) {
      fail(L"Dwindle outgoing ratio count does not match active slot count");
    }
    for (double ratio : dwindleRatios_) {
      if (!std::isfinite(ratio) || ratio < 0.1 || ratio > 0.9) {
        fail(L"Dwindle ratio is outside the supported range");
        break;
      }
    }
    if (!stackWeights_.empty() || !gridWeights_.empty()) {
      fail(L"Dwindle layout retains unrelated layout weights");
    }
  } else if (!stackWeights_.empty() || !gridWeights_.empty() ||
             !dwindleRatios_.empty()) {
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

static bool SameConstraintGeometry(
    const WindowConstraints& a, const WindowConstraints& b) {
  return a.minWidth == b.minWidth &&
         a.minHeight == b.minHeight &&
         a.dpi == b.dpi &&
         a.valid == b.valid;
}

static WindowConstraints MergeConstraintProbeWithCache(
    const WindowConstraints& cached, WindowConstraints observed) {
  if (!observed.valid && cached.valid) {
    const DWORD error = observed.lastProbeError;
    observed = cached;
    observed.lastProbeError = error;
  }
  return observed;
}

Workspace::PlacementAction Workspace::ApplyPlacementObservation(
    HWND hwnd, const PlacementObservation& observation) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::ApplyPlacementObservation");
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

//
// Mutators run on the WM thread and update this value object only; callers save
// the resulting Workspace through the repository. Native window queries and
// placement policy live in observation, reconciliation and arrangement.

// Queries, observation refresh and admission.
bool Workspace::HasRecord(HWND hwnd) const {
  return hwnd && records_.find(hwnd) != records_.end();
}

const WindowRecord* Workspace::Find(HWND hwnd) const {
  auto it = records_.find(hwnd);
  return it == records_.end() ? nullptr : &it->second;
}

WindowRecord* Workspace::FindMutable(HWND hwnd) {
  auto it = records_.find(hwnd);
  return it == records_.end() ? nullptr : &it->second;
}

bool Workspace::IsTiled(HWND hwnd) const {
  const WindowRecord* record = Find(hwnd);
  return record && record->state == ManageState::Tiled &&
         std::find(windows_.begin(), windows_.end(), hwnd) != windows_.end();
}

size_t Workspace::TiledIndex(HWND hwnd) const {
  auto it = std::find(windows_.begin(), windows_.end(), hwnd);
  return it == windows_.end() ? static_cast<size_t>(-1)
                              : static_cast<size_t>(it - windows_.begin());
}

size_t Workspace::LogicalIndex(HWND hwnd) const {
  const WindowRecord* record = Find(hwnd);
  if (!record) return static_cast<size_t>(-1);
  if (record->state == ManageState::Suspended && record->hasSavedSlot) {
    return record->savedSlot.index;
  }
  if (record->state == ManageState::Tiled) {
    const size_t activeIndex = TiledIndex(hwnd);
    if (activeIndex != static_cast<size_t>(-1)) {
      return ActiveIndexToLogicalIndex(activeIndex);
    }
  }
  return static_cast<size_t>(-1);
}

bool Workspace::RememberFocusedWindow(HWND hwnd) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::RememberFocusedWindow");
  if (!hwnd || !HasRecord(hwnd) || lastFocusedWindow_ == hwnd) return false;
  lastFocusedWindow_ = hwnd;
  return true;
}

bool Workspace::UpdateConstraints(
    HWND hwnd, const WindowConstraints& constraints) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::UpdateConstraints");
  WindowRecord* record = FindMutable(hwnd);
  if (!record) return false;

  if (!record->tilingGeneration && record->state != ManageState::Floating) {
    record->tilingGeneration = NextTilingGeneration();
  }
  const WindowConstraints& old = record->constraints;
  const bool changed =
      old.minWidth != constraints.minWidth ||
      old.minHeight != constraints.minHeight ||
      old.dpi != constraints.dpi ||
      old.valid != constraints.valid ||
      old.lastProbeError != constraints.lastProbeError;
  record->constraints = constraints;
  return changed;
}

bool Workspace::HasSuspended() const {
  return std::any_of(
      records_.begin(), records_.end(), [](const auto& kv) {
        return kv.second.state == ManageState::Suspended;
      });
}

bool Workspace::AllTiledVisible(const std::vector<HWND>& snapshot) const {
  return std::all_of(
      windows_.begin(), windows_.end(),
      [&](HWND hwnd) { return ContainsWindow(snapshot, hwnd); });
}

// Refresh observation-owned fields only. Participation, saved slots, admission
// bookkeeping and previous placement outcomes belong to the model, not the probe.
// Keep a valid cached constraint report and remembered floating frame when an
// incoming observation cannot replace them.
WindowRecord& Workspace::UpsertRecord(WindowRecord record) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::UpsertRecord");
  auto [it, inserted] = records_.emplace(record.hwnd, record);
  if (inserted) return it->second;

  WindowRecord& existing = it->second;
  existing.hwnd = record.hwnd;
  if (record.pid) existing.pid = record.pid;
  existing.canMove = record.canMove;
  existing.canResize = record.canResize;
  existing.topmost = record.topmost;
  existing.automaticOverflowMonitorFixed = record.automaticOverflowMonitorFixed;
  if (record.constraints.valid || !existing.constraints.valid) {
    existing.constraints = record.constraints;
  }
  if (!existing.hasFloatingRect) {
    existing.floatingRect = record.floatingRect;
    existing.floatingMonitor = record.floatingMonitor;
    existing.floatingDpi = record.floatingDpi;
    existing.hasFloatingRect = record.hasFloatingRect;
  }
  return existing;
}

bool Workspace::ActivateTiled(HWND hwnd) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::ActivateTiled");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || record->alwaysFloating) return false;

  const ManageState previousState = record->state;
  bool changed = false;
  if (record->pendingDefaultFloatingCenter) {
    record->pendingDefaultFloatingCenter = false;
    changed = true;
  }
  if (record->state != ManageState::Tiled || record->hasSavedSlot ||
      record->suspensionReason != SuspensionReason::None) {
    record->state = ManageState::Tiled;
    record->suspensionReason = SuspensionReason::None;
    record->savedSlot = {};
    record->hasSavedSlot = false;
    if (previousState == ManageState::Floating ||
        previousState == ManageState::Ignored) {
      record->tilingGeneration = NextTilingGeneration();
      record->pendingInsertionPlacement = true;
      // Explicitly re-tiling a floating window is a new placement attempt, but
      // the app's last valid native minimum remains authoritative. If it changed
      // while floating, a corrected placement will trigger a bounded re-probe.
    } else if (!record->tilingGeneration) {
      record->tilingGeneration = NextTilingGeneration();
    }
    changed = true;
  }
  if (!ContainsWindow(windows_, hwnd)) {
    AppendTiledWithDefaultWeight(hwnd);
    record->pendingInsertionPlacement = true;
    changed = true;
  }
  if (changed) DebugValidateMutation(L"Workspace::ActivateTiled");
  return changed;
}

bool Workspace::AdmitFloating(WindowRecord record) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::AdmitFloating");
  const HWND hwnd = record.hwnd;
  if (!hwnd || HasRecord(hwnd)) return false;

  record.state = ManageState::Floating;
  record.tilingGeneration = 0;
  record.pendingInsertionPlacement = false;
  record.suspensionReason = SuspensionReason::None;
  record.savedSlot = {};
  record.hasSavedSlot = false;
  records_.emplace(hwnd, std::move(record));
  DebugValidateMutation(L"Workspace::AdmitFloating");
  return true;
}

bool Workspace::AdmitTiled(WindowRecord record) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::AdmitTiled");
  if (!record.hwnd) return false;
  const HWND hwnd = record.hwnd;
  const bool existed = HasRecord(hwnd);
  if (!existed && !record.tilingGeneration) {
    record.tilingGeneration = NextTilingGeneration();
  }
  UpsertRecord(std::move(record));
  const bool activated = ActivateTiled(hwnd);
  return !existed || activated;
}

// Fresh Automatic admission can insert relative to the retained managed focus.
// Shift suspended logical slots first so every member keeps a unique position.
bool Workspace::AdmitTiledAfter(WindowRecord record, HWND anchor) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::AdmitTiledAfter");
  const HWND hwnd = record.hwnd;
  if (!hwnd || HasRecord(hwnd) || !IsTiled(anchor)) {
    return AdmitTiled(std::move(record));
  }

  const size_t anchorActiveIndex = TiledIndex(anchor);
  if (anchorActiveIndex == static_cast<size_t>(-1)) {
    return AdmitTiled(std::move(record));
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

  EnsureWeights();
  const size_t insertionActiveIndex =
      std::min(anchorActiveIndex + 1, windows_.size());
  windows_.insert(windows_.begin() + insertionActiveIndex, hwnd);

  if (IsGridLayout(layout_)) {
    gridWeights_.insert(
        gridWeights_.begin() + std::min(insertionActiveIndex, gridWeights_.size()),
        insertionActiveIndex < inheritedSlotWeights_.size() ? inheritedSlotWeights_[insertionActiveIndex] : 1.0);
  } else if (IsMasterStackLayout(layout_) &&
             insertionActiveIndex > 0) {
    const size_t stackIndex =
        std::min(insertionActiveIndex - 1, stackWeights_.size());
    stackWeights_.insert(stackWeights_.begin() + stackIndex,
        stackIndex < inheritedSlotWeights_.size() ? inheritedSlotWeights_[stackIndex] : 1.0);
  } else if (layout_ == TileLayout::Dwindle) {
    const size_t ratioIndex =
        std::min(insertionActiveIndex, dwindleRatios_.size());
    dwindleRatios_.insert(dwindleRatios_.begin() + ratioIndex,
        ratioIndex < inheritedSlotWeights_.size() ? inheritedSlotWeights_[ratioIndex] : 0.5);
  }

  record.state = ManageState::Tiled;
  record.pendingInsertionPlacement = true;
  record.suspensionReason = SuspensionReason::None;
  record.savedSlot = {};
  record.hasSavedSlot = false;
  if (!record.tilingGeneration) {
    record.tilingGeneration = NextTilingGeneration();
  }
  records_.emplace(hwnd, std::move(record));
  EnsureWeights();
  DebugValidateMutation(L"Workspace::AdmitTiledAfter");
  return true;
}

bool Workspace::RememberFloatingGeometry(
    HWND hwnd, const RECT& frame, HMONITOR monitor, UINT dpi) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::RememberFloatingGeometry");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || !monitor || frame.right <= frame.left ||
      frame.bottom <= frame.top) {
    return false;
  }

  const UINT normalizedDpi = dpi ? dpi : 96;
  if (record->hasFloatingRect &&
      EqualRect(&record->floatingRect, &frame) &&
      record->floatingMonitor == monitor &&
      record->floatingDpi == normalizedDpi) {
    return false;
  }

  record->floatingRect = frame;
  record->floatingMonitor = monitor;
  record->floatingDpi = normalizedDpi;
  record->hasFloatingRect = true;
  DebugValidateMutation(L"Workspace::RememberFloatingGeometry");
  return true;
}

// Pure model import: interprets an already-observed geometry snapshot as
// authoritative workspace ordering and resize weights. No Win32 queries happen
// inside Workspace.
// Logical slots, suspension, layout changes and rollback.
void Workspace::EnsureWeights() {
  if (IsMasterStackLayout(layout_)) {
    const size_t stackCount = windows_.empty() ? 0 : windows_.size() - 1;
    if (stackWeights_.size() != stackCount) stackWeights_ = Layout::DefaultWeights(stackCount);
    gridWeights_.clear();
    dwindleRatios_.clear();
  } else if (IsGridLayout(layout_)) {
    if (gridWeights_.size() != windows_.size()) gridWeights_ = Layout::DefaultWeights(windows_.size());
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::RemoveActiveAt");
  if (index >= windows_.size()) return;

  SuspendedSlot slot{};
  slot.index = index;
  if (IsGridLayout(layout_)) {
    if (index < gridWeights_.size()) {
      slot.weight = gridWeights_[index];
      slot.hasWeight = true;
      gridWeights_.erase(gridWeights_.begin() + index);
    }
  } else if (IsMasterStackLayout(layout_)) {
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::CloseLogicalGapAfterRemoval");
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::Suspend");
  WindowRecord* existing = FindMutable(hwnd);
  if (existing && existing->state == ManageState::Suspended) {
    if (existing->suspensionReason == reason) return false;
    if (reason == SuspensionReason::Maximized &&
        (existing->suspensionReason == SuspensionReason::Minimized ||
         existing->suspensionReason == SuspensionReason::Hidden)) {
      existing->tilingGeneration = NextTilingGeneration();
    }
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

size_t Workspace::LogicalWindowCount() const {
  size_t count = windows_.size();
  for (const auto& kv : records_) {
    if (kv.second.state == ManageState::Suspended && kv.second.hasSavedSlot) ++count;
  }
  return count;
}

size_t Workspace::VisibleWindowCount() const {
  if (layout_ == TileLayout::Floating) return 0;
  size_t count = windows_.size();
  for (const auto& member : records_) {
    const WindowRecord& record = member.second;
    if (record.state == ManageState::Suspended && record.hasSavedSlot &&
        record.suspensionReason == SuspensionReason::Maximized) ++count;
  }
  return count;
}

Workspace Workspace::EmptyWithInheritedLayoutSettings() const {
  Workspace empty;
  empty.layout_ = layout_;
  empty.masterRatio_ = masterRatio_;
  if (IsMasterStackLayout(layout_)) {
    empty.inheritedSlotWeights_ = stackWeights_;
  } else if (IsGridLayout(layout_)) {
    empty.inheritedSlotWeights_ = gridWeights_;
  } else if (layout_ == TileLayout::Dwindle) {
    empty.inheritedSlotWeights_ = dwindleRatios_;
  }
  if (empty.inheritedSlotWeights_.size() < inheritedSlotWeights_.size()) {
    empty.inheritedSlotWeights_.insert(empty.inheritedSlotWeights_.end(),
        inheritedSlotWeights_.begin() + empty.inheritedSlotWeights_.size(), inheritedSlotWeights_.end());
  }
  return empty;
}

bool Workspace::AdmitInitial(
    WindowRecord record, SuspensionReason reason) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::AdmitInitial");
  const HWND hwnd = record.hwnd;
  if (!hwnd || HasRecord(hwnd) || reason == SuspensionReason::Hidden) return false;
  if (!record.tilingGeneration) {
    record.tilingGeneration = NextTilingGeneration();
  }

  record.state = reason == SuspensionReason::None
                     ? ManageState::Tiled
                     : ManageState::Suspended;
  record.pendingInsertionPlacement = true;
  if (reason == SuspensionReason::None) {
    AppendTiledWithDefaultWeight(hwnd);
  } else {
    record.suspensionReason = reason;
    record.savedSlot = MakeAppendedSuspendedSlot();
    record.hasSavedSlot = true;
  }

  records_.emplace(hwnd, std::move(record));
  EnsureWeights();
  DebugValidateMutation(L"Workspace::AdmitInitial");
  return true;
}

bool Workspace::AdmitInitialAfter(
    WindowRecord record, SuspensionReason reason, HWND anchor) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::AdmitInitialAfter");
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
    record.tilingGeneration = NextTilingGeneration();
  }
  record.state = ManageState::Suspended;
  record.pendingInsertionPlacement = true;
  record.suspensionReason = reason;
  record.savedSlot.index = insertionLogicalIndex;
  if (IsGridLayout(layout_)) {
    record.savedSlot.weight = 1.0;
    record.savedSlot.hasWeight = true;
  } else if (IsMasterStackLayout(layout_)) {
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::Restore");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || record->state != ManageState::Suspended || !record->hasSavedSlot) {
    return false;
  }

  const SuspendedSlot slot = record->savedSlot;
  const bool returnedToVisibleSet = record->suspensionReason == SuspensionReason::Minimized ||
                                   record->suspensionReason == SuspensionReason::Hidden;
  if (returnedToVisibleSet || !record->tilingGeneration) {
    record->tilingGeneration = NextTilingGeneration();
  }
  const size_t index = std::min(
      LogicalIndexToActiveIndex(slot.index, hwnd), windows_.size());
  windows_.insert(windows_.begin() + index, hwnd);

  if (IsGridLayout(layout_)) {
    if (gridWeights_.size() + 1 == windows_.size()) {
      gridWeights_.insert(
          gridWeights_.begin() + std::min(index, gridWeights_.size()),
          slot.hasWeight ? slot.weight : 1.0);
    }
  } else if (IsMasterStackLayout(layout_)) {
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
        slot.hasWeight ? Layout::ClampSplitRatio(slot.weight) : 0.5);
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::Float");
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::Forget");
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::AppendTiledWithDefaultWeight");
  EnsureWeights();
  const size_t oldCount = windows_.size();
  windows_.push_back(hwnd);
  if (IsGridLayout(layout_)) {
    gridWeights_.push_back(oldCount < inheritedSlotWeights_.size() ? inheritedSlotWeights_[oldCount] : 1.0);
  } else if (IsMasterStackLayout(layout_) &&
             oldCount > 0) {
    stackWeights_.push_back(oldCount - 1 < inheritedSlotWeights_.size() ? inheritedSlotWeights_[oldCount - 1] : 1.0);
  } else if (layout_ == TileLayout::Dwindle) {
    dwindleRatios_.push_back(oldCount < inheritedSlotWeights_.size() ? inheritedSlotWeights_[oldCount] : 0.5);
  }
}

SuspendedSlot Workspace::MakeAppendedSuspendedSlot() const {
  SuspendedSlot slot{};
  slot.index = LogicalWindowCount();
  if (IsGridLayout(layout_)) {
    slot.weight = slot.index < inheritedSlotWeights_.size() ? inheritedSlotWeights_[slot.index] : 1.0;
    slot.hasWeight = true;
  } else if (IsMasterStackLayout(layout_)) {
    if (slot.index == 0) slot.wasMaster = true;
    else {
      slot.weight = slot.index - 1 < inheritedSlotWeights_.size() ? inheritedSlotWeights_[slot.index - 1] : 1.0;
      slot.hasWeight = true;
    }
  } else if (layout_ == TileLayout::Dwindle) {
    slot.weight = slot.index < inheritedSlotWeights_.size() ? inheritedSlotWeights_[slot.index] : 0.5;
    slot.hasWeight = true;
  }
  return slot;
}

bool Workspace::ExtractForMigration(HWND hwnd, WindowRecord* outRecord) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::ExtractForMigration");
  const WindowRecord* record = Find(hwnd);
  if (!record) return false;
  if (outRecord) *outRecord = *record;
  return Forget(hwnd);
}

void Workspace::AdmitMigrated(WindowRecord record, HWND anchor) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::AdmitMigrated");
  const HWND hwnd = record.hwnd;
  if (!hwnd) return;
  if (record.state == ManageState::Tiled ||
      record.state == ManageState::Suspended) {
    // A destination workspace admission is newer than its existing members and
    // receives the first opportunity to find a viable destination slot.
    record.tilingGeneration = NextTilingGeneration();
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::MergeRememberedFloatingGeometryFrom");
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::MergeNonTiledRecordsFrom");
  for (const auto& kv : source.records_) {
    if (kv.second.state != ManageState::Tiled) records_[kv.first] = kv.second;
  }
  DebugValidateMutation(L"Workspace::MergeNonTiledRecordsFrom");
}

void Workspace::RememberCurrentLayoutGeometry() {
  RuntimeLifecycle::AssertWmThread(L"Workspace::RememberCurrentLayoutGeometry");

  if (IsMasterStackLayout(layout_)) {
    rememberedMasterStackOrder_ = windows_;
    rememberedMasterStackWeights_ = stackWeights_;
  } else if (IsGridLayout(layout_)) {
    rememberedGridOrder_ = windows_;
    rememberedGridWeights_ = gridWeights_;
  } else if (layout_ == TileLayout::Dwindle) {
    rememberedDwindleOrder_ = windows_;
    rememberedDwindleRatios_ = dwindleRatios_;
  }
}

void Workspace::RestoreRememberedLayoutGeometry() {
  RuntimeLifecycle::AssertWmThread(L"Workspace::RestoreRememberedLayoutGeometry");

  if (IsMasterStackLayout(layout_)) {
    const size_t expected = windows_.empty() ? 0 : windows_.size() - 1;
    if (rememberedMasterStackOrder_ == windows_ &&
        rememberedMasterStackWeights_.size() == expected) {
      stackWeights_ = rememberedMasterStackWeights_;
    } else {
      stackWeights_.clear();
    }
  } else if (IsGridLayout(layout_)) {
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::ResetSuspendedLayoutHints");
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::SetLayout");
  if (layout_ == layout) return;

  RememberCurrentLayoutGeometry();
  layout_ = layout;
  inheritedSlotWeights_.clear();
  ResetSuspendedLayoutHints();
  RestoreRememberedLayoutGeometry();
  EnsureWeights();
  DebugValidateMutation(L"Workspace::SetLayout");
}

bool Workspace::CycleLayout(const std::vector<TileLayout>& cycle) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::CycleLayout");
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::SetMasterRatio");
  masterRatio_ = Layout::ClampSplitRatio(ratio);
  DebugValidateMutation(L"Workspace::SetMasterRatio");
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
      inheritedSlotWeights_ == other.inheritedSlotWeights_ &&
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

// Both rollback policies validate membership before copying geometry. Window
// identity, constraints, placement outcomes and floating frames stay live.
void Workspace::CopyLayoutGeometryFrom(const Workspace& source) {
  windows_ = source.windows_;
  layout_ = source.layout_;
  masterRatio_ = source.masterRatio_;
  inheritedSlotWeights_ = source.inheritedSlotWeights_;
  stackWeights_ = source.stackWeights_;
  gridWeights_ = source.gridWeights_;
  dwindleRatios_ = source.dwindleRatios_;
  rememberedMasterStackOrder_ = source.rememberedMasterStackOrder_;
  rememberedMasterStackWeights_ = source.rememberedMasterStackWeights_;
  rememberedGridOrder_ = source.rememberedGridOrder_;
  rememberedGridWeights_ = source.rememberedGridWeights_;
  rememberedDwindleOrder_ = source.rememberedDwindleOrder_;
  rememberedDwindleRatios_ = source.rememberedDwindleRatios_;
}

bool Workspace::RestoreLayoutStateFrom(const Workspace& source) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::RestoreLayoutStateFrom");
  if (!HasSameWindowMembershipAs(source)) return false;

  CopyLayoutGeometryFrom(source);
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::RestoreCompatibleActiveLayoutStateFrom");
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

  CopyLayoutGeometryFrom(source);

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
  RuntimeLifecycle::AssertWmThread(L"Workspace::MakeMaster");
  if (layout_ == TileLayout::Floating || windows_.size() < 2) return false;
  auto it = std::find(windows_.begin(), windows_.end(), hwnd);
  if (it == windows_.end() || it == windows_.begin()) return false;
  if (oldMaster) *oldMaster = windows_.front();
  std::iter_swap(windows_.begin(), it);
  DebugValidateMutation(L"Workspace::MakeMaster");
  return true;
}

bool Workspace::SwapTiled(HWND first, HWND second) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::SwapTiled");
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::MovePendingAdmissionToIndex");
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

  if (IsGridLayout(layout_) &&
      gridWeights_.size() == oldCount) {
    gridWeights_.erase(gridWeights_.begin() + currentIndex);
  } else if (IsMasterStackLayout(layout_) &&
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
  if (IsGridLayout(layout_)) {
    gridWeights_.insert(gridWeights_.begin() + targetIndex, 1.0);
  } else if (IsMasterStackLayout(layout_) &&
             oldCount > 1) {
    const size_t stackIndex = targetIndex == 0 ? 0 : targetIndex - 1;
    stackWeights_.insert(stackWeights_.begin() + stackIndex, 1.0);
  }

  EnsureWeights();
  DebugValidateMutation(L"Workspace::MovePendingAdmissionToIndex");
  return true;
}

bool Workspace::ClearInsertionPlacement(HWND hwnd) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::ClearInsertionPlacement");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || !record->pendingInsertionPlacement) return false;
  record->pendingInsertionPlacement = false;
  DebugValidateMutation(L"Workspace::ClearInsertionPlacement");
  return true;
}

bool Workspace::ClearDefaultFloatingCenter(HWND hwnd) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::ClearDefaultFloatingCenter");
  WindowRecord* record = FindMutable(hwnd);
  if (!record || !record->pendingDefaultFloatingCenter) return false;
  record->pendingDefaultFloatingCenter = false;
  DebugValidateMutation(L"Workspace::ClearDefaultFloatingCenter");
  return true;
}

bool Workspace::EnforceAlwaysFloating(HWND hwnd) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::EnforceAlwaysFloating");
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
  RuntimeLifecycle::AssertWmThread(L"Workspace::RepairForArrangement");
  for (HWND hwnd : invalidWindows) Forget(hwnd);

  for (WindowRecord record : missingActiveRecords) {
    if (!record.hwnd || !ContainsWindow(windows_, record.hwnd)) continue;
    if (!record.tilingGeneration) {
      record.tilingGeneration = NextTilingGeneration();
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

// Import preferences from captured geometry; never queries live windows.
Workspace Workspace::AdoptGeometry(
    TileLayout layout, const RECT& workArea, LONG gap,
    double fallbackMasterRatio,
    const std::vector<WorkspaceGeometryItem>& items, HWND preferredFirstWin) {
  RuntimeLifecycle::AssertWmThread(L"Workspace::AdoptGeometry");
  Workspace workspace;
  workspace.layout_ = layout;
  for (const auto& item : items) {
    WindowRecord record = item.record;
    if (!record.tilingGeneration) {
      record.tilingGeneration = NextTilingGeneration();
    }
    workspace.windows_.push_back(record.hwnd);
    workspace.records_.emplace(record.hwnd, std::move(record));
  }
  if (items.empty()) return workspace;

  if (IsMasterStackLayout(layout)) {
    const bool horizontal = layout == TileLayout::MasterStackH;
    struct WindowInfo {
      HWND hwnd;
      RECT rect;
    };
    std::vector<WindowInfo> infos;
    infos.reserve(items.size());
    for (const auto& item : items) infos.push_back({item.record.hwnd, item.frame});

    struct Candidate {
      long long score;
      long long axisPos;
      size_t index;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(infos.size());

    for (size_t i = 0; i < infos.size(); ++i) {
      const RECT& r = infos[i].rect;
      long long d[4] = {
          std::llabs(static_cast<long long>(r.left) - workArea.left),
          std::llabs(static_cast<long long>(r.top) - workArea.top),
          std::llabs(static_cast<long long>(r.right) - workArea.right),
          std::llabs(static_cast<long long>(r.bottom) - workArea.bottom),
      };
      std::sort(d, d + 4);
      Candidate candidate{
          d[0] + d[1] + d[2],
          horizontal
              ? std::llabs(static_cast<long long>(r.top) - workArea.top)
              : std::llabs(static_cast<long long>(r.left) - workArea.left),
          i};
      candidates.push_back(candidate);
    }

    std::stable_sort(
        candidates.begin(), candidates.end(),
        [preferredFirstWin, &infos](const Candidate& a, const Candidate& b) {
          const bool aPreferred = preferredFirstWin && infos[a.index].hwnd == preferredFirstWin;
          const bool bPreferred = preferredFirstWin && infos[b.index].hwnd == preferredFirstWin;
          if (aPreferred != bPreferred) return aPreferred;
          if (a.score != b.score) return a.score < b.score;
          return a.axisPos < b.axisPos;
        });

    const size_t masterIndex = candidates.front().index;
    const WindowInfo master = infos[masterIndex];
    std::vector<WindowInfo> stack;
    stack.reserve(infos.size() - 1);
    for (size_t i = 0; i < infos.size(); ++i) {
      if (i != masterIndex) stack.push_back(infos[i]);
    }
    std::stable_sort(
        stack.begin(), stack.end(), [horizontal](const WindowInfo& a, const WindowInfo& b) {
          return horizontal ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
        });

    workspace.windows_.clear();
    workspace.windows_.push_back(master.hwnd);
    for (const auto& item : stack) workspace.windows_.push_back(item.hwnd);

    const LONG totalSize = horizontal ? workArea.bottom - workArea.top
                                      : workArea.right - workArea.left;
    if (totalSize > gap + 1) {
      LONG masterSize = horizontal ? master.rect.bottom - master.rect.top
                                   : master.rect.right - master.rect.left;
      if (masterSize < 1) masterSize = 1;
      workspace.masterRatio_ = Layout::ClampSplitRatio(static_cast<double>(masterSize) / static_cast<double>(totalSize - gap));
    } else {
      workspace.masterRatio_ = Layout::ClampSplitRatio(fallbackMasterRatio);
    }

    workspace.stackWeights_.clear();
    for (const auto& item : stack) {
      LONG size = horizontal ? item.rect.right - item.rect.left
                             : item.rect.bottom - item.rect.top;
      if (size < 1) size = 1;
      workspace.stackWeights_.push_back(static_cast<double>(size));
    }
  } else if (IsGridLayout(layout)) {
    const bool horizontal = layout == TileLayout::Rows;
    struct WindowInfo {
      HWND hwnd;
      RECT rect;
    };
    std::vector<WindowInfo> infos;
    infos.reserve(items.size());
    for (const auto& item : items) infos.push_back({item.record.hwnd, item.frame});

    std::stable_sort(
        infos.begin(), infos.end(), [horizontal](const WindowInfo& a, const WindowInfo& b) {
          return horizontal ? a.rect.top < b.rect.top : a.rect.left < b.rect.left;
        });

    workspace.windows_.clear();
    workspace.gridWeights_.clear();
    for (const auto& item : infos) {
      workspace.windows_.push_back(item.hwnd);
      LONG size = horizontal ? item.rect.bottom - item.rect.top
                             : item.rect.right - item.rect.left;
      if (size < 1) size = 1;
      workspace.gridWeights_.push_back(static_cast<double>(size));
    }
  }

  workspace.EnsureWeights();
  workspace.DebugValidateMutation(L"Workspace::AdoptGeometry");
  return workspace;
}

} // namespace Model
