#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>
#include "Asset.h"
#include <mikktspace.h>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/material.h>
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <numeric>
#include <cmath>
namespace vke {
std::string utf8(const fs::path &p) {
    auto s = p.generic_u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
std::string readFile(const fs::path &p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("Cannot read: " + utf8(p));
    auto n = f.tellg();
    if (n < 0 || n > std::streamoff(2ull << 30))
        throw std::runtime_error("Invalid or oversized file");
    std::string s(size_t(n), '\0');
    f.seekg(0);
    if (!s.empty() && !f.read(s.data(), n))
        throw std::runtime_error("Incomplete read: " + utf8(p));
    return s;
}
void Bounds::add(glm::vec3 p) {
    min = glm::min(min, p);
    max = glm::max(max, p);
}
void Bounds::add(const Bounds &b) {
    if (b.valid()) {
        add(b.min);
        add(b.max);
    }
}
bool Bounds::valid() const {
    return min.x <= max.x && min.y <= max.y && min.z <= max.z;
}
glm::vec3 Bounds::center() const {
    return valid() ? (min + max) * .5f : glm::vec3(0);
}
glm::vec3 Bounds::size() const {
    return valid() ? max - min : glm::vec3(0);
}
Bounds Bounds::transformed(const glm::mat4 &m) const {
    Bounds b;
    if (valid())
        for (int i = 0; i < 8; i++)
            b.add(glm::vec3(
                m * glm::vec4(i & 1 ? max.x : min.x, i & 2 ? max.y : min.y, i & 4 ? max.z : min.z, 1)));
    return b;
}
uint64_t ImportedAsset::triangles() const {
    uint64_t n = 0;
    for (auto &node : nodes)
        for (auto i : node.meshes)
            n += meshes[i].indices.size() / 3;
    return n;
}
size_t ImportedAsset::byteSize() const {
    size_t n = 0;
    for (auto &m : meshes)
        n += m.vertices.size() * sizeof(Vertex) + m.indices.size() * 4;
    for (auto &i : images)
        n += i.rgba.size();
    return n;
}
static glm::vec3 safeNormal(glm::vec3 n) {
    return glm::dot(n, n) > 1e-20f ? glm::normalize(n) : glm::vec3(0, 1, 0);
}
void finishMesh(Mesh &m, bool missingNormals, bool missingTangents) {
    if (m.indices.size() % 3)
        throw std::runtime_error("Non-triangle mesh");
    for (auto i : m.indices)
        if (i >= m.vertices.size())
            throw std::runtime_error("Index outside vertex buffer");
    for (auto &v : m.vertices) {
        if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) || !std::isfinite(v.position.z))
            throw std::runtime_error("Non-finite vertex");
        m.bounds.add(v.position);
        if (missingNormals)
            v.normal = {0, 0, 0};
    }
    if (missingNormals)
        for (size_t i = 0; i < m.indices.size(); i += 3) {
            auto &a = m.vertices[m.indices[i]];
            auto &b = m.vertices[m.indices[i + 1]];
            auto &c = m.vertices[m.indices[i + 2]];
            auto n = glm::cross(b.position - a.position, c.position - a.position);
            a.normal += n;
            b.normal += n;
            c.normal += n;
        }
    for (auto &v : m.vertices)
        v.normal = safeNormal(v.normal);
    if (!missingTangents || m.indices.empty())
        return;
    struct Context {
        Mesh *mesh;
        std::vector<glm::vec4> tangents;
    };
    Context data{&m, std::vector<glm::vec4>(m.indices.size(), {1, 0, 0, 1})};
    SMikkTSpaceInterface api{};
    api.m_getNumFaces = [](const SMikkTSpaceContext *c) {
        return int(static_cast<Context *>(c->m_pUserData)->mesh->indices.size() / 3);
    };
    api.m_getNumVerticesOfFace = [](const SMikkTSpaceContext *, int) { return 3; };
    api.m_getPosition = [](const SMikkTSpaceContext *c, float o[], int f, int v) {
        auto &m = *static_cast<Context *>(c->m_pUserData)->mesh;
        std::memcpy(o, &m.vertices[m.indices[f * 3 + v]].position, 12);
    };
    api.m_getNormal = [](const SMikkTSpaceContext *c, float o[], int f, int v) {
        auto &m = *static_cast<Context *>(c->m_pUserData)->mesh;
        std::memcpy(o, &m.vertices[m.indices[f * 3 + v]].normal, 12);
    };
    api.m_getTexCoord = [](const SMikkTSpaceContext *c, float o[], int f, int v) {
        auto &m = *static_cast<Context *>(c->m_pUserData)->mesh;
        std::memcpy(o, &m.vertices[m.indices[f * 3 + v]].uv0, 8);
    };
    api.m_setTSpaceBasic = [](const SMikkTSpaceContext *c, const float t[], float sign, int f, int v) {
        static_cast<Context *>(c->m_pUserData)->tangents[f * 3 + v] = {t[0], t[1], t[2], sign};
    };
    SMikkTSpaceContext context{&api, &data};
    if (!genTangSpaceDefault(&context))
        throw std::runtime_error("MikkTSpace tangent generation failed");
    struct Key {
        uint32_t index;
        float x, y, z, w;
        bool operator==(const Key &b) const {
            return index == b.index && x == b.x && y == b.y && z == b.z && w == b.w;
        }
    };
    struct Hash {
        size_t operator()(const Key &k) const {
            size_t h = k.index;
            for (float f : {k.x, k.y, k.z, k.w})
                h ^= std::hash<float>{}(f) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<Key, uint32_t, Hash> unique;
    unique.reserve(m.vertices.size());
    std::vector<Vertex> vertices;
    vertices.reserve(m.vertices.size());
    for (size_t i = 0; i < m.indices.size(); i++) {
        auto t = data.tangents[i];
        Key k{m.indices[i], t.x, t.y, t.z, t.w};
        auto [it, added] = unique.emplace(k, uint32_t(vertices.size()));
        if (added) {
            auto v = m.vertices[k.index];
            v.tangent = t;
            vertices.push_back(v);
        }
        m.indices[i] = it->second;
    }
    m.vertices = std::move(vertices);
}
static double number(const tinygltf::Value &o, const char *k, double fallback) {
    return o.Has(k) && o.Get(k).IsNumber() ? o.Get(k).GetNumberAsDouble() : fallback;
}
static double item(const tinygltf::Value &v, size_t i, double fallback) {
    return v.IsArray() && v.ArrayLen() > i && v.Get(int(i)).IsNumber() ? v.Get(int(i)).GetNumberAsDouble()
                                                                       : fallback;
}
static TextureRef texture(const tinygltf::Model &g, int index, int uv, const tinygltf::ExtensionMap &ext) {
    TextureRef r;
    r.uv = uv;
    if (index >= 0) {
        auto &t = g.textures.at(index);
        r.image = t.source;
        if (t.sampler >= 0) {
            auto &s = g.samplers.at(t.sampler);
            r.wrapS = s.wrapS;
            r.wrapT = s.wrapT;
            if (s.minFilter >= 0)
                r.minFilter = s.minFilter;
            if (s.magFilter >= 0)
                r.magFilter = s.magFilter;
        }
    }
    if (auto it = ext.find("KHR_texture_transform"); it != ext.end()) {
        auto &e = it->second;
        r.rotation = float(number(e, "rotation", 0));
        r.uv = int(number(e, "texCoord", uv));
        if (e.Has("offset"))
            r.offset = {item(e.Get("offset"), 0, 0), item(e.Get("offset"), 1, 0)};
        if (e.Has("scale"))
            r.scale = {item(e.Get("scale"), 0, 1), item(e.Get("scale"), 1, 1)};
    }
    if (r.uv < 0 || r.uv > 1)
        throw std::runtime_error("Only TEXCOORD_0 and TEXCOORD_1 are supported");
    return r;
}
static TextureRef extensionTexture(const tinygltf::Model &g, const tinygltf::Value &e, const char *key) {
    if (!e.Has(key))
        return {};
    auto &t = e.Get(key);
    tinygltf::ExtensionMap ext;
    if (t.Has("extensions") && t.Get("extensions").Has("KHR_texture_transform"))
        ext["KHR_texture_transform"] = t.Get("extensions").Get("KHR_texture_transform");
    return texture(g, int(number(t, "index", -1)), int(number(t, "texCoord", 0)), ext);
}
static double component(const unsigned char *p, int type, bool normalized) {
    switch (type) {
    case 5120: {
        int8_t v;
        memcpy(&v, p, 1);
        return normalized ? std::max(-1.0, v / 127.0) : v;
    }
    case 5121:
        return normalized ? *p / 255.0 : *p;
    case 5122: {
        int16_t v;
        memcpy(&v, p, 2);
        return normalized ? std::max(-1.0, v / 32767.0) : v;
    }
    case 5123: {
        uint16_t v;
        memcpy(&v, p, 2);
        return normalized ? v / 65535.0 : v;
    }
    case 5125: {
        uint32_t v;
        memcpy(&v, p, 4);
        return v;
    }
    case 5126: {
        float v;
        memcpy(&v, p, 4);
        return v;
    }
    default:
        throw std::runtime_error("Unsupported accessor component");
    }
}
static std::vector<double> accessor(const tinygltf::Model &g, int id, int expected = 0) {
    auto &a = g.accessors.at(id);
    int n = tinygltf::GetNumComponentsInType(a.type),
        size = tinygltf::GetComponentSizeInBytes(a.componentType);
    if (n <= 0 || size <= 0 || (expected && n != expected) || a.count > 50000000)
        throw std::runtime_error("Invalid accessor shape");
    std::vector<double> out(a.count * n, 0);
    auto copy = [&](int view, size_t offset, size_t count, size_t stride, int type, int components,
                    bool normalized, auto put) {
        auto &v = g.bufferViews.at(view);
        auto &b = g.buffers.at(v.buffer).data;
        size_t bytes = tinygltf::GetComponentSizeInBytes(type) * components;
        if (stride < bytes || v.byteOffset > b.size() || v.byteLength > b.size() - v.byteOffset ||
            offset > v.byteLength ||
            (count &&
             (bytes > v.byteLength - offset || (count - 1) > (v.byteLength - offset - bytes) / stride)))
            throw std::runtime_error("Accessor exceeds buffer");
        for (size_t i = 0; i < count; i++)
            for (int c = 0; c < components; c++)
                put(i, c,
                    component(b.data() + v.byteOffset + offset + i * stride +
                                  c * tinygltf::GetComponentSizeInBytes(type),
                              type, normalized));
    };
    if (a.bufferView >= 0) {
        int stride = a.ByteStride(g.bufferViews.at(a.bufferView));
        if (stride <= 0)
            throw std::runtime_error("Invalid accessor stride");
        copy(a.bufferView, a.byteOffset, a.count, stride, a.componentType, n, a.normalized,
             [&](size_t i, int c, double v) { out[i * n + c] = v; });
    }
    if (a.sparse.isSparse) {
        size_t count = a.sparse.count;
        if (count > a.count)
            throw std::runtime_error("Invalid sparse accessor");
        std::vector<size_t> indices(count);
        copy(a.sparse.indices.bufferView, a.sparse.indices.byteOffset, count,
             tinygltf::GetComponentSizeInBytes(a.sparse.indices.componentType),
             a.sparse.indices.componentType, 1, false, [&](size_t i, int, double v) {
                 if (v < 0 || v >= double(a.count))
                     throw std::runtime_error("Invalid sparse index");
                 indices[i] = size_t(v);
             });
        copy(a.sparse.values.bufferView, a.sparse.values.byteOffset, count, n * size, a.componentType, n,
             a.normalized, [&](size_t i, int c, double v) { out[indices[i] * n + c] = v; });
    }
    return out;
}
static std::shared_ptr<ImportedAsset> importGltf(const fs::path &path) {
    tinygltf::TinyGLTF loader;
    tinygltf::FsCallbacks callbacks{};
    callbacks.FileExists = [](const std::string &p, void *) { return fs::is_regular_file(fs::u8path(p)); };
    callbacks.ExpandFilePath = [](const std::string &p, void *) { return p; };
    callbacks.ReadWholeFile = [](std::vector<unsigned char> *b, std::string *err, const std::string &p,
                                 void *) {
        try {
            auto s = readFile(fs::u8path(p));
            b->assign(s.begin(), s.end());
            return true;
        } catch (const std::exception &e) {
            if (err)
                *err = e.what();
            return false;
        }
    };
    callbacks.WriteWholeFile = [](std::string *, const std::string &, const std::vector<unsigned char> &,
                                  void *) { return false; };
    callbacks.GetFileSizeInBytes = [](size_t *n, std::string *, const std::string &p, void *) {
        std::error_code ec;
        *n = size_t(fs::file_size(fs::u8path(p), ec));
        return !ec;
    };
    loader.SetFsCallbacks(callbacks);
    tinygltf::Model g;
    std::string err, warn;
    auto data = readFile(path);
    auto ext = utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    bool binary = ext == ".glb";
    bool ok = binary ? loader.LoadBinaryFromMemory(&g, &err, &warn,
                                                   reinterpret_cast<const unsigned char *>(data.data()),
                                                   unsigned(data.size()), utf8(path.parent_path()))
                     : loader.LoadASCIIFromString(&g, &err, &warn, data.data(), unsigned(data.size()),
                                                  utf8(path.parent_path()));
    if (!ok)
        throw std::runtime_error("glTF: " + err);
    auto out = std::make_shared<ImportedAsset>();
    out->path = path;
    if (!warn.empty())
        out->warnings.push_back(warn);
    const std::unordered_set<std::string> supported{"KHR_materials_specular", "KHR_texture_transform",
                                                    "KHR_materials_unlit", "KHR_mesh_quantization",
                                                    "KHR_materials_emissive_strength"};
    for (auto &e : g.extensionsRequired)
        if (!supported.contains(e))
            throw std::runtime_error("Unsupported required glTF extension: " + e);
    for (auto &e : g.extensionsUsed)
        if (!supported.contains(e))
            out->warnings.push_back("Optional glTF extension ignored: " + e);
    if (!g.skins.empty() || !g.animations.empty())
        out->warnings.push_back("Animation/skin is not evaluated; displaying static node pose");
    for (auto &im : g.images) {
        ImageData image;
        image.name = im.name;
        image.width = im.width;
        image.height = im.height;
        if (im.bits != 8 || im.component < 1 || im.component > 4 || im.width <= 0 || im.height <= 0 ||
            im.image.size() < size_t(im.width) * im.height * im.component) {
            image.width = image.height = 1;
            image.rgba = {255, 255, 255, 255};
            out->warnings.push_back("Missing/unsupported image: " + im.name + " " + im.uri);
        } else {
            image.rgba.resize(size_t(im.width) * im.height * 4);
            for (size_t i = 0; i < size_t(im.width) * im.height; i++) {
                for (int c = 0; c < 3; c++)
                    image.rgba[i * 4 + c] = im.image[i * im.component + (im.component >= 3 ? c : 0)];
                image.rgba[i * 4 + 3] =
                    im.component == 4 ? im.image[i * 4 + 3] : (im.component == 2 ? im.image[i * 2 + 1] : 255);
            }
        }
        out->images.push_back(std::move(image));
    }
    for (auto &mat : g.materials) {
        Material m;
        m.name = mat.name;
        auto &p = mat.pbrMetallicRoughness;
        m.baseColor = {p.baseColorFactor[0], p.baseColorFactor[1], p.baseColorFactor[2],
                       p.baseColorFactor[3]};
        m.metallic = float(p.metallicFactor);
        m.roughness = float(p.roughnessFactor);
        m.emissive = {mat.emissiveFactor[0], mat.emissiveFactor[1], mat.emissiveFactor[2]};
        m.normalScale = float(mat.normalTexture.scale);
        m.occlusionStrength = float(mat.occlusionTexture.strength);
        m.doubleSided = mat.doubleSided;
        m.alphaMode = mat.alphaMode == "MASK" ? 1 : mat.alphaMode == "BLEND" ? 2 : 0;
        m.alphaCutoff = float(mat.alphaCutoff);
        m.textures[BaseColor] =
            texture(g, p.baseColorTexture.index, p.baseColorTexture.texCoord, p.baseColorTexture.extensions);
        m.textures[MetalRough] =
            texture(g, p.metallicRoughnessTexture.index, p.metallicRoughnessTexture.texCoord,
                    p.metallicRoughnessTexture.extensions);
        m.textures[Normal] =
            texture(g, mat.normalTexture.index, mat.normalTexture.texCoord, mat.normalTexture.extensions);
        m.textures[Occlusion] = texture(g, mat.occlusionTexture.index, mat.occlusionTexture.texCoord,
                                        mat.occlusionTexture.extensions);
        m.textures[Emissive] = texture(g, mat.emissiveTexture.index, mat.emissiveTexture.texCoord,
                                       mat.emissiveTexture.extensions);
        if (auto it = mat.extensions.find("KHR_materials_specular"); it != mat.extensions.end()) {
            auto &e = it->second;
            m.specular = float(number(e, "specularFactor", 1));
            if (e.Has("specularColorFactor")) {
                auto &a = e.Get("specularColorFactor");
                m.specularColor = {item(a, 0, 1), item(a, 1, 1), item(a, 2, 1)};
            }
            if (glm::any(glm::greaterThan(m.specularColor, glm::vec3(1))) ||
                glm::any(glm::lessThan(m.specularColor, glm::vec3(0))) || m.specular < 0 || m.specular > 1)
                out->warnings.push_back(m.name + ": specular factor outside [0,1]; clamped");
            m.specularColor = glm::clamp(m.specularColor, glm::vec3(0), glm::vec3(1));
            m.specular = glm::clamp(m.specular, 0.f, 1.f);
            m.textures[Specular] = extensionTexture(g, e, "specularTexture");
            m.textures[SpecularColor] = extensionTexture(g, e, "specularColorTexture");
        }
        if (auto it = mat.extensions.find("KHR_materials_emissive_strength"); it != mat.extensions.end())
            m.emissive *= float(number(it->second, "emissiveStrength", 1));
        m.unlit = mat.extensions.contains("KHR_materials_unlit");
        out->materials.push_back(m);
    }
    uint32_t defaultMat = uint32_t(out->materials.size());
    out->materials.push_back(Material{});
    out->materials.back().metallic = 1;
    std::vector<std::vector<uint32_t>> meshMap(g.meshes.size());
    for (size_t mi = 0; mi < g.meshes.size(); mi++)
        for (auto &p : g.meshes[mi].primitives) {
            if (p.mode != -1 && p.mode != 4 && p.mode != 5 && p.mode != 6) {
                out->warnings.push_back("Skipping non-triangle primitive");
                continue;
            }
            if (p.extensions.contains("KHR_draco_mesh_compression"))
                throw std::runtime_error("Draco-compressed glTF is not supported");
            if (!p.attributes.contains("POSITION"))
                throw std::runtime_error("Missing POSITION");
            Mesh m;
            m.name = g.meshes[mi].name;
            m.material = p.material < 0 ? defaultMat : uint32_t(p.material);
            if (m.material >= out->materials.size())
                throw std::runtime_error("Invalid material");
            auto pos = accessor(g, p.attributes.at("POSITION"), 3);
            m.vertices.resize(pos.size() / 3);
            for (size_t i = 0; i < m.vertices.size(); i++)
                m.vertices[i].position = {pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]};
            auto attr = [&](const char *name, int n, auto put) {
                auto it = p.attributes.find(name);
                if (it == p.attributes.end())
                    return false;
                auto a = accessor(g, it->second, n);
                if (a.size() != m.vertices.size() * n)
                    throw std::runtime_error("Mismatched attribute counts");
                for (size_t i = 0; i < m.vertices.size(); i++)
                    put(m.vertices[i], a.data() + i * n);
                return true;
            };
            bool normals =
                attr("NORMAL", 3, [](Vertex &v, const double *p) { v.normal = {p[0], p[1], p[2]}; });
            bool tangents =
                attr("TANGENT", 4, [](Vertex &v, const double *p) { v.tangent = {p[0], p[1], p[2], p[3]}; });
            attr("TEXCOORD_0", 2, [](Vertex &v, const double *p) { v.uv0 = {p[0], p[1]}; });
            attr("TEXCOORD_1", 2, [](Vertex &v, const double *p) { v.uv1 = {p[0], p[1]}; });
            if (auto it = p.attributes.find("COLOR_0"); it != p.attributes.end()) {
                int n = tinygltf::GetNumComponentsInType(g.accessors.at(it->second).type);
                if (n != 3 && n != 4)
                    throw std::runtime_error("Invalid vertex color");
                attr("COLOR_0", n,
                     [n](Vertex &v, const double *p) { v.color = {p[0], p[1], p[2], n == 4 ? p[3] : 1}; });
            }
            if (p.indices >= 0) {
                auto idx = accessor(g, p.indices, 1);
                for (double i : idx) {
                    if (i < 0 || i >= double(m.vertices.size()) || std::floor(i) != i)
                        throw std::runtime_error("Invalid mesh index");
                    m.indices.push_back(uint32_t(i));
                }
            } else {
                m.indices.resize(m.vertices.size());
                std::iota(m.indices.begin(), m.indices.end(), 0);
            }
            if (p.mode == 5 || p.mode == 6) {
                auto source = std::move(m.indices);
                for (size_t i = 2; i < source.size(); ++i) {
                    uint32_t a = p.mode == 6 ? source[0] : source[i - 2];
                    uint32_t b = source[i - 1], c = source[i];
                    if (p.mode == 5 && (i & 1))
                        std::swap(a, b);
                    if (a != b && b != c && a != c)
                        m.indices.insert(m.indices.end(), {a, b, c});
                }
            }
            for (const auto &ref : out->materials[m.material].textures)
                if (ref.image >= 0 && !p.attributes.contains(ref.uv ? "TEXCOORD_1" : "TEXCOORD_0"))
                    out->warnings.push_back(m.name + ": material references a missing UV set");
            finishMesh(m, !normals, !tangents);
            meshMap[mi].push_back(uint32_t(out->meshes.size()));
            out->meshes.push_back(std::move(m));
        }
    std::unordered_set<int> stack;
    std::function<void(int, int, glm::mat4)> visit = [&](int id, int parent, glm::mat4 world) {
        if (stack.size() > 1024)
            throw std::runtime_error("glTF node hierarchy exceeds 1024 levels");
        if (!stack.insert(id).second)
            throw std::runtime_error("Cyclic node hierarchy");
        auto &n = g.nodes.at(id);
        AssetNode node;
        node.name = n.name;
        node.parent = parent;
        if (n.matrix.size() == 16)
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    node.local[c][r] = float(n.matrix[c * 4 + r]);
        else {
            glm::vec3 t(0), s(1);
            glm::quat q(1, 0, 0, 0);
            if (n.translation.size() == 3)
                t = {n.translation[0], n.translation[1], n.translation[2]};
            if (n.scale.size() == 3)
                s = {n.scale[0], n.scale[1], n.scale[2]};
            if (n.rotation.size() == 4)
                q = {float(n.rotation[3]), float(n.rotation[0]), float(n.rotation[1]), float(n.rotation[2])};
            node.local = glm::translate(glm::mat4(1), t) * glm::mat4_cast(q) * glm::scale(glm::mat4(1), s);
        }
        node.world = world * node.local;
        if (n.mesh >= 0)
            node.meshes = meshMap.at(n.mesh);
        for (auto i : node.meshes)
            out->bounds.add(out->meshes[i].bounds.transformed(node.world));
        int index = int(out->nodes.size());
        world = node.world;
        out->nodes.push_back(node);
        for (int child : n.children)
            visit(child, index, world);
        stack.erase(id);
    };
    if (!g.scenes.empty()) {
        for (int i : g.scenes.at(g.defaultScene < 0 ? 0 : g.defaultScene).nodes)
            visit(i, -1, glm::mat4(1));
    } else {
        std::unordered_set<int> children;
        for (auto &n : g.nodes)
            for (int c : n.children)
                children.insert(c);
        for (int i = 0; i < int(g.nodes.size()); i++)
            if (!children.contains(i))
                visit(i, -1, glm::mat4(1));
    }
    return out;
}
class MemoryStream : public Assimp::IOStream {
    std::string data;
    size_t pos = 0;

