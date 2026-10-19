// Custom/UI/VaultApp.cpp
//
// 独立的密码库管理器（7zVault.exe）。界面逻辑全在 VaultDialog.cpp 里，
// 这里只负责进程级初始化。

#include "VaultDialog.h"

#include <objbase.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' \
  name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
  processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int r = ShowVaultDialogBox(inst, nullptr, 0, nullptr);
  ::CoUninitialize();
  return r;
}
