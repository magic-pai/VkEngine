#include "Scene.h"
#include <json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>
#include <set>
#ifdef _WIN32
#include <windows.h>
#endif
namespace vke {
glm::mat4 Transform::matrix() const {
    return glm::translate(glm::mat4(1), position) * glm::mat4_cast(glm::quat(glm::radians(rotation))) *
           glm::scale(glm::mat4(1), scale);
}
glm::vec3 Camera::eye() const {
    return target + distance * glm::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                         std::cos(yaw) * std::cos(pitch));
}
glm::mat4 Camera::view() const {
    return glm::lookAt(eye(), target, glm::vec3(0, 1, 0));
}
glm::mat4 Camera::projection(float aspect, bool vulkan) const {
    auto p = glm::perspective(glm::radians(45.f), std::max(aspect, .01f), std::max(.001f, distance * .0005f),
                              std::max(200.f, distance * 20));
    if (vulkan)
        p[1][1] *= -1;
    return p;
}
void Camera::focus(const Bounds &b) {
    if (b.valid()) {
        target = b.center();
        distance = std::max(.1f, glm::length(b.size()) * 1.5f);
    }
}
SceneObject *Scene::find(uint32_t id) {
    for (auto &o : objects)
        if (o.id == id)
            return &o;
    return nullptr;
}
std::optional<glm::vec3> groundIntersection(glm::vec2 p, glm::vec2 extent, const Camera &c) {
    if (extent.x <= 0 || extent.y <= 0 || p.x < 0 || p.y < 0 || p.x >= extent.x || p.y >= extent.y)
        return {};
    glm::vec2 ndc = p / extent * 2.f - 1.f;
    auto inv = glm::inverse(c.projection(extent.x / extent.y) * c.view());
    auto farPoint = inv * glm::vec4(ndc, 1, 1);
    auto origin = c.eye();
    auto dir = glm::normalize(glm::vec3(farPoint) / farPoint.w - origin);
    if (std::abs(dir.y) < 1e-6f)
        return {};
    float t = -origin.y / dir.y;
    if (t < 0 || !std::isfinite(t))
        return {};
    return origin + t * dir;
}
glm::vec3 placement(const Bounds &b, glm::vec3 point) {
    if (!b.valid())
        return point;
    return point - glm::vec3(b.center().x, b.min.y, b.center().z);
}
bool visibleBounds(const Bounds &b, const glm::mat4 &vp) {
    if (!b.valid())
        return false;
    glm::vec4 v[8];
    for (int i = 0; i < 8; i++)
        v[i] = vp *
               glm::vec4(i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z, 1);
    for (int p = 0; p < 6; p++) {
        bool outside = true;
        for (auto a : v) {
            bool o = p == 0   ? a.x < -a.w
                     : p == 1 ? a.x > a.w
                     : p == 2 ? a.y < -a.w
                     : p == 3 ? a.y > a.w
                     : p == 4 ? a.z < 0
                              : a.z > a.w;
            if (!o) {
                outside = false;
                break;
            }
        }
        if (outside)
            return false;
    }
    return true;
}
using json = nlohmann::json;
static json vec(glm::vec3 v) {
    return json::array({v.x, v.y, v.z});
}
static glm::vec3 vector(const json &j) {
    if (!j.is_array() || j.size() != 3)
        throw std::runtime_error("Invalid scene vector");
    glm::vec3 v{j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()};
    for (int i = 0; i < 3; i++)
        if (!std::isfinite(v[i]))
            throw std::runtime_error("Non-finite scene vector");
    return v;
}
static std::string reference(const fs::path &p, const fs::path &dir) {
    if (p.empty())
        return {};
    std::error_code ec;
    auto rel = fs::relative(p, dir, ec);
    return utf8(!ec && !rel.empty() ? rel : p);
}
static fs::path resolve(const std::string &p, const fs::path &dir) {
    if (p.empty())
        return {};
    auto path = fs::u8path(p);
    return fs::absolute(path.is_absolute() ? path : dir / path).lexically_normal();
}
void saveScene(const Scene &s, const fs::path &path) {
    auto absolute = fs::absolute(path);
    json j{{"version", 1}, {"objects", json::array()}};
    for (auto &o : s.objects)
        j["objects"].push_back({{"id", o.id},
                                {"name", o.name},
                                {"asset", reference(o.source, absolute.parent_path())},
                                {"position", vec(o.transform.position)},
                                {"rotation", vec(o.transform.rotation)},
                                {"scale", vec(o.transform.scale)},
                                {"visible", o.visible}});
    auto &c = s.camera;
    j["camera"] = {{"target", vec(c.target)}, {"yaw", c.yaw}, {"pitch", c.pitch}, {"distance", c.distance}};
    auto &e = s.environment;
    j["environment"] = {{"asset", reference(e.path, absolute.parent_path())},
                        {"rotation", e.rotation},
                        {"intensity", e.intensity},
                        {"exposure", e.exposure},
                        {"lightDirection", vec(e.lightDirection)},
                        {"lightColor", vec(e.lightColor)},
                        {"lightIntensity", e.lightIntensity},
                        {"ground", e.ground}};
    auto tmp = absolute;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            throw std::runtime_error("Cannot create scene file");
        f << j.dump(2);
        f.flush();
        if (!f)
            throw std::runtime_error("Scene write failed");
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), absolute.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace scene file");
#else
    fs::rename(tmp, absolute);
