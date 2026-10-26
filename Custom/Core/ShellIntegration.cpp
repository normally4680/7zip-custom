// Custom/Core/ShellIntegration.cpp

#include "ShellIntegration.h"

#include <windows.h>
#include <shlobj.h>

#include <vector>

namespace custom {

namespace {

const wchar_t *kClsid = L"{23170F69-40C1-278A-1000-000100020000}";
const wchar_t *kShellExtName = L"7-Zip Shell Extension";
const wchar_t *kApproved = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";
const wchar_t *kClasses = L"Software\\Classes\\";

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

std::wstring RegErrorText(LSTATUS st)
{
  wchar_t *msg = nullptr;
  const DWORD n = ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, (DWORD)st, 0, (LPWSTR)&msg, 0, nullptr);
  std::wstring text;
  if (n && msg)
  {
    text.assign(msg, n);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
      text.pop_back();
  }
  if (msg)
    ::LocalFree(msg);
  if (text.empty())
  {
    wchar_t b[32];
    swprintf_s(b, L"err %ld", (long)st);
    text = b;
  }
  return text;
}

bool SetString(HKEY root, const std::wstring &subKey, const wchar_t *valueName,
               const std::wstring &value, std::wstring &error)
{
  HKEY key = nullptr;
  LSTATUS st = ::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_WRITE,
                                 nullptr, &key, nullptr);
  if (st == ERROR_SUCCESS)
  {
    st = ::RegSetValueExW(key, valueName, 0, REG_SZ,
                          (const BYTE *)value.c_str(),
                          (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(key);
  }
  if (st != ERROR_SUCCESS)
  {
    error = L"写入注册表失败：";
    error += subKey;
    error += L"\n";
    error += RegErrorText(st);
    return false;
  }
  return true;
}

std::wstring GetString(HKEY root, const std::wstring &subKey, const wchar_t *valueName)
{
  DWORD size = 0;
  if (::RegGetValueW(root, subKey.c_str(), valueName, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t))
    return std::wstring();
  std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, L'\0');
  if (::RegGetValueW(root, subKey.c_str(), valueName, RRF_RT_REG_SZ, nullptr,
                     buf.data(), &size) != ERROR_SUCCESS)
    return std::wstring();
  return std::wstring(buf.data());
}

std::wstring ClsidPath()
{
  return std::wstring(kClasses) + L"CLSID\\" + kClsid;
}

std::wstring HandlerPath(const wchar_t *type)
{
  return std::wstring(kClasses) + type + L"\\shellex\\ContextMenuHandlers\\7-Zip";
}

void DeleteKeyTree(const std::wstring &subKey)
{
  ::RegDeleteTreeW(HKEY_CURRENT_USER, subKey.c_str());
}

void NotifyShell()
{
  ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

} // namespace

bool IsShellContextMenuRegistered(std::wstring &registeredDll)
{
  registeredDll.clear();
  const std::wstring dll = ModuleDir() + L"7-zip.dll";
  if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
    return false;

  const std::wstring inproc = GetString(HKEY_CURRENT_USER,
      ClsidPath() + L"\\InprocServer32", nullptr);
  if (_wcsicmp(inproc.c_str(), dll.c_str()) != 0)
    return false;

  const std::wstring handler = GetString(HKEY_CURRENT_USER, HandlerPath(L"*"), nullptr);
  if (_wcsicmp(handler.c_str(), kClsid) != 0)
    return false;

  registeredDll = inproc;
  return true;
}

bool RegisterShellContextMenu(std::wstring &error)
{
  error.clear();
  const std::wstring dll = ModuleDir() + L"7-zip.dll";
  if (::GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES)
  {
    error = L"找不到 7-zip.dll。\n请确认它和 7zFM.exe 放在同一个目录。";
    return false;
  }

  if (!SetString(HKEY_CURRENT_USER, ClsidPath(), nullptr, kShellExtName, error))
    return false;

  const std::wstring inproc = ClsidPath() + L"\\InprocServer32";
  if (!SetString(HKEY_CURRENT_USER, inproc, nullptr, dll, error))
    return false;
  if (!SetString(HKEY_CURRENT_USER, inproc, L"ThreadingModel", L"Apartment", error))
    return false;

  if (!SetString(HKEY_CURRENT_USER, HandlerPath(L"*"), nullptr, kClsid, error))
    return false;
  if (!SetString(HKEY_CURRENT_USER, HandlerPath(L"Folder"), nullptr, kClsid, error))
    return false;
  if (!SetString(HKEY_CURRENT_USER, HandlerPath(L"Directory"), nullptr, kClsid, error))
    return false;

  // Optional approved list; failure here does not break the shell extension.
  std::wstring ignored;
  SetString(HKEY_CURRENT_USER, kApproved, kClsid, kShellExtName, ignored);

  NotifyShell();
  return true;
}

bool UnregisterShellContextMenu(std::wstring &error)
{
  error.clear();
  DeleteKeyTree(ClsidPath());
  DeleteKeyTree(HandlerPath(L"*"));
  DeleteKeyTree(HandlerPath(L"Folder"));
  DeleteKeyTree(HandlerPath(L"Directory"));

  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, kApproved, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
  {
    ::RegDeleteValueW(key, kClsid);
    ::RegCloseKey(key);
  }

  NotifyShell();
  return true;
}

} // namespace custom
