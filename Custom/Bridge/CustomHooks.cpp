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
#include <tlhelp32.h>

#include <vector>
#include <shellapi.h>

#include <string>

#include "../Core/ImagePreview.h"
#include "../Core/PasswordVault.h"
#include "../Core/SevenZipInProc.h"
#include "../Core/ShellIntegration.h"

namespace {

std::wstring ModuleDir();

SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_currentArchive;   // 7zG 进程用：当前正在处理哪个归档
SRWLOCK g_arcLock = SRWLOCK_INIT;

// 用户刚在密码框里输入过的密码（只放内存，进程退出即消失）。
// 有些归档只加密数据、不加密文件名，打开时 7-Zip 不会问密码；
// 到"看图"才需要，这时用它补上，避免再问一次/拿空密码去解。
struct RecentPassword
{
  std::wstring archive;
  std::wstring password;
};
std::vector<RecentPassword> g_recentPasswords;
SRWLOCK g_pwdLock = SRWLOCK_INIT;

void RememberPassword(const wchar_t *archivePath, const wchar_t *password)
{
  if (!archivePath || !*archivePath || !password || !*password)
    return;

  ::AcquireSRWLockExclusive(&g_pwdLock);
  for (auto &e : g_recentPasswords)
  {
    if (::lstrcmpiW(e.archive.c_str(), archivePath) == 0)
    {
      e.password = password;
      ::ReleaseSRWLockExclusive(&g_pwdLock);
      return;
    }
  }
  if (g_recentPasswords.size() >= 8)
    g_recentPasswords.erase(g_recentPasswords.begin());
  RecentPassword rp;
  rp.archive = archivePath;
  rp.password = password;
  g_recentPasswords.push_back(rp);
  ::ReleaseSRWLockExclusive(&g_pwdLock);
}

bool RecallPassword(const wchar_t *archivePath, std::wstring &out)
{
  if (!archivePath || !*archivePath)
    return false;

  bool hit = false;
  ::AcquireSRWLockShared(&g_pwdLock);
  for (const auto &e : g_recentPasswords)
  {
    if (::lstrcmpiW(e.archive.c_str(), archivePath) == 0)
    {
      out = e.password;
      hit = true;
      break;
    }
  }
  ::ReleaseSRWLockShared(&g_pwdLock);
  return hit;
}

// 默认开启：便携包写 data\hook.log，其余写 %LOCALAPPDATA%\7zip-custom\hook.log。
// Z7CUSTOM_DEBUG=<路径> 可指定位置，设成 "-" 关闭。
SRWLOCK g_logLock = SRWLOCK_INIT;
std::wstring g_logFile;
bool g_logResolved = false;

std::wstring ResolveLogPath()
{
  wchar_t v[MAX_PATH * 2];
  const DWORD n = ::GetEnvironmentVariableW(L"Z7CUSTOM_DEBUG", v,
                                            (DWORD)(sizeof(v) / sizeof(v[0])));
  if (n == 1 && v[0] == L'-')
    return std::wstring();
  if (n > 1)
    return std::wstring(v, n);

  // 依次尝试几个位置，取第一个能写成功的
  std::vector<std::wstring> candidates;
  const std::wstring moduleDir = ModuleDir();

  // 便携模式：exe 旁边有 data\ 就写到 data\hook.log，日志跟着包走
  if (!moduleDir.empty())
  {
    const std::wstring dataDir = moduleDir + L"data";
    const DWORD dataAttr = ::GetFileAttributesW(dataDir.c_str());
    if (dataAttr != INVALID_FILE_ATTRIBUTES && (dataAttr & FILE_ATTRIBUTE_DIRECTORY))
      candidates.push_back(dataDir + L"\\hook.log");
  }

  wchar_t local[MAX_PATH] = {};
  if (::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0)
    candidates.push_back(std::wstring(local) + L"\\7zip-custom\\hook.log");
  if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK)
    candidates.push_back(std::wstring(local) + L"\\7zip-custom\\hook.log");
  wchar_t tmp[MAX_PATH] = {};
  if (::GetTempPathW(MAX_PATH, tmp) > 0)
    candidates.push_back(std::wstring(tmp) + L"7zip-custom-hook.log");
  if (!moduleDir.empty())
    candidates.push_back(moduleDir + L"hook.log");

