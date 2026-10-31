# 项目说明

一个截屏用的工具，可以获取当前屏幕上的内容，能够保存到文件、剪贴板等地方

# 开发环境

Windows 11, C++ 20, CMake

环境为Windows简体中文版，各种工具的输出有极大概率为简体中文，编码可能是UTF-8可能是GBK，注意识别。

环境中带有 bash、python、powershell 等工具，必要时可以使用以提升工作效率

# 开发建议

类要少对外暴露内部成员，只暴露调用所需的方法。.h 中的类为虚类、方法为纯虚方法，.cpp 中实现。这样在包含 .h 的时候就不会引入不必要的头文件。

# 项目结构

## 捕捉模块

采用 Windows RT API 获取当前屏幕的一帧内容，scRGB浮点（单精度或者半精度均可）采样格式，连带宽高等元数据保存到一个对象中

## 系统信息获取模块

获取系统中当前HDR的最高亮度、SDR白点值等信息

## 预览模块

一个全屏展示的窗口，内部使用 ANGLE 进行 HDR 渲染，分为矩形选中区域和未选中区域部分，其中选中区域显示原始画面，未选中区域显示为“原始画面压缩到SDR亮度范围后、降低80%亮度”；如果不存在选中区域，则全屏显示原始画面。

## 裁剪模块

允许鼠标在预览画面上操作，操作分为以下几种

### 框选

将选中区域设置为对角线的两个点为鼠标按下时的位置 and 鼠标当前位置组成的矩形

### 修改

如果鼠标在选中区域的矩形四个角上，则修改这个角的位置；
如果鼠标在选中区域的矩形边上，则修改这个边的位置；

### 完成选择

键盘的 Enter 键或者鼠标在选中区域内部双击都是完成选择的操作；键盘的 ESC 或者鼠标右键是取消操作

## 输出模块

后续补充

## 依赖库（deps/）

