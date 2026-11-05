// Development host (Linux/macOS): GLFW + OpenGL 3. Demo Mode only.
//
//   gsmacro                      interactive
//   gsmacro --screenshots DIR    scripted run that plays demo scenarios and
//                                writes PPM screenshots, then exits
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "gui.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace {

bool writePpm(const std::string& path, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) std::fwrite(&px[(size_t)y * w * 3], 1, (size_t)w * 3, f);
    std::fclose(f);
    return true;
}

struct Shot {
    double atMs;                       // time since script start
    std::function<void(gsm::App&)> action;
    const char* file;                  // nullptr = action only
};

}  // namespace

int main(int argc, char** argv) {
    std::string shotDir;
    int width = 1480, height = 940;
    float scale = 1.0f;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--screenshots") && i + 1 < argc) shotDir = argv[++i];
        else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &width, &height);
        else if (!std::strcmp(argv[i], "--scale") && i + 1 < argc) scale = (float)std::atof(argv[++i]);
    }
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* win = glfwCreateWindow(width, height, "gsmacro - GameSir macro tool (Demo)", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    {
        gsm::App app;
        app.loadFonts(scale);
        app.applyStyle(scale);

        std::vector<Shot> script;
        if (!shotDir.empty()) {
            script = {
                {700, nullptr, "01-overview.ppm"},
                {800, [](gsm::App& a) { a.playScenario("L1 + Square"); }, nullptr},
                {1080, nullptr, "02-left-macro-running.ppm"},
                {1900, [](gsm::App& a) { a.selectMacroTab(1); a.playScenario("R1 + Square x5 rapid (FIFO)"); }, nullptr},
                {2230, nullptr, "03-fifo-queue.ppm"},
                {3000, [](gsm::App& a) { a.setTimelinePaused(true); }, nullptr},
                {3150, nullptr, "04-fifo-timeline.ppm"},
                {3200, [](gsm::App& a) { a.setTimelinePaused(false); a.selectMacroTab(0); a.playScenario("Steering during macro"); }, nullptr},
                {3460, nullptr, "05-stick-override.ppm"},
                {4200, [](gsm::App& a) { a.playScenario("Disconnect mid-macro"); }, nullptr},
                {4430, nullptr, "06-disconnect.ppm"},
                {5200, [](gsm::App& a) { a.setTextMode(0, true); }, nullptr},
                {5400, nullptr, "07-text-editor.ppm"},
                {5500, [](gsm::App& a) { a.setTextMode(0, false); a.selectMacroTab(1); a.triggerMacro(1); }, nullptr},
                {5560, nullptr, "08-test-button-right.ppm"},
                {6000, [](gsm::App& a) { a.showSetup(true); }, nullptr},
                {6200, nullptr, "09-setup-wizard.ppm"},
            };
        }
        size_t next = 0;
        auto start = std::chrono::steady_clock::now();
        auto frameDur = std::chrono::microseconds(16667);
        while (!glfwWindowShouldClose(win)) {
            auto f0 = std::chrono::steady_clock::now();
            glfwPollEvents();
            double t = std::chrono::duration<double, std::milli>(f0 - start).count();
            const char* capture = nullptr;
            while (next < script.size() && t >= script[next].atMs) {
                if (script[next].action) script[next].action(app);
                if (script[next].file) capture = script[next].file;
                next++;
                if (capture) break;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame();
            ImGui::Render();
            int fw, fh;
            glfwGetFramebufferSize(win, &fw, &fh);
            glViewport(0, 0, fw, fh);
            glClearColor(0.078f, 0.090f, 0.110f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (capture) {
                std::string p = shotDir + "/" + capture;
                std::printf("%s %s\n", writePpm(p, fw, fh) ? "wrote" : "FAILED", p.c_str());
            }
            glfwSwapBuffers(win);
            if (!script.empty() && next >= script.size()) break;
            // Cap the GUI at ~60 fps so it never spins a core.
            std::this_thread::sleep_until(f0 + frameDur);
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
