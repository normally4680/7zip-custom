// Custom/Tests/UiProbe.cpp
//
// 菜单注入的自测：自己造一个带菜单的窗口，调用 CustomHook_AttachMainMenu，
// 再把菜单栏读回来检查。不依赖 7zFM 是否在跑，也不受窗口站隔离影响。
//
// 检查点：
//   1. "密码库(&P)" 出现在"帮助"之前
//   2. 子菜单三项齐全
//   3. 重复调用不会插出两个"密码库"

#include <windows.h>

#include <cstdio>
#include <string>

extern "C" int __cdecl CustomHook_AttachMainMenu(void *hWnd);

namespace {

int g_failed = 0;

std::string Utf8(const std::wstring &w)
{
  if (w.empty())
    return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

std::wstring MenuText(HMENU menu, int pos)
{
  wchar_t buf[256] = {};
  ::GetMenuStringW(menu, (UINT)pos, buf, 256, MF_BYPOSITION);
  return buf;
}

void Check(bool cond, const char *what)
{
  printf("%s  %s\n", cond ? "[ ok ]" : "[FAIL]", what);
  if (!cond)
    ++g_failed;
}

int CountVaultMenus(HMENU main)
{
  int n = 0;
  const int count = ::GetMenuItemCount(main);
  for (int i = 0; i < count; ++i)
  {
    const std::wstring t = MenuText(main, i);
    if (t.find(L"\u5bc6\u7801\u5e93") != std::wstring::npos)
      ++n;
  }
  return n;
}

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

INT_PTR CALLBACK StubDlgProc(HWND, UINT, WPARAM, LPARAM) { return FALSE; }

} // namespace

int wmain()
{
  ::SetConsoleOutputCP(CP_UTF8);

  // 造一个和 7zFM 结构近似的菜单：文件 / 编辑 / 查看 / 书签 / 工具 / 帮助
  HMENU main = ::CreateMenu();
  const wchar_t *titles[] = { L"文件(&F)", L"编辑(&E)", L"查看(&V)", L"书签(&A)", L"工具(&T)", L"帮助(&H)" };
  for (int i = 0; i < 6; ++i)
  {
    HMENU sub = ::CreatePopupMenu();
    ::AppendMenuW(sub, MF_STRING, 1000 + i, L"dummy");
    ::AppendMenuW(main, MF_POPUP | MF_STRING, (UINT_PTR)sub, titles[i]);
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = ::DefWindowProcW;
  wc.hInstance = ::GetModuleHandleW(nullptr);
  wc.lpszClassName = L"CustomUiProbeWnd";
  ::RegisterClassExW(&wc);

  HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW,
                                0, 0, 400, 300, nullptr, main, wc.hInstance, nullptr);
  if (!hwnd)
  {
    printf("CreateWindow failed: %lu\n", (unsigned long)::GetLastError());
    return 1;
  }

  const int r1 = CustomHook_AttachMainMenu(hwnd);
  Check(r1 == 1, "hook 返回 1（挂载成功）");

  HMENU m = ::GetMenu(hwnd);
  const int count = ::GetMenuItemCount(m);
  printf("菜单栏共 %d 项：\n", count);
  int vaultPos = -1;
  for (int i = 0; i < count; ++i)
  {
    const std::wstring t = MenuText(m, i);
    printf("  [%d] %s\n", i, Utf8(t).c_str());
    if (t.find(L"\u5bc6\u7801\u5e93") != std::wstring::npos)
      vaultPos = i;
  }

  Check(count == 7, "菜单栏从 6 项变成 7 项");
  Check(vaultPos == 5, "\"密码库\"插在\"帮助\"之前");
  if (vaultPos >= 0)
  {
    HMENU sub = ::GetSubMenu(m, vaultPos);
    const int n = ::GetMenuItemCount(sub);
    printf("子菜单 %d 项：\n", n);
    for (int i = 0; i < n; ++i)
      printf("    - %s\n", Utf8(MenuText(sub, i)).c_str());
    Check(n == 4, "子菜单 4 项（含分隔线）");
  }

  // 幂等：再调一次不应该出现第二个"密码库"
  const int r2 = CustomHook_AttachMainMenu(hwnd);
  Check(r2 == 1, "重复调用仍返回 1");
  Check(CountVaultMenus(::GetMenu(hwnd)) == 1, "重复调用不会插入第二个菜单");

  ::DestroyWindow(hwnd);

  // ---- 密码条目对话框：确认眼睛按钮存在 ----
  {
    const std::wstring dllPath = ModuleDir() + L"7zVaultUi.dll";
    HMODULE h = ::LoadLibraryW(dllPath.c_str());
    Check(h != nullptr, "能加载 7zVaultUi.dll");
    if (h)
    {
      HWND dlg = ::CreateDialogParamW(h, MAKEINTRESOURCEW(102), nullptr, StubDlgProc, 0);
      Check(dlg != nullptr, "能创建密码条目对话框");
      if (dlg)
      {
        Check(::GetDlgItem(dlg, 1102) != nullptr, "密码输入框存在");
        Check(::GetDlgItem(dlg, 1106) != nullptr, "密码框右侧的眼睛按钮存在");
        ::DestroyWindow(dlg);
      }
      ::FreeLibrary(h);
    }
  }

  printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "ALL PASSED", g_failed);
  return g_failed ? 1 : 0;
}
