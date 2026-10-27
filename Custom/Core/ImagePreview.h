// Custom/Core/ImagePreview.h
//
// 无缝看图：不解压整个包，只把"当前目录里的图片"批量解到临时目录，
// 再把用户点的那张交给系统默认看图程序（Honeyview / 照片 …）。
// 看图软件自己按"同目录兄弟文件"实现滚轮翻页，于是就能翻遍整包图片。
//
// 解压优先在进程内直接用 7z.dll 完成；只有进程内失败时才退回 7z.exe。
// 进程内方式不启动子进程，也不需要把密码放进命令行。

#ifndef CUSTOM_IMAGE_PREVIEW_H
#define CUSTOM_IMAGE_PREVIEW_H

#include <string>

namespace custom {

struct PreviewResult
{
  bool ok = false;
  int extracted = 0;
  std::wstring openedPath;   // 传给看图程序的文件
  std::wstring tempDir;      // 临时目录（调试用）
  std::wstring error;
  // true = 环境问题（缺 7z.exe / 建不了临时目录 / 起不了子进程），
  //        这种失败不该静默退回 7-Zip，否则用户只会看到 7-Zip 的"拒绝访问"
  bool envError = false;
  std::wstring inprocError;  // 进程内解压失败的原因（诊断用）
  std::wstring command;      // 实际调用的 7z.exe 命令行（排错用）
  std::wstring launcher;     // 走通了哪一级启动链（诊断用）
  long long launchCode = 0;  // ShellExecute 返回码，或 CreateProcess 成功的 42
  // true = %TEMP% / %LOCALAPPDATA% 都不可写，预览目录退到了程序目录，
  // 通常意味着进程被受限上下文启动（启动外部程序会静默失败）
  bool restrictedEnv = false;
  // true = 归档加密，进程内解压时要过密码（密码为空或错误时需要弹框重试）
  bool needsPassword = false;
};

bool IsImageFile(const wchar_t *name);

// 归档内路径的安全校验：拒绝绝对路径、盘符、"."/".." 组件和冒号（NTFS 数据流），
// 防止恶意归档把预览文件写到临时目录之外。
bool IsSafeInnerPath(const std::wstring &rel);

// 7-Zip 有时传"归档路径 + 归档内路径"拼成的虚拟路径，这里统一成归档内相对路径。
std::wstring NormalizeInnerPath(const std::wstring &archivePath,
                                const std::wstring &innerRelPath);

// 找 Honeyview（它能直接解析压缩包，不需要解压）。
std::wstring FindHoneyview();

// 读 7z 签名头里的格式版本（第 8 字节）：
//   3 / 4 = 7z 格式版本 0.3 / 0.4（26.04 写 0.4，Honeyview 竖线快开只认 ≤0.3）
//   -1    = 不是 7z 或读不到
int SevenZArchiveVersion(const std::wstring &archivePath);

// 用 Honeyview 直接打开归档内某张图，路径格式为 "archive|inner"。
// password 非空时，后台等 Honeyview 的密码框出现并自动填入（加密包用）。
bool LaunchHoneyviewArchive(const std::wstring &honeyviewExe,
                            const std::wstring &archivePath,
                            const std::wstring &innerRelPath,
                            const std::wstring &password = std::wstring());

// 整包打开（不带内路径）：Honeyview 自己浏览归档、需要密码时弹框。
// password 非空时同样自动填入。7z 加密包只有这条路能看（竖线不支持 7z 解密）。
// 代价：显示的是 Honeyview 排序的第一张图，不是用户点的那张。
bool LaunchHoneyviewWholeArchive(const std::wstring &honeyviewExe,
                                 const std::wstring &archivePath,
                                 const std::wstring &password = std::wstring());

// 两步走：先整包打开（让 Honeyview 解析归档并进它的浏览历史），
// 约 2.5s 后再补一发 "archive|inner" 切到用户点的那张图，成功后关掉
// 整包那个窗口。用于"未加密但格式版本 0.4"的 7z：竖线单独打不开，
// 整包+竖线两步可以精确定位。全程后台线程，不占 UI。
bool LaunchHoneyviewWholeThenPipe(const std::wstring &honeyviewExe,
                                  const std::wstring &archivePath,
                                  const std::wstring &innerRelPath);

// innerRelPath : 归档内相对路径，用 '/' 或 '\' 分隔都行
// password     : 可以为空；空的时候用空密码试，失败就返回 false 让 7-Zip 走原流程
// launch       : true = 解完直接 ShellExecute；false = 只准备文件（自测用）
PreviewResult PrepareImagePreview(const std::wstring &archivePath,
                                  const std::wstring &innerRelPath,
                                  const std::wstring &password,
                                  bool launch);

// 清理超过 maxAgeHours 的预览目录
void CleanupPreviewDirs(int maxAgeHours);

// 归档关闭时调用：删除这个归档的整个预览目录
void CleanupArchivePreview(const std::wstring &archivePath);

} // namespace custom

#endif
