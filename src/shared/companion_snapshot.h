// The read-only snapshot MultiWM publishes for MultiWM Companion.
//
// MultiWM (64-bit, one writer) fills a named shared-memory section; the companion
// (loaded into every process, 32- or 64-bit, many readers) maps it read-only.
// Nothing ever flows back to MultiWM.
//
// Both mods compile this file: only Windows and the standard library here.
// Include shared/window_rules.h and shared/split_geometry.h first.
//
// ABI rules: every field has a fixed width and offset, checked below for both
// bitnesses. A change that moves or reinterprets a field bumps kAbiMajor and the
// object names; appending into reserved space bumps kAbiMinor.

#ifndef MULTIWM_SHARED_COMPANION_SNAPSHOT_H
#define MULTIWM_SHARED_COMPANION_SNAPSHOT_H

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace CompanionAbi {

// The section exists while MultiWM or any reader holds a handle, so it outlives a
// MultiWM restart; a restarted MultiWM reopens it and rewrites the header.
inline constexpr wchar_t kSectionName[] = L"Local\\MultiWM.Companion.Snapshot.v6";
// Held open by MultiWM only. Readers probe it to tell a live MultiWM from a
// section that only their own handles keep alive.
inline constexpr wchar_t kAliveEventName[] = L"Local\\MultiWM.Companion.Alive.v6";

inline constexpr uint32_t kMagic = 0x5343574D;  // "MWCS"
inline constexpr uint16_t kAbiMajor = 6;
inline constexpr uint16_t kAbiMinor = 0;

inline constexpr uint32_t kMaxWorkspaces = 16;
inline constexpr uint32_t kMaxTrackedWindows = 2048;
inline constexpr uint32_t kRulesCapacity = 32768;
inline constexpr uint32_t kMaxRestoreWindows = 128;
inline constexpr uint32_t kMaxBspArrivalTiles = 4;
inline constexpr uint32_t kMaxBspPathSplits = 16;

enum SnapshotFlags : uint32_t {
  // MultiWM's WM thread is running and the rest of the body is meaningful.
  kFlagRunning = 1u << 0,
  // Automatic management: unknown windows are admitted, so hints apply.
  kFlagAutomaticMode = 1u << 1,
  // Every rule the companion needs fit into `rules`. Without it a missing
  // Exclude rule could be ignored, so readers give no hints.
  kFlagRulesComplete = 1u << 2,
  // Every window MultiWM knows is listed in `trackedWindows`.
  kFlagTrackedComplete = 1u << 3,
  // A window Automatic overflow floats is centred at its workspace's
  // floatingFrame (FailureCenter). Clear when overflow routes such windows to
  // another monitor or desktop instead; they then float where they are. With
  // one display and desktop creation off, monitor-only routing centres directly.
  kFlagOverflowCentersFloat = 1u << 4,
};

// The current desktop's workspace on one monitor. All rectangles are physical
// screen pixels; frames are visible DWM frames, not GetWindowRect rectangles.
struct WorkspaceHint {
  RECT monitorRect;    // Readers find the workspace by monitor rectangle.
  RECT workArea;       // What MultiWM plans in: monitor work area minus insets.
  RECT tiledFrame;     // Valid when hasTiledFrame: the next tiled slot.
  // Valid when hasFloatingFrame: the default floating size centred in the work
  // area, before size rules. Where a new window goes in the Floating layout, and
  // where overflow centres a window it floats (see kFlagOverflowCentersFloat).
  RECT floatingFrame;
  uint32_t dpi;        // Effective DPI of the monitor.
  uint8_t floatingLayout;    // The workspace uses the Floating layout.
  uint8_t hasTiledFrame;
  uint8_t hasFloatingFrame;
  // Tiled layouts: the next new window will float regardless of its size,
  // because the limit is reached or even a minimal window cannot fit.
  uint8_t newWindowFloats;
};

enum class WindowTreatment : uint32_t {
  Unknown,  // Missing/ambiguous ownership: never infer new admission from it.
  Tiled,    // Includes suspended tiled members keeping a saved slot.
  Floating,
  Ignored,
};

// Sorted by HWND. Ownership is model state, not a claim that a native desktop
// move has completed. Destination hints describe this snapshot's desktop only.
struct TrackedWindowHint {
  uint32_t hwnd;
  uint32_t processId;
  GUID desktopId;
  RECT monitorRect;  // Owning workspace's monitor; zero if no longer attached.
  WindowTreatment treatment;
  // Restored resize borders, retained across hiding and desktop changes. Zero
  // DPI means unavailable; readers must also match the destination's DPI.
  RECT frameMargins;
  uint32_t marginsDpi;
};

// A suspended member of a published workspace, and its planned restore tile.
struct RestoreWindowHint {
  uint32_t hwnd;
  uint32_t processId;
  uint32_t workspaceIndex;
  uint32_t hasFrame;  // 1 when `frame` is valid.
  // The window's tile once restored: MultiWM's own Restore on a copy of the
  // workspace, planned with the window's cached size limits. Absent when that
  // plan is unsatisfiable (MultiWM would then move or float the window).
  RECT frame;
};

// One tile a BSP arrival may get, for a window whose size limits MultiWM
// doesn't know yet. Include shared/split_geometry.h first.
struct BspArrivalTile {
  RECT tile;  // At the saved ratios: the tile a window with no minimum gets.
  // The splits above the tile (SharedSplits::SolveBspPath), root first. Fitting
  // the window's own minimum to them gives MultiWM's tile exactly. A length past
  // kMaxBspPathSplits means the path was too deep to publish.
  uint32_t pathLength;
  uint32_t reserved0;
  SharedSplits::BspPathSplit path[kMaxBspPathSplits];
};

// The tiles MultiWM tries for the next arrival on a BSP workspace, in its order:
// the split tile (focused or largest), that split turned, the largest other
// tile, and that split turned. A window takes the first one that fits it and
// floats when none does.
struct BspArrivalTiles {
  uint32_t count;  // Up to kMaxBspArrivalTiles.
  // 0 while another arrival hasn't settled: arrangement may then turn that one's
  // split instead, so only the first tile, which needs no turn, is certain.
  uint32_t othersSettled;
  int32_t gap;           // The gap MultiWM plans with, physical pixels.
  uint32_t reserved0;
  double adaptiveDeviation;  // How far adaptive splits may move (SharedSplits::SplitFit).
  BspArrivalTile tiles[kMaxBspArrivalTiles];
};

// Where a BSP arrival whose minimum is `minWidth` x `minHeight` goes.
struct BspArrivalChoice {
  enum class Outcome { Tile, Floats, Unknown } outcome = Outcome::Unknown;
  RECT tile{};  // Outcome::Tile: the whole tile, before a maximum size centres the window in it.
};

// Decides as MultiWM's arrangement does: tries `tiles` in its order, fitting
// the minimum to each one's published splits with the planner's own arithmetic
// (SharedSplits::SolveBspPath). The first that fits is MultiWM's tile exactly;
// none means MultiWM floats the window. Unknown when the snapshot can't say: no
// tiles, a path too deep to publish, or a later tile while another arrival
// hasn't settled (arrangement may turn that one's split instead). Pure.
inline BspArrivalChoice ChooseBspArrivalTile(const BspArrivalTiles& tiles, const RECT& workArea, LONG minWidth,
                                             LONG minHeight) {
  const uint32_t count = tiles.count < kMaxBspArrivalTiles ? tiles.count : kMaxBspArrivalTiles;
  for (uint32_t i = 0; i < count; ++i) {
    const BspArrivalTile& tile = tiles.tiles[i];
    if ((i > 0 && !tiles.othersSettled) || tile.pathLength > kMaxBspPathSplits) return {};
    if (const auto frame = SharedSplits::SolveBspPath(workArea, tiles.gap, tiles.adaptiveDeviation, tile.path,
                                                      tile.pathLength, minWidth, minHeight)) {
      return {BspArrivalChoice::Outcome::Tile, *frame};
    }
  }
  if (count == 0 || !tiles.othersSettled) return {};
  return {BspArrivalChoice::Outcome::Floats, {}};
}

struct SnapshotBody {
  uint64_t generation;  // Increases with every publish.
  GUID desktopId;       // The desktop these workspaces belong to.
  uint32_t flags;       // SnapshotFlags.
  uint32_t workspaceCount;
  uint32_t trackedCount;
  uint32_t rulesBytes;
  uint32_t restoreWindowCount;
  // One bit per workspace. Zero from older writers.
  uint16_t bspWorkspaces;  // BSP tile hints may require the full tree to refit.
  uint16_t bspLocalFloats; // Definite BSP admission floats that stay on this monitor.
  WorkspaceHint workspaces[kMaxWorkspaces];
  // Every known window, including owners on other desktops. HWND values are
  // 32-bit significant on Windows. Duplicate owners publish Unknown treatment.
  TrackedWindowHint trackedWindows[kMaxTrackedWindows];
  uint8_t rules[kRulesCapacity];  // See SerializeRules.
  RestoreWindowHint restoreWindows[kMaxRestoreWindows];
  // Per workspace; count 0 unless its bspWorkspaces bit is set.
  BspArrivalTiles bspArrivals[kMaxWorkspaces];
};

struct SnapshotSection {
  uint32_t magic;
  uint16_t abiMajor;
  uint16_t abiMinor;
  uint32_t sectionSize;
  uint32_t writerPid;
  // Seqlock: odd while the writer is changing `body`. Readers copy the body and
  // keep the copy only if the value was even and unchanged around the copy.
  volatile LONG sequence;
  uint32_t reserved0;
  SnapshotBody body;
};

static_assert(kMaxWorkspaces <= 16, "Workspace masks use 16 bits");
static_assert(sizeof(WorkspaceHint) == 72, "WorkspaceHint layout");
static_assert(sizeof(TrackedWindowHint) == 64, "TrackedWindowHint layout");
static_assert(offsetof(TrackedWindowHint, frameMargins) == 44, "TrackedWindowHint margins");
static_assert(sizeof(RestoreWindowHint) == 32, "RestoreWindowHint layout");
static_assert(sizeof(BspArrivalTile) == 24 + sizeof(SharedSplits::BspPathSplit) * kMaxBspPathSplits, "BspArrivalTile layout");
static_assert(offsetof(BspArrivalTiles, tiles) == 24, "BspArrivalTiles layout");
static_assert(sizeof(BspArrivalTiles) == 24 + sizeof(BspArrivalTile) * kMaxBspArrivalTiles, "BspArrivalTiles layout");
static_assert(offsetof(SnapshotBody, bspArrivals) % 8 == 0, "SnapshotBody layout");
static_assert(offsetof(SnapshotBody, desktopId) == 8, "SnapshotBody layout");
static_assert(offsetof(SnapshotBody, workspaces) == 48, "SnapshotBody layout");
static_assert(offsetof(SnapshotBody, trackedWindows) == 48 + sizeof(WorkspaceHint) * kMaxWorkspaces, "SnapshotBody layout");
static_assert(sizeof(SnapshotBody) == 48 + sizeof(WorkspaceHint) * kMaxWorkspaces + sizeof(TrackedWindowHint) * kMaxTrackedWindows +
                                         kRulesCapacity + sizeof(RestoreWindowHint) * kMaxRestoreWindows +
                                         sizeof(BspArrivalTiles) * kMaxWorkspaces,
              "SnapshotBody layout");
static_assert(offsetof(SnapshotSection, sequence) == 16, "SnapshotSection layout");
static_assert(offsetof(SnapshotSection, body) == 24, "SnapshotSection layout");

// Security for both named objects: full access for the creating user and
// SYSTEM; read/map (section) or synchronize/query (event) for everyone,
// including AppContainer apps. No mandatory label: an unlabeled object counts
// as Medium with no-write-up, which already lets Low and AppContainer readers
// in. A Low label would only let a Low process of the same user write here,
// since it matches the owner entry.
inline constexpr wchar_t kSectionSddl[] =
    L"D:(A;;GA;;;OW)(A;;GA;;;SY)(A;;0x0005;;;WD)(A;;0x0005;;;AC)";
inline constexpr wchar_t kAliveEventSddl[] =
    L"D:(A;;GA;;;OW)(A;;GA;;;SY)(A;;0x00100001;;;WD)(A;;0x00100001;;;AC)";

// --- Seqlock -------------------------------------------------------------------

inline void BeginWrite(SnapshotSection* section) {
  // One writer only. Advance even after a predecessor died mid-write: never
  // reset the counter or briefly expose its unfinished body as readable.
  const LONG previous = section->sequence;
  InterlockedExchange(&section->sequence,
                      static_cast<LONG>((static_cast<uint32_t>(previous) + 1u) | 1u));
}

inline void EndWrite(SnapshotSection* section) {
  InterlockedIncrement(&section->sequence);  // Full barrier; the value is even again.
}

// Copies a consistent body into `*out`. Returns false when the writer was busy
// on every attempt; `*out` then holds partial data and must be discarded.
inline bool ReadConsistentBody(const SnapshotSection* section, SnapshotBody* out) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    const LONG before = section->sequence;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (before & 1) {
      YieldProcessor();
      continue;
    }
    std::memcpy(out, &section->body, sizeof(*out));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (section->sequence == before) return true;
  }
  return false;
}

