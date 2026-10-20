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
│   ├── PasswordVault.*     加密密码库的读写与匹配
│   └── ImagePreview.*      看图：批量提取同目录图片到临时区
├── UI/                     独立 UI 层（一份代码，两个宿主）
│   ├── VaultDialog.*       对话框逻辑（核心）
│   ├── VaultApp.cpp        宿主①：7zVault.exe 独立管理器
│   ├── VaultUiExports.cpp  宿主②：7zVaultUi.dll，被 7zFM 进程内调用
│   ├── Vault.rc            我们自己的资源，完全不碰 7-Zip 的 .rc
│   └── resource.h
├── lang/zh-cn.txt          官方简体中文语言包（从 26.04 安装包提取）
├── Tests/
│   ├── VaultTest.cpp       密码库自测（22 项断言）
│   ├── UiProbe.cpp         菜单注入自测（6 项断言）
│   └── PreviewTest.cpp     看图提取自测（含缓存/错误密码）
└── makefile                Custom.lib / 7zVault.exe / 7zVaultUi.dll / 测试
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

## 三、上游到底被改了什么（6 个文件，每处都很小）

| 文件 | 改动 |
|---|---|
| `CPP/7zip/UI/Common/ArchiveOpenCallback.cpp` | `COpenCallbackImp::CryptoGetTextPassword` 里加一段：先从库取密码；用户手输了密码则记下来。约 20 行 |
| `CPP/7zip/UI/FileManager/ExtractCallback.cpp` | `CExtractCallbackImp::CryptoGetTextPassword` 同样的两段（针对"只有数据加密、头没加密"的包） |
| `CPP/7zip/UI/FileManager/MyLoadMenu.cpp` | 1 个 include + 1 行调用：菜单重建完挂上"密码库" |
| `CPP/7zip/UI/FileManager/PanelItemOpen.cpp` | 1 个 include + 12 行：打开条目时先问看图模块 |
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

## 五、菜单是怎么挂上去的

关键取舍：**不修改 7-Zip 的菜单资源**（`.rc` / `IDM_*` / 语言文件），全部运行时注入。

1. `MyLoadMenu()` 在"For each menu rebuild"结尾调 `CustomHook_AttachMainMenu(g_HWND)`
2. 我们的实现用 `InsertMenu` 在"帮助"之前插一个弹出菜单，命令 ID 用 `0x7A01..0x7A03`
   （避开 7-Zip 的 `IDM_*` 区间）
3. 用 `SetWindowSubclass` 截 `WM_COMMAND`，命中我们的 ID 就自己处理、返回 0
4. 幂等：插入前先扫一遍菜单，已有"密码库"就跳过 —— 否则切换界面语言时
   菜单会重建，容易插出两个
5. 点"管理密码库"时 `LoadLibrary("7zVaultUi.dll")` → 调 `CustomUi_ShowVault`
   → 用完 `FreeLibrary`

把 UI 做成 DLL 而不是编进 exe，是为了让 `.rc` 只属于我们：7-Zip 的 `.rc` 一行没动，
上游改资源也不会冲突。

## 六、官方中文

