// Custom/Tests/TestIids.cpp
//
// 7z.dll 用的那些 IID，在 7zFM 里由 7-Zip 自己的目标文件提供；
// 但我们的测试 exe 不链接 7-Zip，所以在这里补一份定义。

#include <initguid.h>

#include "../../CPP/7zip/Archive/IArchive.h"
#include "../../CPP/7zip/IPassword.h"
#include "../../CPP/7zip/IStream.h"
