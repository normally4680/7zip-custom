// Custom/Core/ImagePreview.cpp

#include "ImagePreview.h"
#include "SevenZipInProc.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <cstdio>

#include <cstdint>
#include <vector>

namespace custom {

namespace {

// 按"看图软件普遍支持"挑选，顺序不重要
const wchar_t * const kImageExts[] = {
  L"jpg", L"jpeg", L"jpe", L"png", L"gif", L"bmp", L"webp",
  L"tif", L"tiff", L"ico", L"jxl", L"avif", L"heic", L"heif", L"psd"
};

const int kMaxFilesPerDir = 2000;

std::wstring ModuleDir()
{
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = ::GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
    return std::wstring();
  std::wstring p(buf, n);
  const size_t pos = p.find_last_of(L"\\/");
  return (pos == std::wstring::npos) ? std::wstring() : p.substr(0, pos + 1);
}

std::wstring Join3(const std::wstring &a, const wchar_t *b, const wchar_t *c)
{
  std::wstring r = a;
  if (!r.empty() && r.back() != L'\\')
    r += L'\\';
  r += b;
  r += c;
  return r;
}

// 优先 %TEMP%，不行再退到 %LOCALAPPDATA%（少数环境 TEMP 被策略禁写）
bool EnsureTempRoot(std::wstring &root, std::wstring &error)
{
  std::vector<std::wstring> bases;
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetTempPathW(MAX_PATH, buf);
  if (n > 0 && n < MAX_PATH)
    bases.push_back(std::wstring(buf, n));

  wchar_t local[MAX_PATH];
  if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK)
    bases.push_back(std::wstring(local) + L"\\");

  // 再加一个"程序目录"，某些受限环境只允许写这里
  bases.push_back(ModuleDir());

