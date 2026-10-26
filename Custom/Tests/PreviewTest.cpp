// Custom/Tests/PreviewTest.cpp
//
// 看图模块的提取部分自测（不真的调起看图软件）。
// 用法：PreviewTest.exe <归档路径> <归档内相对路径> [密码]

#include "../Core/ImagePreview.h"
#include "../Core/SevenZipInProc.h"
#include "../Core/ShellIntegration.h"

// 直接调用 hook 层，验证"密码为空时自动查库"这条链路
extern "C" int __cdecl CustomHook_TryOpenImage(const wchar_t *archivePath,
                                               const wchar_t *itemRelPath,
                                               const wchar_t *password);
extern "C" void __cdecl CustomHook_OnArchiveClosed(const wchar_t *archivePath);

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

  if (argc < 2)
  {
    printf("usage: PreviewTest.exe <archive> <inner-path> [password]\n");
    return 2;
  }

  // 路径校验的纯逻辑测试，不需要归档
  if (wcscmp(argv[1], L"--sanitize") == 0)
  {
    struct Case { const wchar_t *path; bool safe; };
    const Case cases[] = {
      { L"a.png", true },
      { L"dir\\a.png", true },
      { L"dir/sub/b.jpg", true },
      { L"../a.png", false },
      { L"dir/../../a.png", false },
      { L"\\abs\\a.png", false },
      { L"C:\\a.png", false },
      { L"dir\\a.png:stream", false },
    };
    int fails = 0;
    for (const Case &c : cases)
    {
      const bool got = custom::IsSafeInnerPath(c.path);
      if (got != c.safe)
      {
        printf("FAIL: %s -> %d (want %d)\n", Utf8(c.path).c_str(), (int)got, (int)c.safe);
        ++fails;
      }
    }
    printf("sanitize: %d cases, %d failed\n", (int)(sizeof(cases) / sizeof(cases[0])), fails);
    return fails ? 1 : 0;
  }

  // 只看归档是否加密（决定走 Honeyview 原生还是解压）
  if (wcscmp(argv[1], L"--enc-check") == 0 && argc >= 3)
  {
    bool enc = false;
    const bool known = custom::ArchiveIsEncrypted(L"7z.dll", argv[2], enc);
    printf("known=%d encrypted=%d\n", (int)known, (int)enc);
    return known ? 0 : 1;
  }

  // 直接走 Honeyview 原生路径（不解压）；可选密码会后台自动填入密码框
  if (wcscmp(argv[1], L"--native") == 0 && argc >= 4)
  {
    const std::wstring hv = custom::FindHoneyview();
    printf("honeyview: %s\n", Utf8(hv).c_str());
    if (hv.empty())
      return 1;
    const std::wstring inner = custom::NormalizeInnerPath(argv[2], argv[3]);
    const std::wstring pwd = (argc >= 5) ? argv[4] : L"";
    const bool ok = custom::LaunchHoneyviewArchive(hv, argv[2], inner, pwd);
    printf("launch=%d inner=%s autofill=%d\n", (int)ok, Utf8(inner).c_str(), (int)!pwd.empty());
    if (ok && !pwd.empty())
      ::Sleep(5000);   // 等后台线程把密码填进 Honeyview 的密码框
    return ok ? 0 : 1;
  }

  // 按用户注册右键菜单（测试用）
  if (wcscmp(argv[1], L"--shell-state") == 0)
  {
    std::wstring dll;
    const bool reg = custom::IsShellContextMenuRegistered(dll);
    printf("registered=%d dll=%s\n", (int)reg, Utf8(dll).c_str());
    return 0;
  }
  if (wcscmp(argv[1], L"--shell-register") == 0)
  {
    std::wstring err;
    const bool ok = custom::RegisterShellContextMenu(err);
    printf("register=%d error=%s\n", (int)ok, Utf8(err).c_str());
    return ok ? 0 : 1;
  }
  if (wcscmp(argv[1], L"--shell-unregister") == 0)
  {
    std::wstring err;
    const bool ok = custom::UnregisterShellContextMenu(err);
    printf("unregister=%d error=%s\n", (int)ok, Utf8(err).c_str());
    return ok ? 0 : 1;
  }

  if (argc < 3)
  {
    printf("usage: PreviewTest.exe <archive> <inner-path> [password]\n");
    return 2;
  }

  if (wcscmp(argv[1], L"--cleanup") == 0 && argc >= 3)
  {
    CustomHook_OnArchiveClosed(argv[2]);
    printf("cleanup done\n");
    return 0;
  }

  // 先用错误密码解一次（应当失败且不留下残缺文件），再用正确密码重试
  if (wcscmp(argv[1], L"--retry") == 0 && argc >= 6)
  {
    const custom::PreviewResult bad =
        custom::PrepareImagePreview(argv[2], argv[3], argv[4], false);
    const custom::PreviewResult good =
        custom::PrepareImagePreview(argv[2], argv[3], argv[5], false);
    const bool pass = !bad.ok && good.ok && !good.openedPath.empty();
    printf("wrong password: ok=%d needsPassword=%d error=%s\n",
           (int)bad.ok, (int)bad.needsPassword, Utf8(bad.error).c_str());
    printf("right password: ok=%d opened=%s\n",
           (int)good.ok, Utf8(good.openedPath).c_str());
    printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }

  if (wcscmp(argv[1], L"--inproc") == 0 && argc >= 4)
  {
    std::vector<std::wstring> exts = { L"bmp", L"png", L"jpg" };
    int count = 0;
    std::wstring err;
    bool pwdRequested = false;
    const std::wstring pwd = (argc >= 5) ? argv[4] : L"";
    const bool ok = custom::ExtractImagesInProcess(L"7z.dll", argv[2], L"风景", pwd, argv[3],
                                                   exts, count, err, pwdRequested);
    printf("inproc ok=%d count=%d pwdreq=%d error=%s\n",
           (int)ok, count, (int)pwdRequested, Utf8(err).c_str());
    return ok ? 0 : 1;
  }

  const bool viaHook = (wcscmp(argv[1], L"--via-hook") == 0);
  const bool viaHookLaunch = (wcscmp(argv[1], L"--via-hook-launch") == 0);
  if (viaHook || viaHookLaunch)
  {
    if (argc < 4)
    {
      printf("usage: PreviewTest.exe --via-hook [--via-hook-launch] <archive> <inner-path>\n");
      return 2;
    }
    // --via-hook 只准备文件；--via-hook-launch 连"调起看图程序"一起测
    if (viaHook)
      ::SetEnvironmentVariableW(L"Z7CUSTOM_PREVIEW_NOLAUNCH", L"1");
    const int r = CustomHook_TryOpenImage(argv[2], argv[3], nullptr);  // 故意不传密码
    if (viaHookLaunch)
      ::Sleep(4000);   // 给 Hook 里的自动填密码线程留时间
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
