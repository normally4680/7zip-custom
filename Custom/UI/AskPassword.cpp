// Custom/UI/AskPassword.cpp
//
// 自绘的密码输入框：左边能选密码库里已有的密码，右边输入框可以改，
// 底下勾选是否把这个包的密码存进库（默认不勾 —— 不再无差别记录每个压缩包）。

#include "AskPassword.h"
#include "resource.h"
#include "VaultDialog.h"

#include <commctrl.h>

#include <vector>

#include "../Core/PasswordVault.h"

namespace {

HINSTANCE g_inst = nullptr;
HFONT g_eyeFont = nullptr;
std::wstring g_archive;
std::wstring g_result;
int g_resultCode = 0;

std::wstring MaskPw(const std::wstring &pw)
{
  if (pw.empty())
    return std::wstring();
  return std::wstring(pw.size() > 10 ? 10 : pw.size(), L'\x2022');
}

std::wstring BaseName(const std::wstring &path)
{
  const size_t pos = path.find_last_of(L"\\/");
  return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

void FillCombo(HWND dlg)
{
  HWND combo = ::GetDlgItem(dlg, IDC_ASK_LIST);
  const int keep = (int)::SendMessageW(combo, CB_GETCURSEL, 0, 0);

  ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);

  custom::PasswordVault &vault = custom::PasswordVault::Instance();
  vault.Load(false);

  int sel = -1;
  const std::wstring base = BaseName(g_archive);
  const auto &entries = vault.Entries();
  for (size_t i = 0; i < entries.size(); ++i)
  {
    const auto &e = entries[i];
    std::wstring text = e.name + L"    " + MaskPw(e.password);
    if (!e.note.empty())
      text += L"    (" + e.note + L")";
    const int idx = (int)::SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
    if (idx >= 0)
      ::SendMessageW(combo, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);

    if (sel < 0 && !base.empty() && _wcsicmp(e.name.c_str(), base.c_str()) == 0)
      sel = idx;
  }

  if (keep >= 0)
    ::SendMessageW(combo, CB_SETCURSEL, (WPARAM)keep, 0);
  else if (sel >= 0)
    ::SendMessageW(combo, CB_SETCURSEL, (WPARAM)sel, 0);
}

void SetEyeFont(HWND dlg)
{
  HDC hdc = ::GetDC(dlg);
  const int h = -::MulDiv(11, ::GetDeviceCaps(hdc, LOGPIXELSY), 72);
  ::ReleaseDC(dlg, hdc);
  if (g_eyeFont)
    ::DeleteObject(g_eyeFont);
  g_eyeFont = ::CreateFontW(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH, L"Segoe MDL2 Assets");
  if (g_eyeFont)
    ::SendDlgItemMessageW(dlg, IDC_ASK_EYE, WM_SETFONT, (WPARAM)g_eyeFont, TRUE);
  ::SetDlgItemTextW(dlg, IDC_ASK_EYE, L"\xE890");
}

void CenterWindowOn(HWND wnd, HWND ref)
{
  RECT rr = {}, rw = {};
  if (!::GetWindowRect(ref, &rr) || !::GetWindowRect(wnd, &rw))
    return;
  const int w = rw.right - rw.left;
  const int h = rw.bottom - rw.top;
  int x = rr.left + ((rr.right - rr.left) - w) / 2;
  int y = rr.top + ((rr.bottom - rr.top) - h) / 2;

  RECT wa = {};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  if (x < wa.left) x = wa.left;
  if (y < wa.top) y = wa.top;
  if (x + w > wa.right) x = wa.right - w;
  if (y + h > wa.bottom) y = wa.bottom - h;

  ::SetWindowPos(wnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// 7zG 的进度窗口默认是在屏幕居中的，和 7zFM 不贴；这里先把它挪到 7zFM 中间，
// 我们的密码框再居中到进度窗口上，视觉上就一致了。
void AlignProgressOnMain(HWND progress)
{
  if (!progress || !::IsWindow(progress))
    return;
  HWND main = ::FindWindowW(L"7-Zip::FM", nullptr);
  if (!main || main == progress || !::IsWindow(main))
    return;
  CenterWindowOn(progress, main);
}

struct FindCtx
{
  HWND main;
  HWND best;
};

BOOL CALLBACK FindProgressProc(HWND h, LPARAM lp)
{
  auto *ctx = reinterpret_cast<FindCtx *>(lp);
  if (h == ctx->main || !::IsWindowVisible(h))
    return TRUE;

  DWORD pid = 0;
  ::GetWindowThreadProcessId(h, &pid);
  if (pid != ::GetCurrentProcessId())
    return TRUE;

  RECT r = {}, rm = {};
  if (!::GetWindowRect(h, &r) || !::GetWindowRect(ctx->main, &rm))
    return TRUE;

  // 进度窗口一定比主窗口小
  if ((r.right - r.left) < (rm.right - rm.left))
  {
    ctx->best = h;
    return FALSE;
  }
  return TRUE;
}

// 在 7zFM 进程里找出"正在打开/解压"的那个进度窗口
HWND FindProgressWindow()
{
  HWND main = ::FindWindowW(L"7-Zip::FM", nullptr);
  if (!main)
    return nullptr;
  FindCtx ctx = { main, nullptr };
  ::EnumWindows(FindProgressProc, (LPARAM)&ctx);
  return ctx.best;
}

void CenterOnOwner(HWND dlg)
{
  HWND owner = ::GetParent(dlg);
  RECT ro = {}, rc = {};
  ::GetWindowRect(dlg, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;

  int x, y;
  if (owner && ::IsWindow(owner) && ::GetWindowRect(owner, &ro))
  {
    x = ro.left + ((ro.right - ro.left) - w) / 2;
    y = ro.top + ((ro.bottom - ro.top) - h) / 2;
  }
  else
  {
    RECT wa = {};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    x = wa.left + ((wa.right - wa.left) - w) / 2;
    y = wa.top + ((wa.bottom - wa.top) - h) / 2;
  }

  // 别跑到屏幕外面去
  RECT wa = {};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
  if (x < wa.left) x = wa.left;
  if (y < wa.top) y = wa.top;
  if (x + w > wa.right) x = wa.right - w;
  if (y + h > wa.bottom) y = wa.bottom - h;

  ::SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK AskProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      ::SetDlgItemTextW(dlg, IDC_ASK_ARC, BaseName(g_archive).c_str());
      CenterOnOwner(dlg);
      ::SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      SetEyeFont(dlg);
      FillCombo(dlg);

      // 选中的条目直接把密码填进输入框，用户不用再抄一遍
      const int sel = (int)::SendDlgItemMessageW(dlg, IDC_ASK_LIST, CB_GETCURSEL, 0, 0);
      if (sel >= 0)
      {
        const LRESULT data = ::SendDlgItemMessageW(dlg, IDC_ASK_LIST, CB_GETITEMDATA, (WPARAM)sel, 0);
        custom::PasswordVault &vault = custom::PasswordVault::Instance();
        if (data >= 0 && (size_t)data < vault.Entries().size())
          ::SetDlgItemTextW(dlg, IDC_ASK_PASS, vault.Entries()[(size_t)data].password.c_str());
      }

      ::SendDlgItemMessageW(dlg, IDC_ASK_PASS, EM_SETSEL, 0, -1);
      ::SetFocus(::GetDlgItem(dlg, IDC_ASK_PASS));
      return FALSE;
    }

    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_ASK_LIST:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            const int sel = (int)::SendDlgItemMessageW(dlg, IDC_ASK_LIST, CB_GETCURSEL, 0, 0);
            if (sel >= 0)
            {
              const LRESULT data = ::SendDlgItemMessageW(dlg, IDC_ASK_LIST, CB_GETITEMDATA, (WPARAM)sel, 0);
              custom::PasswordVault &vault = custom::PasswordVault::Instance();
              if (data >= 0 && (size_t)data < vault.Entries().size())
              {
                ::SetDlgItemTextW(dlg, IDC_ASK_PASS, vault.Entries()[(size_t)data].password.c_str());
                ::SendDlgItemMessageW(dlg, IDC_ASK_PASS, EM_SETSEL, 0, -1);
                ::SetFocus(::GetDlgItem(dlg, IDC_ASK_PASS));
              }
            }
          }
          return TRUE;

        case IDC_ASK_EYE:
        {
          const bool show = (::IsDlgButtonChecked(dlg, IDC_ASK_EYE) == BST_CHECKED);
          const WPARAM ch = show ? 0 : (WPARAM)0x25CF;
          ::SendDlgItemMessageW(dlg, IDC_ASK_PASS, EM_SETPASSWORDCHAR, ch, 0);
          ::InvalidateRect(::GetDlgItem(dlg, IDC_ASK_PASS), nullptr, TRUE);
          return TRUE;
        }

        case IDC_ASK_MANAGE:
          ShowVaultDialogBox(g_inst, dlg, 0, nullptr);
          FillCombo(dlg);
          return TRUE;

        case IDOK:
        {
          wchar_t buf[1024] = {};
          ::GetDlgItemTextW(dlg, IDC_ASK_PASS, buf, _countof(buf));
          if (!buf[0])
          {
            ::MessageBoxW(dlg, L"密码不能为空。", L"输入密码", MB_ICONINFORMATION);
            return TRUE;
          }
          g_result = buf;
          ::SecureZeroMemory(buf, sizeof(buf));

          if (::IsDlgButtonChecked(dlg, IDC_ASK_SAVE) == BST_CHECKED)
          {
            custom::PasswordVault &vault = custom::PasswordVault::Instance();
            vault.Load(false);
            if (!vault.Remember(g_archive, g_result))
            {
              ::MessageBoxW(dlg, L"密码保存失败，但这次解压会继续。",
                            L"密码库", MB_ICONWARNING);
            }
          }

          g_resultCode = 1;
          ::EndDialog(dlg, IDOK);
          return TRUE;
        }

        case IDCANCEL:
          g_resultCode = -1;
          ::EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      break;

    case WM_DESTROY:
      if (g_eyeFont)
      {
        ::DeleteObject(g_eyeFont);
        g_eyeFont = nullptr;
      }
      break;
  }
  return FALSE;
}

} // namespace