#endif
}
Scene loadScene(const fs::path &path) {
    auto j = json::parse(readFile(path));
    if (j.at("version").get<int>() != 1)
        throw std::runtime_error("Unsupported scene version");
    Scene s;
    s.file = fs::absolute(path);
    std::set<uint32_t> ids;
    if (!j.at("objects").is_array() || j.at("objects").size() > 100000)
        throw std::runtime_error("Invalid object list");
    for (auto &v : j.at("objects")) {
        SceneObject o;
        o.id = v.at("id").get<uint32_t>();
        if (!o.id || o.id == UINT32_MAX || !ids.insert(o.id).second)
            throw std::runtime_error("Invalid/duplicate object ID");
        o.name = v.at("name").get<std::string>();
        o.source = resolve(v.at("asset").get<std::string>(), s.file.parent_path());
        o.transform.position = vector(v.at("position"));
        o.transform.rotation = vector(v.at("rotation"));
        o.transform.scale = vector(v.at("scale"));
        for (int i = 0; i < 3; i++)
            if (std::abs(o.transform.scale[i]) < 1e-6)
                throw std::runtime_error("Zero object scale");
        o.visible = v.at("visible").get<bool>();
        if (!fs::is_regular_file(o.source))
            o.error = "Asset missing; use Relocate";
        s.nextId = std::max(s.nextId, o.id + 1);
        s.objects.push_back(o);
    }
    auto &c = j.at("camera");
    s.camera.target = vector(c.at("target"));
    s.camera.yaw = c.at("yaw");
    s.camera.pitch = c.at("pitch");
    s.camera.distance = c.at("distance");
    if (!std::isfinite(s.camera.yaw) || !std::isfinite(s.camera.pitch) || !std::isfinite(s.camera.distance) ||
        s.camera.distance <= 0 || std::abs(s.camera.pitch) > 1.56f)
        throw std::runtime_error("Invalid camera");
    auto &e = j.at("environment");
    s.environment.path = resolve(e.at("asset").get<std::string>(), s.file.parent_path());
    s.environment.rotation = e.at("rotation");
    s.environment.intensity = e.at("intensity");
    s.environment.exposure = e.at("exposure");
    s.environment.lightDirection = vector(e.at("lightDirection"));
    s.environment.lightColor = vector(e.at("lightColor"));
    s.environment.lightIntensity = e.at("lightIntensity");
    s.environment.ground = e.at("ground");
    for (float f : {s.environment.rotation, s.environment.intensity, s.environment.exposure,
                    s.environment.lightIntensity})
        if (!std::isfinite(f))
            throw std::runtime_error("Invalid environment");
    if (glm::length(s.environment.lightDirection) < 1e-5f || s.environment.intensity < 0 ||
        s.environment.lightIntensity < 0)
        throw std::runtime_error("Invalid lighting");
    return s;
}
} // namespace vke
