// RegistryAssociations.cpp

#include "StdAfx.h"

#include "../../../Common/IntToString.h"
#include "../../../Common/StringConvert.h"
#include "../../../Common/StringToInt.h"

#include "../../../Windows/Registry.h"

#include "RegistryAssociations.h"

using namespace NWindows;
using namespace NRegistry;

namespace NRegistryAssoc {

// static NSynchronization::CCriticalSection g_CriticalSection;

static const TCHAR * const kClasses = TEXT("Software\\Classes\\");
// static const TCHAR * const kShellNewKeyName = TEXT("ShellNew");
// static const TCHAR * const kShellNewDataValueName = TEXT("Data");
static const TCHAR * const kDefaultIconKeyName = TEXT("DefaultIcon");
static const TCHAR * const kShellKeyName = TEXT("shell");
static const TCHAR * const kOpenKeyName = TEXT("open");
static const TCHAR * const kCommandKeyName = TEXT("command");
static const char * const k7zipPrefix = "7-Zip.";

static CSysString GetExtProgramKeyName(const CSysString &ext)
{
  return CSysString(k7zipPrefix) + ext;
}

static CSysString GetFullKeyPath(HKEY hkey, const CSysString &name)
{
  CSysString s;
  if (hkey != HKEY_CLASSES_ROOT)
    s = kClasses;
  return s + name;
}

static CSysString GetExtKeyPath(HKEY hkey, const CSysString &ext)
{
  return GetFullKeyPath(hkey, (TEXT(".")) + ext);
}

// 7zip-custom: register the application itself (per hkey) so Explorer offers
// "Always use this app" in the Open With dialog. Without this, Windows only
// shows "Just once" for an unpackaged portable exe.
static void RegisterApplicationInfo(HKEY hkey, const CSysString &ext,
    const UString &programOpenCommand, const UString &iconPath, int iconIndex)
{
  CKey apps;
  if (apps.Create(hkey, GetFullKeyPath(hkey, TEXT("Applications"))) != ERROR_SUCCESS)
    return;

  CKey app;
  if (app.Create(apps, TEXT("7zFM.exe")) != ERROR_SUCCESS)
    return;

  app.SetValue(TEXT("FriendlyAppName"), TEXT("7-Zip File Manager"));

  {
    CKey iconKey;
    if (iconKey.Create(app, kDefaultIconKeyName) == ERROR_SUCCESS)
    {
      UString iconFull = iconPath;
      iconFull.Add_Char(',');
      iconFull.Add_UInt32(iconIndex < 0 ? 0 : (UInt32)iconIndex);
      iconKey.SetValue(NULL, iconFull);
    }
  }

  {
    CKey shellKey, openKey, commandKey;
    if (shellKey.Create(app, kShellKeyName) == ERROR_SUCCESS &&
        openKey.Create(shellKey, kOpenKeyName) == ERROR_SUCCESS &&
        commandKey.Create(openKey, kCommandKeyName) == ERROR_SUCCESS)
      commandKey.SetValue(NULL, programOpenCommand);
  }

  {
    CKey typesKey;
    if (typesKey.Create(app, TEXT("SupportedTypes")) == ERROR_SUCCESS)
    {
      CSysString dotted (TEXT("."));
      dotted += ext;
      typesKey.SetValue(dotted, TEXT(""));
    }
  }

  // Register the app in Windows "Default apps" (Capabilities) as well.
  // Windows 10/11 uses this list to offer a persistent handler.
  if (hkey == HKEY_CURRENT_USER || hkey == HKEY_LOCAL_MACHINE)
  {
    const bool perUser = (hkey == HKEY_CURRENT_USER);
    const TCHAR *capsPath = perUser ? TEXT("Software\\7-Zip\\Capabilities")
                                    : TEXT("SOFTWARE\\7-Zip\\Capabilities");
    const TCHAR *registeredPath = perUser ? TEXT("Software\\RegisteredApplications")
                                          : TEXT("SOFTWARE\\RegisteredApplications");

    CKey caps;
    if (caps.Create(hkey, capsPath) == ERROR_SUCCESS)
    {
      caps.SetValue(TEXT("ApplicationName"), TEXT("7-Zip File Manager"));
      caps.SetValue(TEXT("ApplicationDescription"), TEXT("7-Zip File Manager"));
      CKey fileAssoc;
      if (fileAssoc.Create(caps, TEXT("FileAssociations")) == ERROR_SUCCESS)
      {
        CSysString dotted (TEXT("."));
        dotted += ext;
        fileAssoc.SetValue(dotted, GetExtProgramKeyName(ext));
      }
    }

    CKey registered;
    if (registered.Create(hkey, registeredPath) == ERROR_SUCCESS)
      registered.SetValue(TEXT("7-Zip"), capsPath);
  }
}

static void UnregisterApplicationInfo(HKEY hkey, const CSysString &ext)
{
  CKey typesKey;
  if (typesKey.Open(hkey, GetFullKeyPath(hkey, TEXT("Applications\\7zFM.exe\\SupportedTypes")),
                    KEY_SET_VALUE) == ERROR_SUCCESS)
  {
    CSysString dotted (TEXT("."));
    dotted += ext;
    typesKey.DeleteValue(dotted);
  }

  if (hkey == HKEY_CURRENT_USER || hkey == HKEY_LOCAL_MACHINE)
  {
    const bool perUser = (hkey == HKEY_CURRENT_USER);
    const TCHAR *fileAssocPath = perUser
        ? TEXT("Software\\7-Zip\\Capabilities\\FileAssociations")
        : TEXT("SOFTWARE\\7-Zip\\Capabilities\\FileAssociations");
    CKey fileAssoc;
    if (fileAssoc.Open(hkey, fileAssocPath, KEY_SET_VALUE) == ERROR_SUCCESS)
    {
      CSysString dotted (TEXT("."));
      dotted += ext;
      fileAssoc.DeleteValue(dotted);
    }
  }
}

bool CShellExtInfo::ReadFromRegistry(HKEY hkey, const CSysString &ext)
{
  ProgramKey.Empty();
  IconPath.Empty();
  IconIndex = -1;
  // NSynchronization::CCriticalSectionLock lock(g_CriticalSection);
  {
    CKey extKey;
    if (extKey.Open(hkey, GetExtKeyPath(hkey, ext), KEY_READ) != ERROR_SUCCESS)
      return false;
    if (extKey.QueryValue(NULL, ProgramKey) != ERROR_SUCCESS)
      return false;
  }
  {
    CKey iconKey;
 
    if (iconKey.Open(hkey, GetFullKeyPath(hkey, ProgramKey + CSysString(CHAR_PATH_SEPARATOR) + kDefaultIconKeyName), KEY_READ) == ERROR_SUCCESS)
    {
      UString value;
      if (iconKey.QueryValue(NULL, value) == ERROR_SUCCESS)
      {
        const int pos = value.ReverseFind(L',');
        IconPath = value;
        if (pos >= 0)
        {
          const wchar_t *end;
          const Int32 index = ConvertStringToInt32((const wchar_t *)value + pos + 1, &end);
          if (*end == 0)
          {
            // 9.31: if there is no icon index, we use -1. Is it OK?
            if (pos != (int)value.Len() - 1)
              IconIndex = (int)index;
            IconPath.SetFrom(value, (unsigned)pos);
          }
        }
      }
    }
  }
  return true;
}

bool CShellExtInfo::IsIt7Zip() const
{
  return ProgramKey.IsPrefixedBy_Ascii_NoCase(k7zipPrefix);
}

LONG DeleteShellExtensionInfo(HKEY hkey, const CSysString &ext)
{
  // NSynchronization::CCriticalSectionLock lock(g_CriticalSection);
  CKey rootKey;
  rootKey.Attach(hkey);
  LONG res = rootKey.RecurseDeleteKey(GetExtKeyPath(hkey, ext));
  // then we delete only 7-Zip.* key.
  rootKey.RecurseDeleteKey(GetFullKeyPath(hkey, GetExtProgramKeyName(ext)));
  // 7zip-custom: also drop the legacy shared ProgID ("7-Zip.*") that older
  // builds created for formats without an icon mapping.
  rootKey.RecurseDeleteKey(GetFullKeyPath(hkey, GetExtProgramKeyName(TEXT("*"))));
  rootKey.Detach();
  UnregisterApplicationInfo(hkey, ext);
  return res;
}

LONG AddShellExtensionInfo(HKEY hkey,
    const CSysString &ext,
    const UString &programTitle,
    const UString &programOpenCommand,
    const UString &iconPath, int iconIndex
    // , const void *shellNewData, int shellNewDataSize
    )
{
  LONG res = 0;
  DeleteShellExtensionInfo(hkey, ext);
  // NSynchronization::CCriticalSectionLock lock(g_CriticalSection);
  // 7zip-custom: always use a per-extension ProgID. The upstream code shared
  // "7-Zip.*" for formats without an icon mapping, which left a stale friendly
  // name (".rar" showing "apfs Archive") after upgrades.
  const CSysString programKeyName = GetExtProgramKeyName(ext);
  {
    CKey extKey;
    res = extKey.Create(hkey, GetExtKeyPath(hkey, ext));
    extKey.SetValue(NULL, programKeyName);
    /*
    if (shellNewData != NULL)
    {
      CKey shellNewKey;
      shellNewKey.Create(extKey, kShellNewKeyName);
      shellNewKey.SetValue(kShellNewDataValueName, shellNewData, shellNewDataSize);
    }
    */
  }
  CKey programKey;
  programKey.Create(hkey, GetFullKeyPath(hkey, programKeyName));
  programKey.SetValue(NULL, programTitle);
  {
    CKey iconKey;
    UString iconPathFull = iconPath;
    if (iconIndex < 0)
      iconIndex = 0;
    // if (iconIndex >= 0)
    {
      iconPathFull.Add_Char(',');
      iconPathFull.Add_UInt32((UInt32)iconIndex);
    }
    iconKey.Create(programKey, kDefaultIconKeyName);
    iconKey.SetValue(NULL, iconPathFull);
  }

  CKey shellKey;
  shellKey.Create(programKey, kShellKeyName);
  shellKey.SetValue(NULL, TEXT(""));

  CKey openKey;
  openKey.Create(shellKey, kOpenKeyName);
  openKey.SetValue(NULL, TEXT(""));
  
  CKey commandKey;
  commandKey.Create(openKey, kCommandKeyName);
  commandKey.SetValue(NULL, programOpenCommand);

  RegisterApplicationInfo(hkey, ext, programOpenCommand, iconPath, iconIndex);
  return res;
}

}
