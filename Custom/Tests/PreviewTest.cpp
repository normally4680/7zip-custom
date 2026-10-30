// Custom/Tests/PreviewTest.cpp
//
// 看图模块的提取部分自测（不真的调起看图软件）。
// 用法：PreviewTest.exe <归档路径> <归档内相对路径> [密码]

#include "../Core/ImagePreview.h"
#include "../Core/PasswordVault.h"
#include "../Core/SevenZipInProc.h"
#include "../Core/ShellIntegration.h"

// 直接调用 hook 层，验证"密码为空时自动查库"这条链路
extern "C" int __cdecl CustomHook_TryOpenImage(const wchar_t *archivePath,
                                               const wchar_t *itemRelPath,
                                               const wchar_t *password);
extern "C" int __cdecl CustomHook_TryOpenNestedImage(const wchar_t *outerArchivePath,
                                                     const wchar_t *layerPaths,
                                                     const wchar_t *itemRelPath);
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

  // 验证密码能不能真的解出条目：退出码 0=验证通过 2=密码不可用 1=判断不了
  // 用法：PreviewTest.exe --verify <archive> <inner-path> <password>
  if (wcscmp(argv[1], L"--verify") == 0 && argc >= 5)
  {
    bool ok = false;
    const bool known = custom::ArchiveTryPassword(L"7z.dll", argv[2], argv[3], argv[4], ok);
    printf("known=%d verified=%d\n", (int)known, (int)ok);
    return !known ? 1 : (ok ? 0 : 2);
  }

  // 测试用：列出当前密码库条目名（不打印密码）
  if (wcscmp(argv[1], L"--vault-list") == 0)
  {
    custom::PasswordVault &vault = custom::PasswordVault::Instance();
    vault.Load(false);
    printf("vault entries: %d\n", (int)vault.Entries().size());
    for (const auto &e : vault.Entries())
      printf("  %s\n", Utf8(e.name).c_str());
    return 0;
  }

  // 测试用：把某条目的密码复制到另一个条目名下（不打印密码）
  if (wcscmp(argv[1], L"--vault-copy") == 0 && argc >= 4)
  {
    custom::PasswordVault &vault = custom::PasswordVault::Instance();
    vault.Load(false);
    std::wstring pwd;
    if (!vault.FindForArchive(argv[2], pwd) || pwd.empty())
    {
      printf("source not found\n");
      return 1;
    }
    const bool ok = vault.Remember(argv[3], pwd);
    ::SecureZeroMemory(&pwd[0], pwd.size() * sizeof(wchar_t));
    printf("copy %s -> %s: %d\n", Utf8(argv[2]).c_str(), Utf8(argv[3]).c_str(), (int)ok);
    return ok ? 0 : 1;
  }

  // 测试用：把条目写进当前程序使用的密码库（便于验证嵌套包按文件名查库）
  if (wcscmp(argv[1], L"--vault-remember") == 0 && argc >= 4)
  {
    custom::PasswordVault &vault = custom::PasswordVault::Instance();
    vault.Load(false);
    const bool ok = vault.Remember(argv[2], argv[3]);
    printf("vault remember %s: %d\n", Utf8(argv[2]).c_str(), (int)ok);
    return ok ? 0 : 1;
  }

  // 嵌套包：外层真实归档 + 各内层路径 + 被点条目的虚拟路径
  // 用法：PreviewTest.exe --nested <outer> <virtual-item> <inner1> [inner2 ...]
  // 密码由 hook 自己从会话缓存/密码库按层解析；取不到时会弹本项目密码框。
  if (wcscmp(argv[1], L"--nested") == 0 && argc >= 4)
  {
    std::wstring layers = argv[2];
    for (int i = 4; i < argc; ++i)
    {
      layers += L'\n';
      layers += argv[i];
    }

    // 模拟 7zFM 主窗口：真实调用发生在 7zFM UI 线程上，预问密码框以它为 owner。
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"7-Zip::FM";
    ::RegisterClassExW(&wc);
    HWND testOwner = ::CreateWindowExW(0, L"7-Zip::FM", L"PreviewTest",
                                       WS_OVERLAPPEDWINDOW, 100, 100, 480, 320,
                                       nullptr, nullptr, wc.hInstance, nullptr);
    if (testOwner)
    {
      ::ShowWindow(testOwner, SW_SHOWNOACTIVATE);
      ::SetActiveWindow(testOwner);
    }

    const int r = CustomHook_TryOpenNestedImage(argv[2], layers.c_str(), argv[3]);
    printf("nested hook returned %d (%s)\n", r, r == 1 ? "handled" : "fallback");
    if (r == 1)
      ::Sleep(120000);
    if (testOwner)
      ::DestroyWindow(testOwner);
    return r == 1 ? 0 : 1;
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

  // 整包打开（7z 加密包用）：可选密码后台自动填入
  if (wcscmp(argv[1], L"--whole") == 0 && argc >= 3)
  {
    const std::wstring hv = custom::FindHoneyview();
    printf("honeyview: %s\n", Utf8(hv).c_str());
    if (hv.empty())
      return 1;
    const std::wstring pwd = (argc >= 4) ? argv[3] : L"";
    const bool ok = custom::LaunchHoneyviewWholeArchive(hv, argv[2], pwd);
    printf("whole launch=%d autofill=%d\n", (int)ok, (int)!pwd.empty());
    if (ok)
      ::Sleep(6000);
    return ok ? 0 : 1;
  }

  // 两步走（7z 未加密 0.4 用）：整包打开 → 竖线定位 → 关整包窗口
  if (wcscmp(argv[1], L"--twostep") == 0 && argc >= 4)
  {
    const std::wstring hv = custom::FindHoneyview();
    printf("honeyview: %s\n", Utf8(hv).c_str());
    if (hv.empty())
      return 1;
    const bool ok = custom::LaunchHoneyviewWholeThenPipe(hv, argv[2], argv[3]);
    printf("twostep launch=%d\n", (int)ok);
    if (ok)
      ::Sleep(7000);   // 等后台线程走完两步
    return ok ? 0 : 1;
  }

  // 打印 7z 签名头里的格式版本（0.3/0.4，竖线兼容性判定用）
  if (wcscmp(argv[1], L"--7zver") == 0 && argc >= 3)
  {
    printf("version=%d\n", custom::SevenZArchiveVersion(argv[2]));
    return 0;
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
      ::Sleep(9000);   // 给 Hook 里的后台线程留时间（两步走要 ~5s 再关整包窗口）
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
