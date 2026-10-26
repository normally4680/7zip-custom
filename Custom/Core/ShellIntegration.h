// Custom/Core/ShellIntegration.h
//
// 便携版右键菜单：为当前 Windows 用户注册 7-zip.dll 壳扩展（HKCU），
// 不需要管理员权限，也不影响其他用户。

#ifndef CUSTOM_SHELL_INTEGRATION_H
#define CUSTOM_SHELL_INTEGRATION_H

#include <string>

namespace custom {

// 返回 true = 当前用户已注册到本目录的 7-zip.dll；registeredDll 为注册路径。
bool IsShellContextMenuRegistered(std::wstring &registeredDll);

// 注册/取消当前用户的右键菜单。失败时 error 里有可读原因。
bool RegisterShellContextMenu(std::wstring &error);
bool UnregisterShellContextMenu(std::wstring &error);

} // namespace custom

#endif
