#pragma once
#include "Scene.h"
#include <memory>
#include <optional>
struct GLFWwindow;
namespace vke {
struct RenderStats {
    double gpuMs = 0;
    uint64_t triangles = 0;
    uint32_t draws = 0, instances = 0;
    uint64_t allocatedBytes = 0, budgetBytes = 0;
    uint32_t validationErrors = 0;
    std::string gpu;
};
struct Viewport {
    int x = 0, y = 0, width = 1, height = 1;
};
class Renderer {
  public:
    Renderer(GLFWwindow *window, bool validation, bool vsync);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    void newFrame();
    void render(const Scene &scene, Viewport viewport, uint32_t selected);
    AssetHandle enqueueUpload(std::shared_ptr<ImportedAsset> asset);
    bool ready(AssetHandle handle) const;
    std::shared_ptr<const ImportedAsset> asset(AssetHandle handle) const;
    void retainOnly(const std::vector<AssetHandle> &handles);
    void requestPick(int x, int y);
    std::optional<uint32_t> picked();
    void setEnvironment(const fs::path &path);
    bool environmentBusy() const;
    fs::path environmentPath() const;
    void screenshot(const fs::path &path);
    const RenderStats &stats() const;
    std::vector<std::string> takeMessages();

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace vke
