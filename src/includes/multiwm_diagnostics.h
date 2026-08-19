//=============================================================================
// Diagnostic bookkeeping dump
//=============================================================================

static const wchar_t* DiagnosticLayoutName(TileLayout layout) {
  switch (layout) {
    case TileLayout::MasterStack: return L"MasterStack";
    case TileLayout::Columns: return L"Columns";
    case TileLayout::Rows: return L"Rows";
    case TileLayout::MasterStackH: return L"MasterStackH";
    case TileLayout::BSP: return L"BSP";
    case TileLayout::Monocle: return L"Monocle";
    case TileLayout::Floating: return L"Floating";
    case TileLayout::COUNT: return L"COUNT";
    default: return L"Unknown";
  }
}

static const wchar_t* DiagnosticManageStateName(ManageState state) {
  switch (state) {
    case ManageState::Tiled: return L"Tiled";
    case ManageState::Floating: return L"Floating";
    case ManageState::Suspended: return L"Suspended";
    case ManageState::Ignored: return L"Ignored";
    default: return L"Unknown";
  }
}

static const wchar_t* DiagnosticSuspensionReasonName(SuspensionReason reason) {
  switch (reason) {
    case SuspensionReason::None: return L"None";
    case SuspensionReason::Hidden: return L"Hidden";
    case SuspensionReason::Minimized: return L"Minimized";
    case SuspensionReason::Maximized: return L"Maximized";
    default: return L"Unknown";
  }
}

static const wchar_t* DiagnosticPlacementResultName(PlacementResult result) {
  switch (result) {
    case PlacementResult::Success: return L"Success";
    case PlacementResult::AdjustedByWindow: return L"AdjustedByWindow";
    case PlacementResult::AccessDenied: return L"AccessDenied";
    case PlacementResult::Refused: return L"Refused";
    case PlacementResult::Dead: return L"Dead";
    default: return L"Unknown";
  }
}

static std::wstring DiagnosticGuidString(const GUID& guid) {
  wchar_t buffer[64]{};
  if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) > 0) {
    return buffer;
  }
  return L"{GUID_FORMAT_FAILED}";
}

static std::wstring DiagnosticRectString(const RECT& rect) {
  wchar_t buffer[128]{};
  swprintf(
      buffer, ARRAYSIZE(buffer), L"[%ld,%ld,%ld,%ld] (%ldx%ld)",
      rect.left, rect.top, rect.right, rect.bottom,
      rect.right - rect.left, rect.bottom - rect.top);
  return buffer;
}

static std::wstring DiagnosticWeightsString(const std::vector<double>& weights) {
  std::wstring result = L"[";
  for (size_t i = 0; i < weights.size(); ++i) {
    wchar_t buffer[64]{};
    swprintf(buffer, ARRAYSIZE(buffer), L"%.9g", weights[i]);
    if (i) result += L", ";
    result += buffer;
  }
  result += L"]";
  return result;
}

static std::wstring DiagnosticSanitizeText(const wchar_t* text) {
  if (!text || !*text) return L"";

  std::wstring result = text;
  for (wchar_t& ch : result) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
  }
  return result;
}

struct DiagnosticWindowIdentity {
  bool hwndAlive = false;
  DWORD livePid = 0;
  bool pidMatchesRecord = false;
  std::wstring processName;
  std::wstring imagePath;
  std::wstring className;
  std::wstring title;
};

static DiagnosticWindowIdentity DiagnosticReadWindowIdentity(
    HWND hwnd, DWORD recordPid) {
  DiagnosticWindowIdentity identity;
  identity.hwndAlive = hwnd && IsWindow(hwnd);

  if (identity.hwndAlive) {
    GetWindowThreadProcessId(hwnd, &identity.livePid);
    identity.pidMatchesRecord = recordPid != 0 && identity.livePid == recordPid;

    wchar_t className[256]{};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) > 0) {
      identity.className = DiagnosticSanitizeText(className);
    }

    wchar_t title[512]{};
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) > 0) {
      identity.title = DiagnosticSanitizeText(title);
    }
  }

  // Prefer the PID currently attached to the exact HWND. If the HWND has already
  // disappeared, the saved PID is still useful for best-effort identification,
  // but the dump keeps that distinction visible through hwndAlive/pidMatchesRecord.
  DWORD queryPid = identity.livePid ? identity.livePid : recordPid;
  if (queryPid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, queryPid);
    if (process) {
      std::vector<wchar_t> path(32768);
      DWORD chars = static_cast<DWORD>(path.size());
      if (QueryFullProcessImageNameW(process, 0, path.data(), &chars) && chars > 0) {
        identity.imagePath.assign(path.data(), chars);
        const wchar_t* base = wcsrchr(identity.imagePath.c_str(), L'\\');
        identity.processName = base ? base + 1 : identity.imagePath;
      }
      CloseHandle(process);
    }
  }

  return identity;
}