`Custom/lang/zh-cn.txt` 是从官方 **26.04 安装包**里提取的（`Lang\zh-cn.txt`，
24.05 修订版），构建时 stage 到 `_out\x64\Lang\`。

7-Zip 的加载顺序是：注册表 `HKCU\Software\7-Zip\Lang` → 系统语言自动匹配
→ 都没有就用内置英文。所以只要放对位置，中文 Windows 上会自动变中文；
想手动切换：工具 → 选项 → 语言。

## 七、看图模块（feature/image-preview）

目标：在 7zFM 里双击图片，用 Honeyview 打开后能**滚轮翻遍整包图片**，但不需要
把整个压缩包解开。

做法：`CPanel::OpenItemInArchive` 在创建临时目录前调用
`CustomHook_TryOpenImage(archivePath, relPath, password)`：

1. 判断扩展名是不是图片（jpg/png/gif/webp/tiff/avif/heic/psd…），不是就返回 0；
   归档内路径里带 `..`、盘符、冒号的一律拒绝（防止恶意归档写到临时目录之外）
2. 临时目录按**归档路径**做哈希：`%TEMP%\7zip-custom\preview\<hash>\<归档内目录>`
   - 同一归档的所有目录共用一个根；归档关闭时整个根删除
   - 同一个目录第二次打开直接命中缓存（0 ms）；0 字节或比归档还旧的残留文件不算缓存
3. 优先**进程内用 7z.dll** 批量解出该目录下所有图片（不启动子进程、密码不上命令行）；
   进程内不支持的特殊格式才退回同目录的 `7z.exe`
4. `ShellExecute` / 关联命令 / 系统照片查看器 依次尝试打开被点的那张，
   工作目录设成该临时目录
5. 看图软件按"同目录兄弟文件"翻页 → 整包图片都能滚

失败分两类（这是调试时最容易被误导的地方）：

| 类型 | 例子 | 处理 |
|---|---|---|
| 业务失败 | 密码不对/需要密码 | 自己弹密码框重试；用户取消则返回 1，不再走 7-Zip |
| 环境失败 | 建不了临时目录 / 起不了子进程 | **自己弹框说清楚**并返回 1 |

为什么环境失败不能退回 7-Zip：它的 `CTempDir::Create("7zO")` 会再失败一次，
弹出来的只是"拒绝访问。"——用户完全不知道发生了什么。我们宁可自己解释。

排查工具：`_out\x64\diagnose.bat`，双击会把环境检查 + hook 日志打出来。

**踩过的坑**：`CFolderLink.Password` 只保存"用户在对话框里输入"的密码。
如果密码是密码库直接喂给 `CryptoGetTextPassword` 的，7-Zip 不会把它存进
`CFolderLink`，看图时拿到的就是空密码 —— 加密包必然解不出来。
所以 `CustomHook_TryOpenImage` 在 password 为空时会**自己再查一次库**。

已知取舍：
- 兜底到 7z.exe 时密码仍会进命令行（仅当进程内方式打不开该格式）。
- 预览目录优先 `%TEMP%`，建不了再退 `%LOCALAPPDATA%`；关闭归档时删除本归档的预览根，
  另外按 72 小时清理历史残留。

## 八、跨模块 ABI 约定（改 Bridge 前必读）

1. **只用 C ABI**，函数显式 `__cdecl`。7-Zip 用 `/Gr`（默认 fastcall），
   不写死调用约定会在链接期"看起来正常、运行期栈错乱"。
2. **只传 POD**：`const wchar_t*`、`int`、调用方给的缓冲区。
3. **不跨边界传 C++ 对象**，不跨边界分配/释放内存。谁分配谁释放。
4. 调用方必须能在"外挂模块返回 0"时安全退化 —— 所有 hook 都是"可选增强"。

编译参数刻意与 7-Zip 对齐：`/MT /GS- /EHsc /Zc:wchar_t`。
静态 CRT 一致，内存不会跨到另一个堆上。

## 九、密码库设计

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

## 十、构建

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

## 十一、踩过的坑（别再踩）

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
8. **从 `_out` 直接跑 exe 可能是 Low 完整性**。本机
   `D:\MicrosoftVisualStudioProjects` 整棵树被沙箱/安全工具标了
   `Mandatory Label\Low`，里面的 exe 启动后会以 Low IL 运行：写
   `%TEMP%`/`%LOCALAPPDATA%` 返回 err 5、`ShellExecute` 返回 5、创建出来的子进程
   也显示不出窗口。诊断信息里的“完整性”一栏能直接看到。
   对策：`run.bat` 先把运行时复制到 `%LOCALAPPDATA%\7zip-custom\app`（无标签）再
   启动；本地跑测试也先把 exe 拷到 `%TEMP%`（或从 APPDIR 跑）。
9. **预览临时目录的生命周期**：目录按归档分根
   （`preview\<归档哈希>\<内部目录>`）。上游 `CPanel::CloseOneLevel` 会调
   `CustomHook_OnArchiveClosed` 把该归档的整个预览根删掉；`CPanel::OpenRootFolder`
   在直接 `Clear()` 父链前也会逐个清理。72 小时定期清理只作兜底
   （看图程序占用文件时可能删不掉）。
10. **只加密数据、不加密文件名的归档**：打开时 7-Zip 不会问密码，预览时才需要。
   `ExtractImagesInProcess` 现在用 `bool &passwordRequested` 报告；预览 hook 会弹一次
   密码框并重试，密码只在进程内缓存（最多 8 个归档），勾选“保存到密码库”才落盘。
   错误框里的 7z.exe 命令用 `-p"***"` 屏蔽，日志也不再打印明文密码。
11. **改了 FM 源文件后 7zFM 报 C4599**（`/I` 与预编译头不匹配，`/WX` 下变错误）：
   删掉 `CPP/7zip/Bundles/Fm/x64` 重建即可；旧 PCH 不要留。
12. **关闭 7zFM 后便携包目录删不掉**：直接双击包内 `7zFM.exe` 时 CWD = 包目录，
   调起的 UWP Photos 会继承这个 CWD；UWP/AppContainer 的目录句柄在非提权
   Resource Monitor 里常看不到。`LaunchViewer` 用 `ScopedCurrentDir` 在启动看图
   程序期间把 CWD 切到预览临时目录，`TryWindowsPhotoViewer` 也显式传 image 目录。

## 十二、便携包与发布（2026-10-20）

- 打包：`pwsh Custom/tools/make-portable.ps1 -OutDir dist -Version v1.0.0`。
  从 `_out\x64` 复制运行时，加上 `Custom/portable/` 的随包文件，生成
  `dist\7zip-custom-portable-<version>.zip` 和对应的 `.sha256.txt`。
- 发布：推送 `v*` tag 会触发 `.github/workflows/release.yml`，在
  windows-latest 上全量构建并创建 GitHub Release。
- **发布包不做数字签名**（长期策略）。Windows 可能提示“未知发布者”或
  SmartScreen：先解除 zip 锁定，被拦时选“更多信息 → 仍要运行”。
- 构建产物在 `_out` 里带 Low 完整性标签，**不要直接运行**（写不了 TEMP）；
  本地自测先复制到无标签目录（如 `%LOCALAPPDATA%\7zip-custom\app`）。
  打包脚本只复制文件，不受影响。
- **正式便携包没有 `run.bat`**：解压后直接双击 `7zFM.exe`。`VaultDir()` /
  `DebugLog()` / `ReadConfigValue()` 会自动识别 exe 同目录的 `data\` 和
  `config.ini`：有 `data\` 就把密码库/日志放进去，有 `config.ini` 就先读它。
- `run.bat` 只保留给开发版 `_out`（复制到 `%LOCALAPPDATA%\7zip-custom\app`，
  绕开工作区 Low 完整性标签），**不进正式包**。
- **密码库不能跨电脑**：主密钥用 DPAPI（CurrentUser）保护，换机前删
  `data\vault.key`/`vault.dat`。日志可以随包走。
- 2026-10-20 早期测试包用过自签名证书（`CN=7zip-custom`）；该证书不再用于发布，
  PFX 留在 `%LOCALAPPDATA%\7zip-custom\signing\`，**不要提交**。

## 十三、后续可做

- 控制台接入：`CPP/7zip/UI/Console/OpenCallbackConsole.cpp` 的
  `Open_CryptoGetTextPassword`，加上 `Console/makefile` 的 LIBS 一行。
- 压缩时也记密码：`CPP/7zip/UI/GUI/UpdateCallbackGUI.cpp`。
- 主密码模式：`vault.dat` 里 `flags` 置位，`vault.key` 改成 KDF 派生。
- 看图：`CustomHook_TryOpenImage` 的实现在 `feature/image-preview` 分支。
