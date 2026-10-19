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

#include <windows.h>

#include <string>
#include <vector>

#include "../../CPP/7zip/Archive/IArchive.h"
#include "../../CPP/7zip/IPassword.h"
#include "../../CPP/7zip/IStream.h"
#include "../../CPP/7zip/PropID.h"
#include "../../CPP/Common/MyWindows.h"
#include "../../CPP/Common/MyCom.h"
#include "../../CPP/Common/MyVector.h"

namespace custom {

namespace {

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
  public ICryptoGetTextPassword,
  public ICryptoGetTextPassword2,
  public CMyUnknownImp
{
public:
  Z7_COM_UNKNOWN_IMP_3(IArchiveOpenCallback, ICryptoGetTextPassword, ICryptoGetTextPassword2)
  Z7_IFACE_COM7_IMP(IArchiveOpenCallback)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword2)

public:
  std::wstring Password;
};

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64 *, const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64 *, const UInt64 *)) { return S_OK; }

Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword(BSTR *password))
{
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
  std::wstring OutDir;
  std::vector<std::wstring> Paths;   // index -> 归档内路径
  int Written = 0;
  DWORD LastWin32 = 0;
  int StreamCalls = 0;

private:
  bool MakeDirs(const std::wstring &full)
  {
    const size_t pos = full.find_last_of(L'\\');
    if (pos == std::wstring::npos)
      return true;
    const std::wstring dir = full.substr(0, pos);
    // 逐级创建
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i)
    {
      cur += dir[i];
      if (dir[i] == L'\\' || i + 1 == dir.size())
        ::CreateDirectoryW(cur.c_str(), nullptr);
    }
    return true;
  }
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

  ++StreamCalls;
  if (index >= Paths.size())
    return S_OK;

  std::wstring rel = Paths[index];
  for (auto &c : rel)
    if (c == L'/')
      c = L'\\';

  const std::wstring full = OutDir + rel;
  MakeDirs(full);

  CMyComPtr<IOutStream> streamSpec = (IOutStream *)(new COutFileStream);
  COutFileStream *spec = (COutFileStream *)(IOutStream *)streamSpec;
  if (!spec->Create(full))
  {
    LastWin32 = ::GetLastError();
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

// 尝试用某个 handler 打开
HRESULT TryOpenWithFormat(const SevenZipApi &api, UInt32 formatIndex,
                          const std::wstring &archivePath, const std::wstring &password,
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
    c = (wchar_t)::CharLowerW((LPWSTR)(UINT_PTR)c);
  return ext;
}

} // namespace

bool ExtractImagesInProcess(const wchar_t *dllPath,
                            const std::wstring &archivePath,
                            const std::wstring &innerDir,
                            const std::wstring &password,
                            const std::wstring &outDir,
                            const std::vector<std::wstring> &exts,
                            int &extractedCount,
                            std::wstring &error)
{
  extractedCount = 0;
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
  const std::wstring ext = LowerExt(archivePath);
  std::vector<UInt32> order;
  for (UInt32 i = 0; i < numFormats; ++i)
  {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (api.getHandlerProperty2(i, NArchive::NHandlerPropID::kName, &v) == S_OK && v.vt == VT_BSTR)
    {
      std::wstring name = v.bstrVal;
      for (auto &c : name)
        c = (wchar_t)::CharLowerW((LPWSTR)(UINT_PTR)c);
      if (!ext.empty() && name.find(ext) != std::wstring::npos)
        order.insert(order.begin(), i);
      else
        order.push_back(i);
    }
    PropVariantClear(&v);
  }

  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> stream;
  for (UInt32 idx : order)
  {
    if (TryOpenWithFormat(api, idx, archivePath, password, archive, stream) == S_OK)
      break;
    archive.Release();
    stream.Release();
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
      c = (wchar_t)::CharLowerW((LPWSTR)(UINT_PTR)c);

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

  CMyComPtr<IArchiveExtractCallback> cb =
      (IArchiveExtractCallback *)(new CExtractCallback);
  CExtractCallback *ecsSpec = (CExtractCallback *)(IArchiveExtractCallback *)cb;
  ecsSpec->OutDir = base;
  ecsSpec->Paths = paths;
  ecsSpec->Password = password;
  const HRESULT res = archive->Extract(&indices[0], indices.Size(), false, cb);
  if (res != S_OK)
  {
    wchar_t msg[128];
    swprintf_s(msg, L"解压失败 (0x%08X, 写出 %d, GetStream %d, err %lu)",
               (unsigned)res, ecsSpec->Written, ecsSpec->StreamCalls,
               (unsigned long)ecsSpec->LastWin32);
    error = msg;
    return false;
  }

  extractedCount = ecsSpec->Written;
  return extractedCount > 0;
}

} // namespace custom