static const wchar_t* DiagnosticDisplayText(const std::wstring& text) {
  return text.empty() ? L"<unavailable>" : text.c_str();
}

static size_t DiagnosticActiveIndex(const Workspace& state, HWND hwnd) {
  return state.TiledIndex(hwnd);
}

static size_t DiagnosticLogicalIndex(
    const Workspace& state, HWND hwnd, const WindowRecord&) {
  return state.LogicalIndex(hwnd);
}

static void DiagnosticDumpWindowRecord(
    const Workspace& state, HWND mapHwnd, const WindowRecord& record,
    const wchar_t* prefix, size_t ordinal) {
  const size_t activeIndex = DiagnosticActiveIndex(state, mapHwnd);
  const size_t logicalIndex = DiagnosticLogicalIndex(state, mapHwnd, record);
  const bool hasActiveIndex = activeIndex != static_cast<size_t>(-1);
  const bool hasLogicalIndex = logicalIndex != static_cast<size_t>(-1);

  if (hasLogicalIndex && hasActiveIndex) {
    Wh_Log(
        L"  %s[%zu] logicalIndex=%zu activeIndex=%zu hwnd=%p",
        prefix, ordinal, logicalIndex, activeIndex,
        reinterpret_cast<void*>(mapHwnd));
  } else if (hasLogicalIndex) {
    Wh_Log(
        L"  %s[%zu] logicalIndex=%zu activeIndex=- hwnd=%p",
        prefix, ordinal, logicalIndex,
        reinterpret_cast<void*>(mapHwnd));
  } else if (hasActiveIndex) {
    Wh_Log(
        L"  %s[%zu] logicalIndex=- activeIndex=%zu hwnd=%p",
        prefix, ordinal, activeIndex,
        reinterpret_cast<void*>(mapHwnd));
  } else {
    Wh_Log(
        L"  %s[%zu] logicalIndex=- activeIndex=- hwnd=%p",
        prefix, ordinal, reinterpret_cast<void*>(mapHwnd));
  }

  Wh_Log(
      L"      recordHwnd=%p pid=%lu state=%s suspension=%s",
      reinterpret_cast<void*>(record.hwnd), record.pid,
      DiagnosticManageStateName(record.state),
      DiagnosticSuspensionReasonName(record.suspensionReason));

  DiagnosticWindowIdentity identity = DiagnosticReadWindowIdentity(mapHwnd, record.pid);
  Wh_Log(
      L"      identity: hwndAlive=%d livePid=%lu pidMatchesRecord=%d process=%s",
      identity.hwndAlive ? 1 : 0, identity.livePid,
      identity.pidMatchesRecord ? 1 : 0,
      DiagnosticDisplayText(identity.processName));
  Wh_Log(L"      image: %s", DiagnosticDisplayText(identity.imagePath));
  Wh_Log(L"      class: %s", DiagnosticDisplayText(identity.className));
  Wh_Log(L"      title: \"%s\"", DiagnosticDisplayText(identity.title));

  Wh_Log(
      L"      capabilities: move=%d resize=%d topmost=%d",
      record.canMove ? 1 : 0,
      record.canResize ? 1 : 0,
      record.topmost ? 1 : 0);

  Wh_Log(
      L"      placement: result=%s failures=%d",
      DiagnosticPlacementResultName(record.lastPlacementResult),
      record.placementFailures);

  std::wstring requested = DiagnosticRectString(record.lastRequestedRect);
  std::wstring observed = DiagnosticRectString(record.lastObservedRect);
  Wh_Log(L"      lastRequested=%s", requested.c_str());
  Wh_Log(L"      lastObserved =%s", observed.c_str());

  if (record.hasFloatingRect) {
    std::wstring floating = DiagnosticRectString(record.floatingRect);
    Wh_Log(
        L"      floatingGeometry=%s dpi=%u monitor=%p", floating.c_str(),
        record.floatingDpi, reinterpret_cast<void*>(record.floatingMonitor));
  } else {
    Wh_Log(L"      floatingGeometry=none");
  }

  if (record.hasSavedSlot) {
    Wh_Log(
        L"      savedSlot: index=%zu weight=%.9g hasWeight=%d wasMaster=%d",
        record.savedSlot.index,
        record.savedSlot.weight,
        record.savedSlot.hasWeight ? 1 : 0,
        record.savedSlot.wasMaster ? 1 : 0);
  } else {
    Wh_Log(L"      savedSlot: none");
  }
}

