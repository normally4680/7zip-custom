// Custom/Core/SevenZipInProc.cpp
//
// 在 7zFM 进程内直接用 7z.dll 解压（不启动 7z.exe）。
//
// 为什么不用命令行：
//   * 子进程要自己找 7z.dll，某些环境会拒绝加载（Codec Load Error 80070005）
//   * 安全软件可能直接拦子进程、拦命令行参数
//   * 不需要把密码放进命令行
// 7zFM 打开归档时 7z.dll 已经映射进本进程了，直接复用即可。

#include "SevenZipInProc.h"
#include "ImagePreview.h"   // IsSafeInnerPath：复用同一套条目路径校验

#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>
#include <cwctype>

#include "../../CPP/7zip/Archive/IArchive.h"
#include "../../CPP/7zip/IPassword.h"
#include "../../CPP/7zip/IStream.h"
#include "../../CPP/7zip/PropID.h"
#include "../../CPP/Common/MyWindows.h"
#include "../../CPP/Common/MyCom.h"
#include "../../CPP/Common/MyVector.h"
#include "../../CPP/Windows/PropVariant.h"

namespace custom {

namespace {

// 逐级向上找已存在的祖先，再逐级向下建。
// 不用 SHCreateDirectoryExW（它在这类环境里对多级路径会莫名返回 PATH_NOT_FOUND）。
DWORD CreateDirsRecursive(std::wstring path)
{
  for (auto &c : path)
    if (c == L'/')
      c = L'\\';                       // 正斜杠统一成反斜杠

  while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
    path.pop_back();
  if (path.empty())
    return 0;

  const bool dbg = ::GetEnvironmentVariableW(L"Z7CUSTOM_DEBUG", nullptr, 0) > 0;
  if (dbg) fwprintf(stderr, L"[mkdir] start: %s\n", path.c_str());

  std::vector<std::wstring> missing;   // 从右到左
  std::wstring cur = path;
  while (true)
  {
    const DWORD a = ::GetFileAttributesW(cur.c_str());
    if (dbg) fwprintf(stderr, L"[mkdir] probe %s -> %lu\n", cur.c_str(), (unsigned long)a);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
      break;                       // 找到已存在的祖先
    const size_t pos = cur.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos < 2)
      return ERROR_PATH_NOT_FOUND; // 连盘符根都不存在
    missing.push_back(cur.substr(pos + 1));
    cur = cur.substr(0, pos);
  }

