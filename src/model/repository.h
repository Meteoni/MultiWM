// Workspace snapshots and reverse ownership index.
// Part of MultiWM's single source file; see the include order in src/multiwm.cpp.

namespace Model {

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
  RuntimeLifecycle::AssertWmThread(L"WorkspaceRepository::Save");
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

} // namespace Model
