// Custom/Core/PasswordVault.cpp

#include "PasswordVault.h"
#include "VaultCrypto.h"

#include <windows.h>
#include <shlobj.h>

#include <cstring>
#include <ctime>
#include <cwctype>

namespace custom {

namespace {

const char kMagic[4] = { '7', 'Z', 'P', 'V' };
const uint32_t kVersion = 1;
const wchar_t *kKeyName = L"vault.key";
const wchar_t *kDataName = L"vault.dat";
const size_t kMaxEntries = 4096;
const size_t kMaxFileSize = 8u * 1024u * 1024u;
const size_t kMaxPassword = 1024;

struct FileGuard
{
  HANDLE h = INVALID_HANDLE_VALUE;
  ~FileGuard() { if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
};

std::string WideToUtf8(const std::wstring &s)
{
  if (s.empty())
    return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
  if (n <= 0)
    return std::string();
  std::string out((size_t)n, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const char *p, size_t n)
{
  if (n == 0)
    return std::wstring();
  int w = ::MultiByteToWideChar(CP_UTF8, 0, p, (int)n, nullptr, 0);
  if (w <= 0)
    return std::wstring();
  std::wstring out((size_t)w, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, p, (int)n, &out[0], w);
  return out;
}

void PutU32(std::vector<uint8_t> &v, uint32_t x)
{
  v.push_back((uint8_t)(x & 0xFF));
  v.push_back((uint8_t)((x >> 8) & 0xFF));
  v.push_back((uint8_t)((x >> 16) & 0xFF));
  v.push_back((uint8_t)((x >> 24) & 0xFF));
}

void PutI64(std::vector<uint8_t> &v, int64_t x)
{
  PutU32(v, (uint32_t)((uint64_t)x & 0xFFFFFFFFu));
  PutU32(v, (uint32_t)((uint64_t)x >> 32));
}

void PutStr(std::vector<uint8_t> &v, const std::wstring &s)
{
  const std::string u = WideToUtf8(s);
  PutU32(v, (uint32_t)u.size());
  v.insert(v.end(), u.begin(), u.end());
}

struct Reader
{
  const uint8_t *p;
  size_t n;
  size_t pos = 0;
  bool ok = true;

  bool Need(size_t k)
  {
    if (!ok || pos + k > n) { ok = false; return false; }
    return true;
  }
  uint32_t U32()
  {
    if (!Need(4)) return 0;
    uint32_t x = (uint32_t)p[pos] | ((uint32_t)p[pos + 1] << 8) |
                 ((uint32_t)p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24);
    pos += 4;
    return x;
  }
  int64_t I64()
  {
    uint32_t lo = U32();
    uint32_t hi = U32();
    return (int64_t)(((uint64_t)hi << 32) | lo);
  }
  std::wstring Str()
  {
    uint32_t len = U32();
    if (len > kMaxFileSize || !Need(len)) { ok = false; return std::wstring(); }
    std::wstring s = Utf8ToWide((const char *)(p + pos), len);
    pos += len;
    return s;
  }
};

int64_t FileStamp(const std::wstring &path)
{
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
    return 0;
  return ((int64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
}

std::wstring JoinPath(const std::wstring &dir, const wchar_t *name)
{
  std::wstring p = dir;
  if (!p.empty() && p.back() != L'\\')
    p += L'\\';
  p += name;
  return p;
}

wchar_t LowerCh(wchar_t c)
{
  return (wchar_t)::towlower(c);
}

} // namespace

std::wstring VaultDir()
{
  // 允许用环境变量重定向（便携模式 / 自动化测试用）
  wchar_t overrideDir[MAX_PATH * 2];
  const DWORD n = ::GetEnvironmentVariableW(L"Z7CUSTOM_VAULT_DIR", overrideDir,
                                            (DWORD)(sizeof(overrideDir) / sizeof(overrideDir[0])));
  if (n > 0 && n < sizeof(overrideDir) / sizeof(overrideDir[0]))
    return overrideDir;

  // 便携模式：exe 旁边有 data\ 目录就直接用它，数据跟着文件夹走。
  // （正式便携包里 data\ 是随包带的；安装版/开发版的 _out 没有这个目录。）
  wchar_t exePath[MAX_PATH * 2] = {};
  const DWORD exeLen = ::GetModuleFileNameW(nullptr, exePath,
                                            (DWORD)(sizeof(exePath) / sizeof(exePath[0])));
  if (exeLen > 0 && exeLen < sizeof(exePath) / sizeof(exePath[0]))
  {
    std::wstring exe(exePath, exeLen);
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
      const std::wstring dataDir = exe.substr(0, slash + 1) + L"data";
      const DWORD attr = ::GetFileAttributesW(dataDir.c_str());
      if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return dataDir;
    }
  }

  PWSTR base = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
    return std::wstring();
  std::wstring dir(base);
  ::CoTaskMemFree(base);
  if (!dir.empty() && dir.back() != L'\\')
    dir += L'\\';
  dir += L"7zip-custom";
  return dir;
}

std::wstring NormalizeArchiveName(const std::wstring &path)
{
  size_t pos = path.find_last_of(L"\\/");
  std::wstring name = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
  for (auto &c : name)
    c = LowerCh(c);
  return name;
}

bool WildcardMatchNoCase(const wchar_t *pattern, const wchar_t *text)
{
  if (!pattern || !text)
    return false;
  const wchar_t *star = nullptr;
  const wchar_t *mark = nullptr;

  while (*text)
  {
    if (*pattern == L'?' ||
        LowerCh(*pattern) == LowerCh(*text))
    {
      ++pattern;
      ++text;
    }
    else if (*pattern == L'*')
    {
      star = pattern++;
      mark = text;
    }
    else if (star)
    {
      pattern = star + 1;
      text = ++mark;
    }
    else
      return false;
  }
  while (*pattern == L'*')
    ++pattern;
  return *pattern == 0;
}

PasswordVault &PasswordVault::Instance()
{
  static PasswordVault vault;
  return vault;
}

bool PasswordVault::EnsureDir() const
{
  const std::wstring dir = VaultDir();
  if (dir.empty())
    return false;
  ::CreateDirectoryW(dir.c_str(), nullptr);
  return ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool PasswordVault::ReadFileBytes(const std::wstring &path, std::vector<uint8_t> &data) const
{
  FileGuard f;
  f.h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f.h == INVALID_HANDLE_VALUE)
    return false;

  LARGE_INTEGER size;
  if (!::GetFileSizeEx(f.h, &size) || size.QuadPart < 0 || (uint64_t)size.QuadPart > kMaxFileSize)
    return false;
  data.resize((size_t)size.QuadPart);
  if (data.empty())
    return true;

  DWORD got = 0;
  if (!::ReadFile(f.h, data.data(), (DWORD)data.size(), &got, nullptr) || got != data.size())
    return false;
  return true;
}

bool PasswordVault::WriteFileBytes(const std::wstring &path, const void *data, size_t size) const
{
  // 先写临时文件再原子替换：写到一半崩溃也不会把整个库毁掉
  wchar_t suffix[64];
  swprintf_s(suffix, L".%lu.%lu.tmp", (unsigned long)::GetCurrentProcessId(),
             (unsigned long)::GetCurrentThreadId());
  const std::wstring tmp = path + suffix;

  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  DWORD wrote = 0;
  const bool writeOk = ::WriteFile(h, data, (DWORD)size, &wrote, nullptr) && wrote == size;
  ::CloseHandle(h);
  if (!writeOk)
  {
    ::DeleteFileW(tmp.c_str());
    return false;
  }

  if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    ::DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

bool PasswordVault::LoadKey(std::vector<uint8_t> &key, bool createIfMissing)
{
  const std::wstring path = JoinPath(VaultDir(), kKeyName);
  std::vector<uint8_t> blob;
  if (ReadFileBytes(path, blob) && !blob.empty())
  {
    if (!DpapiUnprotect(blob.data(), blob.size(), key))
    {
      _error = L"主密钥解密失败（可能换了 Windows 账户）";
      return false;
    }
    return key.size() == kAesKeySize;
  }

  if (!createIfMissing)
  {
    // 还没有密钥 = 空库
    key.clear();
    return true;
  }

  if (!EnsureDir())
  {
    _error = L"无法创建配置目录";
    return false;
  }

  key.resize(kAesKeySize);
  if (!CryptoRandom(key.data(), key.size()))
  {
    _error = L"随机数生成失败";
    return false;
  }
  std::vector<uint8_t> prot;
  if (!DpapiProtect(key.data(), key.size(), prot))
  {
    _error = L"DPAPI 加密失败";
    return false;
  }
  if (!WriteFileBytes(path, prot.data(), prot.size()))
  {
    _error = L"主密钥写入失败";
    return false;
  }
  return true;
}

bool PasswordVault::Load(bool force)
{
  if (_loaded && !force)
  {
    const int64_t stamp = FileStamp(JoinPath(VaultDir(), kDataName));
    if (stamp == _fileStamp)
      return true;
  }

  _entries.clear();
  _error.clear();

  const std::wstring path = JoinPath(VaultDir(), kDataName);
  std::vector<uint8_t> raw;
  if (!ReadFileBytes(path, raw) || raw.empty())
  {
    // 没有库文件不是错误，只是空库
    _loaded = true;
    _fileStamp = FileStamp(path);
    return true;
  }

  std::vector<uint8_t> key;
  if (!LoadKey(key, false))
  {
    _loaded = false;
    return false;
  }
  if (key.size() != kAesKeySize)
  {
    _error = L"库文件存在但缺少主密钥";
    _loaded = false;
    return false;
  }

  const size_t headerSize = 4 + 4 + 4 + kGcmIvSize + kGcmTagSize + 4;
  if (raw.size() < headerSize || memcmp(raw.data(), kMagic, 4) != 0)
  {
    _error = L"库文件格式不正确";
    _loaded = false;
    return false;
  }

  const uint8_t *p = raw.data();
  uint32_t version = 0, flags = 0, payLen = 0;
  memcpy(&version, p + 4, 4);
  memcpy(&flags, p + 8, 4);
  const uint8_t *iv      = p + 12;
  const uint8_t *tag     = p + 12 + kGcmIvSize;
  memcpy(&payLen, tag + kGcmTagSize, 4);

  if (version != kVersion || raw.size() != headerSize + payLen)
  {
    _error = L"库文件版本或长度不符";
    _loaded = false;
    return false;
  }

  std::vector<uint8_t> aad;
  aad.insert(aad.end(), (const uint8_t *)kMagic, (const uint8_t *)kMagic + 4);
  PutU32(aad, version);
  PutU32(aad, flags);

  std::vector<uint8_t> plain(payLen);
  const uint8_t *cipherText = tag + kGcmTagSize + 4;
  if (payLen && !AesGcmDecrypt(key.data(), iv, aad.data(), aad.size(),
                               cipherText, payLen, plain.data(), tag))
  {
    _error = L"库文件解密失败（数据被篡改或密钥不匹配）";
    _loaded = false;
    return false;
  }

  if (!Deserialize(plain.data(), plain.size()))
  {
    _entries.clear();
    _loaded = false;
    return false;
  }

  _loaded = true;
  _fileStamp = FileStamp(path);
  return true;
}

bool PasswordVault::Serialize(std::vector<uint8_t> &out) const
{
  out.clear();
  PutU32(out, (uint32_t)_entries.size());
  for (const auto &e : _entries)
  {
    PutU32(out, e.flags);
    PutI64(out, e.lastUsed);
    PutStr(out, e.name);
    PutStr(out, e.password);
    PutStr(out, e.note);
  }
  return true;
}

bool PasswordVault::Deserialize(const uint8_t *data, size_t size)
{
  Reader r{ data, size };
  const uint32_t count = r.U32();
  if (!r.ok || count > kMaxEntries)
  {
    _error = L"条目数量异常";
    return false;
  }

  _entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i)
  {
    VaultEntry e;
    e.flags = r.U32();
    e.lastUsed = r.I64();
    e.name = r.Str();
    e.password = r.Str();
    e.note = r.Str();
    if (!r.ok)
    {
      _error = L"条目解析失败";
      return false;
    }
    _entries.push_back(std::move(e));
  }
  return true;
}

bool PasswordVault::Save()
{
  if (!EnsureDir())
    return false;

  std::vector<uint8_t> key;
  if (!LoadKey(key, true) || key.size() != kAesKeySize)
    return false;

  std::vector<uint8_t> plain;
  Serialize(plain);

  std::vector<uint8_t> aad;
  aad.insert(aad.end(), (const uint8_t *)kMagic, (const uint8_t *)kMagic + 4);
  PutU32(aad, kVersion);
  PutU32(aad, 0);

  uint8_t iv[kGcmIvSize];
  if (!CryptoRandom(iv, sizeof(iv)))
    return false;

  uint8_t tag[kGcmTagSize];
  std::vector<uint8_t> cipher(plain.size());
  if (!plain.empty() &&
      !AesGcmEncrypt(key.data(), iv, aad.data(), aad.size(),
                     plain.data(), plain.size(), cipher.data(), tag))
    return false;

  std::vector<uint8_t> raw;
  raw.insert(raw.end(), (const uint8_t *)kMagic, (const uint8_t *)kMagic + 4);
  PutU32(raw, kVersion);
  PutU32(raw, 0);
  raw.insert(raw.end(), iv, iv + kGcmIvSize);
  raw.insert(raw.end(), tag, tag + kGcmTagSize);
  PutU32(raw, (uint32_t)cipher.size());
  raw.insert(raw.end(), cipher.begin(), cipher.end());

  const std::wstring path = JoinPath(VaultDir(), kDataName);
  if (!WriteFileBytes(path, raw.data(), raw.size()))
  {
    _error = L"库文件写入失败";
    return false;
  }

  _loaded = true;
  _fileStamp = FileStamp(path);
  return true;
}

bool PasswordVault::FindForArchive(const std::wstring &archivePath, std::wstring &password)
{
  const std::wstring base = NormalizeArchiveName(archivePath);
  if (base.empty())
    return false;

  const VaultEntry *bestExact = nullptr;
  const VaultEntry *bestPath = nullptr;
  const VaultEntry *bestWild = nullptr;

  // 完整路径匹配用，循环外算一次
  std::wstring targetPath = archivePath;
  for (auto &c : targetPath)
    c = LowerCh(c);

  for (const auto &e : _entries)
  {
    if (e.password.empty())
      continue;
    const std::wstring pname = NormalizeArchiveName(e.name);
    if (pname.empty())
      continue;

    if (pname.find(L'*') != std::wstring::npos || pname.find(L'?') != std::wstring::npos)
    {
      if (!bestWild && WildcardMatchNoCase(pname.c_str(), base.c_str()))
        bestWild = &e;
      continue;
    }

    if (pname == base)
    {
      if (!bestExact)
        bestExact = &e;
      continue;
    }

    // 完整路径精确匹配（用户显式写了路径的情况）
    if (!bestPath)
    {
      std::wstring full = e.name;
      for (auto &c : full)
        c = LowerCh(c);
      if (full == targetPath)
        bestPath = &e;
    }
  }

  const VaultEntry *hit = bestExact ? bestExact : (bestPath ? bestPath : bestWild);
  if (!hit)
    return false;

  password = hit->password;
  return true;
}

bool PasswordVault::Remember(const std::wstring &archivePath, const std::wstring &password)
{
  if (password.empty() || password.size() > kMaxPassword)
    return false;

  const std::wstring base = NormalizeArchiveName(archivePath);
  if (base.empty())
    return false;

  const int64_t now = (int64_t)::time(nullptr);

  for (auto &e : _entries)
  {
    if (NormalizeArchiveName(e.name) == base)
    {
      e.password = password;
      e.lastUsed = now;
      return Save();
    }
  }

  if (_entries.size() >= kMaxEntries)
    return false;

  VaultEntry e;
  e.name = base;
  e.password = password;
  e.lastUsed = now;
  _entries.push_back(std::move(e));
  return Save();
}

bool PasswordVault::Erase(size_t index)
{
  if (index >= _entries.size())
    return false;
  _entries.erase(_entries.begin() + (ptrdiff_t)index);
  return Save();
}

void PasswordVault::Clear()
{
  _entries.clear();
  _loaded = false;
  _fileStamp = 0;
}

} // namespace custom