  DWORD err = 0;
  for (auto it = missing.rbegin(); it != missing.rend(); ++it)
  {
    cur += L'\\';
    cur += *it;
    if (!::CreateDirectoryW(cur.c_str(), nullptr))
    {
      const DWORD e = ::GetLastError();
      if (e != ERROR_ALREADY_EXISTS)
        err = e;
    }
  }
  return err;
}

typedef HRESULT(WINAPI *CreateObjectFn)(const GUID *, const GUID *, void **);
typedef HRESULT(WINAPI *GetNumberOfFormatsFn)(UInt32 *);
typedef HRESULT(WINAPI *GetHandlerProperty2Fn)(UInt32, PROPID, PROPVARIANT *);

struct SevenZipApi
{
  HMODULE mod = nullptr;
  CreateObjectFn createObject = nullptr;
  GetNumberOfFormatsFn getNumberOfFormats = nullptr;
  GetHandlerProperty2Fn getHandlerProperty2 = nullptr;
  bool ok = false;
};

SevenZipApi LoadApi(const wchar_t *dllPath)
{
  SevenZipApi api;
  api.mod = ::GetModuleHandleW(L"7z.dll");
  if (!api.mod && dllPath && *dllPath)
    api.mod = ::LoadLibraryW(dllPath);
  if (!api.mod)
    return api;

  api.createObject = (CreateObjectFn)::GetProcAddress(api.mod, "CreateObject");
  api.getNumberOfFormats = (GetNumberOfFormatsFn)::GetProcAddress(api.mod, "GetNumberOfFormats");
  api.getHandlerProperty2 = (GetHandlerProperty2Fn)::GetProcAddress(api.mod, "GetHandlerProperty2");
  api.ok = api.createObject && api.getNumberOfFormats && api.getHandlerProperty2;
  return api;
}

std::wstring Utf8OrAnsiToWide(const char *s)
{
  if (!s || !*s)
    return std::wstring();
  int w = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  UINT cp = CP_UTF8;
  if (w <= 1)
  {
    cp = CP_ACP;
    w = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
  }
  if (w <= 1)
    return std::wstring();
  std::wstring out((size_t)w, L'\0');
  ::MultiByteToWideChar(cp, 0, s, -1, &out[0], w);
  out.resize(wcslen(out.c_str()));
  return out;
}

std::wstring PropToString(const PROPVARIANT &v)
{
  if (v.vt == VT_BSTR && v.bstrVal)
    return v.bstrVal;
  if (v.vt == VT_EMPTY)
    return std::wstring();
  return std::wstring();
}

// ------------------------------------------------------------------ 输入流

class CFileInStream Z7_final:
  public IInStream,
  public CMyUnknownImp
{
public:
  Z7_COM_UNKNOWN_IMP_0
  Z7_IFACE_COM7_IMP(ISequentialInStream)
  Z7_IFACE_COM7_IMP(IInStream)

public:
  bool Open(const std::wstring &path)
  {
    _h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return _h != INVALID_HANDLE_VALUE;
  }
  ~CFileInStream()
  {
    if (_h != INVALID_HANDLE_VALUE)
      ::CloseHandle(_h);
  }

private:
  HANDLE _h = INVALID_HANDLE_VALUE;
};

Z7_COM7F_IMF(CFileInStream::Read(void *data, UInt32 size, UInt32 *processed))
{
  DWORD got = 0;
  if (!::ReadFile(_h, data, size, &got, nullptr))
    return E_FAIL;
  if (processed)
    *processed = got;
  return S_OK;
}

Z7_COM7F_IMF(CFileInStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  LARGE_INTEGER li;
  li.QuadPart = offset;
  if (!::SetFilePointerEx(_h, li, &li, seekOrigin))
    return E_FAIL;
  if (newPosition)
    *newPosition = (UInt64)li.QuadPart;
  return S_OK;
}

// ------------------------------------------------------------ 打开回调

class COpenCallback Z7_final:
  public IArchiveOpenCallback,
  public IArchiveOpenVolumeCallback,
  public ICryptoGetTextPassword,
  public ICryptoGetTextPassword2,
  public CMyUnknownImp
{
public:
  Z7_COM_UNKNOWN_IMP_4(IArchiveOpenCallback, IArchiveOpenVolumeCallback,
                       ICryptoGetTextPassword, ICryptoGetTextPassword2)
  Z7_IFACE_COM7_IMP(IArchiveOpenCallback)
  Z7_IFACE_COM7_IMP(IArchiveOpenVolumeCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword2)

public:
  std::wstring Password;
  bool *PasswordRequested = nullptr;
  std::wstring FolderPrefix;   // 归档所在目录（带尾部斜杠）
  std::wstring FileName;       // 归档文件名（卷回调解析基准名用）
};

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64 *, const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64 *, const UInt64 *)) { return S_OK; }

Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword(BSTR *password))
{
  if (PasswordRequested)
    *PasswordRequested = true;
  if (!password)
    return E_INVALIDARG;
  *password = ::SysAllocString(Password.c_str());
  return *password ? S_OK : E_OUTOFMEMORY;
}

Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword2(Int32 *passwordIsDefined, BSTR *password))
{
  if (passwordIsDefined)
    *passwordIsDefined = 1;
  return CryptoGetTextPassword(password);
}

