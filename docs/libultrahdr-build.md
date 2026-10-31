# libultrahdr 构建与兼容性说明

本项目以**预编译静态库**形式使用 libultrahdr，产物放在 `deps/`，源码不入库。
本文件记录构建参数、兼容性约束和踩过的坑，便于日后升级版本时复现。

## 1. 版本

| 项 | 值 |
| --- | --- |
| 上游 | https://github.com/google/libultrahdr.git |
| 版本 | 2.0.2 |
| commit | `5f11729`（构建时 `--depth 1` 拉取的 HEAD，见 `refcode/libultrahdr/COMMIT.txt`） |
| 许可证 | Apache-2.0 / MIT，见 `deps/ultrahdr-LICENSE.txt` |

## 2. 与主工程的兼容性目标

主工程：`Visual Studio 18 2026`（v145）、x64、`/MD`（Debug 为 `/MDd`）、C++20。
libultrahdr 侧必须完全对齐，否则会出现 `LNK2038` / `LNK4098` 或跨 CRT 堆操作崩溃。

| 维度 | 主工程 | libultrahdr 构建时的取值 |
| --- | --- | --- |
| 平台 | x64 | `-A x64` |
| 生成器 | Visual Studio 18 2026 | 相同 |
| 运行库 | MultiThreadedDLL / MultiThreadedDebugDLL | 同左（见第 3 节的两个必需开关） |
| C++ 标准 | 20 | 上游固定 17，ABI 兼容（只暴露 C API） |
| 链接形态 | — | 静态库（`uhdr-static.lib` + `jpeg-static.lib`） |

## 3. 构建命令

```bat
set http_proxy=
set HTTP_PROXY=
set https_proxy=
set HTTPS_PROXY=

cmake -S libultrahdr -B build -G "Visual Studio 18 2026" -A x64 ^
  -DBUILD_FOR_WINUI=ON ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY="MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" ^
  -DUHDR_BUILD_DEPS=ON ^
  -DUHDR_ENABLE_HEIF=OFF ^
  -DUHDR_BUILD_EXAMPLES=OFF ^
  -DUHDR_BUILD_TESTS=OFF ^
  -DUHDR_BUILD_BENCHMARK=OFF ^
  -DUHDR_BUILD_FUZZERS=OFF ^
  -DUHDR_BUILD_JAVA=OFF ^
  -DUHDR_ENABLE_INSTALL=OFF

cmake --build build --config Release --target uhdr-static -- /m
cmake --build build --config Debug   --target uhdr-static -- /m
```

### 参数为什么这么给

- **`-DBUILD_FOR_WINUI=ON`（关键）**：上游 CMake 在 `MSVC AND UHDR_BUILD_DEPS` 成立时会强制
  `BUILD_SHARED_LIBS=FALSE`，并把 `CMAKE_MSVC_RUNTIME_LIBRARY` 改成 `MultiThreaded$<$<CONFIG:Debug>:Debug>`
  （即 `/MT`）。`BUILD_FOR_WINUI` 是上游为此留的逃生口——走到 `elseif(MSVC AND BUILD_FOR_WINUI)`
  分支后会保留父工程传入的 `/MD`。副作用仅是关掉 `UHDR_ENABLE_STATIC_LINKING`（对 MSVC 本来就没用）。
- **显式传 `CMAKE_MSVC_RUNTIME_LIBRARY`（关键）**：上游会把该值原样透传给 libjpeg-turbo 的
  `ExternalProject_Add`。若不传，透传的是空字符串，cl 在无 `/MD` `/MT` 参数时**默认取 `/MT`**，
  libjpeg-turbo 产物即变成 `LIBCMT`，最终链接报 `LNK4098 默认库“LIBCMT”与其他库的使用冲突`。
- **`UHDR_BUILD_DEPS=ON`**：让上游自建 libjpeg-turbo 3.1.0 静态库（`third_party/turbojpeg` 内置源码
  + ExternalProject），避免依赖系统里的 jpeg。
- **`UHDR_ENABLE_HEIF=OFF`**：HEIF/AVIF 需要额外克隆并打补丁 libheif，本项目只用 JPEG+gainmap，
  关掉可减少依赖与构建时间，也避免 `UHDR_ENABLE_*` 宏差异。
- **examples/tests/benchmark/fuzzers/java 全关**：只需要库。
- **`UHDR_ENABLE_INSTALL=OFF`**：`UHDR_BUILD_DEPS=ON` 时上游本来就会强制关闭，显式写上更清楚。