// Reads the body's generation and the writer's process ID as one consistent
// pair, without copying the body. Every publish increases the generation, so a
// reader holding a copy with the same pair can keep using it. The pair, not the
// generation alone: a restarted MultiWM counts again from zero. Returns false
// while a write is in progress; the caller then copies the body as usual.
inline bool ReadConsistentGeneration(const SnapshotSection* section, uint64_t* generation, uint32_t* writerPid) {
  const LONG before = section->sequence;
  std::atomic_thread_fence(std::memory_order_acquire);
  if (before & 1) return false;
  // Plain copies: a 64-bit read isn't atomic on x86, which the recheck covers.
  const uint64_t readGeneration = section->body.generation;
  const uint32_t readPid = section->writerPid;
  std::atomic_thread_fence(std::memory_order_acquire);
  if (section->sequence != before) return false;
  *generation = readGeneration;
  *writerPid = readPid;
  return true;
}

// --- Rules -----------------------------------------------------------------------
//
// Encoding, little-endian, no padding, rules back to back:
//   u8  treatment (WindowRuleTreatment)
//   u8  flags: bit 0 preserveFloatingSize, bit 1 useWorkspaceSize
//   i32 floatingSizeDip.width, i32 floatingSizeDip.height
//   then classNames, titleFragments, processNames, each as
//     u16 count, and per string: u16 length, then that many UTF-16 code units.

