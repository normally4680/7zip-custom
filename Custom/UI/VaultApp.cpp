// Custom/UI/VaultApp.cpp
//
// 独立的密码库管理器（7zVault.exe）。
// 刻意做成"另一个 EXE"而不是 7-Zip 里的一个对话框：
//   * 不需要改 7-Zip 的菜单/资源/命令 ID
//   * 7-Zip 进程里完全不出现我们的 UI 代码
//   * 可以单独调试、单独发布

#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "../Core/PasswordVault.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' \
  name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
  processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

HINSTANCE g_inst = nullptr;
bool g_reveal = false;
custom::PasswordVault *g_vault = nullptr;

std::wstring GetDlgText(HWND dlg, int id)
{
  HWND h = ::GetDlgItem(dlg, id);
  const int n = ::GetWindowTextLengthW(h);
  if (n <= 0)
    return std::wstring();
  std::wstring s((size_t)n, L'\0');
  ::GetWindowTextW(h, &s[0], n + 1);
  return s;
}

void SetDlgText(HWND dlg, int id, const std::wstring &s)
{
  ::SetDlgItemTextW(dlg, id, s.c_str());
}

std::wstring MaskPassword(const std::wstring &pw)
{
  if (pw.empty())
    return std::wstring();
  return std::wstring(pw.size() > 12 ? 12 : pw.size(), L'\x2022');
}

std::wstring FormatTime(int64_t t)
{
  if (t <= 0)
    return std::wstring();
  __time64_t tt = (__time64_t)t;
  struct tm tmv;
  if (_localtime64_s(&tmv, &tt) != 0)
    return std::wstring();
  wchar_t buf[32];
  wcsftime(buf, 32, L"%Y-%m-%d %H:%M", &tmv);
  return buf;
}

void RefreshList(HWND dlg)
{
  HWND list = ::GetDlgItem(dlg, IDC_LIST);
  ::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(list);

  const auto &entries = g_vault->Entries();
  for (size_t i = 0; i < entries.size(); ++i)
  {
    const auto &e = entries[i];
    LVITEMW item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = (int)i;
    item.lParam = (LPARAM)i;
    item.pszText = const_cast<wchar_t *>(e.name.c_str());
    const int row = (int)::SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
    if (row < 0)
      continue;

    const std::wstring pw = g_reveal ? e.password : MaskPassword(e.password);
    ListView_SetItemText(list, row, 1, const_cast<wchar_t *>(pw.c_str()));
    ListView_SetItemText(list, row, 2, const_cast<wchar_t *>(e.note.c_str()));
    const std::wstring ts = FormatTime(e.lastUsed);
    ListView_SetItemText(list, row, 3, const_cast<wchar_t *>(ts.c_str()));
  }

  ::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
  ::InvalidateRect(list, nullptr, TRUE);

  wchar_t hint[512];
  const std::wstring dir = custom::VaultDir();
  if (!g_vault->LastError().empty())
    _snwprintf_s(hint, _countof(hint), _TRUNCATE, L"错误：%s", g_vault->LastError().c_str());
  else
    _snwprintf_s(hint, _countof(hint), _TRUNCATE, L"共 %u 条  |  %s",
                 (unsigned)entries.size(), dir.c_str());
  ::SetDlgItemTextW(dlg, IDC_HINT, hint);
}

int GetSelectedIndex(HWND dlg)
{
  HWND list = ::GetDlgItem(dlg, IDC_LIST);
  const int sel = (int)::SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
  if (sel < 0)
    return -1;
  LVITEMW item = {};
  item.mask = LVIF_PARAM;
  item.iItem = sel;
  if (!::SendMessageW(list, LVM_GETITEMW, 0, (LPARAM)&item))
    return -1;
  return (int)item.lParam;
}

struct EntryDlgData
{
  std::wstring name;
  std::wstring password;
  std::wstring note;
  bool ok = false;
};

INT_PTR CALLBACK EntryProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      auto *d = reinterpret_cast<EntryDlgData *>(lParam);
      ::SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
      SetDlgText(dlg, IDC_E_NAME, d->name);
      SetDlgText(dlg, IDC_E_PASS, d->password);
      SetDlgText(dlg, IDC_E_NOTE, d->note);
      ::SendDlgItemMessageW(dlg, IDC_E_NAME, EM_SETSEL, 0, -1);
      ::SetFocus(::GetDlgItem(dlg, IDC_E_NAME));
      return FALSE;
    }
    case WM_COMMAND:
      if (LOWORD(wParam) == IDOK)
      {
        auto *d = reinterpret_cast<EntryDlgData *>(::GetWindowLongPtrW(dlg, GWLP_USERDATA));
        if (d)
        {
          d->name = GetDlgText(dlg, IDC_E_NAME);
          d->password = GetDlgText(dlg, IDC_E_PASS);
          d->note = GetDlgText(dlg, IDC_E_NOTE);
          if (d->name.empty() || d->password.empty())
          {
            ::MessageBoxW(dlg, L"归档名称和密码都不能为空。", L"密码条目", MB_ICONINFORMATION);
            return TRUE;
          }
          d->ok = true;
        }
        ::EndDialog(dlg, IDOK);
        return TRUE;
      }
      if (LOWORD(wParam) == IDCANCEL)
      {
        ::EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
  }
  return FALSE;
}

