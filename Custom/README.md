# Custom/ —— 7-Zip 定制功能外挂层

这个目录是**唯一**存放我们自己代码的地方。目标是：官方 7-Zip 发新版本时，
`git rebase upstream/main` 只需要处理极少数几行，而不是一场合并灾难。

## 一、总体结构

```
Custom/
├── Bridge/                 与 7-Zip 的唯一接触面
│   ├── CustomHooks.h       纯 C ABI 的 hook 声明（上游源码 include 它）
│   └── CustomHooks.cpp     参数校验 + 加锁 + 分发给 Core
├── Core/                   纯逻辑，不依赖 7-Zip 任何类型
│   ├── VaultCrypto.*       AES-256-GCM（BCrypt）+ DPAPI
│   └── PasswordVault.*     加密密码库的读写与匹配
├── UI/                     独立 UI 层
│   ├── VaultApp.cpp        密码库管理器主程序
│   ├── Vault.rc            我们自己的资源，完全不碰 7-Zip 的 .rc
│   └── resource.h
├── Tests/
│   └── VaultTest.cpp       无界面自测（22 项断言）
└── makefile                Custom.lib / 7zVault.exe / VaultTest.exe
```

## 二、为什么要这么分层

| 层 | 依赖 7-Zip | 上游升级时会怎样 |
|---|---|---|
| `Core/` | 否 | 完全不受影响 |
| `UI/` | 否 | 完全不受影响（独立 EXE + 独立 .rc） |
| `Bridge/` | 只依赖自己 | 头文件只增不改，几乎不受影响 |
| 上游被改的 2 个文件 | —— | 每处 1 个 include + 1 个调用 |

密码库管理器做成**独立 EXE**（`7zVault.exe`）而不是 7-Zip 里的对话框，原因：

1. 不用改 7-Zip 的菜单、命令 ID、`.rc`、`Lang` 资源 —— 这些是上游变动最频繁的部分
2. 7-Zip 进程里不出现我们的窗口代码，出问题也不会拖垮解压
3. 可以单独编译、单独调试、单独发布

## 三、上游到底被改了什么（4 个文件，共 4 处）

| 文件 | 改动 |
|---|---|
| `CPP/7zip/UI/Common/ArchiveOpenCallback.cpp` | `COpenCallbackImp::CryptoGetTextPassword` 里加一段：先从库取密码；用户手输了密码则记下来。约 20 行 |
| `CPP/7zip/UI/FileManager/ExtractCallback.cpp` | `CExtractCallbackImp::CryptoGetTextPassword` 同样的两段（针对"只有数据加密、头没加密"的包） |
| `CPP/7zip/UI/Console/makefile`<br>`CPP/7zip/UI/GUI/makefile`<br>`CPP/7zip/Bundles/Fm/makefile` | 各 1 行：`LIBS = $(LIBS) ...Custom.lib bcrypt.lib crypt32.lib` |

三个 makefile 都要加，因为 `ArchiveOpenCallback.cpp` 会被 7z.exe / 7zG.exe / 7zFM.exe
**三个可执行文件**分别编译一次。少了哪个，哪个就链接失败（`LNK2019 CustomHook_*`）。

> **为什么要放在 `ReOpenCallback` 之前**：加密包第一次打开必然失败，7-Zip 会把
> `ReOpenCallback` 指向 UI 回调后重试。如果 hook 写在 `ReOpenCallback` 分支之后，
> 重试就绕过我们直接弹框了 —— 现象是"日志里能看到函数被调用，但密码框照弹"。

> **为什么不能只用 `FileNames[0]`**：`FileNames` 是在 `GetStream()` 里才填的，
> 要密码的时候它还是空的。所以先用 `FileNames`，空了就用
> `_folderPrefix + _fileInfo.Name`（`Init2` 里 `Find_FollowLink` 记下的名字）。

## 四、已实测通过的链路

```
$ Z7CUSTOM_VAULT_DIR=D:\tmp\vault 7z.exe l enc.7z        # 库里没有
Enter password (will not be echoed): ******              # 手输一次
1 files                                                  # 打开成功

$ Z7CUSTOM_VAULT_DIR=D:\tmp\vault 7z.exe l enc.7z        # 再开一次
1 files                                                  # 直接进，无提示
```

调试时把 `Z7CUSTOM_DEBUG` 设成日志文件路径（或 `1` 写到 `%TEMP%`），
hook 每次命中/未命中都会记一行，这是排查"到底有没有进我们的代码"最快的办法。

## 五、跨模块 ABI 约定（改 Bridge 前必读）

1. **只用 C ABI**，函数显式 `__cdecl`。7-Zip 用 `/Gr`（默认 fastcall），
   不写死调用约定会在链接期"看起来正常、运行期栈错乱"。
