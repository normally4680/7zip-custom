// Custom/UI/VaultUiExports.cpp
//
// 把密码库对话框编译成 DLL，供 7zFM.exe 进程内调用（菜单"密码库"）。
// 单独做成 DLL 的原因：资源随 DLL 走，不需要往 7-Zip 的 .rc 里塞任何东西。

#include "AskPassword.h"
#include "VaultDialog.h"

#include <objbase.h>

static HINSTANCE g_dllInst = nullptr;

extern "C" __declspec(dllexport) int __cdecl CustomUi_ShowVault(void *owner, int mode,
                                                               const wchar_t *suggestName)
{
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int r = ShowVaultDialogBox(g_dllInst, (HWND)owner, mode, suggestName);
  ::CoUninitialize();
  return r;
}

extern "C" __declspec(dllexport) int __cdecl CustomUi_AskPassword(void *owner,
                                                                  const wchar_t *archivePath,
                                                                  wchar_t *buf, int cch)
{
  std::wstring pw;
  const int r = AskPasswordDialog(g_dllInst, (HWND)owner, archivePath, pw);
  if (r == 1 && buf && cch > 0 && (int)pw.size() < cch)
    memcpy(buf, pw.c_str(), (pw.size() + 1) * sizeof(wchar_t));
  return r;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
  if (reason == DLL_PROCESS_ATTACH)
  {
    g_dllInst = inst;
    ::DisableThreadLibraryCalls(inst);
  }
  return TRUE;
}
