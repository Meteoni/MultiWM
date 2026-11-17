// Window-rule vocabulary and matching, shared by MultiWM and MultiWM Companion.
//
// Both mods compile this file, so it may use only Windows and the C++ standard
// library, never a MultiWM chapter. The include guard (rather than #pragma once)
// keeps it safe when WHAM pastes it into a single generated file.

#ifndef MULTIWM_SHARED_WINDOW_RULES_H
#define MULTIWM_SHARED_WINDOW_RULES_H

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

// What a matching window rule does. The numeric values are part of the companion
// snapshot ABI (shared/companion_snapshot.h): append new values, never reorder.
enum class WindowRuleTreatment {
  Exclude,
  DefaultToFloating,
  AlwaysFloating,
  TraceToOwner,
  FloatingPlacementOverride,
  InitialPlacement,
};

namespace SharedRules {

// Shell surfaces MultiWM never manages, whatever the rules say.
inline constexpr const wchar_t* kIgnoredWindowClasses[] = {
    L"Progman",
    L"WorkerW",
    L"Shell_TrayWnd",
    L"Shell_SecondaryTrayWnd",
    L"Windows.UI.Core.CoreWindow",
    L"OperationStatusWindow",
};

inline bool IsIgnoredWindowClass(const wchar_t* className) {
  if (!className) return false;
  for (const wchar_t* ignored : kIgnoredWindowClasses) {
    if (_wcsicmp(className, ignored) == 0) return true;
  }
  return false;
}

inline bool EqualsInsensitive(const std::wstring& a, const std::wstring& b) {
  return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

inline bool ContainsInsensitive(const std::wstring& text, const std::wstring& needle) {
  if (needle.empty()) return false;
  return std::search(
             text.begin(), text.end(), needle.begin(), needle.end(),
             [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }) != text.end();
}

// The window properties a rule can match, each read on first use and then
// cached. Reading the process name can be costly (MultiWM opens the process), so
// rules that don't name a process never pay for it.
//
// The title is read with InternalGetWindowText, which never sends WM_GETTEXT.
// For another process's window that is what GetWindowText returns anyway; inside
// the window's own process it keeps application code from running mid-hook.
class LazyWindowFacts {
 public:
  using ProcessNameReader = std::wstring (*)(HWND);

  LazyWindowFacts(HWND hwnd, ProcessNameReader readProcessName)
      : hwnd_(hwnd), readProcessName_(readProcessName) {}

  HWND Window() const { return hwnd_; }

  const std::wstring& ClassName() {
    if (!haveClass_) {
      wchar_t buffer[256]{};
      if (GetClassNameW(hwnd_, buffer, ARRAYSIZE(buffer))) className_ = buffer;
      haveClass_ = true;
    }
    return className_;
  }

  const std::wstring& Title() {
    if (!haveTitle_) {
      wchar_t buffer[512]{};
      if (InternalGetWindowText(hwnd_, buffer, ARRAYSIZE(buffer))) title_ = buffer;
      haveTitle_ = true;
    }
    return title_;
  }

  const std::wstring& ProcessName() {
    if (!haveProcess_) {
      if (readProcessName_) processName_ = readProcessName_(hwnd_);
      haveProcess_ = true;
    }
    return processName_;
  }

 private:
  HWND hwnd_;
  ProcessNameReader readProcessName_;
  std::wstring className_;
  std::wstring title_;
  std::wstring processName_;
  bool haveClass_ = false;
  bool haveTitle_ = false;
  bool haveProcess_ = false;
};

// True when every non-empty match field of `rule` matches the window: an exact
// class name, a title fragment, an exact process file name, all ignoring case.
// Within a field the values are alternatives. `Rule` is any type with
// `classNames`, `titleFragments` and `processNames` string vectors.
template <class Rule>
bool RuleMatchesWindow(const Rule& rule, LazyWindowFacts& facts) {
  auto anyEqual = [](const std::vector<std::wstring>& candidates, const std::wstring& value) {
    return std::any_of(candidates.begin(), candidates.end(),
                       [&](const std::wstring& candidate) { return EqualsInsensitive(value, candidate); });
  };
  // Cheapest observation first; the process name is last because it can be costly.
  if (!rule.classNames.empty() && !anyEqual(rule.classNames, facts.ClassName())) return false;
  if (!rule.titleFragments.empty() &&
      std::none_of(rule.titleFragments.begin(), rule.titleFragments.end(),
                   [&](const std::wstring& fragment) { return ContainsInsensitive(facts.Title(), fragment); })) {
    return false;
  }
  if (!rule.processNames.empty() && !anyEqual(rule.processNames, facts.ProcessName())) return false;
  return true;
}

// The first rule with `treatment` that matches the window, or nullptr. `Rule`
// also needs a `treatment` member.
template <class Rule>
const Rule* FindFirstMatchingRule(
    const std::vector<Rule>& rules, WindowRuleTreatment treatment, LazyWindowFacts& facts) {
  for (const Rule& rule : rules) {
    if (rule.treatment == treatment && RuleMatchesWindow(rule, facts)) return &rule;
  }
  return nullptr;
}

}  // namespace SharedRules

#endif  // MULTIWM_SHARED_WINDOW_RULES_H
