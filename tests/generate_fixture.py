"""Generate a tiny, self-contained glTF material/transform regression asset."""
import base64
import json
import struct
import zlib
from pathlib import Path


def png():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = b'\x00' + bytes([255, 50, 20, 255, 40, 255, 40, 0])
    pixels += b'\x00' + bytes([40, 80, 255, 255, 255, 255, 255, 255])
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 2, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


data = bytearray()
views = []
accessors = []


def attribute(values, components, shape, component_type=5126):
    while len(data) % 4:
        data.append(0)
    offset = len(data)
    for value in values:
        data.extend(struct.pack('<' + ('f' if component_type == 5126 else 'H') * components, *value))
    views.append({'buffer': 0, 'byteOffset': offset, 'byteLength': len(data) - offset})
    accessors.append({'bufferView': len(views) - 1, 'componentType': component_type,
                      'count': len(values), 'type': shape})
    return len(accessors) - 1


position = attribute([(-.5, 0, 0), (.5, 0, 0), (.5, 1, 0), (-.5, 1, 0)], 3, 'VEC3')
accessors[position].update(min=[-.5, 0, 0], max=[.5, 1, 0])
normal = attribute([(0, 0, 1)] * 4, 3, 'VEC3')
uv = attribute([(0, 1), (1, 1), (1, 0), (0, 0)], 2, 'VEC2')
uv1 = attribute([(0, 0), (0, 1), (1, 1), (1, 0)], 2, 'VEC2')
indices = attribute([(i,) for i in [0, 1, 2, 0, 2, 3]], 1, 'SCALAR', 5123)
color_offset = len(data)
data.extend(bytes([255] * 16))
views.append({'buffer': 0, 'byteOffset': color_offset, 'byteLength': 16})
color_view = len(views) - 1
views.append({'buffer': 0, 'byteOffset': len(data), 'byteLength': 1})
sparse_index_view = len(views) - 1
data.extend(bytes([3, 0, 0, 0]))
views.append({'buffer': 0, 'byteOffset': len(data), 'byteLength': 4})
data.extend(bytes([128, 255, 128, 255]))
accessors.append({'bufferView': color_view, 'componentType': 5121, 'normalized': True,
                  'count': 4, 'type': 'VEC4', 'sparse': {'count': 1,
                  'indices': {'bufferView': sparse_index_view, 'componentType': 5121, 'byteOffset': 0},
                  'values': {'bufferView': len(views) - 1, 'byteOffset': 0}}})
color = len(accessors) - 1
texture = {'index': 0, 'texCoord': 1, 'extensions': {
    'KHR_texture_transform': {'offset': [.1, .2], 'scale': [2, 2], 'rotation': .3}}}
materials = [
    {'name': 'Mask / UV1 transform', 'doubleSided': True, 'alphaMode': 'MASK', 'alphaCutoff': .5,
     'pbrMetallicRoughness': {'baseColorTexture': texture, 'metallicFactor': 0, 'roughnessFactor': .8}},
    {'name': 'Blend / mirrored node', 'alphaMode': 'BLEND', 'doubleSided': True,
     'pbrMetallicRoughness': {'baseColorFactor': [.1, .5, 1, .45], 'metallicFactor': .1, 'roughnessFactor': .2}},
    {'name': 'Metal / specular', 'doubleSided': False,
     'pbrMetallicRoughness': {'baseColorFactor': [.9, .6, .1, 1], 'metallicFactor': 1, 'roughnessFactor': .2},
     'extensions': {'KHR_materials_specular': {'specularFactor': .5, 'specularColorFactor': [1, .8, .6]}}},
]
asset = {
    'asset': {'version': '2.0', 'generator': 'VkEngine regression fixture'},
    'extensionsUsed': ['KHR_texture_transform', 'KHR_materials_specular'],
    'scene': 0, 'scenes': [{'nodes': [0]}],
    'nodes': [{'name': 'Root', 'translation': [0, .2, 0], 'children': [1, 2, 3]},
              {'name': 'Masked', 'mesh': 0, 'translation': [-1.2, 0, 0]},
              {'name': 'Mirrored blend', 'mesh': 1, 'scale': [-1, 1, 1]},
              {'name': 'Metal', 'mesh': 2, 'translation': [1.2, 0, 0]}],
    'meshes': [{'primitives': [{'attributes': {'POSITION': position, 'NORMAL': normal,
                                              'TEXCOORD_0': uv, 'TEXCOORD_1': uv1, 'COLOR_0': color},
                               'indices': indices, 'material': i}]} for i in range(3)],
    'buffers': [{'byteLength': len(data), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode()}],
    'bufferViews': views, 'accessors': accessors, 'materials': materials,
    'images': [{'name': 'RGBA quadrant', 'uri': 'data:image/png;base64,' + base64.b64encode(png()).decode()}],
    'textures': [{'source': 0}],
}
output = Path(__file__).parent / 'fixtures' / 'materials.gltf'
output.parent.mkdir(exist_ok=True)
output.write_text(json.dumps(asset, indent=2) + '\n', encoding='utf-8')

# Old-style RGBE pixels are valid Radiance data and keep this fixture dependency-free.
hdr = b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n'
hdr += bytes([128, 160, 200, 129]) * 16
(output.parent / 'studio-test.hdr').write_bytes(hdr)
scene = {
    'version': 1,
    'objects': [{'id': 1, 'name': 'Material regression', 'asset': 'materials.gltf',
                 'position': [0, -.2, 0], 'rotation': [0, 0, 0], 'scale': [1, 1, 1], 'visible': True}],
    'camera': {'target': [0, .5, 0], 'yaw': .1, 'pitch': .1, 'distance': 4.5},
    'environment': {'asset': 'studio-test.hdr', 'rotation': .3, 'intensity': 1, 'exposure': -1,
                    'lightDirection': [-.5, -1, -.4], 'lightColor': [1, .94, .84],
                    'lightIntensity': 3, 'ground': True},
}
(output.parent / 'materials.vkscene').write_text(json.dumps(scene, indent=2) + '\n', encoding='utf-8')