// 分卷归档：7z.dll 通过这两个方法按名字来找其它卷（Desktop2.z01 等）。
Z7_COM7F_IMF(COpenCallback::GetProperty(PROPID propID, PROPVARIANT *value))
{
  if (!value)
    return E_INVALIDARG;
  ::PropVariantInit(value);
  if (propID == kpidName)
  {
    value->vt = VT_BSTR;
    value->bstrVal = ::SysAllocString(FileName.c_str());
    if (!value->bstrVal)
      return E_OUTOFMEMORY;
  }
  return S_OK;
}

Z7_COM7F_IMF(COpenCallback::GetStream(const wchar_t *name, IInStream **inStream))
{
  if (!inStream)
    return E_INVALIDARG;
  *inStream = nullptr;
  if (!name || !*name)
    return S_FALSE;

  std::wstring volume = name;
  for (auto &c : volume)
    if (c == L'/')
      c = L'\\';
  if (volume[0] == L'\\' || volume.find(L':') != std::wstring::npos ||
      volume.find(L"..") != std::wstring::npos)
    return S_FALSE;

  CFileInStream *file = new CFileInStream;
  CMyComPtr<IInStream> stream = file;
  if (!file->Open(FolderPrefix + volume))
    return S_FALSE;
  *inStream = stream.Detach();
  return S_OK;
}

// ------------------------------------------------------------------ 输出流

class COutFileStream Z7_final:
  public IOutStream,
  public CMyUnknownImp
{
public:
  Z7_COM_UNKNOWN_IMP_0
  Z7_IFACE_COM7_IMP(ISequentialOutStream)
  Z7_IFACE_COM7_IMP(IOutStream)

public:
  bool Create(const std::wstring &path)
  {
    _h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return _h != INVALID_HANDLE_VALUE;
  }
  ~COutFileStream()
  {
    if (_h != INVALID_HANDLE_VALUE)
      ::CloseHandle(_h);
  }

private:
  HANDLE _h = INVALID_HANDLE_VALUE;
};

Z7_COM7F_IMF(COutFileStream::Write(const void *data, UInt32 size, UInt32 *processed))
{
  DWORD wrote = 0;
  if (!::WriteFile(_h, data, size, &wrote, nullptr))
    return E_FAIL;
  if (processed)
    *processed = wrote;
  return S_OK;
}

Z7_COM7F_IMF(COutFileStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  LARGE_INTEGER li;
  li.QuadPart = offset;
  if (!::SetFilePointerEx(_h, li, &li, seekOrigin))
    return E_FAIL;
  if (newPosition)
    *newPosition = (UInt64)li.QuadPart;
  return S_OK;
}

Z7_COM7F_IMF(COutFileStream::SetSize(UInt64 newSize))
{
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)newSize;
  if (!::SetFilePointerEx(_h, li, nullptr, FILE_BEGIN))
    return E_FAIL;
  if (!::SetEndOfFile(_h))
    return E_FAIL;
  return S_OK;
}

// ------------------------------------------------------------ 解压回调

class CExtractCallback Z7_final:
  public IArchiveExtractCallback,
  public ICryptoGetTextPassword,   // 解数据时 7-Zip 会向解压回调要密码
  public ICryptoGetTextPassword2,
  public CMyUnknownImp
{
public:
  Z7_COM_UNKNOWN_IMP_3(IArchiveExtractCallback, ICryptoGetTextPassword, ICryptoGetTextPassword2)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveExtractCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword2)

public:
  std::wstring Password;
  bool *PasswordRequested = nullptr;
  std::wstring OutDir;
  std::vector<std::wstring> Paths;   // index -> 归档内路径
  int Written = 0;
  DWORD LastWin32 = 0;
  std::wstring LastPath;
};