## 4. 产物与落位

| 产物 | 来源 | 目标位置 |
| --- | --- | --- |
| `ultrahdr_api.h` | 仓库根目录 | `deps/include/ultrahdr_api.h` |
| `uhdr-static.lib` | `build/Release/uhdr-static.lib` | `deps/lib/uhdr-static.lib` |
| `jpeg-static.lib` | `build/turbojpeg/src/turbojpeg-build/Release/jpeg-static.lib` | `deps/lib/jpeg-static.lib` |
| `uhdr-static.lib`（debug） | `build/Debug/...` | `deps/debug/lib/uhdr-static.lib` |
| `jpeg-static.lib`（debug） | `build/turbojpeg/src/turbojpeg-build/Debug/...` | `deps/debug/lib/jpeg-static.lib` |

注意点：

- `uhdr-static.lib` 只由 `core` + `image_io` 的对象合并而成（`combine_static_libs`），
  **不包含** libjpeg-turbo 的对象，所以链接时必须把 `jpeg-static.lib` 放在其后一起给出。
- 静态构建在 MSVC 下目标名是 `uhdr-static`（上游规则），产物文件名即 `uhdr-static.lib`，不是 `uhdr.lib`。
- 静态库对外无需任何宏：`UHDR_BUILDING_SHARED_LIBRARY` / `UHDR_USING_SHARED_LIBRARY` 都不定义。
- 两个配置必须分别构建（多配置生成器，一次 configure 即可）。
- `deps/**/*.lib` 由 `.gitattributes` 走 Git LFS。

## 5. 验证方式

用与主工程同款编译选项（`/MD`、`/MDd`、`/std:c++20`）写一个最小往返用例：

1. 造 64×48 `UHDR_IMG_FMT_32bppRGBA1010102` + `UHDR_CT_HLG` 的 HDR 缓冲；
2. `uhdr_create_encoder` → `uhdr_enc_set_raw_image(UHDR_HDR_IMG)` → `uhdr_encode` → `uhdr_get_encoded_stream`，
   检查 `is_uhdr_image() == 1`；
3. `uhdr_create_decoder` → `uhdr_dec_set_image` → 输出设置必须是合法配对
   （`HLG`/`PQ` 配 `1010102`，`LINEAR` 配 `64bppRGBAHalfFloat`，`SRGB` 配 `32bppRGBA8888`，
   否则报 `UHDR_CODEC_INVALID_PARAM: unsupported output pixel format and output color transfer pair`）→ `uhdr_decode`。

判定标准：链接无 `LNK4098` / `LNK2038`，运行输出 `RESULT: PASS`。Release 与 Debug 都要跑。

## 6. 环境坑（Windows 本机）

- **代理环境变量**：本机同时存在 `HTTPS_PROXY` 与 `https_proxy`，MSBuild 用大小写不敏感字典会抛
  `MSB6001 已添加项。字典中的关键字:"HTTPS_PROXY" 所添加的关键字:"https_proxy"`。构建前清空
  `http_proxy/HTTP_PROXY/https_proxy/HTTPS_PROXY`；下载依赖（git clone、ExternalProject）同样需要。
- **`vcvars64.bat` 不可用**：其中会调用 `reg.exe`，可能被安全策略拦截。验证程序请用 CMake 生成器
  驱动编译，不要在 bat 里 `call vcvars64.bat` 后再调 `cl`。
- **Git Bash 传参**：`cmake --build ... -- /m` 会被 MSYS 路径转换改写成 `M:/`，导致
  `MSB1008 只能指定一个项目`。用 `MSYS2_ARG_CONV_EXCL=*` 或在 cmd 里执行。

## 7. 升级到新版本时的检查单

1. 重新 clone，记录新 commit 到 `refcode/libultrahdr/COMMIT.txt`；
2. 检查上游 CMake 是否仍存在 `BUILD_FOR_WINUI` 分支与 `UHDR_BUILD_DEPS` 的 `/MT` 强制逻辑；
3. 按第 3 节重新构建，用第 5 节用例验证；
4. 覆盖 `deps/include/ultrahdr_api.h` 与四份 `.lib`，跑一次主工程 Release + Debug 构建；
5. 同步更新 `refcode/libultrahdr/` 下的 examples/docs 与 `AGENTS.md` 里的版本号。
