// Workspace snapshots and reverse ownership index.
// Part of MultiWM's single source file; see the include order in src/multiwm/multiwm.cpp.

namespace Model {

bool WorkspaceRepository::Load(
    const DesktopMonitorKey& key, Workspace* workspace) const {
  if (!workspace) return false;
  auto it = states_.find(key);
  if (it == states_.end()) return false;
  *workspace = it->second;
  return true;
}

bool WorkspaceRepository::Contains(const DesktopMonitorKey& key) const {
  return states_.find(key) != states_.end();
}

std::optional<TileLayout> WorkspaceRepository::LayoutOf(const DesktopMonitorKey& key) const {
  const auto it = states_.find(key);
  if (it == states_.end()) return std::nullopt;
  return it->second.Layout();
}

void WorkspaceRepository::Save(
    const DesktopMonitorKey& key, const Workspace& workspace) {
  RuntimeLifecycle::AssertWmThread(L"WorkspaceRepository::Save");
  ++Diagnostics::g_runtime.counters.workspaceSaves;
  workspace.Validate(key, L"repository save");

  // Remove this workspace key from the reverse index for the previous snapshot.
  // Re-indexing the whole small workspace keeps the code simple and guarantees the
  // index follows every semantic mutation made through Workspace.
  auto oldStateIt = states_.find(key);
  if (oldStateIt != states_.end()) {
    for (const auto& record : oldStateIt->second.Records()) {
      auto ownersIt = ownersByWindow_.find(record.first);
      if (ownersIt == ownersByWindow_.end()) continue;

      auto& owners = ownersIt->second;
      std::erase_if(owners, [&](const DesktopMonitorKey& ownerKey) {
        return ownerKey == key;
      });
      if (owners.empty()) ownersByWindow_.erase(ownersIt);
    }
  }

  states_[key] = workspace;
  ++revision_;

  // Add the replacement snapshot to the reverse index. Preserve multiple owner
  // keys if they exist so EnsureUniqueWindowOwnership can still repair old bugs.
  for (const auto& record : workspace.Records()) {
    auto& owners = ownersByWindow_[record.first];
    if (std::none_of(
            owners.begin(), owners.end(),
            [&](const DesktopMonitorKey& ownerKey) {
              return ownerKey == key;
            })) {
      owners.push_back(key);
    }
  }
}

bool WorkspaceRepository::IsTracked(HWND hwnd) const {
  if (!hwnd) return false;
  auto it = ownersByWindow_.find(hwnd);
  return it != ownersByWindow_.end() && !it->second.empty();
}

bool WorkspaceRepository::IsTiled(HWND hwnd) const {
  if (!hwnd) return false;
  auto ownersIt = ownersByWindow_.find(hwnd);
  if (ownersIt == ownersByWindow_.end()) return false;
  for (const auto& key : ownersIt->second) {
    auto stateIt = states_.find(key);
    if (stateIt != states_.end() && stateIt->second.IsTiled(hwnd)) return true;
  }
  return false;
}

bool WorkspaceRepository::IsSuspendedMaximized(HWND hwnd) const {
  if (!hwnd) return false;
  auto ownersIt = ownersByWindow_.find(hwnd);
  if (ownersIt == ownersByWindow_.end()) return false;
  for (const auto& key : ownersIt->second) {
    auto stateIt = states_.find(key);
    if (stateIt == states_.end()) continue;
    const WindowRecord* record = stateIt->second.Find(hwnd);
    if (record && record->state == ManageState::Suspended &&
        record->suspensionReason == SuspensionReason::Maximized) {
      return true;
    }
  }
  return false;
}

std::vector<DesktopMonitorKey> WorkspaceRepository::OwnersOf(HWND hwnd) const {
  if (!hwnd) return {};
  auto it = ownersByWindow_.find(hwnd);
  return it != ownersByWindow_.end() ? it->second : std::vector<DesktopMonitorKey>{};
}

std::vector<HWND> WorkspaceRepository::KnownWindows() const {
  std::vector<HWND> known;
  known.reserve(ownersByWindow_.size());
  for (const auto& entry : ownersByWindow_) {
    if (!entry.second.empty()) known.push_back(entry.first);
  }
  return known;
}

std::vector<std::pair<DesktopMonitorKey, Workspace>>
WorkspaceRepository::Snapshot() const {
  return {states_.begin(), states_.end()};
}

std::vector<std::pair<HWND, std::vector<DesktopMonitorKey>>>
WorkspaceRepository::OwnershipIndexSnapshot() const {
  return {ownersByWindow_.begin(), ownersByWindow_.end()};
}

uint64_t WorkspaceRepository::Revision() const {
  return revision_;
}

} // namespace Model
