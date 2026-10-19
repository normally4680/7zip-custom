// Custom/Tests/VaultTest.cpp
//
// 密码库的无界面自测。用 Z7CUSTOM_VAULT_DIR 指向临时目录，
// 不会碰到真实用户数据。

#include "../Core/PasswordVault.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

int g_failed = 0;

void Check(bool cond, const char *what)
{
  printf("%s  %s\n", cond ? "[ ok ]" : "[FAIL]", what);
  if (!cond)
    ++g_failed;
}

std::string ToUtf8(const std::wstring &w)
{
  if (w.empty())
    return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
  // 命令行模式：VaultTest.exe --add <归档路径> <密码>
  // 用于脚本化验证 hook 链路（配合 Z7CUSTOM_VAULT_DIR）
  if (argc == 4 && wcscmp(argv[1], L"--add") == 0)
  {
    wchar_t cwd[MAX_PATH];
    ::GetCurrentDirectoryW(MAX_PATH, cwd);
    if (::GetEnvironmentVariableW(L"Z7CUSTOM_VAULT_DIR", nullptr, 0) == 0)
      ::SetEnvironmentVariableW(L"Z7CUSTOM_VAULT_DIR", (std::wstring(cwd) + L"\\_vaulttest").c_str());
    custom::PasswordVault &v = custom::PasswordVault::Instance();
    v.Load(true);
    if (!v.Remember(argv[2], argv[3]))
    {
      printf("add failed: %s\n", ToUtf8(v.LastError()).c_str());
      return 1;
    }
    printf("added: %s\n", ToUtf8(v.LastError()).c_str());
    return 0;
  }

  if (argc >= 2 && wcscmp(argv[1], L"--list") == 0)
  {
    custom::PasswordVault &v = custom::PasswordVault::Instance();
    v.Load(true);
    for (const auto &e : v.Entries())
      printf("%s\t%s\n", ToUtf8(e.name).c_str(), ToUtf8(e.password).c_str());
    return 0;
  }

  wchar_t cwdRoot[MAX_PATH];
  ::GetCurrentDirectoryW(MAX_PATH, cwdRoot);
  // 测试目录放在当前工作目录下，避免受系统临时目录权限影响
  std::wstring dir = std::wstring(cwdRoot) + L"\\_vaulttest";
  ::CreateDirectoryW(dir.c_str(), nullptr);
  ::SetEnvironmentVariableW(L"Z7CUSTOM_VAULT_DIR", dir.c_str());

  // 每次都从干净状态开始
  ::DeleteFileW((dir + L"\\vault.dat").c_str());
  ::DeleteFileW((dir + L"\\vault.key").c_str());

  custom::PasswordVault &v = custom::PasswordVault::Instance();
  v.Clear();

  Check(v.Load(true), "空库可以正常加载");
  Check(v.Entries().empty(), "空库条目数为 0");

  printf("       dir = %s (ensure=%d)\n", ToUtf8(custom::VaultDir()).c_str(), (int)v.EnsureDir());

  const bool r1 = v.Remember(L"C:\\data\\backup.7z", L"secret-1");
  if (!r1)
    printf("       remember failed, LastError=[%s] GetLastError=%lu\n",
           ToUtf8(v.LastError()).c_str(), (unsigned long)::GetLastError());
  Check(r1, "记住 backup.7z 的密码");
  Check(v.Remember(L"D:\\other\\photos.zip", L"secret-2"), "记住 photos.zip 的密码");
  Check(v.Entries().size() == 2, "共有 2 条记录");

  // 大小写不敏感 + 目录无关
  std::wstring pw;
  Check(v.FindForArchive(L"E:\\moved\\BACKUP.7Z", pw) && pw == L"secret-1",
        "换个目录 + 大写扩展名仍能命中");
  Check(v.FindForArchive(L"C:\\data\\unknown.7z", pw) == false, "未登记的归档不命中");

  // 通配
  {
    custom::VaultEntry e;
    e.name = L"*.rar";
    e.password = L"wild-pass";
    e.note = L"所有 rar 用同一个密码";
    v.Entries().push_back(e);
    Check(v.Save(), "写入通配条目");
    v.Clear();
    Check(v.Load(true), "重新加载");
    Check(v.FindForArchive(L"Z:\\a\\b\\file.rar", pw) && pw == L"wild-pass", "通配 *.rar 命中 file.rar");
    Check(v.FindForArchive(L"Z:\\a\\b\\file.RAR", pw) == true, "通配匹配不区分大小写");
  }

  // 精确名优先于通配
  {
    custom::VaultEntry e;
    e.name = L"important.rar";
    e.password = L"exact-pass";
    v.Entries().push_back(e);
    v.Save();
    v.Clear();
    v.Load(true);
    Check(v.FindForArchive(L"C:\\important.rar", pw) && pw == L"exact-pass",
          "精确文件名优先于通配规则");
  }

  // 覆盖更新
  Check(v.Remember(L"C:\\data\\backup.7z", L"secret-1-new"), "更新已存在的记录");
  v.Clear();
  v.Load(true);
  Check(v.FindForArchive(L"C:\\data\\backup.7z", pw) && pw == L"secret-1-new", "更新后的密码生效");
  Check(v.Entries().size() == 4, "更新不会增加条目数");

  // 密文落盘检查：明文密码不能出现在文件里
  {
    HANDLE h = ::CreateFileW((dir + L"\\vault.dat").c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    Check(h != INVALID_HANDLE_VALUE, "库文件存在");
    if (h != INVALID_HANDLE_VALUE)
    {
      char buf[8192];
      DWORD got = 0;
      ::ReadFile(h, buf, sizeof(buf), &got, nullptr);
      ::CloseHandle(h);
      std::string raw(buf, got);
      Check(raw.find("secret-1-new") == std::string::npos, "密文里看不到明文密码");
      Check(raw.find("wild-pass") == std::string::npos, "密文里看不到通配条目密码");
      Check(got > 0 && raw.compare(0, 4, "7ZPV") == 0, "文件头是 7ZPV");
    }
  }

  // 篡改检测
  {
    HANDLE h = ::CreateFileW((dir + L"\\vault.dat").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
      LARGE_INTEGER pos;
      pos.QuadPart = -5;
      ::SetFilePointerEx(h, pos, nullptr, FILE_END);
      unsigned char c = 0;
      DWORD got = 0;
      ::ReadFile(h, &c, 1, &got, nullptr);
      c ^= 0xFF;
      ::SetFilePointerEx(h, pos, nullptr, FILE_END);
      ::WriteFile(h, &c, 1, &got, nullptr);
      ::CloseHandle(h);
    }
    v.Clear();
    const bool loaded = v.Load(true);
    Check(!loaded || v.Entries().size() != 4, "篡改密文后不会静默返回错误数据");
    if (!loaded)
      printf("       错误信息: %s\n", ToUtf8(v.LastError()).c_str());
  }

  ::SetEnvironmentVariableW(L"Z7CUSTOM_VAULT_DIR", nullptr);
  printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed);
  return g_failed ? 1 : 0;
}