  public:
    explicit MemoryStream(const fs::path &p) : data(readFile(p)) {}
    size_t Read(void *dst, size_t size, size_t count) override {
        if (!size)
            return 0;
        count = std::min(count, (data.size() - pos) / size);
        memcpy(dst, data.data() + pos, size * count);
        pos += size * count;
        return count;
    }
    size_t Write(const void *, size_t, size_t) override {
        return 0;
    }
    aiReturn Seek(size_t off, aiOrigin origin) override {
        size_t base = origin == aiOrigin_SET ? 0 : origin == aiOrigin_CUR ? pos : data.size();
        if (origin == aiOrigin_END) {
            if (off > base)
                return aiReturn_FAILURE;
            pos = base - off;
        } else {
            if (off > data.size() - base)
                return aiReturn_FAILURE;
            pos = base + off;
        }
        return aiReturn_SUCCESS;
    }
    size_t Tell() const override {
        return pos;
    }
    size_t FileSize() const override {
        return data.size();
    }
    void Flush() override {}
};
class UnicodeIO : public Assimp::IOSystem {
  public:
    bool Exists(const char *p) const override {
        return fs::exists(fs::u8path(p));
    }
    char getOsSeparator() const override {
        return '/';
    }
    Assimp::IOStream *Open(const char *p, const char *mode) override {
        if (mode[0] != 'r')
            return nullptr;
        try {
            return new MemoryStream(fs::u8path(p));
        } catch (...) {
            return nullptr;
        }
    }
    void Close(Assimp::IOStream *s) override {
        delete s;
    }
};
static glm::mat4 matrix(const aiMatrix4x4 &a) {
    return glm::transpose(glm::mat4(a.a1, a.a2, a.a3, a.a4, a.b1, a.b2, a.b3, a.b4, a.c1, a.c2, a.c3, a.c4,
                                    a.d1, a.d2, a.d3, a.d4));
}
static std::shared_ptr<ImportedAsset> importAssimp(const fs::path &path) {
    Assimp::Importer importer;
    importer.SetIOHandler(new UnicodeIO);
    auto *s = importer.ReadFile(utf8(path), aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                                aiProcess_ImproveCacheLocality | aiProcess_SortByPType |
                                                aiProcess_FlipUVs);
    if (!s || !s->mRootNode)
        throw std::runtime_error(std::string("Assimp: ") + importer.GetErrorString());
    auto out = std::make_shared<ImportedAsset>();
    out->path = path;
    std::unordered_map<std::string, int> imageCache;
    auto tex = [&](const aiMaterial *mat, aiTextureType type) -> TextureRef {
        TextureRef r;
        aiString ref;
        unsigned uv = 0;
        aiTextureMapMode modes[2]{};
        if (mat->GetTexture(type, 0, &ref, nullptr, &uv, nullptr, nullptr, modes) != AI_SUCCESS)
            return r;
        r.uv = std::min(int(uv), 1);
        if (uv > 1)
            out->warnings.push_back("Texture UV set above 1 approximated using UV1");
        auto wrap = [](aiTextureMapMode m) {
            return m == aiTextureMapMode_Clamp ? 33071 : m == aiTextureMapMode_Mirror ? 33648 : 10497;
        };
        r.wrapS = wrap(modes[0]);
        r.wrapT = wrap(modes[1]);
        aiUVTransform tr;
        if (mat->Get(AI_MATKEY_UVTRANSFORM(type, 0), tr) == AI_SUCCESS) {
            r.offset = {tr.mTranslation.x, tr.mTranslation.y};
            r.scale = {tr.mScaling.x, tr.mScaling.y};
            r.rotation = tr.mRotation;
        }
        std::string key = ref.C_Str();
        if (imageCache.contains(key)) {
            r.image = imageCache[key];
            return r;
        }
        ImageData im;
        im.name = key;
        std::string bytes;
        if (auto *t = s->GetEmbeddedTexture(ref.C_Str())) {
            if (t->mHeight == 0)
                bytes.assign(reinterpret_cast<const char *>(t->pcData), t->mWidth);
            else {
                im.width = int(t->mWidth);
                im.height = int(t->mHeight);
                im.rgba.resize(size_t(im.width) * im.height * 4);
                for (size_t i = 0; i < size_t(im.width) * im.height; i++) {
                    im.rgba[4 * i] = t->pcData[i].r;
                    im.rgba[4 * i + 1] = t->pcData[i].g;
                    im.rgba[4 * i + 2] = t->pcData[i].b;
                    im.rgba[4 * i + 3] = t->pcData[i].a;
                }
            }
        } else {
            auto p = fs::u8path(key);
            if (!p.is_absolute())
                p = path.parent_path() / p;
            if (!fs::exists(p))
                p = path.parent_path() / fs::u8path(key).filename();
            try {
                bytes = readFile(p);
            } catch (const std::exception &) {
                out->warnings.push_back("Missing texture: " + key);
                imageCache[key] = -1;
                return r;
            }
        }
        if (!bytes.empty()) {
            int channels;
            auto *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                                                 int(bytes.size()), &im.width, &im.height, &channels, 4);
            if (!pixels) {
                out->warnings.push_back("Unsupported texture: " + key);
                imageCache[key] = -1;
                return r;
            }
            im.rgba.assign(pixels, pixels + size_t(im.width) * im.height * 4);
            stbi_image_free(pixels);
        }
        r.image = int(out->images.size());
        imageCache[key] = r.image;
        out->images.push_back(std::move(im));
        return r;
    };
    for (unsigned i = 0; i < s->mNumMaterials; i++) {
        auto *a = s->mMaterials[i];
        Material m;
        aiString name;
        a->Get(AI_MATKEY_NAME, name);
        m.name = name.C_Str();
        aiColor4D c(1, 1, 1, 1);
        if (a->Get(AI_MATKEY_BASE_COLOR, c) != AI_SUCCESS)
            a->Get(AI_MATKEY_COLOR_DIFFUSE, c);
        m.baseColor = {c.r, c.g, c.b, c.a};
        float opacity = 1;
        a->Get(AI_MATKEY_OPACITY, opacity);
        m.baseColor.a *= opacity;
        aiColor3D e(0, 0, 0);
        a->Get(AI_MATKEY_COLOR_EMISSIVE, e);
        m.emissive = {e.r, e.g, e.b};
        int two = 0;
        a->Get(AI_MATKEY_TWOSIDED, two);
        m.doubleSided = two != 0;
        bool pbr = a->Get(AI_MATKEY_METALLIC_FACTOR, m.metallic) == AI_SUCCESS;
        if (a->Get(AI_MATKEY_ROUGHNESS_FACTOR, m.roughness) != AI_SUCCESS) {
            float shininess = 0;
            a->Get(AI_MATKEY_SHININESS, shininess);
            m.roughness = glm::clamp(std::sqrt(2.f / (shininess + 2.f)), .04f, 1.f);
        }
        m.textures[BaseColor] = tex(a, aiTextureType_BASE_COLOR);
        if (m.textures[BaseColor].image < 0)
            m.textures[BaseColor] = tex(a, aiTextureType_DIFFUSE);
        m.textures[Normal] = tex(a, aiTextureType_NORMALS);
        m.textures[MetalRough] = tex(a, aiTextureType_METALNESS);
        m.textures[Roughness] = tex(a, aiTextureType_DIFFUSE_ROUGHNESS);
        m.separateMetalRough = true;
        m.textures[Occlusion] = tex(a, aiTextureType_AMBIENT_OCCLUSION);
        m.textures[Emissive] = tex(a, aiTextureType_EMISSIVE);
        m.alphaMode = m.baseColor.a < .999f ? 2 : 0;
        if (!pbr)
            out->warnings.push_back(m.name + ": legacy material approximated as metallic/roughness PBR");
        out->materials.push_back(m);
    }
    if (out->materials.empty())
        out->materials.push_back(Material{});
    std::vector<int> meshMap(s->mNumMeshes, -1);
    for (unsigned i = 0; i < s->mNumMeshes; i++) {
        auto *a = s->mMeshes[i];
        if (!(a->mPrimitiveTypes & aiPrimitiveType_TRIANGLE))
            continue;
        Mesh m;
        m.name = a->mName.C_Str();
        m.material = a->mMaterialIndex;
        m.vertices.resize(a->mNumVertices);
        for (unsigned v = 0; v < a->mNumVertices; v++) {
            auto &o = m.vertices[v];
            auto p = a->mVertices[v];
            o.position = {p.x, p.y, p.z};
            if (a->HasNormals()) {
                auto n = a->mNormals[v];
                o.normal = {n.x, n.y, n.z};
            }
            if (a->HasTextureCoords(0))
                o.uv0 = {a->mTextureCoords[0][v].x, a->mTextureCoords[0][v].y};
            if (a->HasTextureCoords(1))
                o.uv1 = {a->mTextureCoords[1][v].x, a->mTextureCoords[1][v].y};
            if (a->HasVertexColors(0)) {
                auto c = a->mColors[0][v];
                o.color = {c.r, c.g, c.b, c.a};
            }
        }
        for (unsigned f = 0; f < a->mNumFaces; f++)
            if (a->mFaces[f].mNumIndices == 3)
                for (int c = 0; c < 3; c++)
                    m.indices.push_back(a->mFaces[f].mIndices[c]);
        finishMesh(m, !a->HasNormals(), true);
        meshMap[i] = int(out->meshes.size());
        out->meshes.push_back(std::move(m));
    }
    glm::mat4 conversion(1);
    double unit = 100;
    bool knownUnit = s->mMetaData && s->mMetaData->Get("UnitScaleFactor", unit);
    if (knownUnit)
        conversion = glm::scale(glm::mat4(1), glm::vec3(float(unit * .01)));
    int up = 1, upSign = 1, front = 2, frontSign = 1, coord = 0, coordSign = 1;
    if (s->mMetaData && s->mMetaData->Get("UpAxis", up)) {
        s->mMetaData->Get("UpAxisSign", upSign);
        s->mMetaData->Get("FrontAxis", front);
        s->mMetaData->Get("FrontAxisSign", frontSign);
        s->mMetaData->Get("CoordAxis", coord);
        s->mMetaData->Get("CoordAxisSign", coordSign);
        if (up >= 0 && up < 3 && front >= 0 && front < 3 && coord >= 0 && coord < 3 && up != front &&
            up != coord && front != coord) {
            glm::mat4 axes(0);
            axes[coord][0] = float(coordSign);
            axes[up][1] = float(upSign);
            axes[front][2] = float(frontSign);
            axes[3][3] = 1;
            conversion = conversion * axes;
        }
    }
    std::function<void(const aiNode *, int, glm::mat4)> visit = [&](const aiNode *n, int parent,
                                                                    glm::mat4 world) {
        AssetNode node;
        node.name = n->mName.C_Str();
        node.parent = parent;
        node.local = matrix(n->mTransformation);
        node.world = world * node.local;
        for (unsigned j = 0; j < n->mNumMeshes; j++) {
            int m = meshMap.at(n->mMeshes[j]);
            if (m >= 0) {
                node.meshes.push_back(m);
                out->bounds.add(out->meshes[m].bounds.transformed(node.world));
            }
        }
        int id = int(out->nodes.size());
        world = node.world;
        out->nodes.push_back(node);
        for (unsigned j = 0; j < n->mNumChildren; j++)
            visit(n->mChildren[j], id, world);
    };
    visit(s->mRootNode, -1, conversion);
    if (s->HasAnimations())
        out->warnings.push_back("Animation not evaluated; displaying static node pose");
    return out;
}
std::shared_ptr<ImportedAsset> importAsset(const fs::path &source) {
    auto path = fs::weakly_canonical(fs::absolute(source));
    auto ext = utf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    auto out = (ext == ".glb" || ext == ".gltf") ? importGltf(path) : importAssimp(path);
    if (!out->bounds.valid() || out->meshes.empty())
        throw std::runtime_error("Asset has no renderable triangles");
    return out;
}
std::shared_ptr<ImportedAsset> makeGround() {
    auto a = std::make_shared<ImportedAsset>();
    Material mat;
    mat.baseColor = {.22f, .25f, .29f, 1};
    mat.roughness = .8f;
    a->materials.push_back(mat);
    Mesh m;
    m.name = "Ground";
    for (auto p : {glm::vec3(-100, 0, -100), glm::vec3(100, 0, -100), glm::vec3(100, 0, 100),
                   glm::vec3(-100, 0, 100)}) {
        Vertex v;
        v.position = p;
        v.normal = {0, 1, 0};
        v.uv0 = {p.x, p.z};
        m.vertices.push_back(v);
    }
    m.indices = {0, 2, 1, 0, 3, 2};
    finishMesh(m, false, true);
    a->bounds = m.bounds;
    a->meshes.push_back(std::move(m));
    AssetNode n;
    n.meshes = {0};
    a->nodes.push_back(n);
    return a;
}
} // namespace vke
