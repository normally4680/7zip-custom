// Custom/Bridge/CustomHooks.cpp
//
// hook 的实现层：只做参数校验、串行化、把活派发给 Core/。
// 这里不写业务逻辑，也不碰 7-Zip 的任何类型。

#include "CustomHooks.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>

#include "../Core/ImagePreview.h"
#include "../Core/PasswordVault.h"

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;

// 设置环境变量 Z7CUSTOM_DEBUG=1 后，hook 调用会追加到 %TEMP%\7zip-custom.log
void DebugLog(const wchar_t *stage, const wchar_t *detail)
{
  static std::wstring logPath;
  static int enabled = -1;
  if (enabled < 0)
  {
    wchar_t v[MAX_PATH * 2];
    const DWORD n = ::GetEnvironmentVariableW(L"Z7CUSTOM_DEBUG", v, (DWORD)(sizeof(v) / sizeof(v[0])));
    enabled = (n > 0) ? 1 : 0;
    if (enabled)
    {
      // 值里带路径分隔符就当成日志文件路径，否则用 %TEMP%
      logPath = (wcschr(v, L'\\') || wcschr(v, L'/')) ? std::wstring(v) : std::wstring();
    }
  }
  if (!enabled)
    return;

  std::wstring path = logPath;
  if (path.empty())
  {
    wchar_t tmp[MAX_PATH];
    ::GetTempPathW(MAX_PATH, tmp);
    path = std::wstring(tmp) + L"7zip-custom.log";
  }

  HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return;

  std::wstring line = L"[";
  line += stage;
  line += L"] ";
  line += (detail ? detail : L"(null)");
  line += L"\r\n";

  DWORD wrote = 0;
  ::WriteFile(h, line.c_str(), (DWORD)(line.size() * sizeof(wchar_t)), &wrote, nullptr);
  ::CloseHandle(h);
}

// 防止每次打开都要解密一遍库文件之外的重复开销
bool CopyToBuf(const std::wstring &src, wchar_t *buf, int cch)
{
  if (!buf || cch <= 0)
    return false;
  if (src.empty() || (int)src.size() + 1 > cch)
    return false;
  memcpy(buf, src.c_str(), (src.size() + 1) * sizeof(wchar_t));
  return true;
}

} // namespace

extern "C" int CUSTOM_HOOK_API CustomHook_TryPassword(const wchar_t *archivePath,
                                                      wchar_t *buf, int cch)
{
  if (!archivePath || !*archivePath || !buf || cch <= 0)
    return 0;

  DebugLog(L"TryPassword", archivePath);

  ::AcquireSRWLockExclusive(&g_lock);
  custom::PasswordVault &vault = custom::PasswordVault::Instance();
  const bool loaded = vault.Load(false);
  std::wstring pw;
  const bool hit = loaded && vault.FindForArchive(archivePath, pw);
  ::ReleaseSRWLockExclusive(&g_lock);

  if (!hit)
  {
    DebugLog(L"TryPassword.miss", loaded ? L"loaded" : vault.LastError().c_str());
    return 0;
  }
  DebugLog(L"TryPassword.hit", pw.c_str());
  return CopyToBuf(pw, buf, cch) ? 1 : 0;
}

extern "C" void CUSTOM_HOOK_API CustomHook_OnPasswordAccepted(const wchar_t *archivePath,
                                                              const wchar_t *password)
{
  if (!archivePath || !*archivePath || !password || !*password)
    return;

  ::AcquireSRWLockExclusive(&g_lock);
  custom::PasswordVault &vault = custom::PasswordVault::Instance();
  vault.Load(false);
  vault.Remember(archivePath, password);
  ::ReleaseSRWLockExclusive(&g_lock);
}

extern "C" int CUSTOM_HOOK_API CustomHook_TryOpenImage(const wchar_t *archivePath,
                                                       const wchar_t *itemRelPath,
                                                       const wchar_t *password)
{
  if (!archivePath || !*archivePath || !itemRelPath || !*itemRelPath)
    return 0;
  if (!custom::IsImageFile(itemRelPath))
    return 0;

  const custom::PreviewResult r = custom::PrepareImagePreview(
      archivePath, itemRelPath, password ? std::wstring(password) : std::wstring(), true);

  DebugLog(L"TryOpenImage", r.ok ? r.openedPath.c_str() : r.error.c_str());
  // 失败就返回 0，让 7-Zip 走它原来的"解压单个文件再打开"流程
  return r.ok ? 1 : 0;
}

// ---------------------------------------------------------------------------
// 7zFM 主窗口菜单注入
// ---------------------------------------------------------------------------

