#pragma once
#include "Asset.h"
#include <optional>
#include <glm/gtc/quaternion.hpp>
namespace vke {
using AssetHandle = uint64_t;
struct Transform {
    glm::vec3 position{0}, rotation{0}, scale{1};
    glm::mat4 matrix() const;
};
struct SceneObject {
    uint32_t id = 0;
    std::string name;
    fs::path source;
    Transform transform;
    bool visible = true;
    AssetHandle asset = 0;
    std::string error;
};
struct Camera {
    glm::vec3 target{0, 1, 0};
    float yaw = .6f, pitch = .35f, distance = 5;
    glm::vec3 eye() const;
    glm::mat4 view() const;
    glm::mat4 projection(float aspect, bool vulkan = true) const;
    void focus(const Bounds &bounds);
};
struct Environment {
    fs::path path;
    float rotation = 0, intensity = 1, exposure = 0;
    glm::vec3 lightDirection{-.5f, -1, -.4f}, lightColor{1, .94f, .84f};
    float lightIntensity = 3;
    bool ground = true;
};
struct Scene {
    std::vector<SceneObject> objects;
    Camera camera;
    Environment environment;
    uint32_t nextId = 1;
    fs::path file;
    SceneObject *find(uint32_t id);
};
std::optional<glm::vec3> groundIntersection(glm::vec2 pixel, glm::vec2 extent, const Camera &camera);
glm::vec3 placement(const Bounds &bounds, glm::vec3 groundPoint);
bool visibleBounds(const Bounds &b, const glm::mat4 &viewProjection);
void saveScene(const Scene &scene, const fs::path &path);
Scene loadScene(const fs::path &path);
} // namespace vke