2. **只传 POD**：`const wchar_t*`、`int`、调用方给的缓冲区。
3. **不跨边界传 C++ 对象**，不跨边界分配/释放内存。谁分配谁释放。
4. 调用方必须能在"外挂模块返回 0"时安全退化 —— 所有 hook 都是"可选增强"。

编译参数刻意与 7-Zip 对齐：`/MT /GS- /EHsc /Zc:wchar_t`。
静态 CRT 一致，内存不会跨到另一个堆上。

## 六、密码库设计

存储位置：`%LOCALAPPDATA%\7zip-custom\`

| 文件 | 内容 |
|---|---|
| `vault.key` | 32 字节随机主密钥，用 **DPAPI**（当前用户作用域）加密 |
| `vault.dat` | `7ZPV` + 版本 + IV + GCM tag + AES-256-GCM 密文的条目表 |

- **为什么用 DPAPI 而不是主密码**：零操作成本。文件被拷到别的机器/别的 Windows
  账户都解不开。要跨设备同步时再升级成主密码 + Argon2/PBKDF2 模式，格式里已经
  留了 `flags` 字段。
- **GCM 而非 CBC**：带完整性校验，密文被改一个字节直接解密失败，不会解出乱码后
  继续当密码用。
- **匹配规则**（优先级从高到低）：完整路径精确 → 文件名精确 → 通配（`*`、`?`）。
  全部大小写不敏感，`BACKUP.7Z` 和 `backup.7z` 是同一条。

环境变量 `Z7CUSTOM_VAULT_DIR` 可以重定向库目录（便携模式 / 自动化测试用）。

## 七、构建

```bash
build-msvc.bat            # 全部：Custom.lib + 7z.dll/7z.exe/7zG.exe/7zFM.exe + 7zVault + 测试
build-msvc.bat custom     # 只编 Custom.lib
build-msvc.bat vault      # 只编 7zVault.exe 与 VaultTest.exe
build-msvc.bat 7zfm       # 只编 7zFM.exe
```

产物统一在 `_out\x64\`。

跑自测：

```bash
cd _out\x64 && ./VaultTest.exe     # 需要在可写的当前目录下运行
```

## 八、踩过的坑（别再踩）

1. **makefile 里不要写中文注释**。nmake 按 ANSI 代码页读 makefile，UTF-8 中文
   会让它把**下一行**吞掉或错位，症状是"某个变量莫名其妙没生效"。
   上游 makefile 里那行注释已改成英文，`Custom/makefile` 也全英文。
2. **nmake 会把 makefile 宏导出成环境变量**给子进程。所以宏不能叫 `LIB`、
   `LINK`、`RC` —— 我们一开始写了 `LIB = lib`，结果 `link` 找不到 `user32.lib`。
   现在叫 `LIB_TOOL` / `LINK_TOOL`。
3. **批处理必须 CRLF + 纯 ASCII**。
4. **被上游编译的源文件里不要写中文注释**。7-Zip 用 `/WX`，`C4819`（代码页 936
   下无法表示的字符）会直接变成编译错误。我们的对策：
   - 上游文件里的注释写英文
   - `Custom/Bridge/CustomHooks.h` 带 UTF-8 BOM（这样中文注释也安全）
   - `Custom/Core/*.cpp` 用自己的 `/utf-8` 编译
5. **改了 `Custom.lib` 之后必须强制重链接**。nmake 只认 makefile 里写明的依赖，
   而我们的库是塞在 `LIBS` 里的，对象文件没变、exe 比 obj 新，nmake 就认为
   "已经是最新的"直接跳过链接 —— 于是你改了 hook 却毫无反应。
   `build-msvc.bat` 现在每次构建前都会 `del` 掉目标 exe 来强制重链接。
6. **`$(PROG): ...Custom.lib` 这种依赖规则不生效**。真正的链接目标是
   `$(PROGPATH)`（`x64\7z.exe`），不是 `$(PROG)`，而且 `PROGPATH` 在我们的
   makefile 文本执行时还没定义。别绕了，直接在构建脚本里删产物。
7. **`::ListView_xxx` 不行**。commctrl 的 `ListView_*` 是宏，前面加 `::` 会变成
   `::(BOOL)SNDMSG(...)`，报 `C2589: "(": "::" 右边的非法标记`。

## 九、后续可做

- 控制台接入：`CPP/7zip/UI/Console/OpenCallbackConsole.cpp` 的
  `Open_CryptoGetTextPassword`，加上 `Console/makefile` 的 LIBS 一行。
- 压缩时也记密码：`CPP/7zip/UI/GUI/UpdateCallbackGUI.cpp`。
- 主密码模式：`vault.dat` 里 `flags` 置位，`vault.key` 改成 KDF 派生。
- 看图：`CustomHook_TryOpenImage` 的实现在 `feature/image-preview` 分支。