  for (const auto &c : candidates)
  {
    if (c.empty())
      continue;
    const size_t slash = c.find_last_of(L'\\');
    if (slash != std::wstring::npos)
      ::CreateDirectoryW(c.substr(0, slash).c_str(), nullptr);

    HANDLE h = ::CreateFileW(c.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
      ::CloseHandle(h);
      return c;
    }
  }
  return std::wstring();
}

// 超过 1 MB 就换一个文件，只保留上一份，避免日志无限增长
void RotateLogIfLarge(const std::wstring &path)
{
  WIN32_FILE_ATTRIBUTE_DATA fad = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
    return;
  const unsigned long long size =
      ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
  if (size < 1024ull * 1024ull)
    return;
  ::MoveFileExW(path.c_str(), (path + L".old").c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void DebugLog(const wchar_t *stage, const wchar_t *detail)
{
  std::wstring path;
  ::AcquireSRWLockExclusive(&g_logLock);
  if (!g_logResolved)
  {
    g_logResolved = true;
    g_logFile = ResolveLogPath();
    if (!g_logFile.empty())
      RotateLogIfLarge(g_logFile);
  }
  path = g_logFile;
  ::ReleaseSRWLockExclusive(&g_logLock);
  if (path.empty())
    return;

  HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

// 诊断用：谁把 7zFM 拉起来的。explorer.exe/cmd.exe 才说明是正常双击启动，
// Qoder/WorkBuddy 之类的宿主进程往往意味着受限上下文。
std::wstring ParentProcessInfo()
{
  const DWORD myPid = ::GetCurrentProcessId();
  DWORD parentPid = 0;
  std::wstring name;

  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE)
  {
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe))
    {
      do
      {
        if (pe.th32ProcessID == myPid)
        {
          parentPid = pe.th32ParentProcessID;
          break;
        }
      } while (::Process32NextW(snap, &pe));
    }
    if (parentPid)
    {
      pe.dwSize = sizeof(pe);
      if (::Process32FirstW(snap, &pe))
      {
        do
        {
          if (pe.th32ProcessID == parentPid)
          {
            name = pe.szExeFile;
            break;
          }
        } while (::Process32NextW(snap, &pe));
      }
    }
    ::CloseHandle(snap);
  }

  if (name.empty())
    return std::wstring();
  wchar_t buf[128];
  swprintf_s(buf, L"%s (pid %lu)", name.c_str(), (unsigned long)parentPid);
  return buf;
}

std::wstring IntegrityLevelInfo()
{
  HANDLE tok = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok))
    return L"(无法读取)";

  DWORD len = 0;
  ::GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &len);
  std::vector<BYTE> buf(len);
  std::wstring out = L"(未知)";
  if (len && ::GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), len, &len))
  {
    TOKEN_MANDATORY_LABEL *label = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf.data());
    const DWORD count = *::GetSidSubAuthorityCount(label->Label.Sid);
    const DWORD rid = *::GetSidSubAuthority(label->Label.Sid, count - 1);
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID) out = L"System";
    else if (rid >= SECURITY_MANDATORY_HIGH_RID) out = L"High";
    else if (rid >= SECURITY_MANDATORY_MEDIUM_RID) out = L"Medium";
    else if (rid >= SECURITY_MANDATORY_LOW_RID) out = L"Low";
    else out = L"Untrusted";
  }
  ::CloseHandle(tok);
  return out;
}

