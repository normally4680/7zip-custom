// Custom/Core/ImagePreview.h
//
// 无缝看图：不解压整个包，只把"当前目录里的图片"批量解到临时目录，
// 再把用户点的那张交给系统默认看图程序（Honeyview / 照片 …）。
// 看图软件自己按"同目录兄弟文件"实现滚轮翻页，于是就能翻遍整包图片。
//
// 为什么用 7z.exe 而不是 7z.dll：
//   * 复用的是同一个引擎，行为和解压完全一致（含加密、分卷、嵌套）
//   * 不用在我们这边重新实现 IInArchive / IArchiveExtractCallback
//   * 代价是进程启动开销（几十毫秒），对"看一张图"完全可接受

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
  std::wstring command;      // 实际调用的 7z.exe 命令行（排错用）
};

bool IsImageFile(const wchar_t *name);

// innerRelPath : 归档内相对路径，用 '/' 或 '\' 分隔都行
// password     : 可以为空；空的时候用空密码试，失败就返回 false 让 7-Zip 走原流程
// launch       : true = 解完直接 ShellExecute；false = 只准备文件（自测用）
PreviewResult PrepareImagePreview(const std::wstring &archivePath,
                                  const std::wstring &innerRelPath,
                                  const std::wstring &password,
                                  bool launch);

// 清理超过 maxAgeHours 的预览目录
void CleanupPreviewDirs(int maxAgeHours);

} // namespace custom

#endif