static void DumpSavedBookkeeping() {
  // Copy the persistent model once. Everything structural below comes from that
  // immutable snapshot. Per-record title/class/process identity is queried later
  // only for human readability; those lookups are observational and never trigger
  // reconciliation, placement, ownership repair, or any model mutation.
  std::vector<std::pair<DesktopMonitorKey, Workspace>> workspaces =
      g_workspaces.Snapshot();

  // Repository storage is unordered. Give repeated dumps a stable order so diffs
  // are useful: desktop GUID first, monitor handle second.
  std::sort(
      workspaces.begin(), workspaces.end(),
      [](const auto& a, const auto& b) {
        std::wstring aDesktop = DiagnosticGuidString(a.first.desktopId);
        std::wstring bDesktop = DiagnosticGuidString(b.first.desktopId);
        if (aDesktop != bDesktop) return aDesktop < bDesktop;
        return reinterpret_cast<uintptr_t>(a.first.monitor) <
               reinterpret_cast<uintptr_t>(b.first.monitor);
      });

  Wh_Log(L"");
  Wh_Log(L"==================== TILING BOOKKEEPING ====================");
  Wh_Log(L"workspaceCount: %zu", workspaces.size());
  Wh_Log(L"pendingReinforcementCount: %zu", g_wm.pendingDesktopArranges.size());
  Wh_Log(L"lifecycleDirtyWindowCount: %zu", g_wm.lifecycleDirtyWindows.size());

  for (size_t workspaceIndex = 0; workspaceIndex < workspaces.size(); ++workspaceIndex) {
    const DesktopMonitorKey& key = workspaces[workspaceIndex].first;
    const Workspace& state = workspaces[workspaceIndex].second;
    std::wstring desktopId = DiagnosticGuidString(key.desktopId);

    size_t tiledCount = 0;
    size_t floatingCount = 0;
    size_t suspendedCount = 0;
    size_t ignoredCount = 0;
    for (const auto& recordKv : state.Records()) {
      switch (recordKv.second.state) {
        case ManageState::Tiled: ++tiledCount; break;
        case ManageState::Floating: ++floatingCount; break;
        case ManageState::Suspended: ++suspendedCount; break;
        case ManageState::Ignored: ++ignoredCount; break;
      }
    }

    const bool pendingReinforcement = std::any_of(
        g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
        [&](const PendingWorkspaceArrange& pending) {
          return pending.monitor == key.monitor &&
                 IsEqualGUID(pending.desktopId, key.desktopId);
        });

    Wh_Log(L"");
    Wh_Log(
        L"---------------- Workspace %zu / %zu ----------------",
        workspaceIndex + 1, workspaces.size());
    Wh_Log(L"desktopId: %s", desktopId.c_str());
    Wh_Log(L"monitor:   %p", reinterpret_cast<void*>(key.monitor));
    Wh_Log(
        L"layout:    %s (%d)",
        DiagnosticLayoutName(state.Layout()), static_cast<int>(state.Layout()));
    Wh_Log(L"masterRatio: %.9g", state.MasterRatio());
    Wh_Log(
        L"counts: active=%zu records=%zu tiled=%zu suspended=%zu floating=%zu ignored=%zu",
        state.ActiveCount(), state.Records().size(), tiledCount, suspendedCount,
        floatingCount, ignoredCount);
    Wh_Log(L"pendingReinforcement: %d", pendingReinforcement ? 1 : 0);
    const bool invariantsValid = state.Validate(key, L"diagnostic dump");
    Wh_Log(L"invariants: %s", invariantsValid ? L"PASS" : L"FAIL");

    std::wstring stackWeights = DiagnosticWeightsString(state.StackWeights());
    std::wstring gridWeights = DiagnosticWeightsString(state.GridWeights());
    Wh_Log(L"stackWeights: %s", stackWeights.c_str());
    Wh_Log(L"gridWeights:  %s", gridWeights.c_str());

    // Print tiled + suspended records in their reconstructed logical order. This
    // makes the saved slot relationship visible directly instead of forcing the
    // reader to correlate an unordered records{} dump with windows[].
    struct LogicalRecordRef {
      size_t logicalIndex;
      HWND hwnd;
      const WindowRecord* record;
    };
    std::vector<LogicalRecordRef> logicalRecords;
    std::vector<std::pair<HWND, const WindowRecord*>> otherRecords;

    logicalRecords.reserve(state.Records().size());
    otherRecords.reserve(state.Records().size());
    for (const auto& recordKv : state.Records()) {
      const WindowRecord& record = recordKv.second;
      size_t logicalIndex = DiagnosticLogicalIndex(state, recordKv.first, record);
      if ((record.state == ManageState::Tiled || record.state == ManageState::Suspended) &&
          logicalIndex != static_cast<size_t>(-1)) {
        logicalRecords.push_back({logicalIndex, recordKv.first, &record});
      } else {
        otherRecords.push_back({recordKv.first, &record});
      }
    }

    std::sort(
        logicalRecords.begin(), logicalRecords.end(),
        [](const LogicalRecordRef& a, const LogicalRecordRef& b) {
          if (a.logicalIndex != b.logicalIndex) return a.logicalIndex < b.logicalIndex;
          return reinterpret_cast<uintptr_t>(a.hwnd) < reinterpret_cast<uintptr_t>(b.hwnd);
        });
    std::sort(
        otherRecords.begin(), otherRecords.end(),
        [](const auto& a, const auto& b) {
          if (a.second->state != b.second->state) {
            return static_cast<int>(a.second->state) < static_cast<int>(b.second->state);
          }
          return reinterpret_cast<uintptr_t>(a.first) < reinterpret_cast<uintptr_t>(b.first);
        });

    Wh_Log(L"  logicalTiledOrder: %zu", logicalRecords.size());
    if (logicalRecords.empty()) {
      Wh_Log(L"    (none)");
    } else {
      for (size_t i = 0; i < logicalRecords.size(); ++i) {
        DiagnosticDumpWindowRecord(
            state, logicalRecords[i].hwnd, *logicalRecords[i].record,
            L"window", i);
      }
    }

    Wh_Log(L"  nonLayoutRecords: %zu", otherRecords.size());
    if (otherRecords.empty()) {
      Wh_Log(L"    (none)");
    } else {
      for (size_t i = 0; i < otherRecords.size(); ++i) {
        DiagnosticDumpWindowRecord(
            state, otherRecords[i].first, *otherRecords[i].second,
            L"record", i);
      }
    }
  }

  if (!g_wm.lifecycleDirtyWindows.empty()) {
    Wh_Log(L"");
    Wh_Log(L"pendingLifecycleWindows:");
    for (size_t i = 0; i < g_wm.lifecycleDirtyWindows.size(); ++i) {
      Wh_Log(
          L"  [%zu] hwnd=%p",
          i, reinterpret_cast<void*>(g_wm.lifecycleDirtyWindows[i]));
    }
  }

  if (!g_wm.pendingDesktopArranges.empty()) {
    Wh_Log(L"");
    Wh_Log(L"pendingWorkspaceReinforcements:");
    for (size_t i = 0; i < g_wm.pendingDesktopArranges.size(); ++i) {
      std::wstring desktopId = DiagnosticGuidString(g_wm.pendingDesktopArranges[i].desktopId);
      Wh_Log(
          L"  [%zu] desktopId=%s monitor=%p",
          i, desktopId.c_str(),
          reinterpret_cast<void*>(g_wm.pendingDesktopArranges[i].monitor));
    }
  }

  Wh_Log(L"============================================================");
  Wh_Log(L"");
}


}  // namespace Diagnostics