bool IsAppContainerProcess()
{
  HANDLE tok = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &tok))
    return false;
  // TokenIsAppContainer = 29；SDK 头文件会按 _WIN32_WINNT 裁剪，这里直接用数值
  const TOKEN_INFORMATION_CLASS kTokenIsAppContainer = (TOKEN_INFORMATION_CLASS)29;
  DWORD isAc = 0, len = 0;
  const BOOL ok = ::GetTokenInformation(tok, kTokenIsAppContainer, &isAc, sizeof(isAc), &len);
  ::CloseHandle(tok);
  return ok && isAc != 0;
}

std::wstring JobInfo()
{
  BOOL inJob = FALSE;
  if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &inJob) || !inJob)
    return L"不在";

  JOBOBJECT_BASIC_UI_RESTRICTIONS ui = {};
  std::wstring r = L"在";
  if (::QueryInformationJobObject(nullptr, JobObjectBasicUIRestrictions,
                                  &ui, sizeof(ui), nullptr))
  {
    wchar_t b[64];
    swprintf_s(b, L"，UI 限制=0x%lX", (unsigned long)ui.UIRestrictionsClass);
    r += b;
  }
  return r;
}

// 从当前进程里真建一个目录再删掉——这比 GetFileAttributes 更能说明能不能写
std::wstring WriteProbe(const std::wstring &dir)
{
  if (dir.empty())
    return L"(无)";

  wchar_t name[64];
  swprintf_s(name, L"7z-custom-probe-%lu", (unsigned long)::GetCurrentProcessId());
  std::wstring path = dir;
  if (path.back() != L'\\')
    path += L'\\';
  path += name;

  if (::CreateDirectoryW(path.c_str(), nullptr))
  {
    ::RemoveDirectoryW(path.c_str());
    return L"可写";
  }
  const DWORD e = ::GetLastError();
  if (e == ERROR_ALREADY_EXISTS)
  {
    ::RemoveDirectoryW(path.c_str());
    return L"可写";
  }
  wchar_t b[64];
  swprintf_s(b, L"拒绝 (err %lu)", (unsigned long)e);
  return b;
}

// ---------------------------------------------------------------- 看图取密码

