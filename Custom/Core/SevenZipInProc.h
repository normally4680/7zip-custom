// Custom/Core/SevenZipInProc.h
//
// 在进程内用 7z.dll 解压（不启动 7z.exe）。
// 只依赖 7z.dll 的 COM 导出函数，不链接任何 7-Zip 静态库。

#ifndef CUSTOM_SEVEN_ZIP_IN_PROC_H
#define CUSTOM_SEVEN_ZIP_IN_PROC_H

#include <string>
#include <vector>

namespace custom {

// dllPath 用于"进程里还没加载 7z.dll"时兜底 LoadLibrary
// innerDir 为归档内目录（空字符串表示根目录）
bool ExtractImagesInProcess(const wchar_t *dllPath,
                            const std::wstring &archivePath,
                            const std::wstring &innerDir,
                            const std::wstring &password,
                            const std::wstring &outDir,
                            const std::vector<std::wstring> &exts,
                            int &extractedCount,
                            std::wstring &error,
                            // true = 7-Zip 在解压过程中向我们要过密码（说明归档加密）。
                            // 密码为空/错误时用它在上一层弹密码框。
                            bool &passwordRequested);

// 快速判断归档是否需要密码（只看头部和条目属性，不解压）。
//   encrypted : true = 归档加密（含只加密数据的包）
// 返回 false = 判断不了（打不开/格式不支持）；调用方仍可尝试 Honeyview 原生模式，
// 只是不要自动填密码，让 Honeyview 自己弹框。
bool ArchiveIsEncrypted(const wchar_t *dllPath, const std::wstring &archivePath,
                        bool &encrypted);

} // namespace custom

#endif
