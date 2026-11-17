// Reads MultiWM's companion snapshot (shared/companion_snapshot.h).
// Part of the MultiWM Companion mod; see the include order in src/companion/multiwm-companion.cpp.
//
// Runs on application threads, any number at once, so every access goes through
// one lock. The lock is never held across a call that could run application code.

namespace Companion {

// What one placement decision needs from the snapshot, copied out so the lock
// is released before the window is touched.
struct HintInputs {
  uint32_t flags = 0;
  bool tracked = false;  // MultiWM already has a record for the window.
  std::optional<CompanionAbi::WorkspaceHint> workspace;  // The workspace on the window's monitor.
  std::shared_ptr<const std::vector<CompanionAbi::SnapshotRule>> rules;
};

struct SnapshotReader {
  SRWLOCK lock = SRWLOCK_INIT;
  bool unloading = false;
  // Kept open while the mod is loaded: it keeps the section's name alive, so a
  // restarted MultiWM reopens this same section instead of creating a new one.
  HANDLE section = nullptr;
  const CompanionAbi::SnapshotSection* view = nullptr;
  // Opening is retried no sooner than this (GetTickCount64), so a process
  // showing many windows while MultiWM is off doesn't retry each time.
  ULONGLONG nextOpenAttempt = 0;
  // A consistent copy of the body; reused to keep 40 KB off application stacks.
  std::unique_ptr<CompanionAbi::SnapshotBody> body;
  // Decoded rules and the bytes they came from; decoded again only on change.
  std::vector<uint8_t> rulesBytes;
  std::shared_ptr<const std::vector<CompanionAbi::SnapshotRule>> rules;
};

static SnapshotReader g_reader;

constexpr ULONGLONG kOpenRetryMs = 2000;

// MultiWM holds this event while it runs. The section alone can't tell, because
// this process's own handle keeps it alive after MultiWM exits.
static bool IsMultiWmRunning() {
  const HANDLE alive = OpenEventW(SYNCHRONIZE, FALSE, CompanionAbi::kAliveEventName);
  if (!alive) return false;
  CloseHandle(alive);
  return true;
}

// Maps the section if it isn't mapped yet. Caller holds the lock exclusively.
static bool EnsureMappedLocked() {
  if (g_reader.view) return true;
  const ULONGLONG now = GetTickCount64();
  if (now < g_reader.nextOpenAttempt) return false;
  g_reader.nextOpenAttempt = now + kOpenRetryMs;

  const HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, CompanionAbi::kSectionName);
  if (!section) return false;
  auto* view = static_cast<const CompanionAbi::SnapshotSection*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0));
  MEMORY_BASIC_INFORMATION region{};
  if (!view || !VirtualQuery(view, &region, sizeof(region)) ||
      region.RegionSize < sizeof(CompanionAbi::SnapshotSection)) {
    if (view) UnmapViewOfFile(view);
    CloseHandle(section);
    return false;
  }
  g_reader.section = section;
  g_reader.view = view;
  if (!g_reader.body) g_reader.body = std::make_unique<CompanionAbi::SnapshotBody>();
  return true;
}

// Decodes the body's rules unless they are the bytes decoded last time.
// Caller holds the lock exclusively. Leaves `rules` null for malformed data.
static void RefreshRulesLocked(const CompanionAbi::SnapshotBody& body) {
  const uint32_t size = std::min(body.rulesBytes, CompanionAbi::kRulesCapacity);
  if (g_reader.rules && g_reader.rulesBytes.size() == size &&
      std::memcmp(g_reader.rulesBytes.data(), body.rules, size) == 0) {
    return;
  }
  g_reader.rulesBytes.assign(body.rules, body.rules + size);
  auto decoded = std::make_shared<std::vector<CompanionAbi::SnapshotRule>>();
  g_reader.rules = CompanionAbi::DeserializeRules(body.rules, size, decoded.get()) ? decoded : nullptr;
}

static bool IsTrackedIn(const CompanionAbi::SnapshotBody& body, HWND hwnd) {
  const uint32_t value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
  const uint32_t count = std::min(body.trackedCount, CompanionAbi::kMaxTrackedWindows);
  return std::binary_search(body.trackedWindows, body.trackedWindows + count, value);
}

// Fills `*out` for a decision about `hwnd` on the monitor whose rectangle is
// `monitorRect`. Returns false when there is no usable snapshot: MultiWM not
// running, an unknown ABI, or a writer that stayed busy.
static bool ReadHintInputs(HWND hwnd, const RECT& monitorRect, HintInputs* out) {
  AcquireSRWLockExclusive(&g_reader.lock);
  bool ok = !g_reader.unloading && EnsureMappedLocked() && IsMultiWmRunning();
  if (ok) {
    const CompanionAbi::SnapshotSection* section = g_reader.view;
    ok = section->magic == CompanionAbi::kMagic && section->abiMajor == CompanionAbi::kAbiMajor &&
         CompanionAbi::ReadConsistentBody(section, g_reader.body.get());
  }
  if (ok) {
    const CompanionAbi::SnapshotBody& body = *g_reader.body;
    RefreshRulesLocked(body);
    out->flags = body.flags;
    out->tracked = IsTrackedIn(body, hwnd);
    out->rules = g_reader.rules;
    out->workspace.reset();
    const uint32_t count = std::min(body.workspaceCount, CompanionAbi::kMaxWorkspaces);
    for (uint32_t i = 0; i < count; ++i) {
      if (EqualRect(&body.workspaces[i].monitorRect, &monitorRect)) {
        out->workspace = body.workspaces[i];
        break;
      }
    }
  }
  ReleaseSRWLockExclusive(&g_reader.lock);
  return ok;
}

// Unmaps the section. After this, ReadHintInputs always fails. Mod unload.
static void CloseSnapshotReader() {
  AcquireSRWLockExclusive(&g_reader.lock);
  g_reader.unloading = true;
  if (g_reader.view) UnmapViewOfFile(g_reader.view);
  g_reader.view = nullptr;
  if (g_reader.section) CloseHandle(g_reader.section);
  g_reader.section = nullptr;
  g_reader.body.reset();
  g_reader.rules.reset();
  ReleaseSRWLockExclusive(&g_reader.lock);
}

}  // namespace Companion