  DWORD lastError = 0;
  for (const auto &b : bases)
  {
    const std::wstring base = Join3(b, L"7zip-custom", L"");
    std::wstring candidate = base;
    if (!candidate.empty() && candidate.back() != L'\\')
      candidate += L'\\';
    candidate += L"preview";
    ::CreateDirectoryW(base.c_str(), nullptr);
    ::CreateDirectoryW(candidate.c_str(), nullptr);
    const DWORD attr = ::GetFileAttributesW(candidate.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
    {
      // 还要真能往下写：某些沙箱里"目录存在"但子目录建不了
      const std::wstring probe = candidate + L"\\.writetest";
      if (::CreateDirectoryW(probe.c_str(), nullptr))
      {
        ::RemoveDirectoryW(probe.c_str());
        root = candidate + L"\\";
        return true;
      }
      lastError = ::GetLastError();
      continue;
    }
    lastError = (attr == INVALID_FILE_ATTRIBUTES) ? ::GetLastError() : ERROR_DIRECTORY;
  }

  wchar_t msg[512];
  swprintf_s(msg,
      L"没有可写的预览目录（最后一次错误码 %lu）。\n"
      L"如果你是从聊天窗口的文件卡片启动本程序的，请改成在资源管理器里双击 run.bat，"
      L"受限环境不允许写临时目录。", (unsigned long)lastError);
  error = msg;
  return false;
}

uint32_t Fnv1a(const std::wstring &s)
{
  uint32_t h = 2166136261u;
  for (wchar_t c : s)
  {
    h ^= (uint32_t)c;
    h *= 16777619u;
  }
  return h;
}

std::wstring Hex(uint32_t v)
{
  wchar_t buf[16];
  swprintf_s(buf, L"%08x", v);
  return buf;
}

std::wstring Join(const std::wstring &a, const std::wstring &b)
{
  if (a.empty())
    return b;
  std::wstring r = a;
  if (r.back() != L'\\' && r.back() != L'/')
    r += L'\\';
  r += b;
  return r;
}

bool FileExists(const std::wstring &path)
{
  const DWORD a = ::GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void SplitInnerPath(const std::wstring &rel, std::wstring &dir, std::wstring &name)
{
  std::wstring norm = rel;
  for (auto &c : norm)
    if (c == L'/')
      c = L'\\';
  const size_t pos = norm.find_last_of(L'\\');
  if (pos == std::wstring::npos)
  {
    dir.clear();
    name = norm;
  }
  else
  {
    dir = norm.substr(0, pos);
    name = norm.substr(pos + 1);
  }
}

int CountFiles(const std::wstring &dir)
{
  int n = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(Join(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return 0;
  do
  {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      ++n;
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  return n;
}

// 把 7z.exe / 7z.dll 复制到 %TEMP%\7zip-custom\bin 再运行。
// 直接从程序目录跑，某些环境（安全软件、目录 ACL、沙箱）会拒绝加载 7z.dll，
// 表现就是 "Codec Load Error ... 7z.dll : Error #80070005"，退出码 2。
std::wstring EnsureHelperExe()
{
  const std::wstring srcDir = ModuleDir();
  const std::wstring srcExe = srcDir + L"7z.exe";
  const std::wstring srcDll = srcDir + L"7z.dll";
  if (!FileExists(srcExe) || !FileExists(srcDll))
    return srcExe;   // 没有副本可用，只能试原位置

  wchar_t tmp[MAX_PATH] = {};
  if (::GetTempPathW(MAX_PATH, tmp) == 0)
    return srcExe;

  const std::wstring dir = std::wstring(tmp) + L"7zip-custom\\bin";
  ::CreateDirectoryW((std::wstring(tmp) + L"7zip-custom").c_str(), nullptr);
  ::CreateDirectoryW(dir.c_str(), nullptr);

  const std::wstring dstExe = dir + L"\\7z.exe";
  const std::wstring dstDll = dir + L"\\7z.dll";

  // 大小不一致就重新复制（构建后二进制会变）
  const auto sameSize = [](const std::wstring &a, const std::wstring &b)
  {
    WIN32_FILE_ATTRIBUTE_DATA fa = {}, fb = {};
    if (!::GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa) ||
        !::GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb))
      return false;
    return fa.nFileSizeHigh == fb.nFileSizeHigh && fa.nFileSizeLow == fb.nFileSizeLow;
  };

  if (!sameSize(srcExe, dstExe))
    if (!::CopyFileW(srcExe.c_str(), dstExe.c_str(), FALSE))
      return srcExe;
  if (!sameSize(srcDll, dstDll))
    if (!::CopyFileW(srcDll.c_str(), dstDll.c_str(), FALSE))
      return srcExe;

  return FileExists(dstExe) && FileExists(dstDll) ? dstExe : srcExe;
}

// 本机可能根本没注册图片关联（AssocQueryString 返回 ERROR_NO_ASSOCIATION），
// ShellExecute 就会一直失败。这里自己找看图软件：先看配置，再看常见安装位置。
std::wstring ReadConfigValue(const wchar_t *key)
{
  wchar_t local[MAX_PATH] = {};
  if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) != S_OK)
    return std::wstring();
  std::wstring ini = std::wstring(local) + L"\\7zip-custom\\config.ini";

  wchar_t buf[MAX_PATH * 2] = {};
  ::GetPrivateProfileStringW(L"viewer", key, L"", buf,
                             (DWORD)(sizeof(buf) / sizeof(buf[0])), ini.c_str());
  return buf;
}

bool FileExistsSimple(const std::wstring &p)
{
  const DWORD a = ::GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FindImageViewer()
{
  // 1) 配置文件里指定的
  std::wstring configured = ReadConfigValue(L"path");
  if (!configured.empty() && FileExistsSimple(configured))
    return configured;

  // 2) 系统登记的 App Paths
  static const wchar_t * const kNames[] = {
    L"Honeyview.exe", L"IrfanView.exe", L"i_view64.exe", L"i_view32.exe",
    L"JPEGView.exe", L"nomacs.exe", L"Imagine.exe"
  };
  for (const wchar_t *name : kNames)
  {
    for (const wchar_t *root : { L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\",
                                 L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" })
    {
      std::wstring sub = std::wstring(root) + name;
      wchar_t val[MAX_PATH * 2] = {};
      DWORD sz = (DWORD)sizeof(val);
      if (::RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), nullptr,
                         RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS)
      {
        std::wstring p = val;
        if (FileExistsSimple(p))
          return p;
      }
    }
  }

  // 3) 常见安装目录
  static const wchar_t * const kCandidates[] = {
    L"C://Program Files//Honeyview//Honeyview.exe",
    L"C://Program Files (x86)//Honeyview//Honeyview.exe",
    L"C://Program Files//Bandizip//Honeyview.exe",
    L"C://Program Files//IrfanView//i_view64.exe",
    L"C://Program Files (x86)//IrfanView//i_view32.exe",
    L"C://Program Files//JPEGView//JPEGView.exe",
    L"D://Program Files//Honeyview//Honeyview.exe",
    L"D://Honeyview//Honeyview.exe",
    L"D://Program Files (x86)//Honeyview//Honeyview.exe"
  };
  for (const wchar_t *c : kCandidates)
    if (FileExistsSimple(c))
      return c;

  return std::wstring();
}

// 用系统自带的"Windows 照片查看器"打开（不需要文件关联）。
// 这台机器上 .bmp/.jpg/.png 都没关联程序，但 PhotoViewer.dll 是存在的。
bool TryWindowsPhotoViewer(const std::wstring &file)
{
  wchar_t pf[MAX_PATH] = {};
  if (::GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH) == 0)
    return false;

  const std::wstring dll = std::wstring(pf) + L"\\Windows Photo Viewer\\PhotoViewer.dll";
  if (!FileExistsSimple(dll))
    return false;

  wchar_t sys[MAX_PATH] = {};
  ::GetSystemDirectoryW(sys, MAX_PATH);
  std::wstring exe = std::wstring(sys) + L"\\" + L"rundll" + L"32.exe";

  std::wstring cmd = L"\"" + exe + L"\" \"" + dll + L"\", ImageView_Fullscreen \"" + file + L"\"";
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    return false;
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

bool LaunchWithExe(const std::wstring &exe, const std::wstring &file, const std::wstring &dir)
{
  std::wstring cmd = L"\"" + exe + L"\" \"" + file + L"\"";
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi))
    return false;
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

// 用系统关联里登记的"打开命令"直接 CreateProcess。
// ShellExecute 在关联到 UWP（照片）或某些上下文里会返回 5（拒绝访问），
// 但进程创建本身是允许的，所以直接按命令行起更稳。
bool LaunchByAssociation(const std::wstring &file)
{
  const size_t dot = file.find_last_of(L'.');
  if (dot == std::wstring::npos)
    return false;
  const std::wstring ext = file.substr(dot);

  wchar_t cmdLine[2048] = {};
  DWORD sz = (DWORD)(sizeof(cmdLine) / sizeof(cmdLine[0]));
  if (::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_COMMAND, ext.c_str(), L"open", cmdLine, &sz) != S_OK)
    return false;

  // 把 %1/%L/%l/%* 替换成带引号的文件名，其它 %n 去掉
  std::wstring cmd;
  const std::wstring quoted = L"\"" + file + L"\"";
  for (size_t i = 0; i < sz && cmdLine[i]; ++i)
  {
    if (cmdLine[i] == L'%' && i + 1 < sz)
    {
      const wchar_t c = cmdLine[i + 1];
      if (c == L'1' || c == L'L' || c == L'l' || c == L'*')
      {
        cmd += quoted;
        ++i;
        continue;
      }
      if (c == L'%') { cmd += L'%'; ++i; continue; }
      if (c >= L'0' && c <= L'9') { ++i; continue; }
    }
    cmd += cmdLine[i];
  }
  if (cmd.empty())
    return false;

  const size_t slash = file.find_last_of(L'\\');
  const std::wstring workDir = (slash == std::wstring::npos) ? std::wstring()
                                                             : file.substr(0, slash);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  std::wstring mutableCmd = cmd;
  if (!::CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE, 0, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
    return false;
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

// 先 ShellExecute（走默认语义），失败再退回"按关联命令直接起"
INT_PTR LaunchViewer(const std::wstring &file, const std::wstring &dir)
{
  // 1) 自己找到的看图软件（不依赖文件关联）
  {
    const std::wstring viewer = FindImageViewer();
    if (!viewer.empty() && LaunchWithExe(viewer, file, dir))
      return 42;
  }

  // 2) 系统关联
  const INT_PTR se = (INT_PTR)::ShellExecuteW(nullptr, L"open", file.c_str(),
                                              nullptr, dir.c_str(), SW_SHOWNORMAL);
  if (se > 32)
    return se;

  // 3) 关联里登记的命令行
  if (LaunchByAssociation(file))
    return 42;

  // 4) 系统自带照片查看器（不依赖关联）
  if (TryWindowsPhotoViewer(file))
    return 42;

  return se;
}

bool RunAndWait(const std::wstring &cmd, DWORD &exitCode, std::wstring &output)
{
  // 把子进程的 stdout/stderr 重定向到文件，失败时能把 7-Zip 自己的报错带出来
  std::wstring outPath;
  {
    wchar_t tmp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, tmp) > 0)
      outPath = std::wstring(tmp) + L"7zip-custom-7z.log";
  }

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE hOut = ::CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  // stdin 必须给一个"有效但读不到东西"的句柄，否则 7z 在需要密码时会卡死
  HANDLE hNul = ::CreateFileW(L"NUL", GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, nullptr);

  if (hOut != INVALID_HANDLE_VALUE)
  {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : hOut;
  }

  PROCESS_INFORMATION pi = {};
  std::wstring mutableCmd = cmd;
  const BOOL ok = ::CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr,
                                   hOut != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi);
  if (hOut != INVALID_HANDLE_VALUE)
    ::CloseHandle(hOut);
  if (hNul != INVALID_HANDLE_VALUE)
    ::CloseHandle(hNul);
  if (!ok)
    return false;

