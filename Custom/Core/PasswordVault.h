// Custom/Core/PasswordVault.h
//
// 加密密码库：把"归档 → 密码"的映射加密落盘。
//
// 存储位置：%LOCALAPPDATA%\7zip-custom\
//   vault.key  DPAPI 保护的 32 字节随机主密钥（仅当前 Windows 账户可解）
//   vault.dat  AES-256-GCM 密文的条目表
//
// 设计取舍：
//   * 用 DPAPI 保护主密钥而不是让用户记"主密码"，换来零操作成本；
//     安全性依赖 Windows 账户本身（文件被拷走也解不开）。
//   * 加密用 AES-GCM，带完整性校验；篡改密文会解密失败而不是解出乱码。
//   * 匹配规则支持精确文件名、完整路径和 * / ? 通配，方便按目录批量带密码。

#ifndef CUSTOM_PASSWORD_VAULT_H
#define CUSTOM_PASSWORD_VAULT_H

#include <cstdint>
#include <string>
#include <vector>

namespace custom {

struct VaultEntry
{
  std::wstring name;      // 归档文件名（如 backup.7z）或通配式（如 *.7z）
  std::wstring password;
  std::wstring note;
  uint32_t flags = 0;     // 保留
  int64_t lastUsed = 0;   // Unix 秒
};

std::wstring VaultDir();

class PasswordVault
{
public:
  static PasswordVault &Instance();

  bool EnsureDir() const;

  // 从磁盘读取（内部按文件修改时间做缓存，外部工具改过会自动重载）
  bool Load(bool force = false);
  bool Save();

  // 命中返回 true，password 输出明文
  bool FindForArchive(const std::wstring &archivePath, std::wstring &password);

  // 记住某个归档的密码（按文件名归一化后写入）
  bool Remember(const std::wstring &archivePath, const std::wstring &password);

  bool Erase(size_t index);
  void Clear();

  std::vector<VaultEntry> &Entries() { return _entries; }
  const std::vector<VaultEntry> &Entries() const { return _entries; }
  bool Loaded() const { return _loaded; }
  const std::wstring &LastError() const { return _error; }
  void SetError(const std::wstring &e) { _error = e; }

private:
  PasswordVault() = default;

  bool LoadKey(std::vector<uint8_t> &key, bool createIfMissing);
  bool ReadFileBytes(const std::wstring &path, std::vector<uint8_t> &data) const;
  bool WriteFileBytes(const std::wstring &path, const void *data, size_t size) const;
  bool Serialize(std::vector<uint8_t> &out) const;
  bool Deserialize(const uint8_t *data, size_t size);

  std::vector<VaultEntry> _entries;
  bool _loaded = false;
  int64_t _fileStamp = 0;
  std::wstring _error;
};

// 归档文件名归一化：取 basename（小写）
std::wstring NormalizeArchiveName(const std::wstring &path);
// 大小写不敏感的通配匹配，支持 * 和 ?
bool WildcardMatchNoCase(const wchar_t *pattern, const wchar_t *text);

} // namespace custom

#endif
