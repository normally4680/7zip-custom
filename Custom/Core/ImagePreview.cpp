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

  // 嵌套归档：外层包里的压缩包会被 7-Zip 解到临时目录再打开，面板传进来的
  // 却是逻辑虚拟路径，两者前缀对不上（archivePath 是 C:\...\7zO...\x.zip，
  // 路径是 D:\outer.7z\x.zip\sub\1.png）。剥离后仍带盘符就说明没对上，
  // 这时用归档文件名在路径里找最后一个边界，取它后面的部分。
  if (inner.find(L':') != std::wstring::npos)
  {
    const size_t slash = archivePath.find_last_of(L"\\/");
    const std::wstring name = (slash == std::wstring::npos)
        ? archivePath : archivePath.substr(slash + 1);
    if (!name.empty())
    {
      std::wstring lowerInner = inner;
      for (auto &c : lowerInner)
        c = (wchar_t)::towlower(c);
      std::wstring lowerName = name;
      for (auto &c : lowerName)
        c = (wchar_t)::towlower(c);

      size_t best = std::wstring::npos;
      size_t pos = 0;
      while ((pos = lowerInner.find(lowerName, pos)) != std::wstring::npos)
      {
        const size_t end = pos + name.size();
        // 必须是一个路径组件：前面是分隔符或开头，后面是分隔符
        const bool leftOk = (pos == 0) || inner[pos - 1] == L'\\' || inner[pos - 1] == L'/';
        const bool rightOk = (end < inner.size())
            && (inner[end] == L'\\' || inner[end] == L'/');
        if (leftOk && rightOk)
          best = end;
        pos = end;
      }
      if (best != std::wstring::npos)
      {
        inner = inner.substr(best);
        while (!inner.empty() && (inner[0] == L'\\' || inner[0] == L'/'))
          inner.erase(inner.begin());
      }
    }
  }
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
  std::wstring exe;
  std::wstring archive;
  std::vector<std::wstring> passwords;
  std::vector<std::wstring> passwordNames;
  size_t next = 0;
  bool firstPrompt = true;
  bool firstPromptNeedsAsk = false;
  std::wstring firstLayerName;
  std::wstring fallbackName;
  int (*askPassword)(void *owner, const wchar_t *archiveName,
                     wchar_t *buf, int cch) = nullptr;
  int relaunches = 0;
};

void FreeAutofillCtx(AutofillCtx *c)
{
  if (!c)
    return;
  for (auto &p : c->passwords)
    if (!p.empty())
      ::SecureZeroMemory(&p[0], p.size() * sizeof(wchar_t));
  delete c;
}

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

// Honeyview 密码错误时弹的提示框：class #32770、没有 Edit、按钮 ID 101。
struct ErrorDlgCtx { DWORD pid; HWND dlg; };

BOOL CALLBACK FindErrorDlgProc(HWND h, LPARAM lp)
{
  ErrorDlgCtx *c = reinterpret_cast<ErrorDlgCtx *>(lp);
  DWORD pid = 0;
  ::GetWindowThreadProcessId(h, &pid);
  if (pid != c->pid || !::IsWindowVisible(h))
    return TRUE;
  wchar_t cls[64] = {};
  if (::GetClassNameW(h, cls, 64) == 0 || wcscmp(cls, L"#32770") != 0)
    return TRUE;

  bool flags[2] = { false, false };
  ::EnumChildWindows(h, [](HWND ch, LPARAM p) -> BOOL {
    bool *f = reinterpret_cast<bool *>(p);
    wchar_t name[64] = {};
    ::GetClassNameW(ch, name, 64);
    if (_wcsicmp(name, L"Edit") == 0)
      f[0] = true;
    if (_wcsicmp(name, L"Button") == 0 && ::GetDlgCtrlID(ch) == 101)
      f[1] = true;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&flags));

  if (!flags[0] && flags[1])
  {
    c->dlg = h;
    return FALSE;
  }
  return TRUE;
}

bool StartHoneyview(const std::wstring &honeyviewExe, const std::wstring &argQuoted,
                    const std::wstring &archivePath, DWORD &pidOut);