  if (::WaitForSingleObject(pi.hProcess, 30000) == WAIT_TIMEOUT)
    ::TerminateProcess(pi.hProcess, 1);
  ::GetExitCodeProcess(pi.hProcess, &exitCode);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);

  if (!outPath.empty())
  {
    HANDLE h = ::CreateFileW(outPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
      char buf[8192];
      DWORD got = 0;
      if (::ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr) && got > 0)
      {
        buf[got] = 0;
        const int w = ::MultiByteToWideChar(CP_ACP, 0, buf, (int)got, nullptr, 0);
        if (w > 0)
        {
          output.resize((size_t)w);
          ::MultiByteToWideChar(CP_ACP, 0, buf, (int)got, &output[0], w);
        }
      }
      ::CloseHandle(h);
    }
    ::DeleteFileW(outPath.c_str());
  }
  return true;
}

} // namespace

bool IsImageFile(const wchar_t *name)
{
  if (!name)
    return false;
  const wchar_t *dot = wcsrchr(name, L'.');
  if (!dot || !dot[1])
    return false;
  for (const wchar_t *ext : kImageExts)
    if (_wcsicmp(dot + 1, ext) == 0)
      return true;
  return false;
}

void CleanupPreviewDirs(int maxAgeHours)
{
  std::wstring root, err;
  if (!EnsureTempRoot(root, err))
    return;

  FILETIME now;
  ::GetSystemTimeAsFileTime(&now);

  std::wstring pattern = root + L"*";
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return;

  do
  {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
      continue;
    if (fd.cFileName[0] == L'.')
      continue;

    ULARGE_INTEGER last, cur;
    last.LowPart = fd.ftLastWriteTime.dwLowDateTime;
    last.HighPart = fd.ftLastWriteTime.dwHighDateTime;
    cur.LowPart = now.dwLowDateTime;
    cur.HighPart = now.dwHighDateTime;

    const unsigned long long age100ns = cur.QuadPart - last.QuadPart;
    const unsigned long long maxAge = (unsigned long long)maxAgeHours * 3600ull * 10000000ull;
    if (age100ns > maxAge)
    {
      // 目录里都是解出来的临时文件，直接删
      std::wstring dir = Join(root, fd.cFileName);
      std::wstring files = dir + L"\\*";
      WIN32_FIND_DATAW f2;
      HANDLE h2 = ::FindFirstFileW(files.c_str(), &f2);
      if (h2 != INVALID_HANDLE_VALUE)
      {
        do
        {
          if (!(f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
          {
            ::SetFileAttributesW(Join(dir, f2.cFileName).c_str(), FILE_ATTRIBUTE_NORMAL);
            ::DeleteFileW(Join(dir, f2.cFileName).c_str());
          }
        } while (::FindNextFileW(h2, &f2));
        ::FindClose(h2);
      }
      ::RemoveDirectoryW(dir.c_str());
    }
  } while (::FindNextFileW(h, &fd));

  ::FindClose(h);
}

PreviewResult PrepareImagePreview(const std::wstring &archivePath,
                                  const std::wstring &innerRelPath,
                                  const std::wstring &password,
                                  bool launch)
{
  PreviewResult r;

  // 传进来的可能是"归档内完整虚拟路径"（D://x.7z//风景//1.png），也可能是
  // 仅仅"相对于当前面板目录"的文件名（1.png，7-Zip 的 GetItemRelPath 就是这样）。
  // 统一成"归档内相对路径"，否则算出来的目标文件路径会和 7z.exe 实际解出的位置不一致。
  std::wstring inner = innerRelPath;
  if (inner.size() > archivePath.size() &&
      _wcsnicmp(inner.c_str(), archivePath.c_str(), archivePath.size()) == 0)
    inner = inner.substr(archivePath.size());
  while (!inner.empty() && (inner[0] == L'\\' || inner[0] == L'/'))
    inner.erase(inner.begin());

  std::wstring innerDir, fileName;
  SplitInnerPath(inner, innerDir, fileName);
  if (fileName.empty())
  {
    r.error = L"条目名为空";
    return r;
  }
  if (!IsImageFile(fileName.c_str()))
  {
    r.error = L"不是图片";
    return r;
  }

  std::wstring root;
  if (!EnsureTempRoot(root, r.error))
  {
    r.envError = true;
    return r;
  }

  // 同一个归档的同一个目录复用同一个临时目录，第二次看就是瞬间打开
  const std::wstring key = archivePath + L"|" + innerDir;
  std::wstring dir = root + Hex(Fnv1a(key));
  r.tempDir = dir;

  std::wstring target = Join(Join(dir, innerDir), fileName);

  if (!FileExists(target))
  {
    ::CreateDirectoryW(root.c_str(), nullptr);
    ::CreateDirectoryW(dir.c_str(), nullptr);

    CleanupPreviewDirs(72);

    // 首选：进程内用 7z.dll 解压（不启动子进程，绕开一堆环境问题）
    {
      std::vector<std::wstring> extList;
      for (const wchar_t *e : kImageExts)
        extList.push_back(e);

      int count = 0;
      std::wstring ipError;
      const std::wstring dll = ModuleDir() + L"7z.dll";
      if (ExtractImagesInProcess(dll.c_str(), archivePath, innerDir, password, dir,
                                 extList, count, ipError) && FileExists(target))
      {
        r.openedPath = target;
        r.extracted = count;
        r.ok = true;
        if (launch)
        {
          const INT_PTR se = LaunchViewer(target, dir);
          if (se <= 32)
          {
            r.ok = false;
            wchar_t msg[768];
            if (se == 5)
            {
              swprintf_s(msg,
                  L"无法启动看图软件（错误 5：拒绝访问）。\n\n"
                  L"这不是关联问题，而是当前进程被禁止启动外部程序。\n"
                  L"这台机器上图片扩展名可能没有关联程序。\n"
                  L"请在 %%LOCALAPPDATA%%\\7zip-custom\\config.ini 里写：\n"
                  L"  [viewer]\n  path=C:////Program Files\\Honeyview\\Honeyview.exe");
            }
            else
            {
              swprintf_s(msg,
                  L"无法启动看图软件（ShellExecute 返回 %lld）。\n"
                  L"31 = 没有关联程序；建议把 .bmp/.png 关联到 Honeyview。",
                  (long long)se);
            }
            r.error = msg;
          }
        }
        return r;
      }
      r.inprocError = ipError;   // 保留，万一子进程也失败就用它
    }

    std::wstring cmd = L"\"" + ModuleDir() + L"7z.exe\" x \"" + archivePath + L"\" -o\"" + dir +
                       L"\" -y -bd -bso0 -bsp0";
    // 有密码就带上，没有就传空密码，避免子进程卡在交互提示上
    cmd += password.empty() ? L" -p" : (L" -p\"" + password + L"\"");

    for (const wchar_t *ext : kImageExts)
    {
      std::wstring pattern = innerDir.empty()
          ? (std::wstring(L"*.") + ext)
          : (std::wstring(L"\"") + innerDir + L"\\*." + ext + L"\"");
      cmd += L" " + pattern;
    }

    const std::wstring sevenZip = EnsureHelperExe();
    if (!FileExists(sevenZip))
    {
      r.error = L"找不到 7z.exe（它必须和 7zFM.exe 放在同一目录）";
      r.envError = true;
      return r;
    }
    std::wstring cmd2 = cmd;
    // 把命令里的 exe 路径换成实际使用的那个
    {
      const std::wstring oldExe = L"\"" + ModuleDir() + L"7z.exe\"";
      const size_t p = cmd2.find(oldExe);
      if (p != std::wstring::npos)
        cmd2.replace(p, oldExe.size(), L"\"" + sevenZip + L"\"");
    }

    r.command = cmd2;

    DWORD code = 0;
    std::wstring sevenZipOutput;
    if (!RunAndWait(cmd2, code, sevenZipOutput))
    {
      wchar_t msg[256];
      swprintf_s(msg, L"启动 7z.exe 失败（错误码 %lu）", (unsigned long)::GetLastError());
      r.error = msg;
      r.envError = true;
      return r;
    }
    if (code != 0 || !FileExists(target))
    {
      wchar_t msg[512];
      if (code == 0)
        swprintf_s(msg, L"目标文件没有解出来");
      else
        swprintf_s(msg, L"7z.exe 返回 %lu", (unsigned long)code);
      r.error = msg;
      if (!sevenZipOutput.empty())
      {
        // 只保留最后几行，够看就行
        std::wstring tail = sevenZipOutput;
        if (tail.size() > 400)
          tail = tail.substr(tail.size() - 400);
        r.error += L"\n\n7-Zip 的输出：\n";
        r.error += tail;
      }
      return r;
    }
  }

  r.openedPath = target;
  r.extracted = CountFiles(Join(dir, innerDir));
  r.ok = true;

  if (launch)
  {
    const INT_PTR se = LaunchViewer(target, dir);
    if (se <= 32)
    {
      r.ok = false;
            wchar_t msg[768];
            if (se == 5)
            {
              swprintf_s(msg,
                  L"无法启动看图软件（错误 5：拒绝访问）。\n\n"
                  L"这不是关联问题，而是当前进程被禁止启动外部程序。\n"
                  L"这台机器上图片扩展名可能没有关联程序。\n"
                  L"请在 %%LOCALAPPDATA%%\\7zip-custom\\config.ini 里写：\n"
                  L"  [viewer]\n  path=C:////Program Files\\Honeyview\\Honeyview.exe");
            }
            else
            {
              swprintf_s(msg,
                  L"无法启动看图软件（ShellExecute 返回 %lld）。\n"
                  L"31 = 没有关联程序；建议把 .bmp/.png 关联到 Honeyview。",
                  (long long)se);
            }
            r.error = msg;
      return r;
    }
  }

  return r;
}

} // namespace custom
