// Custom/Tests/PreviewTest.cpp
//
// 看图模块的提取部分自测（不真的调起看图软件）。
// 用法：PreviewTest.exe <归档路径> <归档内相对路径> [密码]

#include "../Core/ImagePreview.h"

// 直接调用 hook 层，验证"密码为空时自动查库"这条链路
extern "C" int __cdecl CustomHook_TryOpenImage(const wchar_t *archivePath,
                                               const wchar_t *itemRelPath,
                                               const wchar_t *password);

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

std::string Utf8(const std::wstring &w)
{
  if (w.empty())
    return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

int ListDir(const std::wstring &dir)
{
  int n = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = ::FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE)
    return 0;
  do
  {
    if (fd.cFileName[0] == L'.')
      continue;
    printf("    %s\n", Utf8(fd.cFileName).c_str());
    ++n;
  } while (::FindNextFileW(h, &fd));
  ::FindClose(h);
  return n;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
  ::SetConsoleOutputCP(CP_UTF8);

  if (argc < 3)
  {
    printf("usage: PreviewTest.exe <archive> <inner-path> [password]\n");
    return 2;
  }

  const bool viaHook = (wcscmp(argv[1], L"--via-hook") == 0);
  if (viaHook)
  {
    if (argc < 4)
    {
      printf("usage: PreviewTest.exe --via-hook <archive> <inner-path>\n");
      return 2;
    }
    ::SetEnvironmentVariableW(L"Z7CUSTOM_PREVIEW_NOLAUNCH", L"1");
    const int r = CustomHook_TryOpenImage(argv[2], argv[3], nullptr);  // 故意不传密码
    printf("hook returned %d (%s)\n", r, r == 1 ? "handled" : "fallback to 7-Zip");
    return r == 1 ? 0 : 1;
  }

  const std::wstring archive = argv[1];
  const std::wstring inner = argv[2];
  const std::wstring pwd = (argc > 3) ? argv[3] : L"";

  printf("is image: %s\n", custom::IsImageFile(inner.c_str()) ? "yes" : "no");

  const custom::PreviewResult r = custom::PrepareImagePreview(archive, inner, pwd, false);
  printf("ok=%d extracted=%d\n", (int)r.ok, r.extracted);
  if (!r.ok)
  {
    printf("error: %s\n", Utf8(r.error).c_str());
    return 1;
  }

  printf("opened: %s\n", Utf8(r.openedPath).c_str());
  const DWORD a = ::GetFileAttributesW(r.openedPath.c_str());
  if (a == INVALID_FILE_ATTRIBUTES)
  {
    printf("target file missing!\n");
    return 1;
  }

  printf("temp dir contents:\n");
  const int n = ListDir(r.tempDir);
  printf("files in temp dir: %d\n", n);

  // 同一个归档再来一次，应当直接命中缓存（不需要再解压）
  const ULONGLONG t0 = ::GetTickCount64();
  const custom::PreviewResult r2 = custom::PrepareImagePreview(archive, inner, pwd, false);
  const ULONGLONG dt = ::GetTickCount64() - t0;
  printf("second call: ok=%d, %llu ms (cached)\n", (int)r2.ok, (unsigned long long)dt);

  return (r.ok && r2.ok) ? 0 : 1;
}
