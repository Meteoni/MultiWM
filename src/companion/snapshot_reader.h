// Reads MultiWM's companion snapshot (shared/companion_snapshot.h).
// Part of the MultiWM Companion mod; see the include order in src/companion/multiwm-companion.cpp.
//
// Runs on application threads, any number at once, so every access goes through
// one lock. The lock is never held across a call that could run application code.

namespace Companion {

enum class PlacementIntent { Admission, Restore, Arrival, KeepFloating, Unavailable };

// What one placement decision needs from the snapshot, copied out so the lock
// is released before the window is touched.
struct HintInputs {
  uint32_t flags = 0;
  bool tracked = false;  // MultiWM already has a record for the window.
  PlacementIntent intent = PlacementIntent::Admission;
  std::optional<CompanionAbi::WorkspaceHint> workspace;  // The workspace on the window's monitor.
  // Where MultiWM will tile the window once restored (visible frame). Absent
  // when MultiWM can't tell, or the window isn't a suspended member.
  std::optional<RECT> restoreFrame;
  // The window's resize borders as MultiWM measured them while it was restored,
  // with the DPI they belong to. Absent when MultiWM never measured them.
  std::optional<RECT> restoreFrameMargins;
  uint32_t restoreMarginsDpi = 0;
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
  // A consistent copy of the body; reused to keep the snapshot off app stacks.
  std::unique_ptr<CompanionAbi::SnapshotBody> body;
  // Which publish `body` holds. While MultiWM's generation and process ID still
  // match, a check reuses the copy instead of copying the whole body again.
  bool bodyCurrent = false;
  uint64_t bodyGeneration = 0;
  uint32_t bodyWriterPid = 0;
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

// Brings the cached body up to date with the published one. Most checks find it
// current and skip both the body copy and the rules comparison; only a new
// publish costs a full copy. Returns false, with no usable body, for an unknown
// ABI or a writer that stayed busy. Caller holds the lock exclusively.
static bool RefreshBodyLocked() {
  const CompanionAbi::SnapshotSection* section = g_reader.view;
  if (section->magic != CompanionAbi::kMagic || section->abiMajor != CompanionAbi::kAbiMajor) {
    g_reader.bodyCurrent = false;
    return false;
  }
  uint64_t generation = 0;
  uint32_t writerPid = 0;
  if (g_reader.bodyCurrent && CompanionAbi::ReadConsistentGeneration(section, &generation, &writerPid) &&
      generation == g_reader.bodyGeneration && writerPid == g_reader.bodyWriterPid) {
    return true;
  }
  if (!CompanionAbi::ReadConsistentBody(section, g_reader.body.get())) {
    g_reader.bodyCurrent = false;
    return false;
  }
  RefreshRulesLocked(*g_reader.body);
  // Remember which publish this copy is. If another write landed in between,
  // use the copy for this check but don't trust it for the next one.
  const uint64_t copied = g_reader.body->generation;
  g_reader.bodyCurrent = CompanionAbi::ReadConsistentGeneration(section, &generation, &writerPid) &&
                         generation == copied;
  g_reader.bodyGeneration = copied;
  g_reader.bodyWriterPid = writerPid;
  return true;
}

// Fills `*out` for a decision about `hwnd` on the monitor whose rectangle is
// `monitorRect`. Returns false when there is no usable snapshot: MultiWM not
// running, an unknown ABI, or a writer that stayed busy.
static bool ReadHintInputs(HWND hwnd, const RECT& monitorRect, HintInputs* out) {
  AcquireSRWLockExclusive(&g_reader.lock);
  const bool ok = !g_reader.unloading && EnsureMappedLocked() && IsMultiWmRunning() && RefreshBodyLocked();
  if (ok) {
    const CompanionAbi::SnapshotBody& body = *g_reader.body;
    out->flags = body.flags;
    out->tracked = false;
    out->intent = PlacementIntent::Admission;
    out->rules = g_reader.rules;
    out->workspace.reset();
    out->restoreFrame.reset();
    out->restoreFrameMargins.reset();
    out->restoreMarginsDpi = 0;
    const uint32_t count = std::min(body.workspaceCount, CompanionAbi::kMaxWorkspaces);
    uint32_t workspaceIndex = count;
    for (uint32_t i = 0; i < count; ++i) {
      if (EqualRect(&body.workspaces[i].monitorRect, &monitorRect)) {
        out->workspace = body.workspaces[i];
        workspaceIndex = i;
        break;
      }
    }
    const uint32_t windowValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
    const uint32_t trackedCount = std::min(body.trackedCount, CompanionAbi::kMaxTrackedWindows);
    const auto* trackedEnd = body.trackedWindows + trackedCount;
    const auto* window = std::lower_bound(body.trackedWindows, trackedEnd, windowValue,
        [](const auto& entry, uint32_t value) { return entry.hwnd < value; });
    if (window != trackedEnd && window->hwnd == windowValue) {
      out->tracked = true;
      out->intent = PlacementIntent::Unavailable;
      // A stale or ambiguous record must never fall back to new admission.
      if (window->processId == GetCurrentProcessId()) {
        if (window->marginsDpi) {
          out->restoreFrameMargins = window->frameMargins;
          out->restoreMarginsDpi = window->marginsDpi;
        }
        if (window->treatment == CompanionAbi::WindowTreatment::Floating) {
          out->intent = PlacementIntent::KeepFloating;
        } else if (window->treatment == CompanionAbi::WindowTreatment::Tiled && out->workspace &&
                   !IsEqualGUID(window->desktopId, GUID{}) && !IsEqualGUID(body.desktopId, GUID{}) &&
                   window->monitorRect.right > window->monitorRect.left &&
                   window->monitorRect.bottom > window->monitorRect.top) {
          const bool sameWorkspace = IsEqualGUID(window->desktopId, body.desktopId) &&
                                     EqualRect(&window->monitorRect, &out->workspace->monitorRect);
          out->intent = sameWorkspace ? PlacementIntent::Restore : PlacementIntent::Arrival;
        }
      }
    }
    if (out->intent == PlacementIntent::Restore) {
      const uint32_t restoreCount = std::min(body.restoreWindowCount, CompanionAbi::kMaxRestoreWindows);
      for (uint32_t i = 0; i < restoreCount; ++i) {
        const auto& restore = body.restoreWindows[i];
        if (restore.hwnd == windowValue && restore.processId == GetCurrentProcessId() &&
            restore.workspaceIndex == workspaceIndex) {
          if (restore.hasFrame) out->restoreFrame = restore.frame;
          break;
        }
      }
    }
  }
  ReleaseSRWLockExclusive(&g_reader.lock);
  return ok;
}

// What MultiWM's snapshot says about one window: its tracked entry (zero when
// untracked) and whether it has a restore entry. When MultiWM processes a
// window's show, this changes: an unknown window becomes tracked, a hidden
// member loses its restore entry, an arriving one gets its new desktop or
// monitor, an overflowed one becomes floating. Unrelated republishes don't.
struct WindowFacts {
  CompanionAbi::TrackedWindowHint entry{};
  bool restoreEntry = false;

