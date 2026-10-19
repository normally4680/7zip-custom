// Custom/UI/VaultDialog.h
//
// 密码库对话框：同一份代码被两个宿主复用
//   * 7zVault.exe       独立的密码库管理器
//   * 7zVaultUi.dll     注入到 7zFM.exe 进程内，由菜单"密码库"调起
// 资源来自同一个 Vault.rc，所以两边外观完全一致。

#ifndef CUSTOM_VAULT_DIALOG_H
#define CUSTOM_VAULT_DIALOG_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

// mode 0 = 打开管理器；mode 1 = 打开管理器并立刻弹出"新增条目"
// suggestName 可以为 NULL，用于预填归档名称
int ShowVaultDialogBox(HINSTANCE inst, HWND owner, int mode, const wchar_t *suggestName);

#endif