namespace {

// 命令 ID 选在 0x7A00 段，远离 7-Zip 自己的 IDM_* 区间
enum
{
  kCmdVaultManager = 0x7A01,
  kCmdVaultAdd     = 0x7A02,
  kCmdVaultFolder  = 0x7A03
};

const UINT_PTR kSubclassId = 0x7A99;

std::wstring ModuleDir()
{
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
    return std::wstring();
  std::wstring p(buf, n);
  const size_t pos = p.find_last_of(L"\\/");
  if (pos == std::wstring::npos)
    return std::wstring();
  return p.substr(0, pos + 1);
}

bool MenuHasVault(HMENU main)
{
  const int count = ::GetMenuItemCount(main);
  for (int i = 0; i < count; ++i)
  {
    HMENU sub = ::GetSubMenu(main, i);
    if (sub && ::GetMenuState(sub, kCmdVaultManager, MF_BYCOMMAND) != (UINT)-1)
      return true;
  }
  return false;
}

void ShowVaultUi(HWND hwnd, int mode)
{
  const std::wstring dll = ModuleDir() + L"7zVaultUi.dll";
  HMODULE h = ::LoadLibraryW(dll.c_str());
  if (!h)
  {
    ::MessageBoxW(hwnd,
        L"找不到 7zVaultUi.dll。\n请确认它和 7zFM.exe 放在同一个目录。",
        L"密码库", MB_ICONERROR);
    return;
  }

  typedef int (__cdecl *ShowFn)(void *, int, const wchar_t *);
  ShowFn fn = (ShowFn)::GetProcAddress(h, "CustomUi_ShowVault");
  if (!fn)
  {
    ::FreeLibrary(h);
    ::MessageBoxW(hwnd, L"7zVaultUi.dll 已损坏（缺少导出函数）。", L"密码库", MB_ICONERROR);
    return;
  }

  fn(hwnd, mode, nullptr);
  ::FreeLibrary(h);
}

void OnVaultCommand(HWND hwnd, unsigned cmd)
{
  switch (cmd)
  {
    case kCmdVaultManager: ShowVaultUi(hwnd, 0); break;
    case kCmdVaultAdd:     ShowVaultUi(hwnd, 1); break;
    case kCmdVaultFolder:
    {
      const std::wstring dir = custom::VaultDir();
      if (!dir.empty())
        ::ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    }
  }
}

LRESULT CALLBACK MainWndSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR id, DWORD_PTR)
{
  if (msg == WM_COMMAND)
  {
    const unsigned cmd = LOWORD(wp);
    if (cmd >= kCmdVaultManager && cmd <= kCmdVaultFolder)
    {
      OnVaultCommand(hwnd, cmd);
      return 0;
    }
  }
  return ::DefSubclassProc(hwnd, msg, wp, lp);
}

} // namespace

extern "C" int CUSTOM_HOOK_API CustomHook_AttachMainMenu(void *hWnd)
{
  HWND hwnd = (HWND)hWnd;
  if (!hwnd || !::IsWindow(hwnd))
    return 0;

  HMENU main = ::GetMenu(hwnd);
  if (!main)
    return 0;

  // 菜单重建后会被重新调用：已经有就不重复插入
  if (!MenuHasVault(main))
  {
    HMENU sub = ::CreatePopupMenu();
    if (!sub)
      return 0;

    ::AppendMenuW(sub, MF_STRING, kCmdVaultManager, L"管理密码库(&M)...");
    ::AppendMenuW(sub, MF_STRING, kCmdVaultAdd,     L"新增密码条目(&A)...");
    ::AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(sub, MF_STRING, kCmdVaultFolder,  L"打开密码库存放目录");

    const int count = ::GetMenuItemCount(main);
    // 插在"帮助"之前；插到末尾会影响 7-Zip 自己的菜单索引
    const int pos = (count > 0) ? count - 1 : 0;
    if (!::InsertMenuW(main, (UINT)pos, MF_BYPOSITION | MF_POPUP, (UINT_PTR)sub, L"密码库(&P)"))
    {
      ::DestroyMenu(sub);
      return 0;
    }
    ::DrawMenuBar(hwnd);
  }

  ::SetWindowSubclass(hwnd, MainWndSubclass, kSubclassId, 0);
  DebugLog(L"AttachMainMenu", L"ok");
  return 1;
}

extern "C" void CUSTOM_HOOK_API CustomHook_OnArchiveOpened(const wchar_t *archivePath)
{
  (void)archivePath;
}
