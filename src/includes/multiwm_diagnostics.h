//=============================================================================
// On-demand diagnostic report
//=============================================================================
//
// Diagnostics are intentionally snapshot-based. The WM accumulates only cheap
// counters and a bounded in-memory event tail while running; disk I/O happens
// only when the user explicitly invokes the diagnostic hotkey.

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

static const wchar_t* DiagnosticMouseMoveBehaviorName(MouseMoveBehavior behavior) {
  return behavior == MouseMoveBehavior::Swap ? L"Swap" : L"Float";
}

class ReportBuilder {
 public:
  void Line() { text_ += L"\r\n"; }

  void Line(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    AppendFormat(format, args);
    va_end(args);
    text_ += L"\r\n";
  }

  void TextLine(const wchar_t* prefix, const std::wstring& value) {
    if (prefix) text_ += prefix;
    text_ += value;
    text_ += L"\r\n";
  }

  void Section(const wchar_t* title) {
    Line();
    Line(L"[%ls]", title);
    Line(L"------------------------------------------------------------");
  }

  const std::wstring& Text() const { return text_; }

 private:
  void AppendFormat(const wchar_t* format, va_list args) {
    size_t capacity = 512;
    while (capacity <= 65536) {
      std::vector<wchar_t> buffer(capacity);
      va_list copy;
      va_copy(copy, args);
      int written = vswprintf(buffer.data(), buffer.size(), format, copy);
      va_end(copy);
      if (written >= 0 && static_cast<size_t>(written) < buffer.size()) {
        text_.append(buffer.data(), static_cast<size_t>(written));
        return;
      }
      capacity *= 2;
    }
    text_ += L"<formatting failed>";
  }

  std::wstring text_;
};

static std::wstring DiagnosticGuidString(const GUID& guid) {
  wchar_t buffer[64]{};
  if (StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) > 0) return buffer;
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

static std::wstring DiagnosticLayoutCycleString(const std::vector<TileLayout>& layouts) {
  std::wstring result;
  for (size_t i = 0; i < layouts.size(); ++i) {
    if (i) result += L" -> ";
    result += DiagnosticLayoutName(layouts[i]);
  }
  return result.empty() ? L"<empty>" : result;
}

static std::wstring DiagnosticSanitizeText(const wchar_t* text) {
  if (!text || !*text) return L"";
  std::wstring result = text;
  for (wchar_t& ch : result) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
  }
  return result;
}

static const wchar_t* DiagnosticDisplayText(const std::wstring& text) {
  return text.empty() ? L"<unavailable>" : text.c_str();
}

static std::wstring DiagnosticDurationString(ULONGLONG milliseconds) {
  ULONGLONG remaining = milliseconds;
  const ULONGLONG days = remaining / 86400000ULL;
  remaining %= 86400000ULL;
  const ULONGLONG hours = remaining / 3600000ULL;
  remaining %= 3600000ULL;
  const ULONGLONG minutes = remaining / 60000ULL;
  remaining %= 60000ULL;
  const ULONGLONG seconds = remaining / 1000ULL;
  const ULONGLONG ms = remaining % 1000ULL;

  wchar_t buffer[96]{};
  if (days) {
    swprintf(
        buffer, ARRAYSIZE(buffer), L"%llud %02llu:%02llu:%02llu.%03llu",
        days, hours, minutes, seconds, ms);
  } else {
    swprintf(
        buffer, ARRAYSIZE(buffer), L"%02llu:%02llu:%02llu.%03llu",
        hours, minutes, seconds, ms);
  }
  return buffer;
}

static std::wstring DiagnosticSystemTimeString(const SYSTEMTIME& time) {
  wchar_t buffer[96]{};
  swprintf(
      buffer, ARRAYSIZE(buffer), L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
      time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
      time.wSecond, time.wMilliseconds);
  return buffer;
}

static std::wstring ExpandDiagnosticPath(const std::wstring& raw) {
  if (raw.empty()) return {};

  DWORD required = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
  std::wstring expanded;
  if (required > 0) {
    std::vector<wchar_t> buffer(required);
    DWORD written = ExpandEnvironmentStringsW(raw.c_str(), buffer.data(), required);
    if (written > 0 && written <= required) expanded.assign(buffer.data());
  }
  if (expanded.empty()) expanded = raw;

  DWORD fullRequired = GetFullPathNameW(expanded.c_str(), 0, nullptr, nullptr);
  if (!fullRequired) return expanded;
  std::vector<wchar_t> full(fullRequired + 1);
  if (!GetFullPathNameW(expanded.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr)) {
    return expanded;
  }
  return full.data();
}