int AskPasswordDialog(HINSTANCE inst, HWND owner, const wchar_t *archivePath,
                      std::wstring &outPassword)
{
  g_inst = inst;

  // owner 不是进度窗口时（打开归档这条路径传的是主窗口），自己找一下进度窗口
  HWND progress = owner;
  if (!progress || progress == ::FindWindowW(L"7-Zip::FM", nullptr))
  {
    HWND found = FindProgressWindow();
    if (found)
      progress = found;
  }
  AlignProgressOnMain(progress);
  if (progress)
    owner = progress;   // 让密码框以进度窗口为参照居中
  g_archive = (archivePath ? archivePath : L"");
  g_result.clear();
  g_resultCode = 0;

  INITCOMMONCONTROLSEX icc = {};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES;
  ::InitCommonControlsEx(&icc);

  // 弹框期间把本进程所有其它窗口（主窗口、进度窗口）都禁用，
  // 免得进度窗口还能被点、被前置。DialogBoxParam 只会处理同线程的 owner。
  std::vector<HWND> disabled;
  {
    struct Ctx { std::vector<HWND> *list; } ctx = { &disabled };
    ::EnumWindows([](HWND h, LPARAM lp) -> BOOL
    {
      auto *c = reinterpret_cast<Ctx *>(lp);
      DWORD pid = 0;
      ::GetWindowThreadProcessId(h, &pid);
      if (pid != ::GetCurrentProcessId())
        return TRUE;
      if (!::IsWindowVisible(h) || !::IsWindowEnabled(h))
        return TRUE;
      if (::GetWindow(h, GW_OWNER) == nullptr && ::GetWindow(h, GW_CHILD) == nullptr)
        return TRUE;
      ::EnableWindow(h, FALSE);
      c->list->push_back(h);
      return TRUE;
    }, (LPARAM)&ctx);
  }

  // 跨进程的进度窗口单独禁用
  const bool crossProc = owner && ::IsWindow(owner) &&
      ::GetWindowThreadProcessId(owner, nullptr) != ::GetCurrentThreadId();
  if (crossProc)
    ::EnableWindow(owner, FALSE);

  const INT_PTR r = ::DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_ASKPWD), owner, AskProc, 0);

  for (HWND h : disabled)
    if (::IsWindow(h))
      ::EnableWindow(h, TRUE);
  if (crossProc && ::IsWindow(owner))
  {
    ::EnableWindow(owner, TRUE);
    ::SetActiveWindow(owner);
  }
  if (r != IDOK || g_resultCode != 1)
  {
    outPassword.clear();
    return (g_resultCode == -1) ? -1 : 0;
  }

  outPassword = g_result;
  if (!g_result.empty())
    ::SecureZeroMemory(&g_result[0], g_result.size() * sizeof(wchar_t));
  g_result.clear();
  return 1;
}