// The rule fields the companion needs, with the same member names as MultiWM's
// WindowRule so SharedRules matching works on both.
struct SnapshotRule {
  WindowRuleTreatment treatment = WindowRuleTreatment::Exclude;
  std::vector<std::wstring> classNames;
  std::vector<std::wstring> titleFragments;
  std::vector<std::wstring> processNames;
  bool preserveFloatingSize = true;
  bool useWorkspaceSize = false;
  struct {
    LONG width = 0;
    LONG height = 0;
  } floatingSizeDip;
};

// Trace-to-owner only affects drags of already-managed windows.
inline bool IsRuleUsefulToCompanion(WindowRuleTreatment treatment) {
  return treatment != WindowRuleTreatment::TraceToOwner;
}

namespace Detail {

class ByteWriter {
 public:
  ByteWriter(uint8_t* out, uint32_t capacity) : out_(out), capacity_(capacity) {}
  uint32_t Size() const { return size_; }
  bool Ok() const { return ok_; }

  void Bytes(const void* data, uint32_t size) {
    if (!ok_ || capacity_ - size_ < size) {
      ok_ = false;
      return;
    }
    std::memcpy(out_ + size_, data, size);
    size_ += size;
  }
  void U8(uint8_t value) { Bytes(&value, sizeof(value)); }
  void U16(uint16_t value) { Bytes(&value, sizeof(value)); }
  void I32(int32_t value) { Bytes(&value, sizeof(value)); }

