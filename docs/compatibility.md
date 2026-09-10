# 第一版格式兼容表

“支持导入”表示能转换静态三角网格；非 PBR 原始格式通常无法包含完整金属粗糙度材质。

| 类别 | 首版行为 | 验证范围 |
|---|---|---|
| GLB / glTF 2.0 | 专用 tinygltf 导入器；节点、实例、多材质、内嵌/外部图片、稀疏/交错 accessor、规范化属性、UV0/UV1、顶点颜色；三角形/条带/扇形 | lvjuren、plane 和合成材质/变换回归资产 |
| FBX | Assimp 静态节点与多材质；内嵌/外部贴图；优先读取金属粗糙度；传统材质近似转换 | house.fbx |
| OBJ / MTL | 静态几何、材质组、外部图片；传统材质近似转换 | grassNew.obj、Unicode 临时 OBJ |
| DAE / 3DS / PLY / STL / 其他 Assimp importer | 静态网格与 Assimp 可识别的材质属性 | 库级支持，尚未逐格式验证 |
| BLEND | 由 Assimp 的 Blender importer 尝试读取；不承诺兼容新版 Blender 工程及节点材质 | 未验证，建议导出 GLB |
| USD / CAD 原生工程 | 不提供专用导入链路 | 不支持 |

## glTF 材质

支持基础色、金属度/粗糙度、法线、AO、自发光、透明度、双面；MR 贴图读取 G=粗糙度、B=金属度。基础色、自发光和 specularColor 以 sRGB 采样；其他贴图为线性数据。图片缺失或解码不支持时输出提示；不能读取模型本身时导入失败，既有场景保留。

支持的扩展：

- `KHR_materials_specular`：因子与贴图；超出 [0,1] 的因子限制到有效范围并报告。
- `KHR_texture_transform`：偏移、缩放、旋转和 UV 集覆盖，支持 UV0/UV1。
- `KHR_materials_unlit`、`KHR_materials_emissive_strength`。
- `KHR_mesh_quantization`：支持导入器已实现的量化标量/向量属性。

不支持 Draco、Meshopt 压缩 glTF、KTX2/BasisU、clearcoat、transmission、volume、sheen 等专用材质模型。未知必需扩展拒绝导入；未知可选扩展报告后使用基础材质。骨骼/动画不求值，仅显示文件中的静态节点姿态并报告，不保证变形动画的某一帧外观。

PNG/JPEG 等常规图片由 stb_image 解码；glTF 路径支持 8-bit 图片。HDR 环境仅支持 Radiance `.hdr`，不含 EXR。FBX/OBJ 的独立不透明度、凹凸高度、DCC 专属材质节点未完整转换。图片文件名推断不替代源文件的材质映射。

## 坐标与文件

内部使用右手 Y-up，长度按米解释。glTF 遵循规范；Assimp 路径使用可识别的单位/坐标元数据，缺失时默认一单位一米。材质 UV 与法线约定通过各导入器统一，保留节点层级与镜像变换。由于不同导出器的 FBX 元数据可能不完整，遇到异常尺寸时通过对象缩放修正。

Windows 文件访问采用 Unicode 路径；外部图片以源文件目录解析，找不到时再尝试该目录下的文件名。场景文件优先记录相对场景所在目录的路径。最大单文件读取限制为 2 GiB，纹理尺寸受 GPU 能力限制，上传前检查显存余量。
