// Custom/Bridge/CustomHooks.cpp
//
// hook 的实现层：只做参数校验、串行化、把活派发给 Core/。
// 这里不写业务逻辑，也不碰 7-Zip 的任何类型。

#include "CustomHooks.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <string>

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
                                                       const wchar_t *itemRelPath)
{
  (void)archivePath;
  (void)itemRelPath;
  // feature/image-preview 分支在这里接入
  return 0;
}

extern "C" void CUSTOM_HOOK_API CustomHook_OnArchiveOpened(const wchar_t *archivePath)
{
  (void)archivePath;
}