  void Strings(const std::vector<std::wstring>& values) {
    if (values.size() > 0xFFFF) {
      ok_ = false;
      return;
    }
    U16(static_cast<uint16_t>(values.size()));
    for (const std::wstring& value : values) {
      if (value.size() > 0xFFFF) {
        ok_ = false;
        return;
      }
      U16(static_cast<uint16_t>(value.size()));
      Bytes(value.data(), static_cast<uint32_t>(value.size() * sizeof(wchar_t)));
    }
  }

 private:
  uint8_t* out_;
  uint32_t capacity_;
  uint32_t size_ = 0;
  bool ok_ = true;
};

class ByteReader {
 public:
  ByteReader(const uint8_t* data, uint32_t size) : data_(data), size_(size) {}
  bool AtEnd() const { return offset_ == size_; }
  bool Ok() const { return ok_; }

  void Bytes(void* out, uint32_t size) {
    if (!ok_ || size_ - offset_ < size) {
      ok_ = false;
      return;
    }
    std::memcpy(out, data_ + offset_, size);
    offset_ += size;
  }
  uint8_t U8() {
    uint8_t value = 0;
    Bytes(&value, sizeof(value));
    return value;
  }
  uint16_t U16() {
    uint16_t value = 0;
    Bytes(&value, sizeof(value));
    return value;
  }
  int32_t I32() {
    int32_t value = 0;
    Bytes(&value, sizeof(value));
    return value;
  }