bool FindErrorDialog(DWORD pid, HWND &out)
{
  ErrorDlgCtx c = { pid, nullptr };
  ::EnumWindows(FindErrorDlgProc, reinterpret_cast<LPARAM>(&c));
  out = c.dlg;
  return out != nullptr;
}

void DismissDialog(HWND dlg)
{
  ::PostMessageW(dlg, WM_COMMAND, (WPARAM)IDOK, 0);
  ::Sleep(200);
}


// 找 Honeyview 主窗口，作为本项目密码框的 owner（居中/置前用）。
HWND FindHoneyviewWindow(DWORD pid)
{
  struct Ctx { DWORD pid; HWND hwnd; } c = { pid, nullptr };
  ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    Ctx *x = reinterpret_cast<Ctx *>(lp);
    DWORD p = 0;
    ::GetWindowThreadProcessId(h, &p);
    if (p != x->pid || !::IsWindowVisible(h))
      return TRUE;
    wchar_t cls[64] = {};
    ::GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"HoneyviewClassX") == 0)
    {
      x->hwnd = h;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&c));
  return c.hwnd;
}

// 后台逐个处理 Honeyview 的密码框（嵌套包会连续弹多层）：
//   * 队列里有密码 -> 写入（覆盖 Honeyview 可能过期的预填）
//   * 队列空了但框里有预填 -> 保留预填，直接确定
//   * 空框且没有可填密码 -> 弹本项目自己的密码框
// 每个框先等 300ms 让控件就绪；连续 15 秒没有新框就结束。
DWORD WINAPI AutofillThread(LPVOID param)
{
  AutofillCtx *c = reinterpret_cast<AutofillCtx *>(param);
  ULONGLONG start = ::GetTickCount64();
  int idle = 0;
  HWND lastDlg = nullptr;
  int sameDlg = 0;
  std::wstring lastPassword;   // 最近一次成功提交的密码（Honeyview 重复弹框时复用）


  while (::GetTickCount64() - start < 60000)
  {
    FindDlgCtx fc;
    fc.pid = c->pid;
    ::EnumWindows(FindPasswordDlgProc, reinterpret_cast<LPARAM>(&fc));
    if (!fc.dlg || !fc.edit || !fc.button)
    {
      ::Sleep(50);
      if (++idle > 300)   // 15 秒没有新密码框就算结束（冷启动解析可能较慢）
        break;
      continue;
    }
    idle = 0;

    // 密码框刚建好时内部状态还没就绪，马上输入会被当成空密码。
    ::Sleep(300);

    if (fc.dlg == lastDlg)
    {
      if (++sameDlg > 3)   // 点了确定框还不消失，别死循环
        break;
    }
    else
    {
      lastDlg = fc.dlg;
      sameDlg = 0;
    }

    wchar_t cur[512] = {};
    ::GetWindowTextW(fc.edit, cur, (int)(sizeof(cur) / sizeof(cur[0])));
    const bool prefilled = (cur[0] != 0);

    std::wstring fill;
    bool needAsk = false;
    std::wstring askName;
    int consumedIndex = -1;
    const bool wasFirstPrompt = c->firstPrompt;

    if (c->firstPrompt && c->firstPromptNeedsAsk)
    {
      // 外层可能加密但队列里没有它的密码：预填内容留给 Honeyview，
      // 空框则先弹我们自己的密码框；这一框不消耗内层密码。
      if (!prefilled)
      {
        needAsk = true;
        askName = c->firstLayerName;
      }
    }
    else if (c->next < c->passwords.size())
    {
      // 有按层排好的密码就直接写入（覆盖 Honeyview 可能过期的预填）
      consumedIndex = (int)c->next;
      fill = c->passwords[c->next++];
    }
    else if (!prefilled)
    {
      // 队列用完了：Honeyview 可能对同一层再弹一次，先复用上次成功的密码；
      // 不同层且密码不同时会被 Honeyview 拒绝，再走错误恢复流程。
      if (!lastPassword.empty())
        fill = lastPassword;
      else
      {
        needAsk = true;
        askName = c->fallbackName;
      }
    }

    if (needAsk)
    {
      if (!c->askPassword)
        break;
      wchar_t buf[1024] = {};
      // 以 Honeyview 的密码框本身做 owner：不要禁用主窗口，否则它的
      // 模态密码框可能被重置/重新弹出。
      const int r = c->askPassword(fc.dlg,
                                   askName.empty() ? L"嵌套压缩包" : askName.c_str(),
                                   buf, (int)(sizeof(buf) / sizeof(buf[0])));
      if (r == 1 && buf[0])
        fill = buf;
      ::SecureZeroMemory(buf, sizeof(buf));
      if (fill.empty())
        break;   // 用户取消：把 Honeyview 的框留在那里让用户自己输入
    }

    if (!fill.empty())
    {
      // 用 WM_CHAR 逐字输入：Honeyview 的密码框有内部状态，跨进程直接
      // WM_SETTEXT 有时只改了显示文本、按确定仍被当成空密码。
      ::SendMessageW(fc.edit, EM_SETSEL, 0, -1);
      ::SendMessageW(fc.edit, WM_CLEAR, 0, 0);
      for (const wchar_t ch : fill)
        ::SendMessageW(fc.edit, WM_CHAR, (WPARAM)ch, 0);
      ::Sleep(100);
    }
    if (!fill.empty())
      lastPassword = fill;
    else if (prefilled)
      lastPassword = cur;
    ::SendMessageW(fc.button, BM_CLICK, 0, 0);
    c->firstPrompt = false;

    // 等这个框关掉，再继续等下一层
    for (int k = 0; k < 60; ++k)
    {
      ::Sleep(50);
      if (!::IsWindowVisible(fc.dlg))
        break;
    }

    // 自动填的密码被 Honeyview 拒绝：关掉错误框，用我们的密码框重新问这一层，
    // 然后重启 Honeyview（错误框关掉后这个实例就废了）。
    // 错误框通常比密码框晚几十毫秒出现；最多等 1.5 秒，下一层密码框出现就提前结束。
    HWND errDlg = nullptr;
    for (int k = 0; k < 30; ++k)
    {
      if (FindErrorDialog(c->pid, errDlg))
        break;
      FindDlgCtx nextDlg;
      nextDlg.pid = c->pid;
      ::EnumWindows(FindPasswordDlgProc, reinterpret_cast<LPARAM>(&nextDlg));
      if (nextDlg.dlg && nextDlg.dlg != fc.dlg)
        break;
      ::Sleep(50);
    }
    if (errDlg)
    {
      DismissDialog(errDlg);
      if (c->relaunches >= 2 || c->exe.empty() || !c->askPassword)
        break;

      std::wstring name = askName.empty() ? c->fallbackName : askName;
      if (consumedIndex >= 0 &&
          (size_t)consumedIndex < c->passwordNames.size() &&
          !c->passwordNames[(size_t)consumedIndex].empty())
        name = c->passwordNames[(size_t)consumedIndex];
      if (wasFirstPrompt && consumedIndex < 0)
        name = c->firstLayerName;

      HWND hvWnd = FindHoneyviewWindow(c->pid);
      ::MessageBoxW(hvWnd, L"密码错误，请重新输入。", L"密码错误",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);

      wchar_t buf[1024] = {};
      const int r = c->askPassword(hvWnd,
                                   name.empty() ? L"嵌套压缩包" : name.c_str(),
                                   buf, (int)(sizeof(buf) / sizeof(buf[0])));
      if (r != 1 || !buf[0])
      {
        ::SecureZeroMemory(buf, sizeof(buf));
        break;
      }

      if (consumedIndex >= 0)
        c->passwords[(size_t)consumedIndex] = buf;
      else if (wasFirstPrompt)
      {
        // 外层密码填错：插到队列最前面，重启后先填外层
        c->passwords.insert(c->passwords.begin(), buf);
        c->passwordNames.insert(c->passwordNames.begin(), c->firstLayerName);
        c->firstPromptNeedsAsk = false;
      }
      else
      {
        c->passwords.push_back(buf);
        c->passwordNames.push_back(name);
      }
      ::SecureZeroMemory(buf, sizeof(buf));

      HANDLE ph = ::OpenProcess(PROCESS_TERMINATE, FALSE, c->pid);
      if (ph)
      {
        ::TerminateProcess(ph, 0);
        ::CloseHandle(ph);
      }
      ::Sleep(300);

      DWORD newPid = 0;
      if (!StartHoneyview(c->exe, L"\"" + c->archive + L"\"", c->archive, newPid))
        break;
      c->pid = newPid;
      c->next = 0;
      c->firstPrompt = true;
      lastPassword.clear();
      ++c->relaunches;
      lastDlg = nullptr;
      sameDlg = 0;
      idle = 0;
      start = ::GetTickCount64();
    }
  }

  FreeAutofillCtx(c);
  return 0;
}

