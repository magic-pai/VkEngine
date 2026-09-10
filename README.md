# VkEngine — Vulkan PBR Asset Studio

Windows 上的 Vulkan 1.3 多资产 PBR 查看器。将模型文件拖入视口地面，即可异步加载、摆放、编辑并保存场景。

## 构建与启动

需要 Visual Studio 2022 的 C++ 工具链、CMake 3.24+、Git 和 Vulkan SDK 1.3+。首次配置需要联网下载固定版本依赖。推荐使用 **Developer PowerShell for VS 2022**：

```powershell
cmake --preset windows
cmake --build --preset release --parallel
.\build\Release\VkEngine.exe
```

支持 Debug 构建：`cmake --build --preset debug --parallel`。Debug 默认启用 Vulkan Validation Layer 和同步校验；Release 默认关闭。`--validate` 可在 Release 启用校验。

如果 CMake 尚未加入 PATH，可直接运行 `powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Configuration Release -Test -Package`；脚本会定位 VS 自带的 CMake。

不需要安装 OpenGL 依赖，也不需要原 GlEngine 工程。模型路径通过拖放、文件对话框或命令行传入：

```powershell
.\build\Release\VkEngine.exe --model 'D:\VSproject\GlEngine\assets\model\lvjuren.glb'
```

交付目录可以独立生成：

```powershell
cmake --install build --config Release --prefix out/VkEngine
```

必须保留程序旁的 `shaders` 文件夹。使用 MSVC 动态运行库，其他电脑需安装 Microsoft Visual C++ 2015–2022 x64 Redistributable。依赖许可证随安装包放在 `licenses` 下。

## 操作

| 操作 | 使用方式 |
|---|---|
| 拖入模型 | 从资源管理器拖入中央视口；射线与 Y=0 地面交点作为落点 |
| 多文件拖放 | 第一个模型位于落点，后续模型沿世界 X 轴排列并留出间隔 |
| 导入按钮 | 在当前相机目标对应的地面位置放置资产 |
| 观察 | 鼠标右键旋转，中键平移，滚轮缩放 |
| 选择 | 点击可见模型表面，或点击左侧对象列表；点击背景取消选择 |
| 变换 | W/E/R 切换移动/旋转/缩放；拖动 Gizmo 或编辑右侧数值 |
| 聚焦 | F 聚焦当前对象；Frame all 查看所有可见对象 |
| 删除/隐藏 | Delete 删除；对象列表复选框控制可见性 |
| 场景 | Save / Ctrl+S 保存；Save as / Ctrl+Shift+S 另存；Open scene 打开 |
| 环境 | 调整曝光、IBL 强度、环境旋转和方向光；Load HDR 加载 `.hdr` |
| 截图 | Screenshot 保存到程序目录中的 `screenshot.png` |

拖放时记录释放瞬间的相机射线；加载期间移动相机不会改变落点。释放在面板上或无法命中地面时报告提示。不会自动缩放模型或在每次拖入后移动相机；命令行初次加载会自动取景。

相同文件的多次导入共享 CPU/GPU 资产，各对象拥有独立变换。场景文件使用带版本号的 JSON，保存原文件引用、稳定对象 ID、变换、可见性、相机和环境设置，**不内嵌或复制原始资产**。尽量采用相对路径；移动场景时应一并保留原始模型和贴图。缺失资产保留对象记录，可通过 Relocate asset 重新定位。打开场景会替换当前场景，当前修改应先保存。

## 格式和画质

完整兼容说明见 [docs/compatibility.md](docs/compatibility.md)。重点验证 GLB/glTF、FBX、OBJ；其他 Assimp 格式提供静态网格导入，不等同于完整还原所有材质。

- glTF 金属粗糙度 PBR、法线/AO/自发光、双面、透明裁剪及排序透明混合。
- sRGB/线性贴图区分、mipmap、各向异性过滤、MikkTSpace 切线。
- GGX BRDF、HDR 摄影棚环境、辐照度卷积、预过滤环境高光及 BRDF LUT。
- 方向光、2048² PCF 阴影、地面网格、4× MSAA、曝光与 ACES 风格色调映射。
- 对象 ID 拾取和屏幕轮廓；支持负缩放与多材质节点层级。

第一版不包含动画播放、光追、自动 LOD、CAD/USD、撤销重做和材质节点编辑。透明物体以包围盒中心排序，相交透明物体可能出现混合顺序误差；透明混合材质暂不投射阴影。方向光使用一个覆盖场景的阴影图，极大场景的阴影精度有限。HDR 更换时会等待在途 GPU 工作完成并重建 IBL；普通模型导入按帧分批上传。

## 验证与性能

```powershell
ctest --preset release
.\build\Release\vke_tests.exe 'D:\path\model.glb'
.\build\Release\VkEngine.exe --model 'D:\path\lvjuren.glb' --self-test --validate --report integration.json
.\build\Release\VkEngine.exe --model 'D:\path\lvjuren.glb' --instances 10 --width 2560 --height 1440 --no-ui --no-vsync --no-validation --benchmark 30 --report benchmark.json
```

`--self-test` 是实际 Vulkan 运行期回归，包含 GPU 拾取、模拟文件释放回调、加载期间移动相机、多实例共享、保存恢复、缺失文件重新定位、缩放/最小化及三轮导入删除。它会移动窗口中的鼠标位置、调整窗口大小并最终退出；测试临时场景位于系统临时目录，成功后清理。

`--benchmark` 在资产加载完毕并预热 3 秒后记录指定时长；GPU timestamp 和 CPU 帧循环分别统计平均值/P95，CPU 数据包含提交与必要等待。`--no-ui` 使用完整分辨率视口并关闭选中轮廓，仍保留 4× MSAA、IBL、地面及方向光阴影。测量结果见 [docs/validation.md](docs/validation.md)。

完成两种配置构建后，`tools/validate.ps1 -Model 'D:\path\lvjuren.glb' -Benchmark` 可串行执行 Debug 运行期回归、材质/HDR 场景回归以及两组性能测量，日志和截图写入 `captures`。

## 源码结构

- `Asset`：tinygltf / Assimp → 统一 CPU 资产；图片、材质、节点与网格不依赖 Vulkan。
- `Scene`：对象实例、相机、坐标变换、地面投射与事务式场景文件读写。
- `Renderer`：Vulkan/VMA 资源、双帧同步、16 MiB/帧持久上传缓冲、实例批次、渲染与 GPU 拾取。
- `Platform`：Windows 文件对话框、Unicode 路径和程序目录。
- `main`：后台导入队列、编辑界面、相机输入、资产缓存及回归/测量入口。

着色器在构建时生成 SPIR-V，运行时相对可执行文件定位，不依赖当前工作目录。普通帧循环不使用 `vkDeviceWaitIdle`；该调用仅用于交换链重建和关闭。资源在所属提交完成后释放。
