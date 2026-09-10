#include "Renderer.h"
#include "Platform.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <windows.h>
#include <shellapi.h>
#include <json.hpp>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <set>
#include <utility>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
namespace vke {
using Clock = std::chrono::steady_clock;
struct Options {
    bool validation = false, vsync = true, ui = true, smoke = false, selfTest = false;
    int width = 1600, height = 1000, instances = 1;
    double benchmark = 0;
    fs::path model, scene, capture, report;
};
static Options options() {
    Options o;
#ifndef NDEBUG
    o.validation = true;
#endif
    int argc;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto value = [&]() {
            if (i + 1 >= argc)
                throw std::runtime_error("Missing command argument");
            return std::wstring(argv[++i]);
        };
        if (a == L"--model")
            o.model = value();
        else if (a == L"--scene")
            o.scene = value();
        else if (a == L"--screenshot")
            o.capture = value();
        else if (a == L"--report")
            o.report = value();
        else if (a == L"--width")
            o.width = std::stoi(value());
        else if (a == L"--height")
            o.height = std::stoi(value());
        else if (a == L"--instances")
            o.instances = std::stoi(value());
        else if (a == L"--benchmark")
            o.benchmark = std::stod(value());
        else if (a == L"--validate")
            o.validation = true;
        else if (a == L"--no-validation")
            o.validation = false;
        else if (a == L"--no-vsync")
            o.vsync = false;
        else if (a == L"--no-ui")
            o.ui = false;
        else if (a == L"--smoke")
            o.smoke = true;
        else if (a == L"--self-test")
            o.selfTest = true;
        else if (a == L"--help") {
            std::cout << "VkEngine [--model FILE | --scene FILE] [--instances N] [--validate] [--no-vsync] "
                         "[--no-ui] [--width N --height N] [--smoke] [--benchmark SECONDS --report FILE] "
                         "[--screenshot FILE]\n";
            LocalFree(argv);
            std::exit(0);
        } else
            throw std::runtime_error("Unknown command option");
    }
    LocalFree(argv);
    if (o.width < 640 || o.height < 480 || o.instances < 1 || o.instances > 10000 || o.benchmark < 0)
        throw std::runtime_error("Invalid command option range");
    return o;
}
class ImportWorker {
  public:
    struct Result {
        std::string key;
        std::shared_ptr<ImportedAsset> asset;
        std::string error;
    };

  private:
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::pair<std::string, fs::path>> tasks;
    std::deque<Result> results;
    bool stop = false;
    std::thread thread;

  public:
    ImportWorker()
        : thread([this] {
              for (;;) {
                  std::pair<std::string, fs::path> task;
                  {
                      std::unique_lock lock(mutex);
                      condition.wait(lock, [&] { return stop || !tasks.empty(); });
                      if (stop)
                          return;
                      task = std::move(tasks.front());
                      tasks.pop_front();
                  }
                  Result result;
                  result.key = task.first;
                  try {
                      result.asset = importAsset(task.second);
                  } catch (const std::exception &e) {
                      result.error = e.what();
                  }
                  {
                      std::lock_guard lock(mutex);
                      results.push_back(std::move(result));
                  }
              }
          }) {}
    ~ImportWorker() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        condition.notify_one();
        thread.join();
    }
    void enqueue(std::string key, fs::path path) {
        {
            std::lock_guard lock(mutex);
            tasks.emplace_back(std::move(key), std::move(path));
        }
        condition.notify_one();
    }
    std::deque<Result> take() {
        std::lock_guard lock(mutex);
        return std::exchange(results, {});
    }
};
class App {
    GLFWwindow *window{};
    Options config;
    std::unique_ptr<Renderer> renderer;
    ImportWorker worker;
    Scene scene;
    uint32_t selected = 0;
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    Viewport viewport;
    glm::vec2 viewOrigin{}, viewSize{1};
    bool dirty = false;
    std::deque<std::string> log;
    double cpuMs = 0;
    uint64_t frames = 0;
    struct Cached {
        fs::path path;
        AssetHandle handle = 0;
        std::string error;
        bool loading = true;
    };
    std::unordered_map<std::string, Cached> cache;
    struct Batch {
        std::vector<std::string> keys;
        size_t next = 0;
        glm::vec3 cursor{};
    };
    std::deque<Batch> batches;
    std::unordered_map<uint32_t, std::string> restoring;
    std::vector<double> cpuSamples, gpuSamples;
    std::optional<Clock::time_point> readySince, benchmarkStart;
    bool focused = false, captured = false, reported = false;
    double importMs = 0;
    Clock::time_point started = Clock::now();
    bool hadImportFailure = false;
    int testStage = 0, testCycle = 0;
    Clock::time_point testTime = Clock::now();
    std::optional<uint32_t> lastPick;
    uint32_t expectedPick = 0;
    glm::vec3 expectedDrop{};
    size_t beforeDrop = 0;
    fs::path testDirectory;
    uint64_t memoryBaseline = 0;