// 启动 Honeyview 并返回新进程 pid。argQuoted 是带引号的单个参数
//（"archive" 或 "archive|inner"），工作目录设为归档所在目录。
bool StartHoneyview(const std::wstring &honeyviewExe, const std::wstring &argQuoted,
                    const std::wstring &archivePath, DWORD &pidOut)
{
  pidOut = 0;
  std::wstring cmd = L"\"" + honeyviewExe + L"\" " + argQuoted;
  const size_t slash = archivePath.find_last_of(L"\\/");
  const std::wstring workDir = (slash == std::wstring::npos)
      ? std::wstring() : archivePath.substr(0, slash);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!::CreateProcessW(honeyviewExe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0,
                        nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi))
    return false;
  pidOut = pi.dwProcessId;
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  return true;
}

void StartAutofill(DWORD pid, const std::wstring &password)
{
  AutofillCtx *ctx = new AutofillCtx;
  ctx->pid = pid;
  if (!password.empty())
    ctx->passwords.push_back(password);
  HANDLE th = ::CreateThread(nullptr, 0, AutofillThread, ctx, 0, nullptr);
  if (th)
    ::CloseHandle(th);
  else
    FreeAutofillCtx(ctx);
}

void StartAutofillMulti(DWORD pid, const std::wstring &honeyviewExe,
                        const std::wstring &archivePath,
                        const NestedHoneyviewOptions &options)
{
  AutofillCtx *ctx = new AutofillCtx;
  ctx->pid = pid;
  ctx->exe = honeyviewExe;
  ctx->archive = archivePath;
  ctx->passwords = options.passwords;
  ctx->passwordNames = options.passwordNames;
  ctx->firstPromptNeedsAsk = options.firstPromptNeedsAsk;
  ctx->firstLayerName = options.firstLayerName;
  ctx->fallbackName = options.fallbackName;
  ctx->askPassword = options.askPassword;
  HANDLE th = ::CreateThread(nullptr, 0, AutofillThread, ctx, 0, nullptr);
  if (th)
    ::CloseHandle(th);
  else
    FreeAutofillCtx(ctx);
}