// 看图前把密码"验明正身"：按优先级逐个候选试解一个条目，第一个真正能用的
// 胜出。只加密数据、不加密文件名的包，打开时 7-Zip 不会问密码，密码库里
// 按名字匹配又可能落空——在库里把所有条目都试一遍就能兜住这种情况；
// 一个都不行时弹我们自己的密码框（带密码库选择），用户取消则返回空串，
// 让 Honeyview 自己弹它的框（维持老行为）。
//   hint          : 已知的候选（CFolderLink / 名字匹配查到的库密码），可为空
//   knownEncrypted: ArchiveIsEncrypted 已确认加密（false = 判断不了）
//   allowPrompt   : 候选全失败时是否允许弹框
std::wstring ResolveVerifiedPassword(const std::wstring &dll,
                                     const std::wstring &archivePath,
                                     const std::wstring &inner,
                                     const std::wstring &hint,
                                     bool knownEncrypted,
                                     bool allowPrompt)
{
  // 候选去重收集：hint → 会话缓存 → 密码库全部条目（封顶 20 条防大库卡顿）
  std::vector<std::wstring> cands;
  auto pushCand = [&cands](const std::wstring &p)
  {
    if (p.empty() || cands.size() >= 20)
      return;
    for (const auto &c : cands)
      if (c == p)
        return;
    cands.push_back(p);
  };

  pushCand(hint);
  {
    std::wstring cached;
    if (RecallPassword(archivePath.c_str(), cached))
      pushCand(cached);
  }
  {
    ::AcquireSRWLockShared(&g_lock);
    custom::PasswordVault &vault = custom::PasswordVault::Instance();
    if (vault.Load(false))
    {
      for (const auto &e : vault.Entries())
        pushCand(e.password);
    }
    ::ReleaseSRWLockShared(&g_lock);
  }

  std::wstring verifiedPwd;
  bool fromVault = false;
  for (const auto &c : cands)
  {
    bool ok = false;
    if (custom::ArchiveTryPassword(dll.c_str(), archivePath, inner, c, ok) && ok)
    {
      verifiedPwd = c;
      // 是不是来自密码库（用于决定要不要把新验证的密码写回库）
      ::AcquireSRWLockShared(&g_lock);
      custom::PasswordVault &vault = custom::PasswordVault::Instance();
      for (const auto &e : vault.Entries())
        if (e.password == c) { fromVault = true; break; }
      ::ReleaseSRWLockShared(&g_lock);
      break;
    }
  }

  if (verifiedPwd.empty() && allowPrompt && knownEncrypted)
  {
    wchar_t asked[1024] = {};
    const int a = CustomHook_AskPassword(::GetActiveWindow(), archivePath.c_str(),
                                         asked, (int)(sizeof(asked) / sizeof(asked[0])));
    if (a == 1 && asked[0])
    {
      bool ok = false;
      if (custom::ArchiveTryPassword(dll.c_str(), archivePath, inner, asked, ok) && ok)
        verifiedPwd = asked;
      else
        DebugLog(L"ResolvePwd", L"(asked password failed verify)");
    }
    ::SecureZeroMemory(asked, sizeof(asked));
    if (verifiedPwd.empty())
      DebugLog(L"ResolvePwd", a < 0 ? L"(cancelled)" : L"(ask failed)");
  }

  if (verifiedPwd.empty())
  {
    DebugLog(L"ResolvePwd", L"(no verified password)");
    return verifiedPwd;
  }

  DebugLog(L"ResolvePwd", fromVault ? L"(verified, from vault)" : L"(verified, new)");
  RememberPassword(archivePath.c_str(), verifiedPwd.c_str());
  if (!fromVault)
  {
    // 新验证出来的密码（用户手输/对话框缓存）写回密码库，下次直接命中
    ::AcquireSRWLockExclusive(&g_lock);
    custom::PasswordVault &vault = custom::PasswordVault::Instance();
    if (vault.Load(false))
      vault.Remember(archivePath, verifiedPwd);
    ::ReleaseSRWLockExclusive(&g_lock);
  }
  return verifiedPwd;
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
  DebugLog(L"TryPassword.hit", L"(from vault)");
  RememberPassword(archivePath, pw.c_str());
  return CopyToBuf(pw, buf, cch) ? 1 : 0;
}

extern "C" int CUSTOM_HOOK_API CustomHook_AskPassword(void *owner,
                                                      const wchar_t *archivePath,
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

  HWND hOwner = (HWND)owner;
  if (!hOwner || !::IsWindow(hOwner))
  {
    hOwner = ::GetActiveWindow();
    if (!hOwner)
      hOwner = ::GetForegroundWindow();
  }
  const int r = fn(hOwner, archivePath, buf, cch);
  ::FreeLibrary(h);
  if (r == 1)
    RememberPassword(archivePath, buf);
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
  ::AcquireSRWLockShared(&g_arcLock);
  path = g_currentArchive;
  ::ReleaseSRWLockShared(&g_arcLock);

  if (path.empty())
    return 0;
  return CustomHook_TryPassword(path.c_str(), buf, cch);
}

extern "C" int CUSTOM_HOOK_API CustomHook_AskPasswordForCurrent(void *owner,
                                                               wchar_t *buf, int cch)
{
  std::wstring path;
  ::AcquireSRWLockShared(&g_arcLock);
  path = g_currentArchive;
  ::ReleaseSRWLockShared(&g_arcLock);

  if (path.empty())
    return 0;
  return CustomHook_AskPassword(owner, path.c_str(), buf, cch);
}

