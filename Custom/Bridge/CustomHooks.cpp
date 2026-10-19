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
#include <shlobj.h>

#include <vector>
#include <shellapi.h>

#include <string>

#include "../Core/ImagePreview.h"
#include "../Core/PasswordVault.h"

namespace {

std::wstring ModuleDir();

SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_currentArchive;   // 7zG 进程用：当前正在处理哪个归档
SRWLOCK g_arcLock = SRWLOCK_INIT;

// 设置环境变量 Z7CUSTOM_DEBUG=1 后，hook 调用会追加到 %TEMP%\7zip-custom.log
void DebugLog(const wchar_t *stage, const wchar_t *detail)
{
  // 默认就写日志（%LOCALAPPDATA%\7zip-custom\hook.log），出问题不用再让用户
  // 设环境变量跑一遍；Z7CUSTOM_DEBUG 可以覆盖路径，设成 "-" 则关闭。
  static std::wstring logPath;
  static int enabled = -1;
  if (enabled < 0)
  {
    wchar_t v[MAX_PATH * 2];
    const DWORD n = ::GetEnvironmentVariableW(L"Z7CUSTOM_DEBUG", v, (DWORD)(sizeof(v) / sizeof(v[0])));
    if (n == 1 && v[0] == L'-')
    {
      enabled = 0;
    }
    else
    {
      enabled = 1;
      if (n > 1)
        logPath.assign(v, n);
    }
  }
  if (!enabled)
    return;

  // 依次尝试几个位置，取第一个能写成功的
  std::vector<std::wstring> candidates;
  if (!logPath.empty())
  {
    candidates.push_back(logPath);
  }
  else
  {
    wchar_t local[MAX_PATH] = {};
    if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0)
      candidates.push_back(std::wstring(local) + L"\\7zip-custom\\hook.log");
    if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK)
      candidates.push_back(std::wstring(local) + L"\\7zip-custom\\hook.log");
    wchar_t tmp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, tmp) > 0)
      candidates.push_back(std::wstring(tmp) + L"7zip-custom-hook.log");
    candidates.push_back(ModuleDir() + L"hook.log");
  }

  HANDLE h = INVALID_HANDLE_VALUE;
  for (const auto &c : candidates)
  {
    if (c.empty())
      continue;
    const size_t slash = c.find_last_of(L'\\');
    if (slash != std::wstring::npos)
      ::CreateDirectoryW(c.substr(0, slash).c_str(), nullptr);

    h = ::CreateFileW(c.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
      break;
  }
  if (h == INVALID_HANDLE_VALUE)
    return;

  std::wstring line = L"[";
  line += stage;
  line += L"] ";
  line += (detail ? detail : L"(null)");
  line += L"\r\n";

  // 用系统 ANSI 代码页写（中文系统就是 GBK），这样控制台 type 出来不是乱码
  int n = ::WideCharToMultiByte(CP_ACP, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
  if (n > 0)
  {
    std::string narrow((size_t)n, '\0');
    ::WideCharToMultiByte(CP_ACP, 0, line.c_str(), (int)line.size(), &narrow[0], n, nullptr, nullptr);
    DWORD wrote = 0;
    ::WriteFile(h, narrow.data(), (DWORD)narrow.size(), &wrote, nullptr);
  }
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

extern "C" int CUSTOM_HOOK_API CustomHook_AskPassword(const wchar_t *archivePath,
                                                      wchar_t *buf, int cch)
{
  if (!archivePath || !*archivePath || !buf || cch <= 0)
    return 0;

  const std::wstring dll = ModuleDir() + L"7zVaultUi.dll";
  HMODULE h = ::LoadLibraryW(dll.c_str());
  if (!h)
  {
    DebugLog(L"AskPassword", L"7zVaultUi.dll missing");
    return 0;
  }

  typedef int (__cdecl *AskFn)(void *, const wchar_t *, wchar_t *, int);
  AskFn fn = (AskFn)::GetProcAddress(h, "CustomUi_AskPassword");
  if (!fn)
  {
    ::FreeLibrary(h);
    return 0;
  }

  HWND owner = ::GetActiveWindow();
  if (!owner)
    owner = ::GetForegroundWindow();
  const int r = fn(owner, archivePath, buf, cch);
  ::FreeLibrary(h);
  DebugLog(L"AskPassword", r == 1 ? L"ok" : (r < 0 ? L"cancelled" : L"unavailable"));
  return r;
}

extern "C" void CUSTOM_HOOK_API CustomHook_SetCurrentArchive(const wchar_t *archivePath)
{
  ::AcquireSRWLockExclusive(&g_arcLock);
  g_currentArchive = (archivePath && *archivePath) ? archivePath : L"";
  ::ReleaseSRWLockExclusive(&g_arcLock);
}

extern "C" int CUSTOM_HOOK_API CustomHook_TryPasswordForCurrent(wchar_t *buf, int cch)
{
  std::wstring path;
  ::AcquireSRWLockExclusive(&g_arcLock);
  path = g_currentArchive;
  ::ReleaseSRWLockExclusive(&g_arcLock);

  if (path.empty())
    return 0;
  return CustomHook_TryPassword(path.c_str(), buf, cch);
}

extern "C" int CUSTOM_HOOK_API CustomHook_AskPasswordForCurrent(wchar_t *buf, int cch)
{
  std::wstring path;
  ::AcquireSRWLockExclusive(&g_arcLock);
  path = g_currentArchive;
  ::ReleaseSRWLockExclusive(&g_arcLock);

  if (path.empty())
    return 0;
  return CustomHook_AskPassword(path.c_str(), buf, cch);
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

  // 7-Zip 只会把"对话框里输入的密码"存进 CFolderLink；如果密码是我们从库里
  // 直接喂给它的，CFolderLink.Password 就是空的。所以这里必须自己再查一次库，
  // 否则加密包会拿空密码去解，必然失败。
  std::wstring pwd = password ? password : L"";
  if (pwd.empty())
  {
    ::AcquireSRWLockExclusive(&g_lock);
    custom::PasswordVault &vault = custom::PasswordVault::Instance();
    vault.Load(false);
    std::wstring found;
    if (vault.FindForArchive(archivePath, found))
      pwd = found;
    ::ReleaseSRWLockExclusive(&g_lock);
    DebugLog(L"TryOpenImage.pwd", pwd.empty() ? L"(vault miss)" : L"(from vault)");
  }

  // 自测时用：只准备文件，不真的调起看图程序
  wchar_t nolaunch[8];
  const bool launch = ::GetEnvironmentVariableW(L"Z7CUSTOM_PREVIEW_NOLAUNCH", nolaunch,
                                                (DWORD)(sizeof(nolaunch) / sizeof(nolaunch[0]))) == 0;

  const custom::PreviewResult r = custom::PrepareImagePreview(
      archivePath, itemRelPath, pwd, launch);

  DebugLog(L"TryOpenImage", r.ok ? r.openedPath.c_str() : r.error.c_str());
  if (!r.ok && !r.command.empty())
    DebugLog(L"TryOpenImage.cmd", r.command.c_str());

  if (r.ok)
    return 1;

  // 任何失败都把原因摆到明面上：退回 7-Zip 只会再弹一个"拒绝访问"，
  // 用户看不懂，我们也拿不到信息。
  {
    std::wstring msg = L"预览这张图片失败：\n\n";
    msg += r.error.empty() ? L"(未知原因)" : r.error;
    if (!r.command.empty())
    {
      msg += L"\n\n执行的命令：\n";
      msg += r.command;
    }
    msg += L"\n\n（把这段截图发给开发者即可定位）";
    ::MessageBoxW(nullptr, msg.c_str(), L"7-Zip 看图", MB_ICONWARNING);
    return 1;
  }
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
  kCmdVaultFolder  = 0x7A03,
  kCmdVaultDiag    = 0x7A04
};

const UINT_PTR kSubclassId = 0x7A99;

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

void ShowDiagnostics(HWND hwnd)
{
  std::wstring msg = L"7zip-custom 诊断信息\n\n";
  msg += L"构建时间: ";
  msg += L"" __DATE__ " " __TIME__;
  msg += L"\n程序目录: ";
  msg += ModuleDir();

  const std::wstring sevenZip = ModuleDir() + L"7z.exe";
  const DWORD a = ::GetFileAttributesW(sevenZip.c_str());
  msg += L"\n7z.exe  : ";
  msg += (a != INVALID_FILE_ATTRIBUTES) ? L"存在" : L"缺失";

  wchar_t tmp[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, tmp);
  msg += L"\n临时目录: ";
  msg += tmp;

  msg += L"\n密码库  : ";
  msg += custom::VaultDir();

  msg += L"\n日志    : ";
  msg += ModuleDir() + L"hook.log（或 %LOCALAPPDATA%\\7zip-custom\\hook.log）";

  ::MessageBoxW(hwnd, msg.c_str(), L"7zip-custom 诊断", MB_ICONINFORMATION);
}

void OnVaultCommand(HWND hwnd, unsigned cmd)
{
  switch (cmd)
  {
    case kCmdVaultManager: ShowVaultUi(hwnd, 0); break;
    case kCmdVaultAdd:     ShowVaultUi(hwnd, 1); break;
    case kCmdVaultDiag:    ShowDiagnostics(hwnd); break;
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
    if (cmd >= kCmdVaultManager && cmd <= kCmdVaultDiag)
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
  DebugLog(L"AttachMainMenu.enter", L"called");

  HWND hwnd = (HWND)hWnd;
  if (!hwnd || !::IsWindow(hwnd))
  {
    DebugLog(L"AttachMainMenu", L"bad hwnd");
    return 0;
  }

  HMENU main = ::GetMenu(hwnd);
  if (!main)
  {
    DebugLog(L"AttachMainMenu", L"no menu");
    return 0;
  }

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
    ::AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(sub, MF_STRING, kCmdVaultDiag,    L"诊断信息(&D)...");

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