// pid 的可见顶层窗口里是否有标题含 sub 的（用来确认竖线窗口真开出来了）
bool HasWindowTitled(DWORD pid, const std::wstring &sub)
{
  struct Ctx { DWORD pid; const std::wstring *sub; bool hit; } c = { pid, &sub, false };
  ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
    Ctx *x = reinterpret_cast<Ctx *>(lp);
    DWORD p = 0;
    ::GetWindowThreadProcessId(h, &p);
    if (p != x->pid || !::IsWindowVisible(h))
      return TRUE;
    wchar_t title[512] = {};
    ::GetWindowTextW(h, title, 512);
    if (x->sub->empty() || wcsstr(title, x->sub->c_str()) != nullptr)
    {
      x->hit = true;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&c));
  return c.hit;
}

struct TwoStepCtx
{
  std::wstring exe;
  std::wstring archive;
  std::wstring inner;   // 反斜杠形式
};

// 两步走线程：整包打开 → 等解析进浏览历史 → 竖线切到目标图 → 关掉整包窗口。
// 背景：Honeyview 竖线快开解析 7z 时只认格式版本 ≤0.3（26.04 写 0.4），
// 但整包打开不受限；整包打开过一次后竖线就能用。
DWORD WINAPI WholeThenPipeThread(LPVOID param)
{
  TwoStepCtx *c = reinterpret_cast<TwoStepCtx *>(param);
  DWORD pid1 = 0;
  if (!StartHoneyview(c->exe, L"\"" + c->archive + L"\"", c->archive, pid1))
  {
    delete c;
    return 0;
  }
  ::Sleep(2500);   // 给 Honeyview 时间解析归档
  DWORD pid2 = 0;
  if (!StartHoneyview(c->exe, L"\"" + c->archive + L"|" + c->inner + L"\"", c->archive, pid2))
  {
    delete c;      // 竖线没发出去，至少整包窗口还开着
    return 0;
  }
  ::Sleep(2000);
  // 竖线窗口标题里含目标文件名；确认它真开出来了才关整包窗口
  const size_t slash = c->inner.find_last_of(L'\\');
  const std::wstring name = (slash == std::wstring::npos) ? c->inner : c->inner.substr(slash + 1);
  if (pid1 && pid2 && pid1 != pid2 && HasWindowTitled(pid2, name))
  {
    HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid1);
    if (h)
    {
      ::TerminateProcess(h, 0);
      ::CloseHandle(h);
    }
  }
  delete c;
  return 0;
}

} // namespace

