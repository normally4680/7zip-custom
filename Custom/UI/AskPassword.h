// Custom/UI/AskPassword.h
//
// "输入密码"对话框：替代 7-Zip 自带的那个。
// 相比原版多两样东西：
//   1. 下拉列表里能直接选密码库里存的密码
//   2. "记住这个压缩包的密码" 是可选勾选，不再偷偷记录每一个包

#ifndef CUSTOM_ASK_PASSWORD_H
#define CUSTOM_ASK_PASSWORD_H

#include <windows.h>

#include <string>

// 返回：1 = 用户确定，outPassword 有值
//       0 = 拿不到输入（对话框没建起来等），调用方应回退到 7-Zip 原生流程
//      -1 = 用户点了取消，调用方应当中止
int AskPasswordDialog(HINSTANCE inst, HWND owner, const wchar_t *archivePath,
                      std::wstring &outPassword);

#endif
