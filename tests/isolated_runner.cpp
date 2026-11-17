#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

// Apply DLL-signature isolation at process creation, before Windhawk's parent
// hook or background injector can load anything into a test. Static test builds
// need only Microsoft system DLLs. No installed mod or global setting changes.
int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: isolated_runner.exe <test executable>\n");
    return 125;
  }
  SIZE_T attributeBytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
  if (!attributeBytes) {
    std::fprintf(stderr, "Cannot size process attributes: %lu\n", GetLastError());
    return 125;
  }
  std::vector<unsigned char> attributeStorage(attributeBytes);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) {
    std::fprintf(stderr, "Cannot initialize process attributes: %lu\n", GetLastError());
    return 125;
  }
  DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON;
  if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                                 &policy, sizeof(policy), nullptr, nullptr)) {
    std::fprintf(stderr, "Cannot set test DLL policy: %lu\n", GetLastError());
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    return 125;
  }
  std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
  PROCESS_INFORMATION child{};
  const BOOL created = CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
      nullptr, nullptr, &startup.StartupInfo, &child);
  const DWORD creationError = GetLastError();
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  if (!created) {
    std::fprintf(stderr, "Cannot launch isolated test: %lu\n", creationError);
    return 125;
  }

  PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY actual{};
  const bool isolated = GetProcessMitigationPolicy(child.hProcess, ProcessSignaturePolicy,
                                                   &actual, sizeof(actual)) && actual.MicrosoftSignedOnly;
  DWORD result = 125;
  if (!isolated || ResumeThread(child.hThread) == static_cast<DWORD>(-1)) {
    std::fprintf(stderr, "Test isolation could not be verified; test was not run.\n");
    TerminateProcess(child.hProcess, result);
  } else {
    std::printf("Test isolation: Microsoft-signed DLLs only.\n");
    std::fflush(stdout);
    if (WaitForSingleObject(child.hProcess, INFINITE) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(child.hProcess, &result)) {
      std::fprintf(stderr, "Cannot wait for isolated test: %lu\n", GetLastError());
      TerminateProcess(child.hProcess, 125);
      result = 125;
    }
  }
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  return static_cast<int>(result);
}