int SevenZArchiveVersion(const std::wstring &archivePath)
{
  HANDLE h = ::CreateFileW(archivePath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return -1;
  unsigned char hdr[8] = {};
  DWORD got = 0;
  const BOOL ok = ::ReadFile(h, hdr, sizeof(hdr), &got, nullptr);
  ::CloseHandle(h);
  if (!ok || got != sizeof(hdr))
    return -1;
  // "7z\xBC\xAF\x27\x1C" + 版本主/次字节；次字节即 0.3/0.4 的 3/4
  static const unsigned char kSig[6] = { 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C };
  for (int i = 0; i < 6; ++i)
    if (hdr[i] != kSig[i])
      return -1;
  if (hdr[6] != 0x00)
    return -1;
  return (int)hdr[7];
}

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
  DWORD pid = 0;
  if (!StartHoneyview(honeyviewExe,
                      L"\"" + archivePath + L"|" + inner + L"\"",
                      archivePath, pid))
    return false;

  if (!password.empty())
    StartAutofill(pid, password);
  return true;
}

bool LaunchHoneyviewWholeArchive(const std::wstring &honeyviewExe,
                                 const std::wstring &archivePath,
                                 const std::wstring &password)
{
  if (honeyviewExe.empty() || archivePath.empty())
    return false;

  DWORD pid = 0;
  if (!StartHoneyview(honeyviewExe, L"\"" + archivePath + L"\"", archivePath, pid))
    return false;

  if (!password.empty())
    StartAutofill(pid, password);
  return true;
}

bool LaunchHoneyviewWholeArchiveMulti(const std::wstring &honeyviewExe,
                                      const std::wstring &archivePath,
                                      const NestedHoneyviewOptions &options)
{
  if (honeyviewExe.empty() || archivePath.empty())
    return false;

  DWORD pid = 0;
  if (!StartHoneyview(honeyviewExe, L"\"" + archivePath + L"\"", archivePath, pid))
    return false;

  if (!options.passwords.empty() || options.askPassword)
    StartAutofillMulti(pid, honeyviewExe, archivePath, options);
  return true;
}

bool LaunchHoneyviewWholeThenPipe(const std::wstring &honeyviewExe,
                                  const std::wstring &archivePath,
                                  const std::wstring &innerRelPath)
{
  if (honeyviewExe.empty() || archivePath.empty() || innerRelPath.empty())
    return false;

  std::wstring inner = innerRelPath;
  for (auto &c : inner)
    if (c == L'/')
      c = L'\\';
  if (!IsSafeInnerPath(inner))
    return false;

  TwoStepCtx *ctx = new TwoStepCtx;
  ctx->exe = honeyviewExe;
  ctx->archive = archivePath;
  ctx->inner = inner;
  HANDLE th = ::CreateThread(nullptr, 0, WholeThenPipeThread, ctx, 0, nullptr);
  if (!th)
  {
    delete ctx;
    return false;
  }
  ::CloseHandle(th);
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