Z7_COM7F_IMF(CExtractCallback::SetTotal(UInt64)) { return S_OK; }
Z7_COM7F_IMF(CExtractCallback::SetCompleted(const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(CExtractCallback::PrepareOperation(Int32)) { return S_OK; }

Z7_COM7F_IMF(CExtractCallback::GetStream(UInt32 index, ISequentialOutStream **outStream,
                                         Int32 askExtractMode))
{
  *outStream = nullptr;
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract)
    return S_OK;

  if (index >= Paths.size())
    return S_OK;

  std::wstring rel = Paths[index];
  if (rel.empty())
    return S_OK;
  for (auto &c : rel)
    if (c == L'/')
      c = L'\\';

  const std::wstring full = OutDir + rel;
  {
    const size_t pos = full.find_last_of(L'\\');
    if (pos != std::wstring::npos)
      CreateDirsRecursive(full.substr(0, pos));
  }

  CMyComPtr<IOutStream> streamSpec = (IOutStream *)(new COutFileStream);
  COutFileStream *spec = (COutFileStream *)(IOutStream *)streamSpec;
  if (!spec->Create(full))
  {
    LastWin32 = ::GetLastError();
    LastPath = full;
    if (::GetEnvironmentVariableW(L"Z7CUSTOM_DEBUG", nullptr, 0) > 0)
      fwprintf(stderr, L"[7z-inproc] create failed: %s (err=%lu)\n", full.c_str(), LastWin32);
    return E_FAIL;
  }

  ++Written;
  *outStream = streamSpec.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::SetOperationResult(Int32)) { return S_OK; }

Z7_COM7F_IMF(CExtractCallback::CryptoGetTextPassword(BSTR *password))
{
  if (PasswordRequested)
    *PasswordRequested = true;
  if (!password)
    return E_INVALIDARG;
  *password = ::SysAllocString(Password.c_str());
  return *password ? S_OK : E_OUTOFMEMORY;
}

Z7_COM7F_IMF(CExtractCallback::CryptoGetTextPassword2(Int32 *passwordIsDefined, BSTR *password))
{
  if (passwordIsDefined)
    *passwordIsDefined = 1;
  return CryptoGetTextPassword(password);
}

// ------------------------------------------------------------ 密码验证

// 只吃不吐的输出流：验证密码时用，解出的数据直接丢掉。
// 解码器照常解密、照常算 CRC，密码错/数据坏会在 SetOperationResult 里报出来。
class CVoidOutStream Z7_final:
  public ISequentialOutStream,
  public CMyUnknownImp
{
public:
  Z7_COM_UNKNOWN_IMP_0
  Z7_IFACE_COM7_IMP(ISequentialOutStream)
};

Z7_COM7F_IMF(CVoidOutStream::Write(const void *, UInt32 size, UInt32 *processed))
{
  if (processed)
    *processed = size;
  return S_OK;
}

class CVerifyCallback Z7_final:
  public IArchiveExtractCallback,
  public ICryptoGetTextPassword,
  public ICryptoGetTextPassword2,
  public CMyUnknownImp
{
public:
  Z7_COM_UNKNOWN_IMP_3(IArchiveExtractCallback, ICryptoGetTextPassword, ICryptoGetTextPassword2)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveExtractCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword2)

public:
  std::wstring Password;
  bool SawError = false;
};