| 库 | 头文件 | 库文件 | 说明 |
| --- | --- | --- | --- |
| ANGLE | deps/include/{EGL,GLES*,KHR} | deps/lib/libEGL.lib, libGLESv2.lib (+ bin/*.dll) | HDR 预览渲染 |
| zlib | deps/include/zlib.h | deps/lib/zlib.lib | zlib1.dll 的导入库 |
| libultrahdr v2.0.2 | deps/include/ultrahdr_api.h | deps/lib/uhdr-static.lib + jpeg-static.lib | Ultra HDR (JPEG + gainmap) 编解码，静态库 |

libultrahdr 构建参数（与主工程保持一致，都是 x64 + /MD）：

- 生成器 `Visual Studio 18 2026`，`-A x64`
- `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`
- `-DBUILD_FOR_WINUI=ON`：libultrahdr 在 MSVC + `UHDR_BUILD_DEPS=ON` 时会强制静态库并把运行库改成 /MT，这个开关让它保留 /MD
- `-DUHDR_BUILD_DEPS=ON`（自带 libjpeg-turbo 静态库）、`-DUHDR_ENABLE_HEIF=OFF`、examples/tests/benchmark/fuzzers/java 全关
- 产物：`uhdr-static.lib`（core + image_io 合并）、`jpeg-static.lib`（需在 uhdr-static 之后一起链接）、`ultrahdr_api.h`
- 静态构建无需任何宏；Release/Debug 两套分别放 deps/lib 与 deps/debug/lib
- 示例与文档参考：`refcode/libultrahdr/`（ultrahdr_app.cpp、docs、LICENSE、COMMIT.txt）


# 开发进度记录

如果一个工作项开发完成，请勾选。需求完成后，请自动将其添加到git中提交；
如果todo项导致了上述文档的描述变更，请自动更新文档。

[x] 环境搭建，生成cmake脚本，添加空的 main.cpp，并链接到angle。依赖库在deps文件夹内，可先使用 dir /s/b 或等效命令查看
[x] 提交到git，注意deps中的二进制文件均需要使用LFS。提交的默认用户名已经设置好了，提交信息请自己生成
[x] 开发屏幕捕捉模块，参考代码已经在 refcode/ 文件夹下。我们不需要捕捉单个窗口的功能、只需要全屏幕捕捉。暂时假设显示器只有一个。保存捕捉到的结果的类请自己设计。
[x] 添加了开发建议，按照开发建议重新修改 ScreenCapture.h 和 ScreenCapture.cpp 文件。
[x] 屏幕捕捉后保存的数据为 scRGB 浮点格式的内存数据，故需要将纹理下载到内存中。
[x] 开发预览模块，在捕捉画面之后弹出一个全屏窗口，展示画面内容。使用ANGLE渲染HDR的示例代码在 refcode/ 中。
[x] 预览模块、框选模块的功能变更了，适配新的变更
[x] 程序没有声明高DPI支持导致显示的截屏很糊，需要声明高DPI支持
[x] 预览模块的功能变更了，适配新的变更
[x] 实现功能：在预览画面中框选时，如果鼠标拖拽选中区域，则功能为移动选中区域；
[x] 实现功能：在预览画面中如果已经有选中部分，鼠标拖拽选框的四个脚或者四个边，要有对应功能；为了方便操作，四个角和四个边允许在鼠标按下时有一定误差（定义为常量，目前暂定允许偏离3像素）
[x] 有时预览窗口会卡死没有响应，请添加用于排查故障的日志输出（日志为每次启动程序都会覆盖上次的结果）
[x] 在预览窗口中，选中区域需要使用红色线条描绘
[x] 在调整选中区域时，鼠标按下时的允许偏差改为 3*DPI缩放倍速
[x] 需求变更：当鼠标在选中区域内部双击时，功能等同于 enter 键，即完成区域的选择
[x] 需求变更：当鼠标点击右键时，功能等同于 esc 键，即取消选择
[x] 修复框选拖拽时操作不跟手的问题：预览改为按需重绘，并在拖拽过程中临时关闭 VSync，结束拖拽后恢复
[x] 新增输出功能：完成选区后，将区域内容转换为 BT.2020 HLG（0~1，1=1000nit），再按 BT.2020 BT.1886 到 BT.709 BT.1886 转换，裁剪到 0.0~1.0，转为 8bit 后写入剪贴板
[x] 修复 HDR 剪贴板输出中 HLG 输入亮度错误使用 SDR 白点参与缩放而导致的过曝问题，并补充日志说明 scRGB 绝对亮度基准
[x] 修复问题：截取hdr内容并压缩范围、复制到剪贴板的时候，hdr内容里相当于系统“白色”亮度的部分，会很暗、不是正常白色亮度的值
[x] 添加新模式：将hdr到sdr的转换提前到捕捉屏幕后、选择要裁剪的区域前，并将显示预览的窗口初始化时的模式从hdr改为sdr（8bit整数）；仅在截图时大小写锁定键处于大写锁定状态，则使用这种模式。[x] 修复问题：HDR 转 SDR 进入剪贴板时纯白达不到 1.0。高光压缩的终点改为选区（或整帧）实际峰值而非固定 1000nit；CapsLock 提前转换模式在没有超过 SDR 白点的内容时与输出模块一致走线性 sRGB 路径
[x] HDR→SDR 逻辑整体替换为 ITU-R BT.2446-1 方法 C（参数取方法默认值，与 obs-bt2446 工程的 method C 默认参数一致）：新增 Bt2446MethodC（CPU 实现，供 CapsLock 提前转换使用），输出模块改为在 compute shader 中实现同一套方法 C，GPU/CPU 共用同一份参数；输入侧确认 scRGB 1.0=80nit 与参考工程一致，输出侧因本项目写 8bit 而非线性光，额外补了一次 sRGB OETF；同时更新 flow.md 的管线描述
[x] 方法 C 的高光不再固定裁成纯白：把选区（CapsLock 模式下为整帧）实测峰值作为映射锚点代入 k2/k3/k4 的求解（`Bt2446MethodC::CreateForHdrPeak`，锚点夹在 203~4000 nits）。确认参考实现里的 `hdr_peak_nits` 在方法 C 中是“先除后乘”的空转参数，直接填实测峰值不会改变任何像素，峰值只能通过锚点影响曲线
[x] 引入 libultrahdr 2.0.2：在临时目录用 VS 2026 x64 + /MD 构建静态库，产物（ultrahdr_api.h、uhdr-static.lib、jpeg-static.lib，Release/Debug 各一套）放入 deps 并接入 CMakeLists；示例与文档留存在 refcode/libultrahdr/
