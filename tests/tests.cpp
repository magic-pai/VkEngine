#include "Scene.h"
#include <json.hpp>
#include <iostream>
#include <fstream>
#include <functional>
#include <chrono>
using namespace vke;
void require(bool condition, const char *what) {
    if (!condition)
        throw std::runtime_error(what);
}
int main(int argc, char **argv) {
    try {
        auto temp = fs::temp_directory_path() /
                    fs::u8path("VkEngine_测试_" + std::to_string(
                        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        fs::create_directories(temp);
        Bounds b;
        b.add({-1, -2, -3});
        b.add({1, 2, 3});
        auto p = placement(b, {4, 0, 5});
        require(glm::length(p - glm::vec3(4, 2, 5)) < .0001f, "Bottom-center placement");
        Camera c;
        c.target = {0, 0, 0};
        c.pitch = .6f;
        c.yaw = 0;
        c.distance = 10;
        auto hit = groundIntersection({400, 300}, {800, 600}, c);
        require(hit && glm::length(*hit) < .001f, "Ground ray center");
        require(!groundIntersection({-1, 2}, {800, 600}, c), "Outside viewport");
        c.pitch = 0;
        c.target.y = 2;
        require(!groundIntersection({400, 300}, {800, 600}, c), "Parallel ray");
        Scene s;
        SceneObject o;
        o.id = 5;
        o.name = "绿巨人";
        o.source = temp / "mesh.obj";
        o.transform.position = {3, 4, 5};
        o.transform.scale = {-1, 2, 3};
        s.objects.push_back(o);
        s.environment.exposure = 1.2f;
        s.environment.rotation = .3f;
        auto scenePath = temp / "test.vkscene";
        saveScene(s, scenePath);
        auto round = loadScene(scenePath);
        require(round.objects.size() == 1 && round.objects[0].id == 5 && round.objects[0].name == o.name,
                "Scene identity roundtrip");
        require(round.objects[0].source == o.source && round.objects[0].transform.scale == o.transform.scale,
                "Scene paths/transforms roundtrip");
        require(!round.objects[0].error.empty(), "Missing asset retained");
        require(round.environment.exposure == 1.2f, "Environment roundtrip");
        saveScene(round, scenePath);
        auto bad = nlohmann::json::parse(readFile(scenePath));
        bad["objects"].push_back(bad["objects"][0]);
        {
            std::ofstream f(temp / "bad.vkscene");
            f << bad;
        }
        bool rejected = false;
        try {
            loadScene(temp / "bad.vkscene");
        } catch (...) {
            rejected = true;
        }
        require(rejected, "Duplicate IDs rejected");
        {
            std::ofstream f(o.source);
            f << "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
        }
        auto asset = importAsset(o.source);
        require(asset->triangles() == 1 && asset->bounds.valid(), "Unicode OBJ import");
        for (auto &v : asset->meshes[0].vertices)
            require(std::isfinite(v.tangent.x) && glm::length(v.normal) > .99f, "Generated normals/tangents");
        auto fixturePath = fs::path(VKE_FIXTURES) / "materials.gltf";
        auto fixture = importAsset(fixturePath);
        require(fixture->triangles() == 6 && fixture->nodes.size() == 4, "glTF node hierarchy");
        require(std::abs(fixture->bounds.min.y - .2f) < .0001f &&
                    std::abs(fixture->bounds.max.x - 1.7f) < .0001f,
                "glTF parent transform bounds");
        require(fixture->materials[0].alphaMode == 1 && fixture->materials[1].alphaMode == 2,
                "glTF alpha modes");
        require(fixture->materials[0].textures[BaseColor].uv == 1 &&
                    std::abs(fixture->materials[0].textures[BaseColor].rotation - .3f) < .0001f,
                "glTF texture transforms");
        require(fixture->images.size() == 1 && fixture->images[0].rgba[7] == 0, "Embedded RGBA image");
        require(fixture->materials[2].specular == .5f, "glTF specular extension");
        for (const auto &v : fixture->meshes[0].vertices) {
            float expected = v.position.x < 0 && v.position.y > .5f ? 128.f / 255.f : 1.f;
            require(std::abs(v.color.r - expected) < .0001f,
                    "Sparse normalized attributes use integer sparse indices");
        }
        require(glm::determinant(fixture->nodes[2].world) < 0, "Negative node transform retained");
        auto invalid = nlohmann::json::parse(readFile(fixturePath));
        invalid["extensionsRequired"] = {"KHR_draco_mesh_compression"};
        {
            std::ofstream f(temp / "unsupported.gltf");
            f << invalid;
        }
        rejected = false;
        try {
            importAsset(temp / "unsupported.gltf");
        } catch (const std::exception &e) {
            rejected = std::string(e.what()).find("Unsupported required") != std::string::npos;
        }
        require(rejected, "Required extension rejected");
        fs::remove(temp / "unsupported.gltf");
        if (argc > 1) {
            auto a = importAsset(fs::u8path(argv[1]));
            std::cout << "Asset triangles=" << a->triangles() << " images=" << a->images.size()
                      << " materials=" << a->materials.size() << " bytes=" << a->byteSize() << "\n";
            for (auto &w : a->warnings)
                std::cout << w << "\n";
            require(a->triangles() > 0, "Real asset import");
        }
        fs::remove(temp / "bad.vkscene");
        fs::remove(scenePath);
        fs::remove(o.source);
        fs::remove(temp);
        std::cout << "All CPU tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