  bool operator==(const WindowFacts& other) const {
    return restoreEntry == other.restoreEntry && std::memcmp(&entry, &other.entry, sizeof(entry)) == 0;
  }
};

// Fills `*out` for `hwnd`. Returns false when there is no usable snapshot.
static bool ReadWindowFacts(HWND hwnd, WindowFacts* out) {
  AcquireSRWLockExclusive(&g_reader.lock);
  const bool ok = !g_reader.unloading && EnsureMappedLocked() && IsMultiWmRunning() && RefreshBodyLocked();
  if (ok) {
    const CompanionAbi::SnapshotBody& body = *g_reader.body;
    *out = {};
    const uint32_t windowValue = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
    const uint32_t trackedCount = std::min(body.trackedCount, CompanionAbi::kMaxTrackedWindows);
    const auto* trackedEnd = body.trackedWindows + trackedCount;
    const auto* window = std::lower_bound(body.trackedWindows, trackedEnd, windowValue,
        [](const auto& entry, uint32_t value) { return entry.hwnd < value; });
    if (window != trackedEnd && window->hwnd == windowValue) out->entry = *window;
    const uint32_t restoreCount = std::min(body.restoreWindowCount, CompanionAbi::kMaxRestoreWindows);
    for (uint32_t i = 0; i < restoreCount && !out->restoreEntry; ++i) {
      out->restoreEntry = body.restoreWindows[i].hwnd == windowValue;
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
  g_reader.bodyCurrent = false;
  g_reader.rules.reset();
  ReleaseSRWLockExclusive(&g_reader.lock);
}

}  // namespace Companion