Z7_COM7F_IMF(CVerifyCallback::SetTotal(UInt64)) { return S_OK; }
Z7_COM7F_IMF(CVerifyCallback::SetCompleted(const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(CVerifyCallback::PrepareOperation(Int32)) { return S_OK; }

Z7_COM7F_IMF(CVerifyCallback::GetStream(UInt32, ISequentialOutStream **outStream,
                                        Int32 askExtractMode))
{
  *outStream = nullptr;
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract)
    return S_OK;
  CMyComPtr<ISequentialOutStream> s = (ISequentialOutStream *)(new CVoidOutStream);
  *outStream = s.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CVerifyCallback::SetOperationResult(Int32 opRes))
{
  if (opRes != NArchive::NExtract::NOperationResult::kOK)
    SawError = true;
  return S_OK;
}

Z7_COM7F_IMF(CVerifyCallback::CryptoGetTextPassword(BSTR *password))
{
  if (!password)
    return E_INVALIDARG;
  *password = ::SysAllocString(Password.c_str());
  return *password ? S_OK : E_OUTOFMEMORY;
}

Z7_COM7F_IMF(CVerifyCallback::CryptoGetTextPassword2(Int32 *passwordIsDefined, BSTR *password))
{
  if (passwordIsDefined)
    *passwordIsDefined = 1;
  return CryptoGetTextPassword(password);
}

// 尝试用某个 handler 打开
HRESULT TryOpenWithFormat(const SevenZipApi &api, UInt32 formatIndex,
                          const std::wstring &archivePath, const std::wstring &password,
                          bool *passwordRequested,
                          CMyComPtr<IInArchive> &outArchive, CMyComPtr<IInStream> &outStream)
{
  // kClassID 是一个长度为 16 的 BSTR（不是 PROPVARIANT 直接转 GUID）
  PROPVARIANT vClsid;
  PropVariantInit(&vClsid);
  if (api.getHandlerProperty2(formatIndex, NArchive::NHandlerPropID::kClassID, &vClsid) != S_OK ||
      vClsid.vt != VT_BSTR || !vClsid.bstrVal)
  {
    PropVariantClear(&vClsid);
    return E_FAIL;
  }
  GUID clsid = {};
  memcpy(&clsid, vClsid.bstrVal, sizeof(GUID));
  PropVariantClear(&vClsid);

  const bool dbg = ::GetEnvironmentVariableW(L"Z7CUSTOM_DEBUG", nullptr, 0) > 0;
  if (dbg)
    fwprintf(stderr, L"[7z-inproc] fmt #%u clsid ok\n", (unsigned)formatIndex);

  CMyComPtr<IInArchive> archive;
  if (api.createObject(&clsid, &IID_IInArchive, (void **)&archive) != S_OK || !archive)
  {
    if (dbg) fwprintf(stderr, L"[7z-inproc] fmt #%u createObject failed\n", (unsigned)formatIndex);
    return E_FAIL;
  }

  CMyComPtr<IInStream> stream = (IInStream *)(new CFileInStream);
  CFileInStream *inSpec = (CFileInStream *)(IInStream *)stream;
  if (!inSpec->Open(archivePath))
  {
    if (dbg) fwprintf(stderr, L"[7z-inproc] fmt #%u open file failed err=%lu\n",
                      (unsigned)formatIndex, (unsigned long)::GetLastError());
    return HRESULT_FROM_WIN32(::GetLastError());
  }

  CMyComPtr<IArchiveOpenCallback> cb = (IArchiveOpenCallback *)(new COpenCallback);
  COpenCallback *cbSpec = (COpenCallback *)(IArchiveOpenCallback *)cb;
  cbSpec->Password = password;
  cbSpec->PasswordRequested = passwordRequested;
  {
    const size_t slash = archivePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
      cbSpec->FileName = archivePath;
    else
    {
      cbSpec->FolderPrefix = archivePath.substr(0, slash + 1);
      cbSpec->FileName = archivePath.substr(slash + 1);
    }
  }

  const UInt64 scanSize = 1 << 23;
  const HRESULT openRes = archive->Open(stream, &scanSize, cb);
  if (openRes != S_OK)
  {
    if (::GetEnvironmentVariableW(L"Z7CUSTOM_DEBUG", nullptr, 0) > 0)
      fwprintf(stderr, L"[7z-inproc] format #%u Open -> 0x%08X\n", (unsigned)formatIndex, (unsigned)openRes);
    return E_FAIL;
  }

  outArchive = archive;
  outStream = stream;
  return S_OK;
}

std::wstring LowerExt(const std::wstring &path)
{
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos)
    return std::wstring();
  std::wstring ext = path.substr(dot + 1);
  for (auto &c : ext)
    c = (wchar_t)::towlower(c);
  return ext;
}