  public:
    explicit App(Options options) : config(std::move(options)) {}
    ~App() {
        renderer.reset();
        if (window)
            glfwDestroyWindow(window);
        glfwTerminate();
    }
    int run();

  private:
    void message(std::string text) {
        std::cout << text << "\n";
        log.push_back(std::move(text));
        while (log.size() > 200)
            log.pop_front();
    }
    std::string request(const fs::path &path);
    void add(const std::vector<fs::path> &paths, glm::vec3 point);
    void pollImports();
    void openScene(const fs::path &path);
    void save(bool saveAs);
    void focusAll();
    void focusSelected();
    void ui();
    void input();
    void drop(int count, const char **paths);
    void cleanup();
    bool loading() const;
    void report();
    void selfTest();
};
std::string App::request(const fs::path &p) {
    auto path = fs::weakly_canonical(fs::absolute(p));
    auto key = utf8(path);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return c < 128 ? char(std::tolower(c)) : char(c); });
    if (!cache.contains(key)) {
        cache[key] = {path, 0, {}, true};
        worker.enqueue(key, path);
    }
    return key;
}
void App::add(const std::vector<fs::path> &paths, glm::vec3 point) {
    Batch batch;
    batch.cursor = point;
    for (auto &path : paths)
        batch.keys.push_back(request(path));
    batches.push_back(std::move(batch));
}
void App::pollImports() {
    for (auto &result : worker.take()) {
        auto it = cache.find(result.key);
        if (it == cache.end())
            continue;
        auto &c = it->second;
        c.loading = false;
        if (!result.error.empty()) {
            c.error = result.error;
            message(result.error);
            hadImportFailure = true;
            continue;
        }
        try {
            for (auto &w : result.asset->warnings)
                message(w);
            c.handle = renderer->enqueueUpload(result.asset);
            message("Uploading: " + utf8(c.path.filename()));
        } catch (const std::exception &e) {
            c.error = e.what();
            message(c.error);
            hadImportFailure = true;
        }
    }
    for (auto it = restoring.begin(); it != restoring.end();) {
        auto *object = scene.find(it->first);
        auto &c = cache.at(it->second);
        if (!object) {
            it = restoring.erase(it);
            continue;
        }
        if (!c.error.empty()) {
            object->error = c.error;
            it = restoring.erase(it);
        } else if (c.handle && renderer->ready(c.handle)) {
            object->asset = c.handle;
            object->error.clear();
            it = restoring.erase(it);
        } else
            ++it;
    }
    for (auto batch = batches.begin(); batch != batches.end();) {
        while (batch->next < batch->keys.size()) {
            auto &c = cache.at(batch->keys[batch->next]);
            if (c.loading || (!c.handle && c.error.empty()) || (c.handle && !renderer->ready(c.handle)))
                break;
            if (c.error.empty()) {
                auto asset = renderer->asset(c.handle);
                SceneObject o;
                o.id = scene.nextId++;
                o.name = utf8(c.path.stem());
                o.source = c.path;
                o.asset = c.handle;
                o.transform.position = placement(asset->bounds, batch->cursor);
                scene.objects.push_back(o);
                selected = o.id;
                batch->cursor.x += asset->bounds.size().x + std::max(.1f, asset->bounds.size().x * .1f);
                dirty = true;
                message("Ready: " + o.name + " (" + std::to_string(asset->triangles()) + " triangles)");
            }
            batch->next++;
        }
        if (batch->next == batch->keys.size())
            batch = batches.erase(batch);
        else
            ++batch;
    }
}
bool App::loading() const {
    if (!batches.empty() || !restoring.empty() || renderer->environmentBusy())
        return true;
    return false;
}
void App::openScene(const fs::path &path) {
    try {
        auto replacement = loadScene(path);
        scene = std::move(replacement);
        batches.clear();
        restoring.clear();
        selected = 0;
        dirty = false;
        for (auto &o : scene.objects)
            if (o.error.empty())
                restoring[o.id] = request(o.source);
        renderer->setEnvironment(scene.environment.path);
        message("Scene opened: " + utf8(path));
    } catch (const std::exception &e) {
        message(std::string("Open failed: ") + e.what());
    }
}
void App::save(bool saveAs) {
    try {
        auto path = scene.file;
        if (saveAs || path.empty())
            path = fileDialog(window, true, true);
        if (path.empty())
            return;
        saveScene(scene, path);
        scene.file = fs::absolute(path);
        dirty = false;
        message("Scene saved: " + utf8(path));
    } catch (const std::exception &e) {
        message(e.what());
    }
}
void App::focusAll() {
    Bounds bounds;
    for (auto &o : scene.objects)
        if (o.visible)
            if (auto a = renderer->asset(o.asset))
                bounds.add(a->bounds.transformed(o.transform.matrix()));
    scene.camera.focus(bounds);
}
void App::focusSelected() {
    if (auto *o = scene.find(selected))
        if (auto a = renderer->asset(o->asset))
            scene.camera.focus(a->bounds.transformed(o->transform.matrix()));
}
void App::drop(int count, const char **paths) {
    try {
        double x, y;
        glfwGetCursorPos(window, &x, &y);
        glm::vec2 local = glm::vec2(float(x), float(y)) - viewOrigin;
        auto point = groundIntersection(local, viewSize, scene.camera);
        if (!point) {
            message("Drop a model onto the ground inside the viewport");
            return;
        }
        std::vector<fs::path> files;
        for (int i = 0; i < count; i++)
            files.push_back(fs::u8path(paths[i]));
        add(files, *point);
    } catch (const std::exception &e) {
        message(e.what());
    }
}
void App::cleanup() {
    std::vector<AssetHandle> keep;
    std::set<std::string> needed;
    for (auto &o : scene.objects)
        if (o.asset)
            keep.push_back(o.asset);
    for (auto &b : batches)
        for (size_t i = b.next; i < b.keys.size(); i++)
            needed.insert(b.keys[i]);
    for (auto &[id, key] : restoring)
        needed.insert(key);
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->second.loading || needed.contains(it->first)) {
            if (it->second.handle)
                keep.push_back(it->second.handle);
            ++it;
        } else if (it->second.handle && std::find(keep.begin(), keep.end(), it->second.handle) != keep.end())
            ++it;
        else
            it = cache.erase(it);
    }
    renderer->retainOnly(keep);
}
void App::ui() {
    auto &io = ImGui::GetIO();
    float width = io.DisplaySize.x, height = io.DisplaySize.y;
    float left = config.ui ? 230.f : 0, right = config.ui ? 300.f : 0, top = config.ui ? 64.f : 0,
          bottom = config.ui ? 140.f : 0;
    viewOrigin = {left, top};
    viewSize = {std::max(1.f, width - left - right), std::max(1.f, height - top - bottom)};
    int fbw, fbh;
    glfwGetFramebufferSize(window, &fbw, &fbh);
    float sx = fbw / std::max(width, 1.f), sy = fbh / std::max(height, 1.f);
    viewport = {int(left * sx), int(top * sy), int(viewSize.x * sx), int(viewSize.y * sy)};
    if (!config.ui)
        return;
    auto windowAt = [&](const char *name, ImVec2 pos, ImVec2 size) {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::Begin(name, nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings);
    };
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({width, top});
    ImGui::Begin("Toolbar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored({.35f, .8f, 1, 1}, "VkEngine  /  PBR Asset Studio");
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", scene.file.empty() ? "Untitled" : utf8(scene.file.filename()).c_str(),
                        dirty ? " *" : "");
    if (ImGui::Button("Import model")) {
        auto path = fileDialog(window, false);
        if (!path.empty())
            add({path}, {scene.camera.target.x, 0, scene.camera.target.z});
    }
    ImGui::SameLine();
    if (ImGui::Button("Open scene")) {
        auto path = fileDialog(window, false, true);
        if (!path.empty())
            openScene(path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        save(false);
    ImGui::SameLine();
    if (ImGui::Button("Save as"))
        save(true);
    ImGui::SameLine();
    if (ImGui::Button("Frame all"))
        focusAll();
    ImGui::SameLine();
    if (ImGui::Button("Screenshot")) {
        auto path = executableDirectory() / "screenshot.png";
        renderer->screenshot(path);
    }
    ImGui::End();
    windowAt("Scene", {0, top}, {left, height - top - bottom});
    ImGui::Text("%zu objects", scene.objects.size());
    if (loading())
        ImGui::TextColored({1, .75f, .3f, 1}, "Loading assets...");
    ImGui::Separator();
    for (auto &o : scene.objects) {
        ImGui::PushID(int(o.id));
        bool visible = o.visible;
        if (ImGui::Checkbox("##visible", &visible)) {
            o.visible = visible;
            dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Selectable((o.name + (o.asset ? "" : " [pending]")).c_str(), selected == o.id))
            selected = o.id;
        ImGui::PopID();
    }
    ImGui::End();
    windowAt("Properties", {width - right, top}, {right, height - top - bottom});
    if (auto *o = scene.find(selected)) {
        ImGui::TextWrapped("%s", o->name.c_str());
        ImGui::TextDisabled("Object ID: %u", o->id);
        ImGui::Separator();
        dirty |= ImGui::DragFloat3("Position", glm::value_ptr(o->transform.position), .01f);
        dirty |= ImGui::DragFloat3("Rotation", glm::value_ptr(o->transform.rotation), .5f);
        if (ImGui::DragFloat3("Scale", glm::value_ptr(o->transform.scale), .01f)) {
            for (int i = 0; i < 3; i++)
                if (std::abs(o->transform.scale[i]) < .001)
                    o->transform.scale[i] = std::signbit(o->transform.scale[i]) ? -.001f : .001f;
            dirty = true;
        }
        if (ImGui::RadioButton("Move", operation == ImGuizmo::TRANSLATE))
            operation = ImGuizmo::TRANSLATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", operation == ImGuizmo::ROTATE))
            operation = ImGuizmo::ROTATE;
        ImGui::SameLine();
        if (ImGui::RadioButton("Scale", operation == ImGuizmo::SCALE))
            operation = ImGuizmo::SCALE;
        if (ImGui::Button("Focus [F]"))
            focusSelected();
        ImGui::SameLine();
        bool remove = ImGui::Button("Delete");
        if (!o->error.empty()) {
            ImGui::TextWrapped("%s", o->error.c_str());
            if (ImGui::Button("Relocate asset")) {
                auto path = fileDialog(window, false);
                if (!path.empty()) {
                    o->source = path;
                    o->error.clear();
                    restoring[o->id] = request(path);
                    dirty = true;
                }
            }
        }
        if (auto a = renderer->asset(o->asset)) {
            ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(a->triangles()));
            ImGui::Text("Meshes: %zu / Images: %zu", a->meshes.size(), a->images.size());
            if (ImGui::CollapsingHeader("Materials"))
                for (auto &m : a->materials) {
                    ImGui::PushID(&m);
                    if (ImGui::TreeNode(m.name.c_str())) {
                        ImGui::Text("Metallic %.3f", m.metallic);
                        ImGui::Text("Roughness %.3f", m.roughness);
                        ImGui::Text("Alpha: %s", m.alphaMode == 2   ? "Blend"
                                                 : m.alphaMode == 1 ? "Mask"
                                                                    : "Opaque");
                        ImGui::Text("Double sided: %s", m.doubleSided ? "yes" : "no");
                        for (int t = 0; t < TextureCount; t++)
                            if (m.textures[t].image >= 0 && m.textures[t].image < int(a->images.size()))
                                ImGui::TextWrapped("%s", a->images[m.textures[t].image].name.c_str());
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
        }
        if (remove) {
            scene.objects.erase(std::remove_if(scene.objects.begin(), scene.objects.end(),
                                               [&](auto &obj) { return obj.id == selected; }),
                                scene.objects.end());
            selected = 0;
            dirty = true;
        }
    } else
        ImGui::TextWrapped("Drop model files onto the ground. Click a model to select it.");
    ImGui::SeparatorText("Environment");
    dirty |= ImGui::SliderFloat("Exposure", &scene.environment.exposure, -5, 5);
    dirty |= ImGui::SliderAngle("Rotation##env", &scene.environment.rotation, -180, 180);
    dirty |= ImGui::SliderFloat("IBL intensity", &scene.environment.intensity, 0, 5);
    dirty |= ImGui::SliderFloat("Sun intensity", &scene.environment.lightIntensity, 0, 10);
    dirty |= ImGui::ColorEdit3("Sun color", glm::value_ptr(scene.environment.lightColor));
    if (ImGui::DragFloat3("Sun direction", glm::value_ptr(scene.environment.lightDirection), .01f)) {
        if (glm::length(scene.environment.lightDirection) < .001f)
            scene.environment.lightDirection = {0, -1, 0};
        dirty = true;
    }
    dirty |= ImGui::Checkbox("Ground", &scene.environment.ground);
    ImGui::BeginDisabled(renderer->environmentBusy());
    if (ImGui::Button("Load HDR")) {
        auto path = fileDialog(window, false, false, true);
        if (!path.empty()) {
            renderer->setEnvironment(path);
            scene.environment.path = path;
            dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Studio")) {
        renderer->setEnvironment({});
        scene.environment.path.clear();
        dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::End();
    windowAt("Status", {0, height - bottom}, {width, bottom});
    auto &stats = renderer->stats();
    ImGui::Text("%s  |  CPU %.2f ms  GPU %.2f ms  |  %u draws  %llu triangles  |  GPU memory %.0f / %.0f MiB",
                stats.gpu.c_str(), cpuMs, stats.gpuMs, stats.draws,
                static_cast<unsigned long long>(stats.triangles), stats.allocatedBytes / 1048576.,
                stats.budgetBytes / 1048576.);
    ImGui::TextDisabled("RMB orbit   MMB pan   Wheel zoom   F focus   W/E/R transform   Ctrl+S save");
    if (!log.empty())
        ImGui::TextWrapped("%s", log.back().c_str());
    if (ImGui::Button("View log"))
        ImGui::OpenPopup("Import and renderer log");
    if (ImGui::BeginPopup("Import and renderer log")) {
        ImGui::BeginChild("messages", {760, 350});
        for (auto &line : log)
            ImGui::TextWrapped("%s", line.c_str());
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::End();
    if (auto *o = scene.find(selected); o && o->asset) {
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
        ImGuizmo::SetRect(viewOrigin.x, viewOrigin.y, viewSize.x, viewSize.y);
        auto view = scene.camera.view(), projection = scene.camera.projection(viewSize.x / viewSize.y, false),
             model = o->transform.matrix();
        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), operation, ImGuizmo::LOCAL,
                             glm::value_ptr(model));
        if (ImGuizmo::IsUsing()) {
            ImGuizmo::DecomposeMatrixToComponents(
                glm::value_ptr(model), glm::value_ptr(o->transform.position),
                glm::value_ptr(o->transform.rotation), glm::value_ptr(o->transform.scale));
            for (int i = 0; i < 3; i++)
                if (std::abs(o->transform.scale[i]) < .001f)
                    o->transform.scale[i] = .001f;
            dirty = true;
        }
    }
}
void App::input() {
    auto &io = ImGui::GetIO();
    glm::vec2 mouse{io.MousePos.x, io.MousePos.y};
    bool inside = mouse.x >= viewOrigin.x && mouse.y >= viewOrigin.y && mouse.x < viewOrigin.x + viewSize.x &&
                  mouse.y < viewOrigin.y + viewSize.y;
    if (inside && !ImGuizmo::IsUsing()) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            scene.camera.yaw -= io.MouseDelta.x * .006f;
            scene.camera.pitch = glm::clamp(scene.camera.pitch + io.MouseDelta.y * .006f, -1.5f, 1.5f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            auto view = scene.camera.view();
            glm::vec3 right{view[0][0], view[1][0], view[2][0]}, up{view[0][1], view[1][1], view[2][1]};
            scene.camera.target +=
                (-right * io.MouseDelta.x + up * io.MouseDelta.y) * (scene.camera.distance / viewSize.y);
        }
        scene.camera.distance =
            glm::clamp(scene.camera.distance * std::exp(-io.MouseWheel * .12f), .02f, 100000.f);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver() && !io.WantCaptureMouse) {
            auto local = mouse - viewOrigin;
            renderer->requestPick(int(local.x / viewSize.x * viewport.width),
                                  int(local.y / viewSize.y * viewport.height));
        }
    }
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F))
            focusSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_W))
            operation = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E))
            operation = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R))
            operation = ImGuizmo::SCALE;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) && selected && !ImGuizmo::IsUsing()) {
            scene.objects.erase(std::remove_if(scene.objects.begin(), scene.objects.end(),
                                               [&](auto &o) { return o.id == selected; }),
                                scene.objects.end());
            selected = 0;
            dirty = true;
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
            save(io.KeyShift);
    }
}
void App::selfTest() {
    auto require = [](bool condition, const char *text) {
        if (!condition)
            throw std::runtime_error(std::string("Integration test failed: ") + text);
    };
    auto advance = [&](int stage) {
        testStage = stage;
        testTime = Clock::now();
    };
    double elapsed = std::chrono::duration<double>(Clock::now() - testTime).count();
    if (testStage == 0) {
        require(!scene.objects.empty(), "model required");
        auto &o = scene.objects.front();
        auto a = renderer->asset(o.asset);
        require(bool(a), "asset ready");
        auto center = a->bounds.transformed(o.transform.matrix()).center();
        auto clip =
            scene.camera.projection(viewSize.x / viewSize.y) * scene.camera.view() * glm::vec4(center, 1);
        auto ndc = glm::vec2(clip) / clip.w;
        renderer->requestPick(int((ndc.x * .5f + .5f) * viewport.width),
                              int((ndc.y * .5f + .5f) * viewport.height));
        expectedPick = o.id;
        lastPick.reset();
        advance(1);
    } else if (testStage == 1 && lastPick) {
        require(*lastPick == expectedPick, "GPU picks mesh surface");
        message("TEST: asynchronous GPU object picking passed");
        glm::vec2 local{viewSize.x * .72f, viewSize.y * .7f};
        auto hit = groundIntersection(local, viewSize, scene.camera);
        require(hit.has_value(), "ground ray hit");
        expectedDrop = *hit;
        beforeDrop = scene.objects.size();
        glfwSetCursorPos(window, viewOrigin.x + local.x, viewOrigin.y + local.y);
        auto path = utf8(config.model);
        const char *paths[] = {path.c_str(), path.c_str()};
        drop(2, paths);
        scene.camera.yaw += .4f;
        advance(2);
    } else if (testStage == 2 && !loading()) {
        require(scene.objects.size() == beforeDrop + 2, "multiple file drop count");
        auto &first = scene.objects[beforeDrop];
        auto &second = scene.objects[beforeDrop + 1];
        auto a = renderer->asset(first.asset);
        require(first.asset == second.asset && first.asset == scene.objects.front().asset,
                "duplicate asset GPU resource sharing");
        require(glm::length(first.transform.position - placement(a->bounds, expectedDrop)) < .002f,
                "drop anchor retained while camera moves");
        require(second.transform.position.x > first.transform.position.x, "batch ground arrangement");
        message("TEST: multi-file drop, fixed anchor and shared GPU resources passed");
        scene.objects.front().visible = false;
        scene.objects.back().transform.scale.x = -1;
        renderer->requestPick(0, 0);
        lastPick.reset();
        advance(3);
    } else if (testStage == 3 && lastPick) {
        require(*lastPick == 0, "background pick clears selection");
        testDirectory =
            fs::temp_directory_path() / ("VkEngineIntegration-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(testDirectory);
        saveScene(scene, testDirectory / "roundtrip.vkscene");
        openScene(testDirectory / "roundtrip.vkscene");
        advance(4);
    } else if (testStage == 4 && !loading()) {
        require(scene.objects.size() == beforeDrop + 2 && !scene.objects.front().visible &&
                    scene.objects.back().transform.scale.x == -1,
                "scene save/open roundtrip");
        scene.objects.front().source = testDirectory / "missing.glb";
        saveScene(scene, testDirectory / "missing.vkscene");
        openScene(testDirectory / "missing.vkscene");
        advance(5);
    } else if (testStage == 5 && !loading()) {
        require(!scene.objects.front().error.empty(), "missing asset record preserved");
        auto &object = scene.objects.front();
        object.source = config.model;
        object.error.clear();
        restoring[object.id] = request(config.model);
        {
            std::ofstream bad(testDirectory / "bad.vkscene");
            bad << "{invalid";
        }
        auto count = scene.objects.size();
        openScene(testDirectory / "bad.vkscene");
        require(scene.objects.size() == count, "invalid scene leaves current scene intact");
        advance(6);
    } else if (testStage == 6 && !loading()) {
        require(scene.objects.front().asset != 0 && scene.objects.front().error.empty(), "asset relocation");
        message("TEST: save/open, missing asset, relocation and invalid scene passed");
        glfwSetWindowSize(window, 1100, 760);
        advance(7);
    } else if (testStage == 7 && elapsed > .35) {
        glfwIconifyWindow(window);
        advance(8);
    } else if (testStage == 9) {
        glfwSetWindowSize(window, config.width, config.height);
        scene.objects.clear();
        restoring.clear();
        selected = 0;
        advance(10);
    } else if (testStage == 10 && elapsed > .3) {
        if (testCycle == 0)
            memoryBaseline = renderer->stats().allocatedBytes;
        if (testCycle >= 3) {
            require(renderer->stats().allocatedBytes <= memoryBaseline + 16 * 1024 * 1024,
                    "resource allocation returns to baseline");
            message("TEST: resize/minimize and repeated import/delete resource lifetime passed");
            add({config.model}, {0, 0, 0});
            advance(12);
        } else {
            add({config.model}, {0, 0, 0});
            advance(11);
        }
    } else if (testStage == 11 && !loading()) {
        require(scene.objects.size() == 1, "reload after deletion");
        testCycle++;
        scene.objects.clear();
        selected = 0;
        advance(10);
    } else if (testStage == 12 && !loading()) {
        focusAll();
        selected = scene.objects.front().id;
        if (!config.capture.empty())
            renderer->screenshot(config.capture);
        advance(13);
    } else if (testStage == 13 && elapsed > .5) {
        require(renderer->stats().validationErrors == 0, "zero Vulkan validation errors");
        message("All runtime integration tests passed");
        for (auto name : {"roundtrip.vkscene", "missing.vkscene", "bad.vkscene"})
            fs::remove(testDirectory / name);
        fs::remove(testDirectory);
        report();
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        advance(14);
    }
    if ((testStage == 1 || testStage == 3) &&
        std::chrono::duration<double>(Clock::now() - testTime).count() > 5)
        throw std::runtime_error("GPU pick readback timed out");
}
void App::report() {
    auto mean = [](const std::vector<double> &v) {
        return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.) / v.size();
    };
    auto percentile = [](std::vector<double> v) {
        if (v.empty())
            return 0.;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(std::ceil(v.size() * .95)) - 1)];
    };
    auto &s = renderer->stats();
    nlohmann::json data{{"gpu", s.gpu},
                        {"width", viewport.width},
                        {"height", viewport.height},
                        {"objects", scene.objects.size()},
                        {"msaa", 4},
                        {"validation", config.validation},
                        {"validationErrors", s.validationErrors},
                        {"frames", cpuSamples.size()},
                        {"durationSeconds", config.benchmark},
                        {"cpuMeanMs", mean(cpuSamples)},
                        {"cpuP95Ms", percentile(cpuSamples)},
                        {"gpuMeanMs", mean(gpuSamples)},
                        {"gpuP95Ms", percentile(gpuSamples)},
                        {"gpuAllocatedBytes", s.allocatedBytes},
                        {"triangles", s.triangles},
                        {"drawCalls", s.draws},
                        {"importMilliseconds", importMs}};
    std::cout << data.dump(2) << "\n";
    if (!config.report.empty()) {
        std::ofstream f(config.report);
        if (!f)
            throw std::runtime_error("Cannot write report");
        f << data.dump(2);
    }
}
int App::run() {
    glfwSetErrorCallback([](int, const char *error) { std::cerr << error << "\n"; });
    if (!glfwInit())
        throw std::runtime_error("GLFW initialization failed");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, config.benchmark > 0 ? GLFW_FALSE : GLFW_TRUE);
    window = glfwCreateWindow(config.width, config.height, "VkEngine | PBR Asset Studio", nullptr, nullptr);
    if (!window)
        throw std::runtime_error("Window creation failed");
    renderer = std::make_unique<Renderer>(window, config.validation, config.vsync);
    glfwSetWindowUserPointer(window, this);
    glfwSetDropCallback(window, [](GLFWwindow *w, int n, const char **p) {
        static_cast<App *>(glfwGetWindowUserPointer(w))->drop(n, p);
    });
    if (!config.scene.empty())
        openScene(config.scene);
    if (!config.model.empty()) {
        std::vector<fs::path> paths(size_t(config.instances), config.model);
        add(paths, {0, 0, 0});
    }
    while (!glfwWindowShouldClose(window)) {
        auto frameStart = Clock::now();
        glfwPollEvents();
        if (config.selfTest && testStage == 8 &&
            std::chrono::duration<double>(Clock::now() - testTime).count() > .25) {
            glfwRestoreWindow(window);
            testStage = 9;
        }
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        if (w == 0 || h == 0) {
            glfwWaitEventsTimeout(.05);
            continue;
        }
        renderer->newFrame();
        if (!renderer->environmentBusy())
            scene.environment.path = renderer->environmentPath();
        ImGuizmo::BeginFrame();
        pollImports();
        if (auto id = renderer->picked()) {
            selected = *id;
            lastPick = *id;
        }
        for (auto &m : renderer->takeMessages())
            message(m);
        if (!loading() && !focused && (!config.model.empty() || !config.scene.empty())) {
            if (config.scene.empty())
                focusAll();
            focused = true;
            importMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            readySince = Clock::now();
        }
        ui();
        if (config.selfTest && focused)
            selfTest();
        input();
        ImGui::Render();
        renderer->render(scene, viewport, config.ui ? selected : 0);
        cleanup();
        frames++;
        cpuMs = std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count();
        if (readySince) {
            double sinceReady = std::chrono::duration<double>(Clock::now() - *readySince).count();
            if (config.benchmark > 0 && sinceReady > 3) {
                if (!benchmarkStart)
                    benchmarkStart = Clock::now();
                cpuSamples.push_back(cpuMs);
                gpuSamples.push_back(renderer->stats().gpuMs);
                if (std::chrono::duration<double>(Clock::now() - *benchmarkStart).count() >=
                    config.benchmark) {
                    if (!reported) {
                        report();
                        reported = true;
                    }
                    if (!config.capture.empty() && !captured) {
                        renderer->screenshot(config.capture);
                        captured = true;
                    } else if (captured || config.capture.empty()) {
                        if (config.capture.empty() || sinceReady > config.benchmark + 3.2)
                            break;
                    }
                }
            } else if (config.smoke && !config.selfTest && sinceReady > 2) {
                if (!captured && !config.capture.empty()) {
                    renderer->screenshot(config.capture);
                    captured = true;
                }
                if (sinceReady > 3) {
                    report();
                    break;
                }
            } else if (!config.smoke && !config.benchmark && !config.capture.empty() && !captured &&
                       sinceReady > 2) {
                renderer->screenshot(config.capture);
                captured = true;
            }
        }
        if ((config.smoke || config.selfTest || config.benchmark > 0) &&
            std::chrono::duration<double>(Clock::now() - started).count() > config.benchmark + 180)
            throw std::runtime_error("Automated run timed out");
    }
    return renderer->stats().validationErrors || hadImportFailure ? 1 : 0;
}
} // namespace vke
int main() {
    try {
        auto options = vke::options();
        vke::App app(options);
        return app.run();
    } catch (const std::exception &e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        if (GetConsoleWindow()) {
        } else
            vke::showError(e.what());
        return 1;
    }
}
