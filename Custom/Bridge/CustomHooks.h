// Custom/Bridge/CustomHooks.h
//
// ============================================================================
//  这是唯一被 7-Zip 上游源码 #include 的头文件。
//  改动它 = 需要重新审视上游 patch，所以请保持"只增不改"。
//
//  铁律（跨模块 ABI 约定）：
//   1. 只用 C ABI，函数显式 __cdecl（7-Zip 用 /Gr，默认调用约定会变）
//   2. 只传 POD：const wchar_t*、int、调用方提供的缓冲区
//   3. 不跨边界传递 C++ 对象，不跨边界 new/delete、malloc/free
//   4. 任何 hook 都必须能在"外挂模块整个不存在"时安全退化（返回值 0）
// ============================================================================

#ifndef CUSTOM_HOOKS_H
#define CUSTOM_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

#define CUSTOM_HOOK_API __cdecl

// ---------------------------------------------------------------------------
// 密码库
// ---------------------------------------------------------------------------

// 在弹出 7-Zip 自带密码框之前调用，尝试从密码库取密码。
//   archivePath : 归档主卷完整路径（UTF-16，非 NULL）
//   buf / cch   : 调用方提供的缓冲区（宽字符个数，含结尾 0）
// 返回 1 = 已填入，调用方应直接用 buf，不再弹框
// 返回 0 = 未命中/未启用，调用方走原有流程
int CUSTOM_HOOK_API CustomHook_TryPassword(const wchar_t *archivePath,
                                           wchar_t *buf, int cch);

// 弹出"输入密码"对话框（我们自己的：可以从密码库选，也可以勾选记住）。
// 返回 1 = 用户确定，buf 有值；0 = 用不了（DLL 缺失等），调用方回退原生流程；
//      -1 = 用户取消。
// owner 用 void* 传（HWND），NULL 时自己找前台窗口；决定对话框在谁身上居中
int CUSTOM_HOOK_API CustomHook_AskPassword(void *owner, const wchar_t *archivePath,
                                           wchar_t *buf, int cch);

// 用户手动输入密码且被归档接受后调用，用于"记住密码"。
void CUSTOM_HOOK_API CustomHook_OnPasswordAccepted(const wchar_t *archivePath,
                                                   const wchar_t *password);

// ---------------------------------------------------------------------------
// 看图
// ---------------------------------------------------------------------------

// 文件管理器准备"解压单个条目到临时目录再用外部程序打开"之前调用。
//   archivePath : 当前归档路径（嵌套归档时是已被解出来的外层级临时文件）
//   itemRelPath : 条目在归档内的相对路径（用 / 分隔）
// 返回 1 = 外挂模块已接管（自己解压 + 自己调起看图程序），上游直接 return
// 返回 0 = 不接管，走 7-Zip 原有单文件解压流程
// password 可以是 NULL（归档未加密，或拿不到密码）
int CUSTOM_HOOK_API CustomHook_TryOpenImage(const wchar_t *archivePath,
                                            const wchar_t *itemRelPath,
                                            const wchar_t *password);

// 7zG（独立的解压/压缩进程）用：在开始处理前记录当前归档路径，
// 之后它的密码框就能查密码库。传 NULL 表示清空。
void CUSTOM_HOOK_API CustomHook_SetCurrentArchive(const wchar_t *archivePath);
int CUSTOM_HOOK_API CustomHook_TryPasswordForCurrent(wchar_t *buf, int cch);
int CUSTOM_HOOK_API CustomHook_AskPasswordForCurrent(void *owner, wchar_t *buf, int cch);

// 文件管理器主窗口菜单：7zFM 每次重建菜单后调用，可重复调用（内部会去重）。
// 在菜单栏"帮助"之前插入"密码库(&P)"。返回 1 = 已挂载。
// hWnd 用 void* 传，保持这个头文件不依赖 windows.h。
int CUSTOM_HOOK_API CustomHook_AttachMainMenu(void *hWnd);

// 归档成功打开后调用（供后续功能挂载，当前为空实现）
void CUSTOM_HOOK_API CustomHook_OnArchiveOpened(const wchar_t *archivePath);

#ifdef __cplusplus
}
#endif

#endif // CUSTOM_HOOKS_H