extern "C" void CUSTOM_HOOK_API CustomHook_OnPasswordAccepted(const wchar_t *archivePath,
                                                              const wchar_t *password)
{
  if (!archivePath || !*archivePath || !password || !*password)
    return;

  RememberPassword(archivePath, password);

  ::AcquireSRWLockExclusive(&g_lock);
  custom::PasswordVault &vault = custom::PasswordVault::Instance();
  if (vault.Load(false))
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
  if (!pwd.empty())
  {
    RememberPassword(archivePath, pwd.c_str());
  }
  else
  {
    ::AcquireSRWLockExclusive(&g_lock);
    custom::PasswordVault &vault = custom::PasswordVault::Instance();
    vault.Load(false);
    std::wstring found;
    if (vault.FindForArchive(archivePath, found))
      pwd = found;
    ::ReleaseSRWLockExclusive(&g_lock);

    bool fromCache = false;
    if (pwd.empty())
      fromCache = RecallPassword(archivePath, pwd);
    DebugLog(L"TryOpenImage.pwd",
             pwd.empty() ? L"(vault/cache miss)"
                         : (fromCache ? L"(from dialog cache)" : L"(from vault)"));
  }

  // 自测时用：只准备文件，不真的调起看图程序
  wchar_t nolaunch[8];
  const bool launch = ::GetEnvironmentVariableW(L"Z7CUSTOM_PREVIEW_NOLAUNCH", nolaunch,
                                                (DWORD)(sizeof(nolaunch) / sizeof(nolaunch[0]))) == 0;

  // 首选：Honeyview 原生打开，完全不解压。调用方式按格式/状态分流
  //（能力均为 PrintWindow 实测，详见 Custom/README.md）：
  //   zip/tar        → 竖线 "archive|inner"，加密 zip 自动填验证过的密码
  //   7z 未加密 ≤0.3 → 竖线（Bandizip 压的包都是 0.3）
  //   7z 未加密 0.4  → 整包打开后补竖线（26.04 写 0.4，竖线快开只认 ≤0.3）
  //   7z 加密        → 整包打开 + 自动填密码（竖线不支持 7z 解密），
  //                    显示的是第一张图，用户在 Honeyview 里翻页
  if (launch)
  {
    const std::wstring hv = custom::FindHoneyview();
    if (!hv.empty())
    {
      const wchar_t *dot = ::wcsrchr(archivePath, L'.');
      const bool is7z = dot && ::lstrcmpiW(dot, L".7z") == 0;

      bool encrypted = false;
      const std::wstring dll = ModuleDir() + L"7z.dll";
      const bool known = custom::ArchiveIsEncrypted(dll.c_str(), archivePath, encrypted);
      const std::wstring inner = custom::NormalizeInnerPath(archivePath, itemRelPath);
      if (!inner.empty() && custom::IsSafeInnerPath(inner))
      {
        bool ok = false;
        std::wstring detail = L"honeyview: " + inner;

        if (is7z && known && encrypted)
        {
          // 竖线不支持 7z 解密：整包打开，密码先验证再自动填；
          // 用户取消密码框时带空密码整包打开，Honeyview 自己问
          const std::wstring pwd2 = ResolveVerifiedPassword(dll, archivePath, inner,
                                                            pwd, true, true);
          ok = custom::LaunchHoneyviewWholeArchive(hv, archivePath, pwd2);
          detail = std::wstring(L"honeyview whole: ") + archivePath;
          detail += pwd2.empty() ? L" (prompt)" : L" (autofill)";
        }
        else if (is7z && !known)
        {
          // 加密状态判断不了：整包打开不带密码，Honeyview 需要时会自己问
          ok = custom::LaunchHoneyviewWholeArchive(hv, archivePath);
          detail = std::wstring(L"honeyview whole: ") + archivePath + L" (unknown state)";
        }
        else if (is7z)
        {
          const int ver = custom::SevenZArchiveVersion(archivePath);
          if (ver >= 0 && ver <= 3)
          {
            ok = custom::LaunchHoneyviewArchive(hv, archivePath, inner);
          }
          else
          {
            ok = custom::LaunchHoneyviewWholeThenPipe(hv, archivePath, inner);
            detail += L" (whole-then-pipe)";
          }
        }
        else
        {
          // zip/tar/rar 等：竖线。加密（或判断不了）时先验出可用的密码再启动，
          // 拿未验证的库密码自动填框只会让 Honeyview 填进错误密码。
          std::wstring autofill;
          if (!known || encrypted)
            autofill = ResolveVerifiedPassword(dll, archivePath, inner, pwd,
                                               known && encrypted, true);
          if (custom::LaunchHoneyviewArchive(hv, archivePath, inner, autofill))
          {
            if (!autofill.empty())
              detail += L" (autofill)";
            else if (known && encrypted)
              detail += L" (prompt)";
            ok = true;
          }
        }

        if (ok)
        {
          DebugLog(L"TryOpenImage.native", detail.c_str());
          return 1;
        }
      }
      DebugLog(L"TryOpenImage.native", L"(launch failed, fallback)");
    }
  }

  custom::PreviewResult r = custom::PrepareImagePreview(
      archivePath, itemRelPath, pwd, launch);

  // 只加密数据、不加密文件名的归档，打开时不会问密码；到预览才发现要密码。
  // 先静默把密码库/缓存里能验证过的密码试一遍（验证规则和 Honeyview 原生
  // 路径一致），不行再弹一次我们自己的密码框，拿到密码后重试。
  if (!r.ok && r.needsPassword && launch)
  {
    const std::wstring dll = ModuleDir() + L"7z.dll";
    if (pwd.empty())
    {
      const std::wstring v = ResolveVerifiedPassword(
          dll, archivePath, custom::NormalizeInnerPath(archivePath, itemRelPath),
          std::wstring(), true, false);
      if (!v.empty())
      {
        pwd = v;
        DebugLog(L"TryOpenImage.pwd", L"(resolved from vault/cache)");
        r = custom::PrepareImagePreview(archivePath, itemRelPath, pwd, launch);
      }
    }
    if (!r.ok && r.needsPassword)
    {
      wchar_t asked[1024] = {};
      const int a = CustomHook_AskPassword(::GetActiveWindow(), archivePath,
                                           asked, (int)(sizeof(asked) / sizeof(asked[0])));
      if (a == 1 && asked[0])
      {
        pwd = asked;
        RememberPassword(archivePath, pwd.c_str());
        DebugLog(L"TryOpenImage.pwd", L"(asked again)");
        ::SecureZeroMemory(asked, sizeof(asked));
        r = custom::PrepareImagePreview(archivePath, itemRelPath, pwd, launch);
      }
      else
      {
        ::SecureZeroMemory(asked, sizeof(asked));
        if (a < 0)
        {
          DebugLog(L"TryOpenImage.needPwd", L"(cancelled)");
          return 1;   // 用户取消：不要再让 7-Zip 弹它自己的密码框
        }
      }
    }
  }

  if (!r.ok && r.needsPassword)
    r.error = pwd.empty() ? L"归档已加密，需要密码" : L"归档已加密，密码不正确";

  if (r.restrictedEnv)
  {
    std::wstring detail = L"预览目录只能用程序目录（完整性 ";
    detail += IntegrityLevelInfo();
    detail += L"）";
    DebugLog(L"TryOpenImage.env", detail.c_str());
  }

  DebugLog(L"TryOpenImage", r.ok ? r.openedPath.c_str() : r.error.c_str());
  if (!r.ok && !r.command.empty())
    DebugLog(L"TryOpenImage.cmd", r.command.c_str());
  if (r.ok && !r.launcher.empty())
  {
    // 启动链自己报告走了哪一级、返回码多少：ShellExecute 返回 >32 不代表
    // 窗口真的起来了，排查"看着成功其实没打开"时全靠这一行。
    wchar_t code[32];
    swprintf_s(code, L"%lld", r.launchCode);
    DebugLog(L"TryOpenImage.launch", (r.launcher + L" -> " + code).c_str());
  }

  if (r.ok)
    return 1;

  // 任何失败都把原因摆到明面上：退回 7-Zip 只会再弹一个"拒绝访问"，
  // 用户看不懂，我们也拿不到信息。
  if (!launch)   // 自测模式：只记日志，不弹框
    return 0;

  {
    std::wstring msg = L"预览这张图片失败：\n\n";
    msg += r.error.empty() ? L"(未知原因)" : r.error;
    if (!r.inprocError.empty())
    {
      msg += L"\n\n（进程内解压失败原因：";
      msg += r.inprocError;
      msg += L"）";
    }
    if (!r.command.empty())
    {
      msg += L"\n\n执行的命令：\n";
      msg += r.command;
    }
    const std::wstring il = IntegrityLevelInfo();
    if (il == L"Low" || il == L"Untrusted")
      msg += L"\n\n当前程序的完整性级别是 " + il +
             L"，在这种受限环境里无法启动外部程序。\n"
             L"请从资源管理器里重新双击 7zFM.exe（不要从聊天窗口或 IDE 的文件预览启动）。";
    else if (r.restrictedEnv)
      msg += L"\n\n注意：当前进程写不了 %TEMP%（预览目录退到了程序目录），"
             L"很可能是受限上下文启动的。\n"
             L"请从资源管理器重新启动 7zFM.exe 后再试。";
    msg += L"\n\n（把这段截图发给开发者即可定位）";
    HWND owner = ::GetActiveWindow();
    ::MessageBoxW(owner ? owner : nullptr, msg.c_str(), L"7-Zip 看图", MB_ICONWARNING);
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
  kCmdVaultDiag    = 0x7A04,
  kCmdShellRegister   = 0x7A05,
  kCmdShellUnregister = 0x7A06,
  kCmdOpenAppDir      = 0x7A07
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

bool MenuHasExtras(HMENU main)
{
  const int count = ::GetMenuItemCount(main);
  for (int i = 0; i < count; ++i)
  {
    HMENU sub = ::GetSubMenu(main, i);
    if (sub && ::GetMenuState(sub, kCmdShellRegister, MF_BYCOMMAND) != (UINT)-1)
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

  msg += L"\n父进程  : ";
  {
    const std::wstring parent = ParentProcessInfo();
    msg += parent.empty() ? L"(未知，父进程可能已退出)" : parent;
  }

  msg += L"\n完整性  : ";
  msg += IntegrityLevelInfo();
  msg += L"\nAppContainer: ";
  msg += IsAppContainerProcess() ? L"是" : L"否";
  msg += L"\n作业对象: ";
  msg += JobInfo();

  const std::wstring sevenZip = ModuleDir() + L"7z.exe";
  const DWORD a = ::GetFileAttributesW(sevenZip.c_str());
  msg += L"\n7z.exe  : ";
  msg += (a != INVALID_FILE_ATTRIBUTES) ? L"存在" : L"缺失";

  msg += L"\n看图引擎: ";
  {
    const std::wstring hv = custom::FindHoneyview();
    msg += hv.empty() ? L"解压到临时目录（未找到 Honeyview）"
                      : (L"Honeyview 原生：" + hv);
  }

  msg += L"\n右键菜单: ";
  {
    std::wstring reg;
    msg += custom::IsShellContextMenuRegistered(reg) ? L"已注册（当前用户）" : L"未注册";
  }

  wchar_t tmp[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, tmp);
  msg += L"\n临时目录: ";
  msg += tmp;
  msg += L"\nTEMP 写入: ";
  msg += WriteProbe(tmp);

  msg += L"\n密码库  : ";
  msg += custom::VaultDir();

  wchar_t localPath[MAX_PATH] = {};
  if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localPath) == S_OK)
  {
    msg += L"\nLOCALAPPDATA 写入: ";
    msg += WriteProbe(localPath);
  }

  // 能否启动外部程序（看图要靠它）——受限环境会在这里失败
  msg += L"\n启动外部程序: ";
  {
    wchar_t shell[MAX_PATH] = {};
    ::GetEnvironmentVariableW(L"COMSPEC", shell, MAX_PATH);
    std::wstring testCmd = shell;
    testCmd += L" /c exit";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!testCmd.empty() &&
        ::CreateProcessW(nullptr, &testCmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &si, &pi))
    {
      ::WaitForSingleObject(pi.hProcess, 3000);
      ::CloseHandle(pi.hThread);
      ::CloseHandle(pi.hProcess);
      msg += L"可用";
    }
    else
    {
      wchar_t eb[64];
      swprintf_s(eb, L"被拒绝（错误码 %lu）", (unsigned long)::GetLastError());
      msg += eb;
    }
  }

  msg += L"\n日志    : ";
  {
    const std::wstring dataDir = ModuleDir() + L"data";
    const DWORD da = ::GetFileAttributesW(dataDir.c_str());
    if (da != INVALID_FILE_ATTRIBUTES && (da & FILE_ATTRIBUTE_DIRECTORY))
      msg += dataDir + L"\\hook.log（便携）";
    else
      msg += ModuleDir() + L"hook.log（或 %LOCALAPPDATA%\\7zip-custom\\hook.log）";
  }

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
    case kCmdShellRegister:
    {
      std::wstring err;
      if (custom::RegisterShellContextMenu(err))
        ::MessageBoxW(hwnd,
            L"已为当前用户注册右键菜单。\n\n"
            L"在资源管理器中右键文件或文件夹即可看到 7-Zip。\n"
            L"不需要管理员权限；取消勾选/取消注册即可移除。",
            L"右键菜单", MB_ICONINFORMATION);
      else
        ::MessageBoxW(hwnd, err.c_str(), L"右键菜单", MB_ICONERROR);
      break;
    }
    case kCmdShellUnregister:
    {
      std::wstring err;
      if (custom::UnregisterShellContextMenu(err))
        ::MessageBoxW(hwnd, L"已取消当前用户的右键菜单注册。", L"右键菜单",
                      MB_ICONINFORMATION);
      else
        ::MessageBoxW(hwnd, err.c_str(), L"右键菜单", MB_ICONERROR);
      break;
    }
    case kCmdOpenAppDir:
      ::ShellExecuteW(hwnd, L"open", ModuleDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
  }
}

