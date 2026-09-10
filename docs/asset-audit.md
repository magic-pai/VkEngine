# 全目录资产解析与渲染检查

检查日期：2026-09-10。递归扫描 `D:\VSproject\GlEngine\assets\model`，共 12 个模型入口；PNG/JPEG、MTL、BIN 与授权说明作为依赖，不作为独立模型。

**结果：11/12 解析并显示模型，1/12 导入失败。成功出图不等于 PBR/坐标/动画完全还原。12 次运行 Vulkan validationErrors 均为 0，stderr 为空，没有超时。**

使用既有 Release 查看器，逐模型独立运行 `--smoke --validate`，自动取景并截图；CPU 导入入口单独运行。不是同一场景同时加载测试，也不是长时间性能测试。

| 模型 | 模型三角形 | 导入图像 | 结果 |
|---|---:|---:|---|
| `49_moved.fbx` | 498,379 | 3 | 可渲染 / 材质近似 |
| `bag\backpack.obj` | 67,907 | 1 | 部分还原 |
| `dinosaur\source\Rampaging T-Rex.glb` | — | — | 导入失败 |
| `Fist Fight B.fbx` | 12,626 | 2 | 静态可渲染 |
| `grass.fbx` | 8 | 0 | 仅几何 |
| `grassNew.obj` | 480 | 0 | 仅几何和顶点色 |
| `house.fbx` | 29,811 | 6 | 可渲染 / 材质近似 |
| `lvjuren.glb` | 499,719 | 3 | 可渲染 |
| `plane.glb` | 1,499,640 | 3 | 可渲染 |
| `poCity\scene.gltf` | 987,024 | 1 | 可渲染 / 放置待核对 |
| `texture_pbr_v128_moved.fbx` | 3,072,938 | 3 | 可渲染 / 材质近似 |
| `tian\tian.fbx` | 376,029 | 3 | 可渲染 / 变换待核对 |

## 逐项观察

- **49_moved.fbx：** 机甲几何和颜色贴图可见；3 张图像，传统材质按 PBR 近似转换。模型尺度较大，未与源 DCC 比较。
- **backpack.obj：** 背包可见，但仅加载 diffuse.jpg。MTL 的 map_Bump 和 map_Ks 未完整映射；目录内 AO/roughness 未被 MTL 引用。原说明还注明 specular.jpg 由 metallic 改名，不能直接按名称保证 PBR 语义。
- **Rampaging T-Rex.glb：** 必需扩展 KHR_materials_pbrSpecularGlossiness 未实现。文件另有 1 个 skin 和 5 段动画。截图仅是空场景，不能算渲染成功。
- **Fist Fight B.fbx：** 角色几何和贴图可见；2 张图像，传统材质近似。含动画，仅展示静态节点姿态，不验证蒙皮或动画。
- **grass.fbx：** 8 个三角形、0 张图像，画面是白色交叉卡片，未显示草纹理和透明裁剪。
- **grassNew.obj：** 480 个三角形、0 张图像；OBJ 无 mtllib/usemtl，顶点记录含红色分量。红色卡片来自顶点色，不是正确草材质。
- **house.fbx：** 建筑、贴图和阴影可见；6 张图像、3 个材质，传统材质近似转换。
- **lvjuren.glb：** 499,719 三角形、3 张内嵌贴图。越界 specularColorFactor 依既有规则限制并报告。
- **plane.glb：** 1,499,640 三角形、3 张贴图，飞行器可见。越界 specular 参数依既有规则限制并报告。
- **scene.gltf：** poCity 城市可见，外部 BIN/JPEG 读取成功，使用 KHR_materials_unlit。截图显示主体与地面有距离，需核对模型包围盒/源数据和放置逻辑。
- **texture_pbr_v128_moved.fbx：** 披萨店场景可见；3,072,938 三角形、35 个网格、3 张图像；传统材质近似转换。
- **tian.fbx：** 建筑组件可见，3 张图像。自动放置偏移很大，当前朝向/尺度需对照源文件确认；目录中有 5 张命名 PBR 贴图，当前仅 3 张进入材质链路，不宣称完整还原。

## 优先处理项

1. 支持恐龙所需的 Specular/Glossiness glTF 材质工作流；动画播放仍是单独范围。
2. 完善 OBJ/FBX 传统材质到 PBR 的映射，重点是背包法线、高光/金属度语义和外部贴图绑定。
3. 草资产需要有效草贴图与透明材质输入；不应仅凭几何加载成功判断还原正确。
4. 对照源软件核对 tian 的单位/朝向及城市模型的包围盒落地行为。

## 可复现证据

所有最终截图、CPU 日志、stdout/stderr 和 JSON 位于 `captures/asset-audit-final`。`report.html` 为内嵌缩略图的独立报告，`contact-sheet.png` 为 12 张画面的总览。JSON 中 triangles 包含地面 2 个三角形；上表采用 CPU 导入的模型三角形数。

复跑：

```powershell
.\tools\audit-assets.ps1 -Directory 'D:\VSproject\GlEngine\assets\model' -OutputDirectory 'D:\VSproject\VkEngine\captures\asset-audit-rerun'
```

测试过程中修正了 CPU 测试共用固定临时目录的问题：现在每次运行使用独立目录，避免上次失败残留导致误报。最终结果来自修正后的完整重跑。未修改模型源文件或生产导入/渲染逻辑。