void OnAdd(HWND dlg)
{
  EntryDlgData d;
  if (::DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ENTRY), dlg, EntryProc, (LPARAM)&d) != IDOK || !d.ok)
    return;

  custom::VaultEntry e;
  e.name = d.name;
  e.password = d.password;
  e.note = d.note;
  e.lastUsed = (int64_t)time(nullptr);
  g_vault->Entries().push_back(std::move(e));
  if (!g_vault->Save())
    ::MessageBoxW(dlg, g_vault->LastError().c_str(), L"保存失败", MB_ICONERROR);
  g_vault->Load(true);
  RefreshList(dlg);
}

void OnEdit(HWND dlg)
{
  const int idx = GetSelectedIndex(dlg);
  if (idx < 0 || (size_t)idx >= g_vault->Entries().size())
  {
    ::MessageBoxW(dlg, L"请先选中一条记录。", L"密码库", MB_ICONINFORMATION);
    return;
  }

  auto &entries = g_vault->Entries();
  EntryDlgData d;
  d.name = entries[idx].name;
  d.password = entries[idx].password;
  d.note = entries[idx].note;

  if (::DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ENTRY), dlg, EntryProc, (LPARAM)&d) != IDOK || !d.ok)
    return;

  entries[idx].name = d.name;
  entries[idx].password = d.password;
  entries[idx].note = d.note;
  entries[idx].lastUsed = (int64_t)time(nullptr);
  if (!g_vault->Save())
    ::MessageBoxW(dlg, g_vault->LastError().c_str(), L"保存失败", MB_ICONERROR);
  g_vault->Load(true);
  RefreshList(dlg);
}

void OnDelete(HWND dlg)
{
  const int idx = GetSelectedIndex(dlg);
  if (idx < 0 || (size_t)idx >= g_vault->Entries().size())
  {
    ::MessageBoxW(dlg, L"请先选中一条记录。", L"密码库", MB_ICONINFORMATION);
    return;
  }

  const std::wstring name = g_vault->Entries()[idx].name;
  if (::MessageBoxW(dlg, (L"确定删除 \"" + name + L"\" 的密码记录？").c_str(),
                    L"删除确认", MB_ICONWARNING | MB_YESNO) != IDYES)
    return;

  if (!g_vault->Erase((size_t)idx))
    ::MessageBoxW(dlg, g_vault->LastError().c_str(), L"保存失败", MB_ICONERROR);
  g_vault->Load(true);
  RefreshList(dlg);
}

INT_PTR CALLBACK VaultProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      HWND list = ::GetDlgItem(dlg, IDC_LIST);
      ::SetWindowLongPtrW(list, GWL_EXSTYLE,
          ::GetWindowLongPtrW(list, GWL_EXSTYLE) | WS_EX_CLIENTEDGE);

      struct Col { const wchar_t *title; int width; } cols[] = {
        { L"归档名称", 120 },
        { L"密码",      90 },
        { L"备注",     110 },
        { L"上次使用", 120 },
      };
      for (int i = 0; i < 4; ++i)
      {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t *>(cols[i].title);
        c.cx = cols[i].width;
        c.iSubItem = i;
        ::SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&c);
      }
      ::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE,
          LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

      g_vault->Load(true);
      RefreshList(dlg);
      return TRUE;
    }

    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_ADD: OnAdd(dlg); return TRUE;
        case IDC_EDIT: OnEdit(dlg); return TRUE;
        case IDC_DEL: OnDelete(dlg); return TRUE;
        case IDC_REVEAL:
          g_reveal = (::IsDlgButtonChecked(dlg, IDC_REVEAL) == BST_CHECKED);
          RefreshList(dlg);
          return TRUE;
        case IDC_CLOSE:
        case IDCANCEL:
          ::EndDialog(dlg, 0);
          return TRUE;
      }
      break;

    case WM_NOTIFY:
    {
      auto *nm = reinterpret_cast<NMHDR *>(lParam);
      if (nm->idFrom == IDC_LIST && nm->code == NM_DBLCLK)
      {
        OnEdit(dlg);
        return TRUE;
      }
      break;
    }
  }
  return FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
  g_inst = inst;

  INITCOMMONCONTROLSEX icc = {};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
  ::InitCommonControlsEx(&icc);

  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  custom::PasswordVault &vault = custom::PasswordVault::Instance();
  g_vault = &vault;
  if (!vault.Load(true) && !vault.LastError().empty())
  {
    ::MessageBoxW(nullptr, vault.LastError().c_str(), L"密码库", MB_ICONERROR);
  }

  ::DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_VAULT), nullptr, VaultProc, 0);
  ::CoUninitialize();
  return 0;
}