struct FormatCandidate
{
  UInt32 index;
  bool nameMatch;
};

void BuildFormatOrder(const SevenZipApi &api, UInt32 numFormats,
                      const std::wstring &archivePath,
                      std::vector<FormatCandidate> &order)
{
  order.clear();
  const std::wstring ext = LowerExt(archivePath);
  for (UInt32 i = 0; i < numFormats; ++i)
  {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (api.getHandlerProperty2(i, NArchive::NHandlerPropID::kName, &v) == S_OK && v.vt == VT_BSTR)
    {
      std::wstring name = v.bstrVal;
      for (auto &c : name)
        c = (wchar_t)::towlower(c);
      const bool nameMatch = !ext.empty() && name.find(ext) != std::wstring::npos;
      if (nameMatch)
        order.insert(order.begin(), { i, true });
      else
        order.push_back({ i, false });
    }
    PropVariantClear(&v);
  }
}

} // namespace

bool ExtractImagesInProcess(const wchar_t *dllPath,
                            const std::wstring &archivePath,
                            const std::wstring &innerDir,
                            const std::wstring &password,
                            const std::wstring &outDir,
                            const std::vector<std::wstring> &exts,
                            int &extractedCount,
                            std::wstring &error,
                            bool &passwordRequested)
{
  extractedCount = 0;
  passwordRequested = false;
  const SevenZipApi api = LoadApi(dllPath);
  if (!api.ok)
  {
    error = L"7z.dll 不可用";
    return false;
  }

  UInt32 numFormats = 0;
  if (api.getNumberOfFormats(&numFormats) != S_OK || numFormats == 0)
  {
    error = L"7z.dll 没有可用格式";
    return false;
  }

  // 先按扩展名找匹配的 handler，再不行就全试
  std::vector<FormatCandidate> order;
  BuildFormatOrder(api, numFormats, archivePath, order);

  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> stream;
  for (const FormatCandidate &c : order)
  {
    bool asked = false;
    if (TryOpenWithFormat(api, c.index, archivePath, password, &asked, archive, stream) == S_OK)
    {
      passwordRequested = passwordRequested || asked;
      break;
    }
    passwordRequested = passwordRequested || asked;
    archive.Release();
    stream.Release();
    // 扩展名对上了、也向我们要过密码：说明格式没错，只是密码不对。
    // 继续拿同一个密码去试其它 handler 没有意义，直接交给上层弹框。
    if (c.nameMatch && asked)
      break;
  }

  if (!archive)
  {
    error = L"7z.dll 打不开这个归档";
    return false;
  }

  UInt32 numItems = 0;
  if (archive->GetNumberOfItems(&numItems) != S_OK)
  {
    error = L"读不到条目数";
    return false;
  }

  std::wstring dirLower = innerDir;
  for (auto &c : dirLower)
    if (c == L'/')
      c = L'\\';

  CRecordVector<UInt32> indices;
  std::vector<std::wstring> paths(numItems);
  std::wstring base = outDir;
  if (!base.empty() && base.back() != L'\\')
    base += L'\\';

  for (UInt32 i = 0; i < numItems; ++i)
  {
    PROPVARIANT vPath, vDir;
    PropVariantInit(&vPath);
    PropVariantInit(&vDir);
    std::wstring rel;
    if (archive->GetProperty(i, kpidPath, &vPath) == S_OK)
      rel = PropToString(vPath);
    bool isDir = false;
    if (archive->GetProperty(i, kpidIsDir, &vDir) == S_OK && vDir.vt == VT_BOOL)
      isDir = (vDir.boolVal != VARIANT_FALSE);
    PropVariantClear(&vPath);
    PropVariantClear(&vDir);

    if (isDir || rel.empty())
      continue;

    std::wstring norm = rel;
    for (auto &c : norm)
      if (c == L'/')
        c = L'\\';

    // 恶意归档可能带 "..\..\x.png" 之类的条目，绝不能拼进输出路径
    if (!IsSafeInnerPath(norm))
      continue;

    const size_t pos = norm.find_last_of(L'\\');
    std::wstring parent = (pos == std::wstring::npos) ? L"" : norm.substr(0, pos);
    if (_wcsicmp(parent.c_str(), dirLower.c_str()) != 0)
      continue;

    const std::wstring name = (pos == std::wstring::npos) ? norm : norm.substr(pos + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
      continue;
    std::wstring e = name.substr(dot + 1);
    for (auto &c : e)
      c = (wchar_t)::towlower(c);

    bool wanted = false;
    for (const auto &x : exts)
      if (_wcsicmp(e.c_str(), x.c_str()) == 0) { wanted = true; break; }
    if (!wanted)
      continue;

    indices.Add(i);
    paths[i] = norm;   // 存归档内相对路径，GetStream 里再拼输出目录
  }

  if (indices.IsEmpty())
  {
    error = L"这个目录里没有图片";
    return false;
  }

  // 目录必须我们自己建到位（不依赖调用方），否则 err=3
  {
    const DWORD de = CreateDirsRecursive(base.substr(0, base.size() - 1));
    if (de != 0)
    {
      wchar_t msg[256];
      swprintf_s(msg, L"建不了输出目录 (err %lu)", (unsigned long)de);
      error = msg;
      return false;
    }
  }

  CMyComPtr<IArchiveExtractCallback> cb =
      (IArchiveExtractCallback *)(new CExtractCallback);
  CExtractCallback *ecsSpec = (CExtractCallback *)(IArchiveExtractCallback *)cb;
  ecsSpec->OutDir = base;
  ecsSpec->Paths = paths;
  ecsSpec->Password = password;
  ecsSpec->PasswordRequested = &passwordRequested;
  const HRESULT res = archive->Extract(&indices[0], indices.Size(), false, cb);
  if (res != S_OK)
  {
    error = L"解压失败 0x";
    wchar_t hb[16]; swprintf_s(hb, L"%08X", (unsigned)res);
    error += hb;
    if (!ecsSpec->LastPath.empty())
    {
      error += L"，写文件失败：";
      error += ecsSpec->LastPath;
      wchar_t eb[16]; swprintf_s(eb, L"%lu", (unsigned long)ecsSpec->LastWin32);
      error += L" (err ";
      error += eb;
      error += L")";
    }
    return false;
  }

  extractedCount = ecsSpec->Written;
  return extractedCount > 0;
}

bool ArchiveIsEncrypted(const wchar_t *dllPath, const std::wstring &archivePath,
                        bool &encrypted)
{
  encrypted = false;
  const SevenZipApi api = LoadApi(dllPath);
  if (!api.ok)
    return false;

  UInt32 numFormats = 0;
  if (api.getNumberOfFormats(&numFormats) != S_OK || numFormats == 0)
    return false;

  std::vector<FormatCandidate> order;
  BuildFormatOrder(api, numFormats, archivePath, order);

  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> stream;
  bool opened = false;
  for (const FormatCandidate &c : order)
  {
    bool asked = false;
    if (TryOpenWithFormat(api, c.index, archivePath, std::wstring(), &asked,
                          archive, stream) == S_OK)
    {
      opened = true;
      break;
    }
    archive.Release();
    stream.Release();
    // 扩展名对上了、也向我们要过密码：头部加密，空密码打不开
    if (c.nameMatch && asked)
    {
      encrypted = true;
      return true;
    }
  }
  if (!opened)
    return false;

  UInt32 numItems = 0;
  if (archive->GetNumberOfItems(&numItems) != S_OK)
    return false;

  for (UInt32 i = 0; i < numItems; ++i)
  {
    PROPVARIANT v;
    PropVariantInit(&v);
    const bool got = archive->GetProperty(i, kpidEncrypted, &v) == S_OK;
    const bool enc = got && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    PropVariantClear(&v);
    if (enc)
    {
      encrypted = true;
      break;
    }
  }
  return true;
}

bool ArchiveTryPassword(const wchar_t *dllPath,
                        const std::wstring &archivePath,
                        const std::wstring &innerPath,
                        const std::wstring &password,
                        bool &verified)
{
  verified = false;
  const SevenZipApi api = LoadApi(dllPath);
  if (!api.ok)
    return false;

  UInt32 numFormats = 0;
  if (api.getNumberOfFormats(&numFormats) != S_OK || numFormats == 0)
    return false;

  std::vector<FormatCandidate> order;
  BuildFormatOrder(api, numFormats, archivePath, order);

  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> stream;
  bool opened = false;
  for (const FormatCandidate &c : order)
  {
    bool asked = false;
    if (TryOpenWithFormat(api, c.index, archivePath, password, &asked, archive, stream) == S_OK)
    {
      opened = true;
      break;
    }
    archive.Release();
    stream.Release();
    // 扩展名对上了、开归档时向我们要过密码却失败了：头加密 + 密码错。
    if (c.nameMatch && asked)
      return true;   // verified = false
  }
  if (!opened)
    return false;

  // 头没加密的包用什么密码都开得开，密码对不对要看解数据：挑一个条目试解。
  UInt32 numItems = 0;
  if (archive->GetNumberOfItems(&numItems) != S_OK || numItems == 0)
    return false;

  // 验证条目：优先用目标文件本身，否则取最小的文件条目（解得最快）。
  std::wstring want = innerPath;
  for (auto &ch : want)
    if (ch == L'/')
      ch = L'\\';

  int target = -1;
  int fallback = -1;
  UInt64 fallbackSize = 0;
  for (UInt32 i = 0; i < numItems && target < 0; ++i)
  {
    PROPVARIANT vPath, vDir, vSize;
    PropVariantInit(&vPath);
    PropVariantInit(&vDir);
    PropVariantInit(&vSize);
    std::wstring rel;
    if (archive->GetProperty(i, kpidPath, &vPath) == S_OK)
      rel = PropToString(vPath);
    bool isDir = false;
    if (archive->GetProperty(i, kpidIsDir, &vDir) == S_OK && vDir.vt == VT_BOOL)
      isDir = (vDir.boolVal != VARIANT_FALSE);
    UInt64 sz = 0;
    if (archive->GetProperty(i, kpidSize, &vSize) == S_OK && vSize.vt == VT_UI8)
      sz = vSize.uhVal.QuadPart;
    PropVariantClear(&vPath);
    PropVariantClear(&vDir);
    PropVariantClear(&vSize);

    if (isDir || rel.empty())
      continue;

    if (!want.empty())
    {
      std::wstring norm = rel;
      for (auto &ch : norm)
        if (ch == L'/')
          ch = L'\\';
      if (_wcsicmp(norm.c_str(), want.c_str()) == 0)
      {
        target = (int)i;
        break;
      }
    }

    if (fallback < 0 || sz < fallbackSize)
    {
      fallback = (int)i;
      fallbackSize = sz;
    }
  }

  const int idx = (target >= 0) ? target : fallback;
  if (idx < 0)
    return false;

  CVerifyCallback *cbSpec = new CVerifyCallback;
  cbSpec->Password = password;
  CMyComPtr<IArchiveExtractCallback> cb = cbSpec;
  UInt32 index = (UInt32)idx;
  const HRESULT res = archive->Extract(&index, 1, false, cb);
  verified = (res == S_OK && !cbSpec->SawError);
  return true;
}

} // namespace custom
