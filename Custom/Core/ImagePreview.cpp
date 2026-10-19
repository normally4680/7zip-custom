// Custom/Core/ImagePreview.cpp

#include "ImagePreview.h"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

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

  DWORD lastError = 0;
  for (const auto &b : bases)
  {
    const std::wstring candidate = Join3(b, L"7zip-custom", L"\\preview\\");
    ::CreateDirectoryW(Join3(b, L"7zip-custom", L"").c_str(), nullptr);
    ::CreateDirectoryW(candidate.c_str(), nullptr);
    if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
      root = candidate;
      return true;
    }
    lastError = ::GetLastError();
  }

  wchar_t msg[256];
  swprintf_s(msg, L"无法创建预览目录（错误码 %lu）", (unsigned long)lastError);
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

bool RunAndWait(const std::wstring &cmd, DWORD &exitCode)
{
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi = {};

  std::wstring mutableCmd = cmd;
  if (!::CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return false;

  ::WaitForSingleObject(pi.hProcess, 60000);
  ::GetExitCodeProcess(pi.hProcess, &exitCode);
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
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

  std::wstring innerDir, fileName;
  SplitInnerPath(innerRelPath, innerDir, fileName);
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

  const std::wstring sevenZip = ModuleDir() + L"7z.exe";
  if (!FileExists(sevenZip))
  {
    r.error = L"找不到 7z.exe（它必须和 7zFM.exe 放在同一目录）";
    r.envError = true;
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

    std::wstring cmd = L"\"" + sevenZip + L"\" x \"" + archivePath + L"\" -o\"" + dir +
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

    r.command = cmd;
    DWORD code = 0;
    if (!RunAndWait(cmd, code))
    {
      wchar_t msg[256];
      swprintf_s(msg, L"启动 7z.exe 失败（错误码 %lu）", (unsigned long)::GetLastError());
      r.error = msg;
      r.envError = true;
      return r;
    }
    if (code != 0 || !FileExists(target))
    {
      wchar_t msg[256];
      if (code == 0)
        swprintf_s(msg, L"目标文件没有解出来");
      else
        swprintf_s(msg, L"7z.exe 返回 %lu（密码不对时会这样）", (unsigned long)code);
      r.error = msg;
      return r;
    }
  }

  r.openedPath = target;
  r.extracted = CountFiles(Join(dir, innerDir));
  r.ok = true;

  if (launch)
  {
    const HINSTANCE inst = ::ShellExecuteW(nullptr, L"open", target.c_str(),
                                           nullptr, dir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)inst <= 32)
    {
      r.ok = false;
      r.error = L"没有可用的看图程序（ShellExecute 失败）";
      return r;
    }
  }

  return r;
}

} // namespace custom