static bool EnsureDiagnosticDirectory(std::wstring path) {
  if (path.empty()) return false;
  for (wchar_t& ch : path) {
    if (ch == L'/') ch = L'\\';
  }

  while (path.size() > 3 && path.back() == L'\\') path.pop_back();

  DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  }

  size_t scanFrom = 0;
  if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') {
    scanFrom = 3;
  } else if (path.rfind(L"\\\\", 0) == 0) {
    // Skip the \\server\share portion of a UNC path before creating children.
    size_t serverEnd = path.find(L'\\', 2);
    if (serverEnd == std::wstring::npos) return false;
    size_t shareEnd = path.find(L'\\', serverEnd + 1);
    scanFrom = shareEnd == std::wstring::npos ? path.size() : shareEnd + 1;
  }

  for (size_t i = scanFrom; i < path.size(); ++i) {
    if (path[i] != L'\\') continue;
    std::wstring prefix = path.substr(0, i);
    if (prefix.empty()) continue;
    DWORD prefixAttributes = GetFileAttributesW(prefix.c_str());
    if (prefixAttributes != INVALID_FILE_ATTRIBUTES) continue;
    if (!CreateDirectoryW(prefix.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
  }

  if (!CreateDirectoryW(path.c_str(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    return false;
  }
  attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool WriteUtf8DiagnosticFile(
    const std::wstring& path, const std::wstring& text, DWORD* outError) {
  if (outError) *outError = ERROR_SUCCESS;
  HANDLE file = CreateFileW(
      path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (outError) *outError = GetLastError();
    return false;
  }

  bool success = true;
  const BYTE bom[] = {0xEF, 0xBB, 0xBF};
  DWORD written = 0;
  if (!WriteFile(file, bom, sizeof(bom), &written, nullptr) || written != sizeof(bom)) {
    success = false;
  }

  if (success && !text.empty()) {
    int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
        nullptr, nullptr);
    if (byteCount <= 0) {
      success = false;
    } else {
      std::vector<char> utf8(static_cast<size_t>(byteCount));
      int converted = WideCharToMultiByte(
          CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(),
          byteCount, nullptr, nullptr);
      if (converted != byteCount) {
        success = false;
      } else {
        const char* cursor = utf8.data();
        size_t remaining = utf8.size();
        while (success && remaining > 0) {
          DWORD chunk = static_cast<DWORD>(
              std::min<size_t>(remaining, static_cast<size_t>(0x7ffff000)));
          DWORD chunkWritten = 0;
          if (!WriteFile(file, cursor, chunk, &chunkWritten, nullptr) ||
              chunkWritten != chunk) {
            success = false;
            break;
          }
          cursor += chunkWritten;
          remaining -= chunkWritten;
        }
      }
    }
  }

  if (!success && outError) *outError = GetLastError();
  CloseHandle(file);
  if (!success) DeleteFileW(path.c_str());
  return success;
}

static bool WriteReportToConfiguredDirectory(
    const std::wstring& report, std::wstring* outPath, DWORD* outError) {
  if (outPath) outPath->clear();
  if (outError) *outError = ERROR_SUCCESS;

  const std::wstring directory = ExpandDiagnosticPath(g_settings.diagnosticsOutputPath);
  if (!EnsureDiagnosticDirectory(directory)) {
    if (outError) *outError = GetLastError();
    return false;
  }

  SYSTEMTIME local{};
  GetLocalTime(&local);
  wchar_t baseName[128]{};
  swprintf(
      baseName, ARRAYSIZE(baseName),
      L"TilingHelper-Diagnostics-%04u%02u%02u-%02u%02u%02u",
      local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
      local.wSecond);

  for (int suffix = 0; suffix < 1000; ++suffix) {
    std::wstring fileName = baseName;
    if (suffix) {
      fileName += L"-";
      fileName += std::to_wstring(suffix + 1);
    }
    fileName += L".txt";

    std::wstring path = directory;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += fileName;

    DWORD error = ERROR_SUCCESS;
    if (WriteUtf8DiagnosticFile(path, report, &error)) {
      if (outPath) *outPath = path;
      return true;
    }
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
      if (outError) *outError = error;
      return false;
    }
  }

  if (outError) *outError = ERROR_FILE_EXISTS;
  return false;
}

struct DiagnosticWindowIdentity {
  bool hwndAlive = false;
  DWORD livePid = 0;
  bool pidMatchesRecord = false;
  std::wstring processName;
  std::wstring imagePath;
  std::wstring className;
  std::wstring title;

  bool visible = false;
  bool minimized = false;
  bool maximized = false;
  bool cloaked = false;
  bool hasFrame = false;
  RECT frame{};
  HMONITOR monitor = nullptr;
  UINT dpi = 96;
};

static DiagnosticWindowIdentity DiagnosticReadWindowIdentity(
    HWND hwnd, DWORD recordPid) {
  DiagnosticWindowIdentity identity;
  identity.hwndAlive = hwnd && IsWindow(hwnd);

  if (identity.hwndAlive) {
    GetWindowThreadProcessId(hwnd, &identity.livePid);
    identity.pidMatchesRecord = recordPid != 0 && identity.livePid == recordPid;
    identity.visible = IsWindowVisible(hwnd) != FALSE;
    identity.minimized = IsIconic(hwnd) != FALSE;
    identity.maximized = IsZoomed(hwnd) != FALSE;
    identity.cloaked = IsWindowCloaked(hwnd);
    identity.hasFrame = GetWindowFrameRect(hwnd, &identity.frame) &&
                        identity.frame.right > identity.frame.left &&
                        identity.frame.bottom > identity.frame.top;
    identity.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (identity.monitor) identity.dpi = GetMonitorEffectiveDpi(identity.monitor);

    wchar_t className[256]{};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) > 0) {
      identity.className = DiagnosticSanitizeText(className);
    }

    wchar_t title[512]{};
    if (GetWindowTextW(hwnd, title, ARRAYSIZE(title)) > 0) {
      identity.title = DiagnosticSanitizeText(title);
    }
  }

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

static size_t DiagnosticActiveIndex(const Workspace& state, HWND hwnd) {
  return state.TiledIndex(hwnd);
}

static size_t DiagnosticLogicalIndex(
    const Workspace& state, HWND hwnd, const WindowRecord&) {
  return state.LogicalIndex(hwnd);
}

static bool DiagnosticIsMinimizedSentinelFrame(const RECT& frame) {
  return frame.left <= -30000 || frame.top <= -30000;
}

struct DiagnosticRecordAudit {
  bool deadHwnd = false;
  bool pidMismatch = false;
  bool hasPlacementFailure = false;
};

static DiagnosticRecordAudit DiagnosticDumpWindowRecord(
    ReportBuilder& report, const Workspace& state, HWND mapHwnd,
    const WindowRecord& record, const wchar_t* prefix, size_t ordinal) {
  DiagnosticRecordAudit audit;
  const size_t activeIndex = DiagnosticActiveIndex(state, mapHwnd);
  const size_t logicalIndex = DiagnosticLogicalIndex(state, mapHwnd, record);
  const bool hasActiveIndex = activeIndex != static_cast<size_t>(-1);
  const bool hasLogicalIndex = logicalIndex != static_cast<size_t>(-1);

  report.Line(
      L"  %ls[%zu] logicalIndex=%ls activeIndex=%ls hwnd=%p",
      prefix, ordinal,
      hasLogicalIndex ? std::to_wstring(logicalIndex).c_str() : L"-",
      hasActiveIndex ? std::to_wstring(activeIndex).c_str() : L"-",
      reinterpret_cast<void*>(mapHwnd));
  report.Line(
      L"      recordHwnd=%p pid=%lu state=%ls suspension=%ls",
      reinterpret_cast<void*>(record.hwnd), record.pid,
      DiagnosticManageStateName(record.state),
      DiagnosticSuspensionReasonName(record.suspensionReason));

  DiagnosticWindowIdentity identity = DiagnosticReadWindowIdentity(mapHwnd, record.pid);
  audit.deadHwnd = !identity.hwndAlive;
  audit.pidMismatch = identity.hwndAlive && record.pid != 0 && !identity.pidMatchesRecord;
  audit.hasPlacementFailure = record.placementFailures > 0 ||
                              record.lastPlacementResult != PlacementResult::Success;

  report.Line(
      L"      identity: hwndAlive=%d livePid=%lu pidMatchesRecord=%d process=%ls",
      identity.hwndAlive ? 1 : 0, identity.livePid,
      identity.pidMatchesRecord ? 1 : 0,
      DiagnosticDisplayText(identity.processName));
  report.TextLine(L"      image: ", identity.imagePath.empty() ? L"<unavailable>" : identity.imagePath);
  report.TextLine(L"      class: ", identity.className.empty() ? L"<unavailable>" : identity.className);
  report.TextLine(L"      title: ", identity.title.empty() ? L"<unavailable>" : identity.title);
  report.Line(
      L"      liveState: visible=%d minimized=%d maximized=%d cloaked=%d monitor=%p dpi=%u",
      identity.visible ? 1 : 0, identity.minimized ? 1 : 0,
      identity.maximized ? 1 : 0, identity.cloaked ? 1 : 0,
      reinterpret_cast<void*>(identity.monitor), identity.dpi);
  if (identity.minimized && identity.hasFrame &&
      DiagnosticIsMinimizedSentinelFrame(identity.frame)) {
    report.Line(L"      liveFrame=<Windows minimized sentinel omitted>");
  } else if (identity.hasFrame) {
    report.Line(L"      liveFrame=%ls", DiagnosticRectString(identity.frame).c_str());
  } else {
    report.Line(L"      liveFrame=<unavailable>");
  }

  report.Line(
      L"      capabilities: move=%d resize=%d topmost=%d generation=%llu",
      record.canMove ? 1 : 0, record.canResize ? 1 : 0, record.topmost ? 1 : 0,
      static_cast<unsigned long long>(record.tilingGeneration));
  const std::wstring constraintMaxWidth = record.constraints.maxWidthBounded
      ? std::to_wstring(record.constraints.maxWidth) : L"unbounded";
  const std::wstring constraintMaxHeight = record.constraints.maxHeightBounded
      ? std::to_wstring(record.constraints.maxHeight) : L"unbounded";
  report.Line(
      L"      constraints: valid=%d dpi=%u min=%ldx%ld max=%ls x %ls timeout=%d error=%lu",
      record.constraints.valid ? 1 : 0, record.constraints.dpi,
      record.constraints.minWidth, record.constraints.minHeight,
      constraintMaxWidth.c_str(), constraintMaxHeight.c_str(),
      record.constraints.lastProbeTimedOut ? 1 : 0,
      record.constraints.lastProbeError);
  report.Line(
      L"      placement: result=%ls failures=%d",
      DiagnosticPlacementResultName(record.lastPlacementResult), record.placementFailures);
  report.Line(L"      lastRequested=%ls", DiagnosticRectString(record.lastRequestedRect).c_str());
  report.Line(L"      lastObserved =%ls", DiagnosticRectString(record.lastObservedRect).c_str());

  if (record.hasFloatingRect) {
    report.Line(
        L"      floatingGeometry=%ls dpi=%u monitor=%p",
        DiagnosticRectString(record.floatingRect).c_str(), record.floatingDpi,
        reinterpret_cast<void*>(record.floatingMonitor));
  } else {
    report.Line(L"      floatingGeometry=none");
  }

  if (record.hasSavedSlot) {
    report.Line(
        L"      savedSlot: index=%zu weight=%.9g hasWeight=%d wasMaster=%d",
        record.savedSlot.index, record.savedSlot.weight,
        record.savedSlot.hasWeight ? 1 : 0, record.savedSlot.wasMaster ? 1 : 0);
  } else {
    report.Line(L"      savedSlot: none");
  }
  return audit;
}

static uint64_t CounterDelta(uint64_t current, uint64_t previous) {
  return current >= previous ? current - previous : current;
}

static void DiagnosticCounterRow(
    ReportBuilder& report, const wchar_t* label, uint64_t current,
    uint64_t previous, ULONGLONG uptimeMs, ULONGLONG intervalMs) {
  const uint64_t delta = CounterDelta(current, previous);
  const double lifetimeRate = uptimeMs
      ? static_cast<double>(current) * 60000.0 / static_cast<double>(uptimeMs)
      : 0.0;
  const double intervalRate = intervalMs
      ? static_cast<double>(delta) * 60000.0 / static_cast<double>(intervalMs)
      : 0.0;
  report.Line(
      L"%-34ls %12llu %12llu %12.2f %12.2f",
      label, static_cast<unsigned long long>(current),
      static_cast<unsigned long long>(delta), lifetimeRate, intervalRate);
}

static void DiagnosticCounterHeader(ReportBuilder& report) {
  report.Line(L"%-34ls %12ls %12ls %12ls %12ls", L"counter", L"total", L"since dump", L"total/min", L"period/min");
  report.Line(L"%-34ls %12ls %12ls %12ls %12ls", L"----------------------------------", L"------------", L"------------", L"------------", L"------------");
}

static void DumpDerivedPerformanceIndicators(
    ReportBuilder& report, const Counters& c) {
  const double coalescingFactor = c.lifecycleReconciles
      ? static_cast<double>(c.lifecycleScheduleRequests) /
            static_cast<double>(c.lifecycleReconciles)
      : 0.0;
  const double hwndsPerReconcile = c.reconcileCalls
      ? static_cast<double>(c.reconcileWindowsExamined) /
            static_cast<double>(c.reconcileCalls)
      : 0.0;
  const double hwndsPerEnumeration = c.enumWindowsPasses
      ? static_cast<double>(c.enumWindowsVisited) /
            static_cast<double>(c.enumWindowsPasses)
      : 0.0;
  const double placementNoOpPercent = c.placementChecks
      ? static_cast<double>(c.placementNoOps) * 100.0 /
            static_cast<double>(c.placementChecks)
      : 0.0;
  const uint64_t problematicPlacements = c.placementAdjusted +
      c.placementAccessDenied + c.placementRefused + c.placementDead;
  const double placementProblemPercent = c.placementChecks
      ? static_cast<double>(problematicPlacements) * 100.0 /
            static_cast<double>(c.placementChecks)
      : 0.0;

  report.Line(L"Derived indicators:");
  report.Line(
      L"  lifecycle coalescing factor: %.2f schedule requests / settled reconcile",
      coalescingFactor);
  report.Line(
      L"  average known HWNDs examined per reconcile call: %.2f",
      hwndsPerReconcile);
  report.Line(
      L"  average top-level HWNDs visited per EnumWindows pass: %.2f",
      hwndsPerEnumeration);
  report.Line(
      L"  placement checks avoided by already-correct geometry: %.2f%%",
      placementNoOpPercent);
  report.Line(
      L"  placement checks ending adjusted/refused/dead: %.2f%%",
      placementProblemPercent);
}

static void DumpCounters(
    ReportBuilder& report, const Counters& c, const Counters& previous,
    ULONGLONG uptimeMs, ULONGLONG intervalMs) {
  report.Line(L"Event / reconciliation churn");
  DiagnosticCounterHeader(report);
  DiagnosticCounterRow(report, L"Lifecycle events processed", c.lifecycleEventsProcessed, previous.lifecycleEventsProcessed, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  destroy", c.lifecycleDestroy, previous.lifecycleDestroy, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  minimize start", c.lifecycleMinimizeStart, previous.lifecycleMinimizeStart, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  minimize end", c.lifecycleMinimizeEnd, previous.lifecycleMinimizeEnd, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  state change", c.lifecycleStateChange, previous.lifecycleStateChange, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  location change", c.lifecycleLocationChange, previous.lifecycleLocationChange, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  hide", c.lifecycleHide, previous.lifecycleHide, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  show", c.lifecycleShow, previous.lifecycleShow, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  cloaked", c.lifecycleCloaked, previous.lifecycleCloaked, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  uncloaked", c.lifecycleUncloaked, previous.lifecycleUncloaked, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Lifecycle schedule requests", c.lifecycleScheduleRequests, previous.lifecycleScheduleRequests, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Lifecycle timer resets", c.lifecycleTimerResets, previous.lifecycleTimerResets, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Settled lifecycle reconciles", c.lifecycleReconciles, previous.lifecycleReconciles, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Dirty HWNDs processed", c.lifecycleDirtyWindowsProcessed, previous.lifecycleDirtyWindowsProcessed, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Move/size end messages", c.moveSizeEndMessages, previous.moveSizeEndMessages, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Foreground/tray refreshes", c.trayRefreshMessages, previous.trayRefreshMessages, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"User move gestures", c.userMoveGestures, previous.userMoveGestures, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"User resize gestures", c.userResizeGestures, previous.userResizeGestures, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Unclassified/no-op gestures", c.userNoopGestures, previous.userNoopGestures, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Floating geometry user updates", c.floatingGeometryUserUpdates, previous.floatingGeometryUserUpdates, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Tiled drag -> Floating actions", c.tiledFloatActions, previous.tiledFloatActions, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Tiled drag swap actions", c.tiledSwapActions, previous.tiledSwapActions, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Divider weight updates", c.dividerUpdates, previous.dividerUpdates, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Reconcile calls", c.reconcileCalls, previous.reconcileCalls, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Known HWNDs examined", c.reconcileWindowsExamined, previous.reconcileWindowsExamined, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Workspace saves", c.workspaceSaves, previous.workspaceSaves, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Workspace migrations", c.workspaceMigrations, previous.workspaceMigrations, uptimeMs, intervalMs);

  report.Line();
  report.Line(L"Enumeration / layout / placement");
  DiagnosticCounterHeader(report);
  DiagnosticCounterRow(report, L"EnumWindows passes", c.enumWindowsPasses, previous.enumWindowsPasses, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Top-level HWNDs visited", c.enumWindowsVisited, previous.enumWindowsVisited, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Tile candidates collected", c.tileCandidatesCollected, previous.tileCandidatesCollected, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Arrange calls", c.arrangeCalls, previous.arrangeCalls, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Arrange passes", c.arrangePasses, previous.arrangePasses, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Layout plans built", c.layoutPlansBuilt, previous.layoutPlansBuilt, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Placement checks", c.placementChecks, previous.placementChecks, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Placement no-ops", c.placementNoOps, previous.placementNoOps, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Placement preflight stops", c.placementPreflightStops, previous.placementPreflightStops, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"SetWindowPos calls", c.setWindowPosCalls, previous.setWindowPosCalls, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Placement success", c.placementSuccess, previous.placementSuccess, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Adjusted by window", c.placementAdjusted, previous.placementAdjusted, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Placement access denied", c.placementAccessDenied, previous.placementAccessDenied, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Placement refused", c.placementRefused, previous.placementRefused, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Placement dead HWND", c.placementDead, previous.placementDead, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Floating geometry repairs", c.floatingGeometryRepairs, previous.floatingGeometryRepairs, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Constraint probes", c.constraintProbeAttempts, previous.constraintProbeAttempts, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  constraint probe successes", c.constraintProbeSuccesses, previous.constraintProbeSuccesses, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  constraint probe timeouts", c.constraintProbeTimeouts, previous.constraintProbeTimeouts, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"  constraint probe failures", c.constraintProbeFailures, previous.constraintProbeFailures, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Constraint-unsatisfiable plans", c.constraintUnsatisfiablePlans, previous.constraintUnsatisfiablePlans, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Constraint-triggered floats", c.constraintFloats, previous.constraintFloats, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Conformance leases started", c.conformanceLeasesStarted, previous.conformanceLeasesStarted, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Conformance lease reinforcement attempts", c.conformanceLeaseRepairs, previous.conformanceLeaseRepairs, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Conformance lease exhausted", c.conformanceLeaseExhausted, previous.conformanceLeaseExhausted, uptimeMs, intervalMs);

  report.Line();
  report.Line(L"Virtual desktop / runtime lifecycle / user commands");
  DiagnosticCounterHeader(report);
  DiagnosticCounterRow(report, L"Native VD changes processed", c.virtualDesktopChangesProcessed, previous.virtualDesktopChangesProcessed, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Fallback VD changes detected", c.virtualDesktopFallbackChanges, previous.virtualDesktopFallbackChanges, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"VD API init attempts", c.vdApiInitAttempts, previous.vdApiInitAttempts, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"VD API init successes", c.vdApiInitSuccesses, previous.vdApiInitSuccesses, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"VD API init failures", c.vdApiInitFailures, previous.vdApiInitFailures, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"VD API reinitializations", c.vdApiReinitializations, previous.vdApiReinitializations, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"VD notification register attempts", c.vdNotificationRegisterAttempts, previous.vdNotificationRegisterAttempts, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"VD notification register successes", c.vdNotificationRegisterSuccesses, previous.vdNotificationRegisterSuccesses, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"VD notification register failures", c.vdNotificationRegisterFailures, previous.vdNotificationRegisterFailures, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Runtime maintenance runs", c.runtimeMaintenanceRuns, previous.runtimeMaintenanceRuns, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Runtime maintenance retries", c.runtimeMaintenanceRetries, previous.runtimeMaintenanceRetries, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Runtime maintenance exhausted", c.runtimeMaintenanceExhausted, previous.runtimeMaintenanceExhausted, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Explorer restart signals", c.shellRestartSignals, previous.shellRestartSignals, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Display/work-area signals", c.displayTopologySignals, previous.displayTopologySignals, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"WinEvent hook install failures", c.winEventHookInstallFailures, previous.winEventHookInstallFailures, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Tile commands", c.tileCommands, previous.tileCommands, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Layout cycle commands", c.layoutCycleCommands, previous.layoutCycleCommands, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Float focused commands", c.floatFocusedCommands, previous.floatFocusedCommands, uptimeMs, intervalMs);
  DiagnosticCounterRow(report, L"Swap master commands", c.swapMasterCommands, previous.swapMasterCommands, uptimeMs, intervalMs);
}

static uint64_t DiagnosticFileTime100ns(const FILETIME& value) {
  ULARGE_INTEGER ticks{};
  ticks.LowPart = value.dwLowDateTime;
  ticks.HighPart = value.dwHighDateTime;
  return ticks.QuadPart;
}

static ULONGLONG Diagnostic100nsToMilliseconds(uint64_t ticks100ns) {
  return static_cast<ULONGLONG>(ticks100ns / 10000ULL);
}

struct DiagnosticCpuSnapshot {
  bool processAvailable = false;
  bool wmAvailable = false;
  uint64_t processKernel100ns = 0;
  uint64_t processUser100ns = 0;
  uint64_t wmKernel100ns = 0;
  uint64_t wmUser100ns = 0;

  uint64_t ProcessTotal100ns() const {
    return processKernel100ns + processUser100ns;
  }

  uint64_t WmTotal100ns() const {
    return wmKernel100ns + wmUser100ns;
  }
};

static DiagnosticCpuSnapshot CaptureDiagnosticCpuSnapshot() {
  DiagnosticCpuSnapshot snapshot;
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
    snapshot.processAvailable = true;
    snapshot.processKernel100ns = DiagnosticFileTime100ns(kernel);
    snapshot.processUser100ns = DiagnosticFileTime100ns(user);
  }

  if (g_wm.thread &&
      GetThreadTimes(g_wm.thread, &creation, &exit, &kernel, &user)) {
    snapshot.wmAvailable = true;
    snapshot.wmKernel100ns = DiagnosticFileTime100ns(kernel);
    snapshot.wmUser100ns = DiagnosticFileTime100ns(user);
  }
  return snapshot;
}

static uint64_t DiagnosticCpuDelta100ns(
    uint64_t endValue, uint64_t startValue) {
  return endValue >= startValue ? endValue - startValue : 0;
}

static void AccumulateDiagnosticSelfCost(
    ULONGLONG startTickMs, const DiagnosticCpuSnapshot& startCpu) {
  const ULONGLONG endTickMs = GetTickCount64();
  const DiagnosticCpuSnapshot endCpu = CaptureDiagnosticCpuSnapshot();

  const ULONGLONG wallMs =
      endTickMs >= startTickMs ? endTickMs - startTickMs : 0;
  uint64_t processCpu100ns = 0;
  uint64_t wmCpu100ns = 0;
  if (startCpu.processAvailable && endCpu.processAvailable) {
    processCpu100ns = DiagnosticCpuDelta100ns(
        endCpu.ProcessTotal100ns(), startCpu.ProcessTotal100ns());
  }
  if (startCpu.wmAvailable && endCpu.wmAvailable) {
    wmCpu100ns = DiagnosticCpuDelta100ns(
        endCpu.WmTotal100ns(), startCpu.WmTotal100ns());
  }

  ++g_runtime.lifetimeDiagnosticCost.attempts;
  g_runtime.lifetimeDiagnosticCost.wallMs += wallMs;
  g_runtime.lifetimeDiagnosticCost.processCpu100ns += processCpu100ns;
  g_runtime.lifetimeDiagnosticCost.wmCpu100ns += wmCpu100ns;

  ++g_runtime.wmSessionDiagnosticCost.attempts;
  g_runtime.wmSessionDiagnosticCost.wallMs += wallMs;
  g_runtime.wmSessionDiagnosticCost.processCpu100ns += processCpu100ns;
  g_runtime.wmSessionDiagnosticCost.wmCpu100ns += wmCpu100ns;
}

static void DumpCpuUsage(
    ReportBuilder& report, ULONGLONG processUptimeMs, ULONGLONG wmSessionMs,
    const DiagnosticCpuSnapshot& snapshot) {
  if (snapshot.processAvailable) {
    const uint64_t raw100ns = snapshot.ProcessTotal100ns();
    const uint64_t priorDiagnostic100ns = std::min<uint64_t>(
        raw100ns, g_runtime.lifetimeDiagnosticCost.processCpu100ns);
    const uint64_t operational100ns = raw100ns - priorDiagnostic100ns;
    const ULONGLONG rawMs = Diagnostic100nsToMilliseconds(raw100ns);
    const ULONGLONG operationalMs =
        Diagnostic100nsToMilliseconds(operational100ns);
    const double rawAverageCore = processUptimeMs
        ? static_cast<double>(rawMs) * 100.0 /
              static_cast<double>(processUptimeMs)
        : 0.0;
    const double operationalAverageCore = processUptimeMs
        ? static_cast<double>(operationalMs) * 100.0 /
              static_cast<double>(processUptimeMs)
        : 0.0;
    report.Line(
        L"Tool process CPU raw: kernel=%ls user=%ls total=%ls average-one-core=%.3f%%",
        DiagnosticDurationString(
            Diagnostic100nsToMilliseconds(snapshot.processKernel100ns)).c_str(),
        DiagnosticDurationString(
            Diagnostic100nsToMilliseconds(snapshot.processUser100ns)).c_str(),
        DiagnosticDurationString(rawMs).c_str(), rawAverageCore);
    report.Line(
        L"Tool process CPU excluding prior diagnostic reporting: total=%ls average-one-core=%.3f%%",
        DiagnosticDurationString(operationalMs).c_str(), operationalAverageCore);
  } else {
    report.Line(L"Tool process CPU: <unavailable>");
  }

  if (snapshot.wmAvailable) {
    const uint64_t raw100ns = snapshot.WmTotal100ns();
    const uint64_t priorDiagnostic100ns = std::min<uint64_t>(
        raw100ns, g_runtime.wmSessionDiagnosticCost.wmCpu100ns);
    const uint64_t operational100ns = raw100ns - priorDiagnostic100ns;
    const ULONGLONG rawMs = Diagnostic100nsToMilliseconds(raw100ns);
    const ULONGLONG operationalMs =
        Diagnostic100nsToMilliseconds(operational100ns);
    const double rawAverageCore = wmSessionMs
        ? static_cast<double>(rawMs) * 100.0 /
              static_cast<double>(wmSessionMs)
        : 0.0;
    const double operationalAverageCore = wmSessionMs
        ? static_cast<double>(operationalMs) * 100.0 /
              static_cast<double>(wmSessionMs)
        : 0.0;
    report.Line(
        L"WM thread CPU raw: kernel=%ls user=%ls total=%ls average-one-core=%.3f%%",
        DiagnosticDurationString(
            Diagnostic100nsToMilliseconds(snapshot.wmKernel100ns)).c_str(),
        DiagnosticDurationString(
            Diagnostic100nsToMilliseconds(snapshot.wmUser100ns)).c_str(),
        DiagnosticDurationString(rawMs).c_str(), rawAverageCore);
    report.Line(
        L"WM thread CPU excluding prior diagnostic reporting: total=%ls average-one-core=%.3f%%",
        DiagnosticDurationString(operationalMs).c_str(), operationalAverageCore);
  } else {
    report.Line(L"WM thread CPU: <unavailable>");
  }
}

static void DumpPlatformHealth(ReportBuilder& report) {
  report.Line(
      L"WM thread: id=%lu COM(STA)=%d",
      g_wm.threadId.load(std::memory_order_acquire),
      g_wm.comInitialized ? 1 : 0);
  if (g_vd.lastKnownDesktopValid) {
    report.Line(
        L"Current virtual desktop: %ls",
        DiagnosticGuidString(g_vd.lastKnownDesktop).c_str());
  } else {
    report.Line(L"Current virtual desktop: <unknown>");
  }
  report.Line(
      L"Virtual desktop API: initialized=%d abiResolved=%d Explorer=%lu.%lu usesHMonitor=%d",
      g_vd.initialized ? 1 : 0, g_vd.abiResolved ? 1 : 0,
      g_vd.explorerBuild, g_vd.explorerRevision,
      g_vd.abi.usesHMonitor ? 1 : 0);
  HWND currentShell = GetShellWindow();
  DWORD currentShellPid = 0;
  if (currentShell && IsWindow(currentShell)) {
    GetWindowThreadProcessId(currentShell, &currentShellPid);
  }
  report.Line(
      L"Explorer shell identity: boundPid=%lu currentPid=%lu match=%d",
      g_vd.shellPid, currentShellPid,
      g_vd.shellPid && currentShellPid && g_vd.shellPid == currentShellPid ? 1 : 0);
  DWORD currentShellPid = 0;
  HWND shell = GetShellWindow();
  if (shell && IsWindow(shell)) GetWindowThreadProcessId(shell, &currentShellPid);
  report.Line(
      L"Explorer shell binding: currentPid=%lu boundPid=%lu match=%d",
      currentShellPid, g_vd.shellPid,
      currentShellPid && g_vd.shellPid && currentShellPid == g_vd.shellPid ? 1 : 0);
  report.Line(
      L"VD notifications: registered=%d cookie=%lu methods=%d currentChangedIndex=%d changeQueued=%ld lastHr=0x%08X",
      g_vd.notificationsRegistered ? 1 : 0, g_vd.notificationCookie,
      g_vd.abi.notification.methodCount,
      g_vd.abi.notification.currentChangedIndex, g_vd.changeQueued,
      static_cast<unsigned int>(g_vd.notificationLastHr));
  const ULONGLONG now = GetTickCount64();
  const std::wstring notificationAttemptAge =
      g_vd.notificationLastAttemptTickMs
          ? DiagnosticDurationString(
                now >= g_vd.notificationLastAttemptTickMs
                    ? now - g_vd.notificationLastAttemptTickMs
                    : 0)
          : L"never";
  const std::wstring notificationSuccessAge =
      g_vd.notificationLastSuccessTickMs
          ? DiagnosticDurationString(
                now >= g_vd.notificationLastSuccessTickMs
                    ? now - g_vd.notificationLastSuccessTickMs
                    : 0)
          : L"never";
  report.Line(
      L"VD notification timing: lastAttemptAge=%ls lastSuccessAge=%ls",
      notificationAttemptAge.c_str(), notificationSuccessAge.c_str());
  report.Line(
      L"Runtime maintenance: running=%d timerArmed=%d attempt=%u/%u deferredLifecycle=%d forceMonitorReconcile=%d",
      g_wm.maintenanceRunning ? 1 : 0, g_wm.maintenanceTimer ? 1 : 0,
      g_wm.maintenanceAttempts, RuntimeLifecycle::kMaintenanceMaxAttempts,
      g_wm.lifecycleRetryAfterPlatformRecovery ? 1 : 0,
      g_wm.forceMonitorReconcile ? 1 : 0);
  report.Line(
      L"WinEvent hooks: foreground=%d location=%d moveSize=%d minimize=%d hideDestroy=%d cloak=%d state=%d",
      g_hooks.foreground ? 1 : 0, g_hooks.locationChange ? 1 : 0,
      g_hooks.moveSize ? 1 : 0, g_hooks.minimize ? 1 : 0,
      g_hooks.hideDestroy ? 1 : 0, g_hooks.cloak ? 1 : 0,
      g_hooks.state ? 1 : 0);

  size_t moveStarts = 0, moveEnds = 0, movePoints = 0;
  AcquireSRWLockShared(&g_moveSize.lock);
  moveStarts = g_moveSize.startRects.size();
  moveEnds = g_moveSize.endRects.size();
  movePoints = g_moveSize.endPoints.size();
  ReleaseSRWLockShared(&g_moveSize.lock);
  report.Line(
      L"Move/size cache: starts=%zu ends=%zu endPoints=%zu", moveStarts,
      moveEnds, movePoints);
}

static void DumpRecentEvents(ReportBuilder& report, ULONGLONG now) {
  if (g_runtime.significantEvents.empty()) {
    report.Line(L"(none)");
    return;
  }

  for (const RecentEvent& event : g_runtime.significantEvents) {
    ULONGLONG sinceStart = event.tickMs >= g_runtime.processStartedTickMs
        ? event.tickMs - g_runtime.processStartedTickMs
        : 0;
    ULONGLONG age = now >= event.tickMs ? now - event.tickMs : 0;
    report.Line(
        L"+%ls  age=%ls  %ls",
        DiagnosticDurationString(sinceStart).c_str(),
        DiagnosticDurationString(age).c_str(), event.text.c_str());
  }
}

static void DumpLifecycleBursts(ReportBuilder& report, ULONGLONG now) {
  if (g_runtime.lifecycleBursts.empty()) {
    report.Line(L"(none)");
    return;
  }

  for (const LifecycleBurst& burst : g_runtime.lifecycleBursts) {
    ULONGLONG sinceStart = burst.tickMs >= g_runtime.processStartedTickMs
        ? burst.tickMs - g_runtime.processStartedTickMs
        : 0;
    ULONGLONG age = now >= burst.tickMs ? now - burst.tickMs : 0;
    report.Line(
        L"+%ls  age=%ls  dirtyWindows=%zu pendingArranges=%zu",
        DiagnosticDurationString(sinceStart).c_str(),
        DiagnosticDurationString(age).c_str(),
        burst.dirtyWindows, burst.pendingArranges);
  }
}

static void WriteDiagnosticReport() {
  InitializeProcessRuntime();
  const ULONGLONG reportStartTickMs = GetTickCount64();
  const DiagnosticCpuSnapshot reportStartCpu = CaptureDiagnosticCpuSnapshot();
  const ULONGLONG now = reportStartTickMs;
  const ULONGLONG uptimeMs = now - g_runtime.processStartedTickMs;
  const ULONGLONG wmSessionMs = g_runtime.wmSessionStartedTickMs
      ? now - g_runtime.wmSessionStartedTickMs
      : 0;
  const ULONGLONG intervalMs = g_runtime.lastReportTickMs
      ? now - g_runtime.lastReportTickMs
      : uptimeMs;
  const Counters counters = g_runtime.counters;
  const Counters previous = g_runtime.lastReportCounters;
  const uint64_t reportSequence = g_runtime.reportSequence + 1;

  std::vector<std::pair<DesktopMonitorKey, Workspace>> workspaces =
      g_workspaces.Snapshot();
  std::vector<std::pair<HWND, std::vector<DesktopMonitorKey>>> ownershipIndex =
      g_workspaces.OwnershipIndexSnapshot();
  std::sort(
      workspaces.begin(), workspaces.end(),
      [](const auto& a, const auto& b) {
        std::wstring aDesktop = DiagnosticGuidString(a.first.desktopId);
        std::wstring bDesktop = DiagnosticGuidString(b.first.desktopId);
        if (aDesktop != bDesktop) return aDesktop < bDesktop;
        return reinterpret_cast<uintptr_t>(a.first.monitor) <
               reinterpret_cast<uintptr_t>(b.first.monitor);
      });

  size_t totalRecords = 0;
  size_t tiledCount = 0;
  size_t floatingCount = 0;
  size_t suspendedCount = 0;
  size_t ignoredCount = 0;
  std::unordered_map<HWND, size_t> ownerCounts;
  std::unordered_map<HWND, std::vector<DesktopMonitorKey>> expectedOwners;
  for (const auto& workspaceKv : workspaces) {
    for (const auto& recordKv : workspaceKv.second.Records()) {
      ++totalRecords;
      ++ownerCounts[recordKv.first];
      expectedOwners[recordKv.first].push_back(workspaceKv.first);
      switch (recordKv.second.state) {
        case ManageState::Tiled: ++tiledCount; break;
        case ManageState::Floating: ++floatingCount; break;
        case ManageState::Suspended: ++suspendedCount; break;
        case ManageState::Ignored: ++ignoredCount; break;
      }
    }
  }
  size_t duplicateOwners = 0;
  for (const auto& ownerKv : ownerCounts) {
    if (ownerKv.second > 1) ++duplicateOwners;
  }

  auto sameKeySet = [](
      const std::vector<DesktopMonitorKey>& a,
      const std::vector<DesktopMonitorKey>& b) {
    if (a.size() != b.size()) return false;
    for (const DesktopMonitorKey& key : a) {
      if (std::none_of(
              b.begin(), b.end(), [&](const DesktopMonitorKey& candidate) {
                return Model::DesktopMonitorKeyEqual{}(key, candidate);
              })) {
        return false;
      }
    }
    return true;
  };

  size_t ownershipIndexMismatches = 0;
  std::unordered_set<HWND> indexedHwnds;
  for (const auto& indexKv : ownershipIndex) {
    indexedHwnds.insert(indexKv.first);
    auto expectedIt = expectedOwners.find(indexKv.first);
    if (expectedIt == expectedOwners.end() ||
        !sameKeySet(indexKv.second, expectedIt->second)) {
      ++ownershipIndexMismatches;
    }
  }
  for (const auto& expectedKv : expectedOwners) {
    if (indexedHwnds.find(expectedKv.first) == indexedHwnds.end()) {
      ++ownershipIndexMismatches;
    }
  }

  SYSTEMTIME local{}, utc{};
  GetLocalTime(&local);
  GetSystemTime(&utc);

  ReportBuilder report;
  report.Line(L"============================================================");
  report.Line(L"TILING HELPER DIAGNOSTIC REPORT");
  report.Line(L"============================================================");
  report.Line(L"Mod version: 1.13.0");
  report.Line(L"Report sequence: %llu", static_cast<unsigned long long>(reportSequence));
  report.Line(L"Generated local: %ls", DiagnosticSystemTimeString(local).c_str());
  report.Line(L"Generated UTC:   %ls", DiagnosticSystemTimeString(utc).c_str());
  report.Line(L"Tool runtime:    %ls", DiagnosticDurationString(uptimeMs).c_str());
  report.Line(L"WM session:      %ls", DiagnosticDurationString(wmSessionMs).c_str());
  report.Line(
      L"Since last report: %ls%ls",
      DiagnosticDurationString(intervalMs).c_str(),
      g_runtime.lastReportTickMs ? L"" : L" (first report)");
  report.Line(L"PID: %lu", GetCurrentProcessId());
  report.Line(L"Privacy note: this report contains window titles and executable paths.");

  report.Section(L"Current runtime summary");
  report.Line(L"Management mode: %ls", IsAutomaticMode() ? L"Automatic" : L"Manual");
  report.Line(
      L"Workspaces=%zu records=%zu tiled=%zu suspended=%zu perWindowFloating=%zu ignored=%zu",
      workspaces.size(), totalRecords, tiledCount, suspendedCount, floatingCount,
      ignoredCount);
  report.Line(
      L"Pending lifecycle HWNDs=%zu pending workspace reinforcements=%zu duplicate-owned HWNDs=%zu",
      g_wm.lifecycleDirtyWindows.size(), g_wm.pendingDesktopArranges.size(),
      duplicateOwners);
  report.Line(
      L"Active conformance leases=%zu runtime-maintenance timer=%d attempt=%u",
      CountActiveConformanceLeases(), g_wm.maintenanceTimer ? 1 : 0,
      g_wm.maintenanceAttempts);
  report.Line(
      L"Reverse ownership index: entries=%zu mismatches=%zu",
      ownershipIndex.size(), ownershipIndexMismatches);
  report.Line(
      L"WM sessions started=%llu settings reloads=%llu prior reports written=%llu prior report write failures=%llu",
      static_cast<unsigned long long>(counters.wmSessionStarts),
      static_cast<unsigned long long>(counters.settingsReloads),
      static_cast<unsigned long long>(counters.reportsWritten),
      static_cast<unsigned long long>(counters.reportWriteFailures));

  report.Section(L"Configuration snapshot");
  report.Line(L"Default layout: %ls", DiagnosticLayoutName(g_settings.defaultLayout));
  report.TextLine(L"Layout cycle: ", DiagnosticLayoutCycleString(g_settings.layoutCycle));
  report.Line(
      L"Workspace geometry (96-DPI logical px): gap=%ld insets=[%ld,%ld,%ld,%ld] floatingDefault=%ldx%ld",
      g_settings.gapDip, g_settings.insetsDip.left, g_settings.insetsDip.top,
      g_settings.insetsDip.right, g_settings.insetsDip.bottom,
      g_settings.floatingDefaultSizeDip.width,
      g_settings.floatingDefaultSizeDip.height);
  report.Line(L"Master percent: %ld", g_settings.masterPercent);
  report.Line(L"Mouse move behavior: %ls", DiagnosticMouseMoveBehaviorName(g_settings.mouseMoveBehavior));
  report.Line(L"Tiled conformance lease: %u ms", g_settings.conformanceLeaseMs);
  report.Line(L"Lifecycle settle delay: %u ms", g_settings.reconcileDelayMs);
  report.Line(L"Exclusion rules: %zu", g_settings.exclusionRules.size());
  report.TextLine(L"Diagnostic output setting: ", g_settings.diagnosticsOutputPath);
  report.TextLine(L"Diagnostic output resolved: ", ExpandDiagnosticPath(g_settings.diagnosticsOutputPath));

  report.Section(L"Platform / hook health");
  DumpPlatformHealth(report);

  report.Section(L"Performance and churn counters");
  DumpCpuUsage(report, uptimeMs, wmSessionMs, reportStartCpu);
  report.Line();
  DumpDerivedPerformanceIndicators(report, counters);
  report.Line();
  report.Line(
      L"Rates are normalized by tool runtime and by the interval since the previous successful report.");
  DumpCounters(report, counters, previous, uptimeMs, intervalMs);

  report.Section(L"Recent significant events (bounded in-memory tail)");
  report.Line(L"Newest events are at the bottom; at most %zu are retained.", kRecentEventLimit);
  DumpRecentEvents(report, now);

  report.Section(L"Recent lifecycle bursts (bounded in-memory tail)");
  report.Line(
      L"Newest bursts are at the bottom; at most %zu are retained.",
      kLifecycleBurstLimit);
  DumpLifecycleBursts(report, now);

  report.Section(L"Workspace / window state");

  size_t invariantFailures = 0;
  size_t deadRecords = 0;
  size_t pidMismatches = 0;
  size_t recordsWithPlacementFailures = 0;

  for (size_t workspaceIndex = 0; workspaceIndex < workspaces.size(); ++workspaceIndex) {
    const DesktopMonitorKey& key = workspaces[workspaceIndex].first;
    const Workspace& state = workspaces[workspaceIndex].second;
    const std::wstring desktopId = DiagnosticGuidString(key.desktopId);

    size_t workspaceTiled = 0, workspaceFloating = 0, workspaceSuspended = 0,
           workspaceIgnored = 0;
    for (const auto& recordKv : state.Records()) {
      switch (recordKv.second.state) {
        case ManageState::Tiled: ++workspaceTiled; break;
        case ManageState::Floating: ++workspaceFloating; break;
        case ManageState::Suspended: ++workspaceSuspended; break;
        case ManageState::Ignored: ++workspaceIgnored; break;
      }
    }

    const bool pendingReinforcement = std::any_of(
        g_wm.pendingDesktopArranges.begin(), g_wm.pendingDesktopArranges.end(),
        [&](const PendingWorkspaceArrange& pending) {
          return pending.monitor == key.monitor &&
                 IsEqualGUID(pending.desktopId, key.desktopId);
        });

    std::vector<std::wstring> invariantErrors;
    const bool invariantsValid =
        state.Validate(key, L"diagnostic report", &invariantErrors, false);
    if (!invariantsValid) ++invariantFailures;

    report.Line();
    report.Line(
        L"---------------- Workspace %zu / %zu ----------------",
        workspaceIndex + 1, workspaces.size());
    report.Line(L"desktopId: %ls", desktopId.c_str());
    report.Line(
        L"currentDesktop: %ls",
        !g_vd.lastKnownDesktopValid
            ? L"unknown"
            : (IsEqualGUID(key.desktopId, g_vd.lastKnownDesktop) ? L"YES" : L"no"));
    report.Line(L"monitor:   %p", reinterpret_cast<void*>(key.monitor));

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (key.monitor && GetMonitorInfoW(
            key.monitor, reinterpret_cast<MONITORINFO*>(&monitorInfo))) {
      report.Line(L"device:    %ls", monitorInfo.szDevice);
      report.Line(L"monitorRect: %ls", DiagnosticRectString(monitorInfo.rcMonitor).c_str());
      report.Line(L"systemWork:  %ls", DiagnosticRectString(monitorInfo.rcWork).c_str());
      report.Line(L"monitorDpi:  %u", GetMonitorEffectiveDpi(key.monitor));
      RECT configuredWork{};
      if (GetWorkspaceWorkArea(key.monitor, &configuredWork)) {
        report.Line(L"configuredWork: %ls", DiagnosticRectString(configuredWork).c_str());
      }
    } else {
      report.Line(L"monitorInfo: <unavailable / stale handle>");
    }

    report.Line(
        L"layout=%ls (%d) masterRatio=%.9g",
        DiagnosticLayoutName(state.Layout()), static_cast<int>(state.Layout()),
        state.MasterRatio());
    report.Line(
        L"counts: active=%zu records=%zu tiled=%zu suspended=%zu floating=%zu ignored=%zu",
        state.ActiveCount(), state.Records().size(), workspaceTiled,
        workspaceSuspended, workspaceFloating, workspaceIgnored);
    report.Line(L"pendingReinforcement=%d invariants=%ls",
                pendingReinforcement ? 1 : 0,
                invariantsValid ? L"PASS" : L"FAIL");
    if (!invariantErrors.empty()) {
      report.Line(L"invariantErrors:");
      for (const std::wstring& error : invariantErrors) {
        report.Line(L"  - %ls", error.c_str());
      }
    }
    report.Line(L"stackWeights: %ls", DiagnosticWeightsString(state.StackWeights()).c_str());
    report.Line(L"gridWeights:  %ls", DiagnosticWeightsString(state.GridWeights()).c_str());

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

    report.Line(L"  logicalTiledOrder: %zu", logicalRecords.size());
    if (logicalRecords.empty()) {
      report.Line(L"    (none)");
    } else {
      for (size_t i = 0; i < logicalRecords.size(); ++i) {
        DiagnosticRecordAudit audit = DiagnosticDumpWindowRecord(
            report, state, logicalRecords[i].hwnd, *logicalRecords[i].record,
            L"window", i);
        deadRecords += audit.deadHwnd ? 1 : 0;
        pidMismatches += audit.pidMismatch ? 1 : 0;
        recordsWithPlacementFailures += audit.hasPlacementFailure ? 1 : 0;
      }
    }

    report.Line(L"  nonLayoutRecords: %zu", otherRecords.size());
    if (otherRecords.empty()) {
      report.Line(L"    (none)");
    } else {
      for (size_t i = 0; i < otherRecords.size(); ++i) {
        DiagnosticRecordAudit audit = DiagnosticDumpWindowRecord(
            report, state, otherRecords[i].first, *otherRecords[i].second,
            L"record", i);
        deadRecords += audit.deadHwnd ? 1 : 0;
        pidMismatches += audit.pidMismatch ? 1 : 0;
        recordsWithPlacementFailures += audit.hasPlacementFailure ? 1 : 0;
      }
    }
  }

  report.Section(L"Pending work");
  if (g_wm.lifecycleDirtyWindows.empty()) {
    report.Line(L"pendingLifecycleWindows: none");
  } else {
    report.Line(L"pendingLifecycleWindows: %zu", g_wm.lifecycleDirtyWindows.size());
    for (size_t i = 0; i < g_wm.lifecycleDirtyWindows.size(); ++i) {
      report.Line(
          L"  [%zu] hwnd=%p", i,
          reinterpret_cast<void*>(g_wm.lifecycleDirtyWindows[i]));
    }
  }

  if (g_wm.pendingDesktopArranges.empty()) {
    report.Line(L"pendingWorkspaceReinforcements: none");
  } else {
    report.Line(
        L"pendingWorkspaceReinforcements: %zu",
        g_wm.pendingDesktopArranges.size());
    for (size_t i = 0; i < g_wm.pendingDesktopArranges.size(); ++i) {
      const std::wstring desktopId =
          DiagnosticGuidString(g_wm.pendingDesktopArranges[i].desktopId);
      report.Line(
          L"  [%zu] desktopId=%ls monitor=%p", i, desktopId.c_str(),
          reinterpret_cast<void*>(g_wm.pendingDesktopArranges[i].monitor));
    }
  }

  report.Section(L"Audit summary");
  report.Line(L"Workspace invariant failures: %zu", invariantFailures);
  report.Line(L"Duplicate-owned HWNDs:        %zu", duplicateOwners);
  report.Line(L"Ownership-index mismatches:   %zu", ownershipIndexMismatches);
  report.Line(L"Dead HWND records:            %zu", deadRecords);
  report.Line(L"Live PID mismatches:           %zu", pidMismatches);
  report.Line(L"Records with placement issue:  %zu", recordsWithPlacementFailures);

  const uint64_t lifecycleCategorySum =
      counters.lifecycleDestroy + counters.lifecycleMinimizeStart +
      counters.lifecycleMinimizeEnd + counters.lifecycleStateChange +
      counters.lifecycleLocationChange + counters.lifecycleHide +
      counters.lifecycleShow + counters.lifecycleCloaked +
      counters.lifecycleUncloaked;
  const uint64_t placementResultSum =
      counters.placementSuccess + counters.placementAdjusted +
      counters.placementAccessDenied + counters.placementRefused +
      counters.placementDead;
  const uint64_t placementPathSum =
      counters.placementNoOps + counters.placementPreflightStops +
      counters.setWindowPosCalls;
  const uint64_t constraintProbeResultSum =
      counters.constraintProbeSuccesses + counters.constraintProbeTimeouts +
      counters.constraintProbeFailures;
  const uint64_t vdInitResultSum =
      counters.vdApiInitSuccesses + counters.vdApiInitFailures;
  const uint64_t vdNotificationResultSum =
      counters.vdNotificationRegisterSuccesses +
      counters.vdNotificationRegisterFailures;
  const uint64_t priorReportResultSum =
      counters.reportsWritten + counters.reportWriteFailures;
  const uint64_t classifiedGestureSum =
      counters.userMoveGestures + counters.userResizeGestures +
      counters.userNoopGestures;

  const bool lifecycleAccountingOk =
      lifecycleCategorySum == counters.lifecycleEventsProcessed;
  const bool placementResultsOk =
      placementResultSum == counters.placementChecks;
  const bool placementPathsOk =
      placementPathSum == counters.placementChecks;
  const bool constraintProbeAccountingOk =
      constraintProbeResultSum == counters.constraintProbeAttempts;
  const bool vdInitAccountingOk =
      vdInitResultSum == counters.vdApiInitAttempts;
  const bool vdNotificationAccountingOk =
      vdNotificationResultSum == counters.vdNotificationRegisterAttempts;
  const bool reportAccountingOk =
      priorReportResultSum == g_runtime.lifetimeDiagnosticCost.attempts;
  const bool gestureAccountingOk =
      classifiedGestureSum <= counters.moveSizeEndMessages;

  report.Line();
  report.Line(L"Telemetry consistency:");
  report.Line(
      L"  lifecycle categories == lifecycle events: %ls (%llu == %llu)",
      lifecycleAccountingOk ? L"PASS" : L"FAIL",
      static_cast<unsigned long long>(lifecycleCategorySum),
      static_cast<unsigned long long>(counters.lifecycleEventsProcessed));
  report.Line(
      L"  placement result sum == placement checks: %ls (%llu == %llu)",
      placementResultsOk ? L"PASS" : L"FAIL",
      static_cast<unsigned long long>(placementResultSum),
      static_cast<unsigned long long>(counters.placementChecks));
  report.Line(
      L"  placement path sum == placement checks: %ls (%llu == %llu)",
      placementPathsOk ? L"PASS" : L"FAIL",
      static_cast<unsigned long long>(placementPathSum),
      static_cast<unsigned long long>(counters.placementChecks));
  report.Line(
      L"  constraint probe results == attempts: %ls (%llu == %llu)",
      constraintProbeAccountingOk ? L"PASS" : L"FAIL",
      static_cast<unsigned long long>(constraintProbeResultSum),
      static_cast<unsigned long long>(counters.constraintProbeAttempts));
  report.Line(
      L"  VD init successes + failures == attempts: %ls (%llu == %llu)",
      vdInitAccountingOk ? L"PASS" : L"FAIL",
      static_cast<unsigned long long>(vdInitResultSum),
      static_cast<unsigned long long>(counters.vdApiInitAttempts));
  report.Line(
      L"  VD notification successes + failures == attempts: %ls (%llu == %llu)",
      vdNotificationAccountingOk ? L"PASS" : L"FAIL",
      static_cast<unsigned long long>(vdNotificationResultSum),
      static_cast<unsigned long long>(counters.vdNotificationRegisterAttempts));
  report.Line(
      L"  prior report results == measured diagnostic attempts: %ls (%llu == %llu)",
      reportAccountingOk ? L"PASS" : L"FAIL",
      static_cast<unsigned long long>(priorReportResultSum),
      static_cast<unsigned long long>(
          g_runtime.lifetimeDiagnosticCost.attempts));
  report.Line(
      L"  classified gestures <= move/size end messages: %ls (%llu <= %llu)",
      gestureAccountingOk ? L"PASS" : L"FAIL",
      static_cast<unsigned long long>(classifiedGestureSum),
      static_cast<unsigned long long>(counters.moveSizeEndMessages));

  const bool allWinEventHooks =
      g_hooks.foreground && g_hooks.locationChange && g_hooks.moveSize &&
      g_hooks.minimize && g_hooks.hideDestroy && g_hooks.cloak && g_hooks.state;
  DWORD auditShellPid = 0;
  HWND auditShell = GetShellWindow();
  if (auditShell && IsWindow(auditShell)) {
    GetWindowThreadProcessId(auditShell, &auditShellPid);
  }
  const bool shellBindingHealthy =
      !g_vd.initialized ||
      (auditShellPid != 0 && g_vd.shellPid == auditShellPid);
  const bool vdCoreHealthy = g_vd.initialized && g_vd.abiResolved;
  const bool vdNotificationsHealthy =
      g_vd.abi.notification.methodCount <= 0 || g_vd.notificationsRegistered;
  const bool currentDesktopHealthy =
      !g_vd.initialized || g_vd.lastKnownDesktopValid;

  const bool structuralHealthy =
      invariantFailures == 0 && duplicateOwners == 0 &&
      ownershipIndexMismatches == 0;
  const bool runtimeWindowHealthy =
      deadRecords == 0 && pidMismatches == 0 &&
      recordsWithPlacementFailures == 0 && allWinEventHooks &&
      shellBindingHealthy && vdCoreHealthy && vdNotificationsHealthy &&
      currentDesktopHealthy;
  const bool telemetryHealthy =
      lifecycleAccountingOk && placementResultsOk && placementPathsOk &&
      constraintProbeAccountingOk && vdInitAccountingOk &&
      vdNotificationAccountingOk && reportAccountingOk && gestureAccountingOk;
  const bool overallHealthy =
      structuralHealthy && runtimeWindowHealthy && telemetryHealthy;

  report.Line();
  report.Line(
      L"Model structural audit:       %ls",
      structuralHealthy ? L"PASS" : L"REVIEW NEEDED");
  report.Line(
      L"Runtime / window health:      %ls",
      runtimeWindowHealthy ? L"PASS" : L"REVIEW NEEDED");
  report.Line(
      L"Telemetry accounting audit:   %ls",
      telemetryHealthy ? L"PASS" : L"REVIEW NEEDED");
  report.Line(
      L"Overall diagnostic status:    %ls",
      overallHealthy ? L"PASS" : L"REVIEW NEEDED");

  const ULONGLONG buildEndTickMs = GetTickCount64();
  const DiagnosticCpuSnapshot buildEndCpu = CaptureDiagnosticCpuSnapshot();
  const ULONGLONG currentBuildWallMs =
      buildEndTickMs >= reportStartTickMs
          ? buildEndTickMs - reportStartTickMs
          : 0;
  const uint64_t currentBuildProcessCpu100ns =
      reportStartCpu.processAvailable && buildEndCpu.processAvailable
          ? DiagnosticCpuDelta100ns(
                buildEndCpu.ProcessTotal100ns(),
                reportStartCpu.ProcessTotal100ns())
          : 0;
  const uint64_t currentBuildWmCpu100ns =
      reportStartCpu.wmAvailable && buildEndCpu.wmAvailable
          ? DiagnosticCpuDelta100ns(
                buildEndCpu.WmTotal100ns(), reportStartCpu.WmTotal100ns())
          : 0;

  report.Section(L"Diagnostic reporting self-cost");
  report.Line(
      L"Prior diagnostic attempts: %llu lifetime, %llu current WM session",
      static_cast<unsigned long long>(
          g_runtime.lifetimeDiagnosticCost.attempts),
      static_cast<unsigned long long>(
          g_runtime.wmSessionDiagnosticCost.attempts));
  report.Line(
      L"Prior completed diagnostic cost (includes file writes): wall=%ls processCPU=%ls wmCPU=%ls",
      DiagnosticDurationString(
          g_runtime.lifetimeDiagnosticCost.wallMs).c_str(),
      DiagnosticDurationString(
          Diagnostic100nsToMilliseconds(
              g_runtime.lifetimeDiagnosticCost.processCpu100ns)).c_str(),
      DiagnosticDurationString(
          Diagnostic100nsToMilliseconds(
              g_runtime.lifetimeDiagnosticCost.wmCpu100ns)).c_str());
  report.Line(
      L"Current report construction so far (file write excluded): wall=%ls processCPU=%ls wmCPU=%ls",
      DiagnosticDurationString(currentBuildWallMs).c_str(),
      DiagnosticDurationString(
          Diagnostic100nsToMilliseconds(currentBuildProcessCpu100ns)).c_str(),
      DiagnosticDurationString(
          Diagnostic100nsToMilliseconds(currentBuildWmCpu100ns)).c_str());
  report.Line(
      L"Note: the next report's prior-cost totals include this report's final file-write cost.");

  report.Line();
  report.Line(L"========================== END REPORT ==========================");

  std::wstring outputPath;
  DWORD writeError = ERROR_SUCCESS;
  if (!WriteReportToConfiguredDirectory(report.Text(), &outputPath, &writeError)) {
    ++g_runtime.counters.reportWriteFailures;
    Wh_Log(
        L"Failed to write diagnostic report to '%ls' (error=%lu)",
        g_settings.diagnosticsOutputPath.c_str(), writeError);
    RecordEvent(L"diagnostic report write failed error=%lu", writeError);
    AccumulateDiagnosticSelfCost(reportStartTickMs, reportStartCpu);
    return;
  }

  ++g_runtime.counters.reportsWritten;
  ++g_runtime.reportSequence;
  g_runtime.lastReportCounters = counters;
  // Account for the successful report in the baseline so the next delta doesn't
  // misleadingly attribute this report write to unrelated runtime activity.
  g_runtime.lastReportCounters.reportsWritten = g_runtime.counters.reportsWritten;
  RecordEvent(L"diagnostic report written: %ls", outputPath.c_str());
  Wh_Log(L"Diagnostic report written: %ls", outputPath.c_str());
  AccumulateDiagnosticSelfCost(reportStartTickMs, reportStartCpu);
  // The interval baseline is the end of a successful diagnostic operation, so
  // report-generation time is not charged to the next "since dump" period.
  g_runtime.lastReportTickMs = GetTickCount64();
}

}  // namespace Diagnostics

