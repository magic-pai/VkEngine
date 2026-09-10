#pragma once
#include <glm/glm.hpp>
#include <filesystem>
#include <array>
#include <vector>
#include <string>
#include <memory>
#include <limits>
namespace vke {
namespace fs = std::filesystem;
std::string utf8(const fs::path &path);
std::string readFile(const fs::path &path);
struct Bounds {
    glm::vec3 min{std::numeric_limits<float>::max()}, max{-std::numeric_limits<float>::max()};
    void add(glm::vec3 p);
    void add(const Bounds &b);
    bool valid() const;
    glm::vec3 center() const;
    glm::vec3 size() const;
    Bounds transformed(const glm::mat4 &m) const;
};
struct Vertex {
    glm::vec3 position{}, normal{};
    glm::vec4 tangent{1, 0, 0, 1};
    glm::vec2 uv0{}, uv1{};
    glm::vec4 color{1};
};
struct ImageData {
    std::string name;
    int width = 1, height = 1;
    std::vector<unsigned char> rgba;
};
struct TextureRef {
    int image = -1, uv = 0;
    glm::vec2 offset{0}, scale{1};
    float rotation = 0;
    int wrapS = 10497, wrapT = 10497, minFilter = 9987, magFilter = 9729;
};
enum TextureSlot {
    BaseColor,
    Normal,
    MetalRough,
    Occlusion,
    Emissive,
    Specular,
    SpecularColor,
    Roughness,
    TextureCount
};
struct Material {
    std::string name = "Default";
    glm::vec4 baseColor{1};
    glm::vec3 emissive{0};
    float metallic = 0, roughness = 1, normalScale = 1, occlusionStrength = 1, specular = 1;
    glm::vec3 specularColor{1};
    float alphaCutoff = .5f;
    int alphaMode = 0;
    bool doubleSided = false, separateMetalRough = false, unlit = false;
    std::array<TextureRef, TextureCount> textures;
};
struct Mesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t material = 0;
    Bounds bounds;
};
struct AssetNode {
    std::string name;
    int parent = -1;
    glm::mat4 local{1}, world{1};
    std::vector<uint32_t> meshes;
};
struct ImportedAsset {
    fs::path path;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<ImageData> images;
    std::vector<AssetNode> nodes;
    Bounds bounds;
    std::vector<std::string> warnings;
    uint64_t triangles() const;
    size_t byteSize() const;
};
std::shared_ptr<ImportedAsset> importAsset(const fs::path &path);
std::shared_ptr<ImportedAsset> makeGround();
void finishMesh(Mesh &mesh, bool normalsMissing, bool tangentsMissing);
} // namespace vke
