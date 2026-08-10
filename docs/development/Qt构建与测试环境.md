# Qt构建与测试环境

## 1. 说明

- 本文记录的是党欢电脑上已经确认可用的固定路径，不代表团队所有电脑都使用相同目录。
- 记录固定路径的目的是让智能体在对应电脑上直接构建，避免每次搜索 Qt、编译器、CMake、Ninja 或读取大量环境信息。
- 当前电脑存在下列路径时直接使用，不重新搜索、下载或安装其他工具。
- 团队其他成员电脑路径不同时，优先复用其已有的 `CMakeCache.txt` 和 Qt Creator 构建套件，不把本文路径强行套用到其他电脑。
- 不得对只用于容纳多个构建目录的外层 `build` 目录执行构建或 CTest。

## 2. 团队其他电脑使用规则

- 本文中的 `D:\Tool`、`D:\project` 和 `/opt/Qt/qt6.8` 均为党欢电脑的本地固定路径，不是项目依赖或团队统一目录。
- 只有当前电脑对应路径实际存在时，智能体才直接使用本文固定命令。
- 团队其他电脑优先复用当前仓库已有的 `CMakeCache.txt`、Qt Creator Kit和本机工具链。
- 其他电脑没有有效构建缓存时，应根据本机Qt、编译器和源码目录生成独立构建目录，不照搬本文绝对路径。
- 本地Qt路径、编译器路径和构建目录不得写入 `CMakeLists.txt` 或其他项目公共配置。
- `CMakeCache.txt`、Ninja/JOM生成文件和构建产物属于本地环境，不得提交。
- 不同生成器必须使用不同构建目录，禁止在同一缓存中切换Ninja、JOM、MinGW或其他生成器。

## 3. Linux固定Qt路径

党欢Linux电脑使用Qt 6.8.3 GCC 64位环境：

```text
Qt6_DIR=/opt/Qt/qt6.8/6.8.3/gcc_64/lib/cmake/Qt6
qt-cmake=/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake
```

需要新配置工程时直接调用：

```bash
/opt/Qt/qt6.8/6.8.3/gcc_64/bin/qt-cmake \
  -S . \
  -B build-codex-linux \
  -DCMAKE_BUILD_TYPE=Debug
```

构建：

```bash
cmake --build build-codex-linux
```

测试：

```bash
ctest --test-dir build-codex-linux --output-on-failure
```

Linux环境可能运行在8GB内存虚拟机中，不确认机器资源时不得默认使用全部处理器核心进行高并发构建或测试。

## 4. Windows固定工具路径

党欢Windows电脑同时保留以下两套Qt环境：

- Qt 6.8.3 + MSVC 2022 64位，用于当前版本开发和Qt 6兼容验证。
- Qt 5.12.8 + MSVC 2017 64位，用于工控机版本兼容验证。

Qt 6.8.3环境保留Ninja与JOM两套构建方式；Qt 5.12.8环境使用VS2017附带的Ninja。

```text
源码目录：
D:\project\CompositeRobot\code\C++\robot_visual

Visual Studio环境脚本：
D:\Tool\AInstall\VS2022\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat

CMake：
D:\Tool\AInstall\CMake\bin\cmake.exe

CTest：
D:\Tool\AInstall\CMake\bin\ctest.exe

Qt：
D:\Tool\AInstall\Qt\6.8.3\msvc2022_64

Ninja：
D:\Tool\AInstall\VS2022\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

JOM：
D:\Tool\AInstall\Qt\Tools\QtCreator\bin\jom\jom.exe

Qt 5.12.8：
D:\Tool\AInstall\Qt5.12.8\5.12.8\msvc2017_64

Qt 5.12.8 CMake目录：
D:\Tool\AInstall\Qt5.12.8\5.12.8\msvc2017_64\lib\cmake\Qt5

Visual Studio 2017环境脚本：
D:\Tool\AInstall\VS2017\Community\VC\Auxiliary\Build\vcvars64.bat

MSVC 2017编译器：
D:\Tool\AInstall\VS2017\Community\VC\Tools\MSVC\14.16.27023\bin\HostX64\x64\cl.exe

Visual Studio 2017附带的Ninja：
D:\Tool\AInstall\VS2017\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
```

以下命令均在 `cmd.exe` 中执行。

## 5. Windows推荐方式：MSVC与Ninja

Ninja使用独立构建目录：

```text
D:\project\CompositeRobot\code\C++\robot_visual\build-codex-msvc-ninja
```

### 5.1 配置

```bat
call "D:\Tool\AInstall\VS2022\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

D:\Tool\AInstall\CMake\bin\cmake.exe ^
  -S D:/project/CompositeRobot/code/C++/robot_visual ^
  -B D:/project/CompositeRobot/code/C++/robot_visual/build-codex-msvc-ninja ^
  -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="D:/Tool/AInstall/VS2022/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" ^
  -DCMAKE_PREFIX_PATH=D:/Tool/AInstall/Qt/6.8.3/msvc2022_64 ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DBUILD_TESTING=ON
```

### 5.2 构建

```bat
call "D:\Tool\AInstall\VS2022\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

D:\Tool\AInstall\CMake\bin\cmake.exe ^
  --build D:/project/CompositeRobot/code/C++/robot_visual/build-codex-msvc-ninja ^
  --config Debug
```

### 5.3 测试

