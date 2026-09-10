# 第一版验收记录

日期：2026-09-10。系统：Windows，NVIDIA GeForce RTX 5070 Ti，驱动 616.64，Vulkan 1.4.351。工程要求 Vulkan 1.3；构建使用 VS 2022 / MSVC 19.44 与 Vulkan SDK 1.4.350.0。

## 构建与正确性

| 检查 | 结果 |
|---|---|
| Debug / Release 编译 | 通过 |
| Debug / Release CTest | 通过；一个 CPU 测试入口内包含多项行为断言 |
| lvjuren.glb | 499,719 三角形、3 张 2048² 内嵌图片；MikkTSpace 生成切线；越界 specularColorFactor 报告并限制范围 |
| house.fbx | 29,811 三角形、6 张图片、3 个材质；传统材质转换警告 |
| grassNew.obj | 480 三角形；不含源材质贴图 |
| plane.glb | 1,499,640 三角形、3 张图片；越界 specular 参数报告 |
| Unicode OBJ 路径 | 临时中文目录内导入通过 |
| 外部贴图丢失 | 保留几何，报告缺失 URI 和默认图片替代 |
| 合成 glTF 资产 | 节点层级、负缩放、UV1/纹理变换、MASK/BLEND、specular、嵌入 RGBA 与规范化稀疏属性断言通过 |
| 必需扩展 | 不支持的 required 扩展拒绝导入 |
| 外部 HDR 场景 | 实际解码 HDR、重建 IBL、渲染并截图通过 |

Debug 最终运行期回归：**0 个 Vulkan 校验错误，stderr 为空**。启用了 Khronos Validation Layer 和同步校验。Release 材质/HDR 回归同样为 0 错误。

最终安装目录 `out/VkEngine` 已从工作区外的当前目录启动，并完成 lvjuren 导入、截图和正常退出，校验错误为 0。house.fbx 也完成了实际 Vulkan 渲染与截图，确认多材质贴图可见，校验错误为 0。

运行期回归验证 GPU 对象 ID 异步读回、背景取消选择、多文件释放回调、加载期间相机变化不改变落点、重复模型共用 AssetHandle、隐藏和镜像对象、保存恢复、缺失资产保留与重新定位、坏文件不替换场景、窗口缩放/最小化恢复、三轮删除重载及分配量回归。

这里的拖放回归调用真实的文件释放处理函数并使用 GLFW 鼠标坐标；未通过自动化操作资源管理器完成跨进程鼠标拖拽。Gizmo 已在截图中确认显示，鼠标拖动手感仍属于人工交互验收范围。材质外观已查看实际截图，未与商业 DCC 渲染结果作像素级比较。

## 30 秒性能测量

最终 Release，2560×1440 完整视口，关闭 VSync、UI 和校验，4× MSAA、HDR IBL、2048² 方向光阴影和地面开启。资产加载完毕后预热 3 秒，每个场景采样 30 秒。十实例沿地面排列且全部在视锥内；相机使用查看全部，模型在画面中比单实例更小。

| 场景 | CPU 平均 / P95 | GPU 平均 / P95 | 采样帧数 | 可见三角形 | GPU 分配 |
|---|---:|---:|---:|---:|---:|
| 1 个 lvjuren | 0.926 / 1.296 ms | 0.880 / 1.263 ms | 32,395 | 499,721 | 369.58 MiB |
| 10 个共享实例 | 2.719 / 3.224 ms | 2.676 / 3.181 ms | 11,033 | 4,997,192 | 369.58 MiB |

两个测试均满足目标 60 FPS 的 16.67 ms 帧预算，且重复实例没有增加模型几何和贴图分配。统计包含地面的 2 个三角形；drawCalls=4 记录阴影、背景及 HDR 物体绘制，未计色调映射全屏三角形。GPU 分配量是应用 VMA 分配总量，不是整个驱动占用。

初次启动到模型可用约 1.49 秒（包含初始化和导入，本机缓存状态下）；不作为其他资产或硬件的加载时间保证。CPU 数据包含帧循环与 GPU 等待。GPU 时间来自 Vulkan timestamp，不能直接等同于所有场景的帧率保证。

## 证据与复现

工作区 `captures` 保存完整日志、截图和 JSON：

- `integration-final.stdout.log` / `integration-final.stderr.log` / `integration-final.json`
- `materials-final.png` / `materials-final.json`
- `benchmark-1.json` / `benchmark-10.json` 及对应截图、stdout/stderr

精简 JSON 证据也保存在 `docs/results`，随交付包附带。使用 `tools/validate.ps1 -Model <lvjuren.glb> -Benchmark` 可重新执行。CPU 测试使用 `ctest --preset debug` 和 `ctest --preset release`。
