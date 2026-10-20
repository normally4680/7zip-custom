# 7zip-custom

基于 **7-Zip 26.04** 的轻量修改版，保留原始 7-Zip 的全部解压/压缩能力，并在文件管理器里增加了两个日常更常用的功能：

- **密码库**：常用压缩包的密码统一保管，打开/解压时自动填充。
- **无缝看图**：在压缩包里双击图片，直接用本机看图软件打开，并可滚轮翻遍当前目录的图片，不需要先解压整个包。

所有自定义逻辑都集中在 [`Custom/`](Custom/) 目录，对上游 `CPP/` 的改动只有极少的注入点，便于跟随 7-Zip 升级。

## 下载与使用

1. 打开 [Releases](https://github.com/normally4680/7zip-custom/releases) 下载最新的 `7zip-custom-portable-*.zip`。
2. 右键 zip → 属性 → 勾选“解除锁定”（可选，减少 SmartScreen 提示），然后解压到任意可写目录。
3. 双击 `7zFM.exe` 即可，**不需要任何启动脚本**。
4. 密码库和日志会自动保存在 exe 同目录的 `data\`；换电脑时整个文件夹一起拷走即可。

## 功能说明

### 密码库

- 菜单栏「密码库」提供管理、新增条目、打开数据目录、诊断信息。
- 支持按文件名、通配符（`*.7z`）或完整路径匹配。
- 数据使用 AES-256-GCM 加密，主密钥由 Windows DPAPI（当前用户）保护。
- 打开加密包时优先查库；未命中会弹出本项目的密码框，可勾选“把这个包存进密码库”。
- **限制**：DPAPI 绑定本机当前 Windows 用户，`data\vault.key` / `vault.dat` 不能跨电脑解密。换机前删除这两个文件，到新机器重建即可。

### 无缝看图

- 双击压缩包内的图片，只把**当前目录**的图片批量解到临时目录，再交给默认看图程序。
- 同一目录再次打开直接命中缓存；关闭归档时自动删除该归档的预览临时目录。
- 加密包（包括只加密数据的包）会在需要时弹出密码框，密码只在进程内缓存，勾选后才写入密码库。
- 推荐使用 [Honeyview](https://www.bandisoft.com/honeyview/) 之类的看图软件；也可以编辑 exe 同目录的 `config.ini` 指定程序：

```ini
[viewer]
path=C:\Program Files\Honeyview\Honeyview.exe
```

- Windows「照片」可用鼠标滚轮翻页（该版本方向键不翻页），且只识别当前文件夹。

## 从源码构建

环境要求：

- Windows 10/11 x64
- Visual Studio 2022 或更新版本（“使用 C++ 的桌面开发”工作负载）
- Windows 10/11 SDK

```bat
build-msvc.bat
```

构建脚本会自动定位 Visual Studio（也可提前设置 `VSDIR`），产物在 `_out\x64`。

生成便携包：

```powershell
pwsh Custom/tools/make-portable.ps1 -OutDir dist -Version v1.0.0
```

## 已知限制

- 仅支持 64 位 Windows 10/11。
- 看图只提取当前目录，不跨子目录翻页。
- 发布包未签名，Windows 可能提示“未知发布者”或 SmartScreen：可先解除 zip 锁定，被拦时选择“更多信息 → 仍要运行”。
- 本仓库是修改版，**不是** 7-Zip 官方发行版。

## 目录结构

```
CPP/                7-Zip 上游源码（改动极少）
Custom/Bridge/      对 7-Zip 暴露的 C ABI hook
Custom/Core/        密码库、进程内解压、看图逻辑
Custom/UI/          密码库管理器 / 密码输入框
Custom/portable/    便携包随包文件（说明、配置）
Custom/tools/       打包与诊断脚本
```

更详细的架构、踩坑记录和 ABI 约定见 [`Custom/README.md`](Custom/README.md)。

## 许可证

本项目基于 7-Zip 26.04，遵循其许可证（GNU LGPL 及 unRAR 代码限制），详见 [DOC/License.txt](DOC/License.txt)。