```bat
set PATH=D:\Tool\AInstall\Qt\6.8.3\msvc2022_64\bin;D:\project\CompositeRobot\code\C++\robot_visual\3rd\HuaYansdk\HuayanRobotLibrary-C++-V1.0.15.0\MSVC;%PATH%
set QT_QPA_PLATFORM=offscreen

D:\Tool\AInstall\CMake\bin\ctest.exe ^
  --test-dir D:/project/CompositeRobot/code/C++/robot_visual/build-codex-msvc-ninja ^
  --output-on-failure ^
  -C Debug
```

## 6. Windows兼容方式：Qt Creator与JOM

Qt Creator现有构建目录为：

```text
D:\project\CompositeRobot\code\C++\robot_visual\build\Desktop_Qt_6_8_3_MSVC2022_64bit_Debug
```

该目录使用 `NMake Makefiles JOM`。如果目录中已经存在有效的 `CMakeCache.txt`，直接构建：

```bat
call "D:\Tool\AInstall\VS2022\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

set PATH=D:\Tool\AInstall\Qt\Tools\QtCreator\bin\jom;D:\Tool\AInstall\Qt\6.8.3\msvc2022_64\bin;%PATH%

D:\Tool\AInstall\CMake\bin\cmake.exe ^
  --build D:/project/CompositeRobot/code/C++/robot_visual/build/Desktop_Qt_6_8_3_MSVC2022_64bit_Debug ^
  --config Debug
```

运行测试：

```bat
set PATH=D:\Tool\AInstall\Qt\6.8.3\msvc2022_64\bin;D:\project\CompositeRobot\code\C++\robot_visual\3rd\HuaYansdk\HuayanRobotLibrary-C++-V1.0.15.0\MSVC;%PATH%
set QT_QPA_PLATFORM=offscreen

D:\Tool\AInstall\CMake\bin\ctest.exe ^
  --test-dir D:/project/CompositeRobot/code/C++/robot_visual/build/Desktop_Qt_6_8_3_MSVC2022_64bit_Debug ^
  --output-on-failure ^
  -C Debug
```

Ninja和JOM必须使用不同构建目录，不得在已有缓存中切换生成器。

## 7. Windows工控机兼容环境：Qt 5.12.8、MSVC 2017与Ninja

Qt Creator当前已经验证通过的构建目录为：

```text
D:\project\CompositeRobot\code\C++\build-robot_visual-Desktop_Qt_5_12_8_MSVC2017_64bit-Debug
```

该目录已使用Qt 5.12.8、MSVC 2017和Ninja生成。目录内存在有效的
`CMakeCache.txt` 时，不要重新配置，直接执行：

```bat
call "D:\Tool\AInstall\VS2017\Community\VC\Auxiliary\Build\vcvars64.bat"

set PATH=D:\Tool\AInstall\Qt5.12.8\5.12.8\msvc2017_64\bin;D:\project\CompositeRobot\code\C++\robot_visual\3rd\HuaYansdk\HuayanRobotLibrary-C++-V1.0.15.0\MSVC;%PATH%

D:\Tool\AInstall\CMake\bin\cmake.exe ^
  --build D:/project/CompositeRobot/code/C++/build-robot_visual-Desktop_Qt_5_12_8_MSVC2017_64bit-Debug ^
  --config Debug
```

需要新建Qt 5.12.8构建目录时，使用独立目录并执行：

```bat
call "D:\Tool\AInstall\VS2017\Community\VC\Auxiliary\Build\vcvars64.bat"

D:\Tool\AInstall\CMake\bin\cmake.exe ^
  -S D:/project/CompositeRobot/code/C++/robot_visual ^
  -B D:/project/CompositeRobot/code/C++/build-robot_visual-Desktop_Qt_5_12_8_MSVC2017_64bit-Debug ^
  -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="D:/Tool/AInstall/VS2017/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" ^
  -DCMAKE_PREFIX_PATH=D:/Tool/AInstall/Qt5.12.8/5.12.8/msvc2017_64 ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DBUILD_TESTING=ON
```

运行测试：

```bat
set PATH=D:\Tool\AInstall\Qt5.12.8\5.12.8\msvc2017_64\bin;D:\project\CompositeRobot\code\C++\robot_visual\3rd\HuaYansdk\HuayanRobotLibrary-C++-V1.0.15.0\MSVC;%PATH%
set QT_QPA_PLATFORM=offscreen

D:\Tool\AInstall\CMake\bin\ctest.exe ^
  --test-dir D:/project/CompositeRobot/code/C++/build-robot_visual-Desktop_Qt_5_12_8_MSVC2017_64bit-Debug ^
  --output-on-failure ^
  -C Debug
```

Qt 5.12.8、Qt 6.8.3以及Ninja、JOM必须分别使用各自的构建目录。
出现生成器或缓存目录不匹配错误时，应删除对应的失效构建目录后重新配置，
不得复制或改写其他构建目录中的`CMakeCache.txt`。

## 8. 使用原则

- 具体构建目录包含有效 `CMakeCache.txt` 时直接构建，不重复配置。
- 构建Qt 5.12.8时必须先加载VS2017环境；构建Qt 6.8.3时必须先加载VS2022环境，不得混用编译器。
- CTest必须在包含 `CTestTestfile.cmake` 的实际构建目录运行。
- Windows测试找不到Qt或华沿SDK运行时DLL时，先检查本文测试命令中的 `PATH`。
- 测试被操作系统策略或外部依赖阻止时，应报告为“未运行”，不得报告为“测试失败”或“测试通过”。
- 交付时分别说明构建结果、实际运行的测试数量、通过数量、失败数量和未运行原因。