  std::vector<std::wstring> Strings() {
    std::vector<std::wstring> values(U16());
    for (std::wstring& value : values) {
      value.resize(U16());
      Bytes(value.data(), static_cast<uint32_t>(value.size() * sizeof(wchar_t)));
      if (!ok_) return {};
    }
    return values;
  }

 private:
  const uint8_t* data_;
  uint32_t size_;
  uint32_t offset_ = 0;
  bool ok_ = true;
};

}  // namespace Detail

// Writes the rules the companion needs into `out`, in order. Returns the number
// of bytes written. `*complete` is false when a rule did not fit; the bytes
// written are then a valid prefix (whole rules only).
template <class Rule>
uint32_t SerializeRules(const std::vector<Rule>& rules, uint8_t* out, uint32_t capacity, bool* complete) {
  Detail::ByteWriter writer(out, capacity);
  uint32_t lastWholeRule = 0;
  bool allFit = true;
  for (const Rule& rule : rules) {
    if (!IsRuleUsefulToCompanion(rule.treatment)) continue;
    writer.U8(static_cast<uint8_t>(rule.treatment));
    writer.U8(static_cast<uint8_t>((rule.preserveFloatingSize ? 1 : 0) | (rule.useWorkspaceSize ? 2 : 0)));
    writer.I32(static_cast<int32_t>(rule.floatingSizeDip.width));
    writer.I32(static_cast<int32_t>(rule.floatingSizeDip.height));
    writer.Strings(rule.classNames);
    writer.Strings(rule.titleFragments);
    writer.Strings(rule.processNames);
    if (!writer.Ok()) {
      allFit = false;
      break;
    }
    lastWholeRule = writer.Size();
  }
  if (complete) *complete = allFit;
  return lastWholeRule;
}

// Decodes what SerializeRules wrote. Returns false, leaving `*out` empty, for
// malformed data.
inline bool DeserializeRules(const uint8_t* data, uint32_t size, std::vector<SnapshotRule>* out) {
  out->clear();
  Detail::ByteReader reader(data, size);
  while (reader.Ok() && !reader.AtEnd()) {
    SnapshotRule rule;
    const uint8_t treatment = reader.U8();
    if (treatment > static_cast<uint8_t>(WindowRuleTreatment::InitialPlacement)) {
      out->clear();
      return false;
    }
    rule.treatment = static_cast<WindowRuleTreatment>(treatment);
    const uint8_t flags = reader.U8();
    rule.preserveFloatingSize = (flags & 1) != 0;
    rule.useWorkspaceSize = (flags & 2) != 0;
    rule.floatingSizeDip.width = reader.I32();
    rule.floatingSizeDip.height = reader.I32();
    rule.classNames = reader.Strings();
    rule.titleFragments = reader.Strings();
    rule.processNames = reader.Strings();
    if (reader.Ok()) out->push_back(std::move(rule));
  }
  if (!reader.Ok()) out->clear();
  return reader.Ok();
}

}  // namespace CompanionAbi

#endif  // MULTIWM_SHARED_COMPANION_SNAPSHOT_H
