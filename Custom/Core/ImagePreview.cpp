// Custom/Core/ImagePreview.cpp

#include "ImagePreview.h"
#include "SevenZipInProc.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <cstdio>

#include <cstdint>
#include <cwctype>
#include <vector>

namespace custom {

namespace {

// 按"看图软件普遍支持"挑选，顺序不重要
const wchar_t * const kImageExts[] = {
  L"jpg", L"jpeg", L"jpe", L"png", L"gif", L"bmp", L"webp",
  L"tif", L"tiff", L"ico", L"jxl", L"avif", L"heic", L"heif", L"psd"
};

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
bool EnsureTempRoot(std::wstring &root, std::wstring &error, bool &restricted)
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
  const std::wstring moduleDir = ModuleDir();
  if (!moduleDir.empty())
    bases.push_back(moduleDir);

  DWORD lastError = 0;
  for (size_t i = 0; i < bases.size(); ++i)
  {
    if (bases[i].empty())
      continue;
    const std::wstring base = Join3(bases[i], L"7zip-custom", L"");
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
      bool writable = ::CreateDirectoryW(probe.c_str(), nullptr) != FALSE;
      // 上次崩溃留下的探测目录，说明这里本来就能写
      if (!writable && ::GetLastError() == ERROR_ALREADY_EXISTS)
        writable = true;
      if (writable)
      {
        ::RemoveDirectoryW(probe.c_str());
        root = candidate + L"\\";
        // 连 %TEMP% 和 %LOCALAPPDATA% 都写不了、只能退到程序目录时，
        // 基本可以断定进程被受限上下文启动（此时启动外部程序通常也会静默失败）。
        restricted = (i + 1 == bases.size());
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
      L"当前运行环境不允许写临时目录，请把程序解压到普通文件夹后，"
      L"从资源管理器里双击 7zFM.exe 再试。", (unsigned long)lastError);
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

// 预览目录按归档路径命名。打开和关闭归档时拿到的路径可能在大小写/斜杠/
// 尾部分隔符上略有差别，先规范化再哈希，否则关闭时会删不到。
std::wstring ArchiveTempKey(const std::wstring &archivePath)
{
  // 统一转成绝对路径（7zFM 给的是绝对路径；测试/命令行可能给相对路径），
  // 再统一大小写和分隔符，保证"打开"和"关闭"算出来的键一致。
  std::wstring norm;
  wchar_t full[MAX_PATH * 2] = {};
  const DWORD n = ::GetFullPathNameW(archivePath.c_str(),
      (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
  if (n > 0 && n < sizeof(full) / sizeof(full[0]))
    norm.assign(full, n);
  if (norm.empty())
    norm = archivePath;
  while (norm.size() > 1 && (norm.back() == L'\\' || norm.back() == L'/'))
    norm.pop_back();
  for (auto &c : norm)
    c = (wchar_t)::towlower(c);
  return Hex(Fnv1a(norm));
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

unsigned long long FileSize(const std::wstring &path)
{
  WIN32_FILE_ATTRIBUTE_DATA fad = {};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
    return 0;
  if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    return 0;
  return ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

// 提取失败/密码错误时，7z 可能留下 0 字节或写了一半的文件。
// 只有非空文件才能当作"已经解压好"的缓存。
bool IsUsableFile(const std::wstring &path)
{
  return FileSize(path) > 0;
}

// 缓存文件比归档还旧，说明归档被重新生成过，继续用会显示旧图。
bool CacheStillValid(const std::wstring &archive, const std::wstring &target)
{
  if (!IsUsableFile(target))
    return false;
  WIN32_FILE_ATTRIBUTE_DATA fa = {}, ft = {};
  if (!::GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &fa) ||
      !::GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &ft))
    return true;   // 判断不了就按旧的用，最多重解一次
  return ::CompareFileTime(&fa.ftLastWriteTime, &ft.ftLastWriteTime) <= 0;
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

  // 大小或修改时间不一致就重新复制（构建后二进制会变）
  const auto sameFile = [](const std::wstring &a, const std::wstring &b)
  {
    WIN32_FILE_ATTRIBUTE_DATA fa = {}, fb = {};
    if (!::GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa) ||
        !::GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb))
      return false;
    return fa.nFileSizeHigh == fb.nFileSizeHigh &&
           fa.nFileSizeLow == fb.nFileSizeLow &&
           fa.ftLastWriteTime.dwHighDateTime == fb.ftLastWriteTime.dwHighDateTime &&
           fa.ftLastWriteTime.dwLowDateTime == fb.ftLastWriteTime.dwLowDateTime;
  };

  if (!sameFile(srcExe, dstExe))
    if (!::CopyFileW(srcExe.c_str(), dstExe.c_str(), FALSE))
      return srcExe;
  if (!sameFile(srcDll, dstDll))
    if (!::CopyFileW(srcDll.c_str(), dstDll.c_str(), FALSE))
      return srcExe;

  return FileExists(dstExe) && FileExists(dstDll) ? dstExe : srcExe;
}

// 本机可能根本没注册图片关联（AssocQueryString 返回 ERROR_NO_ASSOCIATION），
// ShellExecute 就会一直失败。这里自己找看图软件：先看配置，再看常见安装位置。
std::wstring ReadConfigValue(const wchar_t *key)
{
  std::vector<std::wstring> inis;

  // 便携模式：优先读 exe 同目录的 config.ini
  const std::wstring portableIni = ModuleDir() + L"config.ini";
  const DWORD attr = ::GetFileAttributesW(portableIni.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
    inis.push_back(portableIni);

  wchar_t local[MAX_PATH] = {};
  if (::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) == S_OK)
    inis.push_back(std::wstring(local) + L"\\7zip-custom\\config.ini");

  for (const auto &ini : inis)
  {
    wchar_t buf[MAX_PATH * 2] = {};
    ::GetPrivateProfileStringW(L"viewer", key, L"", buf,
                               (DWORD)(sizeof(buf) / sizeof(buf[0])), ini.c_str());
    if (buf[0])
      return buf;
  }
  return std::wstring();
}

bool FileExistsSimple(const std::wstring &p)
{
  const DWORD a = ::GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ReadRegString(HKEY root, const wchar_t *subKey, const wchar_t *valueName)
{
  wchar_t val[MAX_PATH * 2] = {};
  DWORD sz = (DWORD)sizeof(val);
  if (::RegGetValueW(root, subKey, valueName, RRF_RT_REG_SZ, nullptr, val, &sz) == ERROR_SUCCESS)
    return val;
  return std::wstring();
}

std::wstring FileNameOf(const std::wstring &path)
{
  const size_t pos = path.find_last_of(L"\\/");
  return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
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

bool HasVisibleTopWindow(DWORD pid)
{
  struct Context { DWORD pid; bool found; } ctx = { pid, false };
  ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    Context *c = reinterpret_cast<Context *>(lp);
    DWORD p = 0;
    ::GetWindowThreadProcessId(h, &p);
    if (p == c->pid && ::IsWindowVisible(h))
    {
      c->found = true;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&ctx));
  return ctx.found;
}

// 用系统自带的"Windows 照片查看器"打开（不需要文件关联）。
// CreateProcess 成功不代表窗口真的出来了：受限上下文里子进程可能被静默丢弃，
// 所以这里要等到可见窗口才敢说成功。
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
  const size_t slash = file.find_last_of(L'\\');
  const std::wstring workDir = (slash == std::wstring::npos) ? std::wstring()
                                                             : file.substr(0, slash);
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
                        workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
    return false;
  ::CloseHandle(pi.hThread);

  bool shown = false;
  for (int i = 0; i < 30; ++i)   // 最多等 3 秒
  {
    if (::WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0)
      break;                     // 进程已退出 = 没开起来
    if (HasVisibleTopWindow(pi.dwProcessId))
    {
      shown = true;
      break;
    }
  }
  if (!shown && ::WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT)
    ::TerminateProcess(pi.hProcess, 0);   // 活着却不肯显示窗口，别留孤儿进程
  ::CloseHandle(pi.hProcess);
  return shown;
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

// 错误框/日志里不要把密码原样带出去
std::wstring MaskPassword(const std::wstring &cmd, const std::wstring &pwd)
{
  if (pwd.empty())
    return cmd;
  const std::wstring needle = L"-p\"" + pwd + L"\"";
  std::wstring out = cmd;
  size_t pos = 0;
  while ((pos = out.find(needle, pos)) != std::wstring::npos)
  {
    out.replace(pos, needle.size(), L"-p\"***\"");
    pos += 7;
  }
  return out;
}

// 启动看图程序期间把工作目录切到预览临时目录，避免子进程（尤其是 UWP 照片）
// 继承 7zFM 的工作目录——直接双击便携包里的 7zFM.exe 时，那就是包目录，
// 会导致关闭 7zFM 后包目录仍被看图程序占用、删不掉。
class ScopedCurrentDir
{
public:
  explicit ScopedCurrentDir(const std::wstring &dir)
  {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetCurrentDirectoryW((DWORD)(sizeof(buf) / sizeof(buf[0])), buf);
    if (n > 0 && n < (DWORD)(sizeof(buf) / sizeof(buf[0])))
    {
      old_.assign(buf, n);
      have_ = true;
    }
    if (!dir.empty())
      ::SetCurrentDirectoryW(dir.c_str());
  }
  ~ScopedCurrentDir()
  {
    if (have_)
      ::SetCurrentDirectoryW(old_.c_str());
  }
private:
  std::wstring old_;
  bool have_ = false;
};

// 先 ShellExecute（走默认语义），失败再退回"按关联命令直接起"
INT_PTR LaunchViewer(const std::wstring &file, const std::wstring &dir, std::wstring &how)
{
  ScopedCurrentDir cwdGuard(dir);
  // 1) 自己找到的看图软件（不依赖文件关联）
  {
    const std::wstring viewer = FindImageViewer();
    if (!viewer.empty())
    {
      if (LaunchWithExe(viewer, file, dir))
      {
        how = L"viewer " + viewer;
        return 42;
      }
      how = L"viewer-failed " + viewer;
    }
  }

  // 2) 系统关联
  const INT_PTR se = (INT_PTR)::ShellExecuteW(nullptr, L"open", file.c_str(),
                                              nullptr, dir.c_str(), SW_SHOWNORMAL);
  if (se > 32)
  {
    how = L"shell";
    return se;
  }

  // 3) 关联里登记的命令行
  if (LaunchByAssociation(file))
  {
    how = L"assoc-cmd";
    return 42;
  }

  // 4) 系统自带照片查看器（不依赖关联）
  if (TryWindowsPhotoViewer(file))
  {
    how = L"photoviewer";
    return 42;
  }

  how += how.empty() ? L"none" : L";none";
  return se;
}

// 文件已经解出来后统一处理"调起看图程序"，失败时填好用户能看懂的错误。
bool LaunchAndReport(PreviewResult &r, const std::wstring &file, const std::wstring &dir)
{
  std::wstring how;
  const INT_PTR se = LaunchViewer(file, dir, how);
  r.launcher = how;
  r.launchCode = (long long)se;
  if (se > 32)
    return true;

  r.ok = false;
  wchar_t msg[768];
  if (se == 5)
  {
    swprintf_s(msg,
        L"无法启动看图软件（错误 5：拒绝访问）。\n\n"
        L"当前进程被禁止启动外部程序，不是文件关联问题。\n"
        L"请确认 7zFM 是从资源管理器双击启动的，而不是从聊天窗口 / IDE 的文件预览启动。\n"
        L"如果确实需要指定看图程序，可在程序目录的 config.ini 写：\n"
        L"  [viewer]\n  path=C:\\Program Files\\Honeyview\\Honeyview.exe");
  }
  else
  {
    swprintf_s(msg,
        L"无法启动看图软件（ShellExecute 返回 %lld）。\n"
        L"31 = 没有关联程序；建议安装 Honeyview，或在程序目录的 config.ini 里指定 "
        L"[viewer] path。",
        (long long)se);
  }
  r.error = msg;
  return false;
}

bool RunAndWait(const std::wstring &cmd, DWORD &exitCode, std::wstring &output)
{
  // 把子进程的 stdout/stderr 重定向到文件，失败时能把 7-Zip 自己的报错带出来
  std::wstring outPath;
  {
    wchar_t tmp[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, tmp) > 0)
    {
      wchar_t name[64];
      swprintf_s(name, L"7zip-custom-7z-%lu.log", (unsigned long)::GetCurrentProcessId());
      outPath = std::wstring(tmp) + name;
    }
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

bool IsSafeInnerPath(const std::wstring &rel)
{
  if (rel.empty())
    return false;
  if (rel[0] == L'\\' || rel[0] == L'/' || rel[0] == L':')
    return false;
  if (rel.size() >= 2 && rel[1] == L':')
    return false;

  size_t start = 0;
  while (start <= rel.size())
  {
    size_t end = rel.find_first_of(L"\\/", start);
    if (end == std::wstring::npos)
      end = rel.size();
    const std::wstring part = rel.substr(start, end - start);
    if (part == L".." || part == L".")
      return false;
    // 冒号会让 CreateFile 把后面当成 NTFS 数据流
    if (part.find(L':') != std::wstring::npos)
      return false;
    if (end == rel.size())
      break;
    start = end + 1;
  }
  return true;
}

std::wstring NormalizeInnerPath(const std::wstring &archivePath,
                                const std::wstring &innerRelPath)
{
  std::wstring inner = innerRelPath;
  if (inner.size() > archivePath.size() &&
      _wcsnicmp(inner.c_str(), archivePath.c_str(), archivePath.size()) == 0)
    inner = inner.substr(archivePath.size());
  while (!inner.empty() && (inner[0] == L'\\' || inner[0] == L'/'))
    inner.erase(inner.begin());
  return inner;
}

std::wstring FindHoneyview()
{
  // 1) config.ini 里显式指定的（必须真的是 Honeyview）
  const std::wstring configured = ReadConfigValue(L"path");
  if (!configured.empty())
  {
    if (FileExistsSimple(configured) &&
        _wcsicmp(FileNameOf(configured).c_str(), L"Honeyview.exe") == 0)
      return configured;
    // 用户明确指定了别的看图程序，尊重配置，不走 Honeyview 原生模式
    return std::wstring();
  }

  // 2) Bandisoft 自己登记的安装路径 / App Paths
  const wchar_t * const kRegRoots[] = { L"HKEY_LOCAL_MACHINE", L"HKEY_CURRENT_USER" };
  for (const wchar_t *rootName : kRegRoots)
  {
    const HKEY root = (::lstrcmpiW(rootName, L"HKEY_CURRENT_USER") == 0) ? HKEY_CURRENT_USER
                                                                        : HKEY_LOCAL_MACHINE;
    std::wstring p = ReadRegString(root, L"Software\\Honeyview", L"ProgramPath");
    if (!p.empty() && FileExistsSimple(p))
      return p;
    p = ReadRegString(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Honeyview.exe",
                      nullptr);
    if (!p.empty() && FileExistsSimple(p))
      return p;
    p = ReadRegString(root, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Honeyview.exe",
                      nullptr);
    if (!p.empty() && FileExistsSimple(p))
      return p;
  }

  // 3) 常见安装目录
  static const wchar_t * const kCandidates[] = {
    L"C://Program Files//Honeyview//Honeyview.exe",
    L"C://Program Files (x86)//Honeyview//Honeyview.exe",
    L"D://Honeyview//Honeyview.exe",
    L"D://Program Files//Honeyview//Honeyview.exe",
    L"D://Program Files (x86)//Honeyview//Honeyview.exe",
    L"C://Program Files//Bandizip//Honeyview.exe"
  };
  for (const wchar_t *c : kCandidates)
    if (FileExistsSimple(c))
      return c;

  return std::wstring();
}

namespace {

struct AutofillCtx
{
  DWORD pid = 0;
  std::wstring password;
};

struct FindDlgCtx
{
  DWORD pid = 0;
  HWND dlg = nullptr;
  HWND edit = nullptr;
  HWND button = nullptr;
};

BOOL CALLBACK FindPasswordDlgProc(HWND h, LPARAM lp)
{
  FindDlgCtx *c = reinterpret_cast<FindDlgCtx *>(lp);
  DWORD pid = 0;
  ::GetWindowThreadProcessId(h, &pid);
  if (pid != c->pid || !::IsWindowVisible(h))
    return TRUE;

  wchar_t cls[64] = {};
  if (::GetClassNameW(h, cls, 64) == 0 || wcscmp(cls, L"#32770") != 0)
    return TRUE;

  struct ChildCtx { HWND edit; HWND button; } cc = { nullptr, nullptr };
  ::EnumChildWindows(h, [](HWND ch, LPARAM p) -> BOOL {
    ChildCtx *x = reinterpret_cast<ChildCtx *>(p);
    wchar_t name[64] = {};
    ::GetClassNameW(ch, name, 64);
    if (!x->edit && _wcsicmp(name, L"Edit") == 0)
      x->edit = ch;
    if (!x->button && _wcsicmp(name, L"Button") == 0 && ::GetDlgCtrlID(ch) == IDOK)
      x->button = ch;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&cc));
  if (!cc.edit || !cc.button)
    return TRUE;

  // Honeyview 5.53 的密码框 Edit ID 是 1151；其他版本用标题兜底
  wchar_t title[256] = {};
  ::GetWindowTextW(h, title, 256);
  std::wstring lower = title;
  for (auto &ch : lower)
    ch = (wchar_t)::towlower(ch);
  const bool looksPassword = ::GetDlgCtrlID(cc.edit) == 1151 ||
      lower.find(L"password") != std::wstring::npos ||
      wcsstr(title, L"密码") != nullptr;
  if (!looksPassword)
    return TRUE;

  c->dlg = h;
  c->edit = cc.edit;
  c->button = cc.button;
  return FALSE;
}

// 后台等 Honeyview 的密码框出现，自动填入密码并点确定；最多等 10 秒。
DWORD WINAPI AutofillThread(LPVOID param)
{
  AutofillCtx *c = reinterpret_cast<AutofillCtx *>(param);
  for (int i = 0; i < 200; ++i)
  {
    FindDlgCtx fc;
    fc.pid = c->pid;
    ::EnumWindows(FindPasswordDlgProc, reinterpret_cast<LPARAM>(&fc));
    if (fc.dlg && fc.edit && fc.button)
    {
      ::SendMessageW(fc.edit, WM_SETTEXT, 0, (LPARAM)c->password.c_str());
      ::SendMessageW(fc.button, BM_CLICK, 0, 0);
      break;
    }
    ::Sleep(50);
  }
  if (!c->password.empty())
    ::SecureZeroMemory(&c->password[0], c->password.size() * sizeof(wchar_t));
  delete c;
  return 0;
}

} // namespace

bool LaunchHoneyviewArchive(const std::wstring &honeyviewExe,
                            const std::wstring &archivePath,
                            const std::wstring &innerRelPath,
                            const std::wstring &password)
{
  if (honeyviewExe.empty() || archivePath.empty() || innerRelPath.empty())
    return false;

  std::wstring inner = innerRelPath;
  for (auto &c : inner)
    if (c == L'/')
      c = L'\\';
  if (!IsSafeInnerPath(inner))
    return false;

  // Honeyview 的虚拟路径格式："<归档>|<归档内路径>"（Bandizip 也是这样调的）
  std::wstring cmd = L"\"" + honeyviewExe + L"\" \"" + archivePath + L"|" + inner + L"\"";
  const size_t slash = archivePath.find_last_of(L"\\/");
  const std::wstring workDir = (slash == std::wstring::npos)
      ? std::wstring() : archivePath.substr(0, slash);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!::CreateProcessW(honeyviewExe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0,
                        nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
    return false;

  const DWORD pid = pi.dwProcessId;
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);

  if (!password.empty())
  {
    AutofillCtx *ctx = new AutofillCtx;
    ctx->pid = pid;
    ctx->password = password;
    HANDLE th = ::CreateThread(nullptr, 0, AutofillThread, ctx, 0, nullptr);
    if (th)
      ::CloseHandle(th);
    else
    {
      ::SecureZeroMemory(&ctx->password[0], ctx->password.size() * sizeof(wchar_t));
      delete ctx;
    }
  }
  return true;
}

void DeleteTree(const std::wstring &path)
{
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW((path + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE)
  {
    do
    {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
        continue;
      const std::wstring child = path + L"\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        DeleteTree(child);
      else
      {
        ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::DeleteFileW(child.c_str());
      }
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
  }
  ::RemoveDirectoryW(path.c_str());
}

void CleanupPreviewDirs(int maxAgeHours)
{
  std::wstring root, err;
  bool restricted = false;
  if (!EnsureTempRoot(root, err, restricted))
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
      // 每个子目录是一个归档的预览根，里面可能还有内部目录层级
      DeleteTree(Join(root, fd.cFileName));
    }
  } while (::FindNextFileW(h, &fd));

  ::FindClose(h);
}

void CleanupArchivePreview(const std::wstring &archivePath)
{
  if (archivePath.empty())
    return;

  std::wstring root, err;
  bool restricted = false;
  if (!EnsureTempRoot(root, err, restricted))
    return;

  DeleteTree(root + ArchiveTempKey(archivePath));
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
  std::wstring inner = NormalizeInnerPath(archivePath, innerRelPath);

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
  if (!IsSafeInnerPath(inner))
  {
    r.error = L"条目路径不安全，已拒绝";
    return r;
  }

  std::wstring root;
  if (!EnsureTempRoot(root, r.error, r.restrictedEnv))
  {
    r.envError = true;
    return r;
  }

  // 同一个归档的所有目录共用一个临时根：关闭归档时整目录删除。
  // 内部目录仍然保留在路径里，看图软件按同目录兄弟翻页。
  std::wstring dir = root + ArchiveTempKey(archivePath);
  r.tempDir = dir;

  std::wstring target = Join(Join(dir, innerDir), fileName);
  const std::wstring outSubDir = Join(dir, innerDir);

  // 上次密码错误/解压中断可能留下 0 字节文件，不能当成"已经解压好"。
  if (CacheStillValid(archivePath, target))
  {
    r.openedPath = target;
    r.extracted = CountFiles(outSubDir);
    r.ok = true;
    if (launch)
      LaunchAndReport(r, target, dir);
    return r;
  }
  DeleteTree(outSubDir);

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
    bool pwdRequested = false;
    const std::wstring dll = ModuleDir() + L"7z.dll";
    if (ExtractImagesInProcess(dll.c_str(), archivePath, innerDir, password, dir,
                               extList, count, ipError, pwdRequested) &&
        IsUsableFile(target))
    {
      r.openedPath = target;
      r.extracted = count;
      r.ok = true;
      if (launch)
        LaunchAndReport(r, target, dir);
      return r;
    }

    r.inprocError = ipError;
    r.needsPassword = pwdRequested;
    // 失败时清掉写了一半的文件，否则下次会把残缺文件当缓存
    DeleteTree(outSubDir);

    if (pwdRequested)
    {
      // 归档加密：7z.exe 兜底用的也是同一个（空/错误）密码，必然失败。
      // 交给上层弹密码框，拿到密码后重试。
      r.error = L"归档已加密，需要密码";
      if (r.inprocError.empty())
        r.inprocError = r.error;
      return r;
    }
  }

  // 兜底：启动 7z.exe（多用于进程内方式不支持的特殊归档）
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

  r.command = MaskPassword(cmd2, password);

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
  if (code != 0 || !IsUsableFile(target))
  {
    DeleteTree(outSubDir);
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

  r.openedPath = target;
  r.extracted = CountFiles(outSubDir);
  r.ok = true;
  if (launch)
    LaunchAndReport(r, target, dir);
  return r;
}

} // namespace custom
