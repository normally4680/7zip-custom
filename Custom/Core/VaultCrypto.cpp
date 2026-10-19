// Custom/Core/VaultCrypto.cpp

#include "VaultCrypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <cstring>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace custom {

namespace {

// DPAPI 附加熵：让本程序的密文不能被其他程序"顺带"解开。
const char kEntropy[] = "7zip-custom/vault/v1";

struct AlgProv
{
  BCRYPT_ALG_HANDLE alg = nullptr;
  ~AlgProv() { if (alg) ::BCryptCloseAlgorithmProvider(alg, 0); }
};

bool OpenGcm(AlgProv &p)
{
  if (!NT_SUCCESS(::BCryptOpenAlgorithmProvider(&p.alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
    return false;
  return NT_SUCCESS(::BCryptSetProperty(p.alg, BCRYPT_CHAINING_MODE,
      (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
}

struct KeyHandle
{
  BCRYPT_KEY_HANDLE h = nullptr;
  ~KeyHandle() { if (h) ::BCryptDestroyKey(h); }
};

bool InitAuthInfo(BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO &info,
                  std::vector<uint8_t> &macCtx,
                  const uint8_t *iv,
                  uint8_t *tag,
                  const void *aad, size_t aadSize)
{
  // macContext 大小必须 >= sizeof(BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO)
  macCtx.assign(sizeof(BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO), 0);
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce         = const_cast<PUCHAR>(iv);
  info.cbNonce         = (ULONG)kGcmIvSize;
  info.pbTag           = tag;
  info.cbTag           = (ULONG)kGcmTagSize;
  info.pbAuthData      = const_cast<PUCHAR>(static_cast<const uint8_t *>(aad));
  info.cbAuthData      = (ULONG)aadSize;
  info.pbMacContext    = macCtx.data();
  info.cbMacContext    = (ULONG)macCtx.size();
  return true;
}

} // namespace

bool CryptoRandom(void *buf, size_t size)
{
  return NT_SUCCESS(::BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)size,
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool AesGcmEncrypt(const uint8_t *key,
                   const uint8_t *iv,
                   const void *aad, size_t aadSize,
                   const void *in, size_t inSize,
                   void *out,
                   uint8_t *tag)
{
  AlgProv p;
  if (!OpenGcm(p))
    return false;

  DWORD objLen = 0, got = 0;
  if (!NT_SUCCESS(::BCryptGetProperty(p.alg, BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&objLen, sizeof(objLen), &got, 0)))
    return false;

  std::vector<uint8_t> obj(objLen);
  KeyHandle k;
  if (!NT_SUCCESS(::BCryptGenerateSymmetricKey(p.alg, &k.h, obj.data(), objLen,
        (PUCHAR)key, (ULONG)kAesKeySize, 0)))
    return false;

  std::vector<uint8_t> macCtx;
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  InitAuthInfo(info, macCtx, iv, tag, aad, aadSize);

  ULONG outLen = 0;
  NTSTATUS st = ::BCryptEncrypt(k.h,
      (PUCHAR)const_cast<void *>(in), (ULONG)inSize,
      &info, nullptr, 0,
      (PUCHAR)out, (ULONG)inSize, &outLen, 0);
  return NT_SUCCESS(st) && outLen == inSize;
}

bool AesGcmDecrypt(const uint8_t *key,
                   const uint8_t *iv,
                   const void *aad, size_t aadSize,
                   const void *in, size_t inSize,
                   void *out,
                   const uint8_t *tag)
{
  AlgProv p;
  if (!OpenGcm(p))
    return false;

  DWORD objLen = 0, got = 0;
  if (!NT_SUCCESS(::BCryptGetProperty(p.alg, BCRYPT_OBJECT_LENGTH,
        (PUCHAR)&objLen, sizeof(objLen), &got, 0)))
    return false;

  std::vector<uint8_t> obj(objLen);
  KeyHandle k;
  if (!NT_SUCCESS(::BCryptGenerateSymmetricKey(p.alg, &k.h, obj.data(), objLen,
        (PUCHAR)key, (ULONG)kAesKeySize, 0)))
    return false;

  std::vector<uint8_t> macCtx;
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  // GCM 解密需要可写的 tag 指针来做校验
  std::vector<uint8_t> tagCopy(tag, tag + kGcmTagSize);
  InitAuthInfo(info, macCtx, iv, tagCopy.data(), aad, aadSize);

  ULONG outLen = 0;
  NTSTATUS st = ::BCryptDecrypt(k.h,
      (PUCHAR)const_cast<void *>(in), (ULONG)inSize,
      &info, nullptr, 0,
      (PUCHAR)out, (ULONG)inSize, &outLen, 0);
  return NT_SUCCESS(st) && outLen == inSize;
}

bool DpapiProtect(const void *in, size_t inSize, std::vector<uint8_t> &out)
{
  DATA_BLOB din, dent, dout;
  din.pbData = (BYTE *)const_cast<void *>(in);
  din.cbData = (DWORD)inSize;
  dent.pbData = (BYTE *)kEntropy;
  dent.cbData = (DWORD)(sizeof(kEntropy) - 1);
  dout.pbData = nullptr;
  dout.cbData = 0;

  if (!::CryptProtectData(&din, L"7zip-custom", &dent, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &dout))
    return false;

  out.assign(dout.pbData, dout.pbData + dout.cbData);
  ::LocalFree(dout.pbData);
  return true;
}

bool DpapiUnprotect(const void *in, size_t inSize, std::vector<uint8_t> &out)
{
  DATA_BLOB din, dent, dout;
  din.pbData = (BYTE *)const_cast<void *>(in);
  din.cbData = (DWORD)inSize;
  dent.pbData = (BYTE *)kEntropy;
  dent.cbData = (DWORD)(sizeof(kEntropy) - 1);
  dout.pbData = nullptr;
  dout.cbData = 0;

  if (!::CryptUnprotectData(&din, nullptr, &dent, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &dout))
    return false;

  out.assign(dout.pbData, dout.pbData + dout.cbData);
  ::LocalFree(dout.pbData);
  return true;
}

} // namespace custom