LRESULT CALLBACK MainWndSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR id, DWORD_PTR)
{
  if (msg == WM_COMMAND)
  {
    const unsigned cmd = LOWORD(wp);
    if (cmd >= kCmdVaultManager && cmd <= kCmdOpenAppDir)
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
  bool changed = false;
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
    changed = true;
  }

  if (!MenuHasExtras(main))
  {
    HMENU sub = ::CreatePopupMenu();
    if (!sub)
      return 0;

    ::AppendMenuW(sub, MF_STRING, kCmdShellRegister,
                  L"注册右键菜单（当前用户）(&R)");
    ::AppendMenuW(sub, MF_STRING, kCmdShellUnregister, L"取消右键菜单(&U)");
    ::AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(sub, MF_STRING, kCmdOpenAppDir, L"打开程序目录(&O)");

    const int count = ::GetMenuItemCount(main);
    const int pos = (count > 0) ? count - 1 : 0;
    if (!::InsertMenuW(main, (UINT)pos, MF_BYPOSITION | MF_POPUP, (UINT_PTR)sub, L"附加功能(&X)"))
    {
      ::DestroyMenu(sub);
      return 0;
    }
    changed = true;
  }

  if (changed)
    ::DrawMenuBar(hwnd);

  ::SetWindowSubclass(hwnd, MainWndSubclass, kSubclassId, 0);
  DebugLog(L"AttachMainMenu", L"ok");
  return 1;
}

extern "C" void CUSTOM_HOOK_API CustomHook_OnArchiveOpened(const wchar_t *archivePath)
{
  (void)archivePath;
}

extern "C" void CUSTOM_HOOK_API CustomHook_OnArchiveClosed(const wchar_t *archivePath)
{
  if (!archivePath || !*archivePath)
    return;
  DebugLog(L"ArchiveClosed", archivePath);
  custom::CleanupArchivePreview(archivePath);
}
