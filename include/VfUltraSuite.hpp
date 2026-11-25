#pragma once

#include "VfCore.hpp"
#include "VfModules.hpp"
#include "VfExtratools.hpp"
#include "VfFormatRegistry.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#if defined(BUILD_WITH_IMGUI)
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifndef VORTEXFORGE_VERSION
#define VORTEXFORGE_VERSION "1.0.0"
#endif

#ifndef VORTEXFORGE_GITHUB_REPO
#define VORTEXFORGE_GITHUB_REPO "yourname/VortexForge"
#endif

namespace VfUltra {

namespace fs = std::filesystem;

// ============================================================================
// 0. МУЛЬТИЯЗЫЧНОСТЬ (RU / EN)
// ============================================================================
enum class UiLang { RU = 0, EN = 1 };

struct Loc {
    static const char* tr(UiLang lang, const char* ru, const char* en) {
        return (lang == UiLang::EN) ? en : ru;
    }
};

// ============================================================================
// 0.5 ПРОВЕРКА ОБНОВЛЕНИЙ ЧЕРЕЗ GITHUB RELEASES (WINHTTP)
// ============================================================================
struct UpdateCheckState {
    std::atomic<bool> checking{false};
    std::atomic<bool> done{false};
    bool updateAvailable = false;
    std::string latestVersion;
    std::string statusMessage;
    std::mutex mtx;

    void startAsyncCheck() {
        if (checking.exchange(true)) return;
        done = false;

        std::thread([this]() {
            std::string foundTag;
            bool ok = false;

#ifdef _WIN32
            std::string repo = VORTEXFORGE_GITHUB_REPO;
            std::wstring wPath = L"/repos/" + std::wstring(repo.begin(), repo.end()) + L"/releases/latest";

            HINTERNET hSession = WinHttpOpen(L"VortexForge-Updater/1.0",
                                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                             WINHTTP_NO_PROXY_NAME,
                                             WINHTTP_NO_PROXY_BYPASS, 0);
            if (hSession) {
                HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
                if (hConnect) {
                    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
                                                            nullptr, WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                            WINHTTP_FLAG_SECURE);
                    if (hRequest) {
                        std::wstring headers = L"User-Agent: VortexForge\r\nAccept: application/vnd.github+json\r\n";
                        if (WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.size()),
                                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                            WinHttpReceiveResponse(hRequest, nullptr)) {

                            std::string body;
                            DWORD dwSize = 0;
                            while (WinHttpQueryDataAvailable(hRequest, &dwSize) && dwSize > 0) {
                                std::vector<char> buf(dwSize + 1, 0);
                                DWORD dwDownloaded = 0;
                                if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded) && dwDownloaded > 0) {
                                    body.append(buf.data(), dwDownloaded);
                                } else {
                                    break;
                                }
                            }

                            size_t tagPos = body.find("\"tag_name\"");
                            if (tagPos != std::string::npos) {
                                size_t q1 = body.find('"', tagPos + 10);
                                size_t q2 = (q1 != std::string::npos) ? body.find('"', q1 + 1) : std::string::npos;
                                if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1) {
                                    foundTag = body.substr(q1 + 1, q2 - q1 - 1);
                                    if (!foundTag.empty() && (foundTag[0] == 'v' || foundTag[0] == 'V')) {
                                        foundTag.erase(0, 1);
                                    }
                                    ok = true;
                                }
                            }
                        }
                        WinHttpCloseHandle(hRequest);
                    }
                    WinHttpCloseHandle(hConnect);
                }
                WinHttpCloseHandle(hSession);
            }
#endif
            {
                std::lock_guard<std::mutex> lock(mtx);
                if (ok && !foundTag.empty()) {
                    latestVersion = foundTag;
                    updateAvailable = (foundTag != std::string(VORTEXFORGE_VERSION));
                    statusMessage = updateAvailable
                        ? ("Доступна новая версия: v" + foundTag + " (текущая v" + std::string(VORTEXFORGE_VERSION) + ")")
                        : ("У вас актуальная версия v" + std::string(VORTEXFORGE_VERSION));
                } else {
                    updateAvailable = false;
                    statusMessage = "Не удалось проверить обновления (проверьте интернет или репозиторий GitHub).";
                }
            }
            checking = false;
            done = true;
        }).detach();
    }
};

// ============================================================================
// 1. VPK INSPECTOR & EXTRACTOR (С ЗАЩИТОЙ ОТ ПОВРЕЖДЁННЫХ ФАЙЛОВ)
// ============================================================================
struct VpkEntry {
    std::string fullPath;
    uint32_t crc = 0;
    uint16_t preloadBytes = 0;
    uint16_t archiveIndex = 0;
    uint32_t entryOffset = 0;
    uint32_t entryLength = 0;
    std::vector<uint8_t> preloadData;
};

class VpkInspector {
public:
    static bool readDirectory(const fs::path& vpkPath, std::vector<VpkEntry>& outEntries, uint32_t& outVersion) {
        outEntries.clear();
        outVersion = 0;

        std::error_code ec;
        uintmax_t fileSize = fs::file_size(vpkPath, ec);
        if (ec || fileSize < 12) return false;

        std::ifstream in(vpkPath, std::ios::binary);
        if (!in.is_open()) return false;

        uint32_t signature = 0;
        uint32_t version = 0;
        uint32_t treeSize = 0;
        in.read(reinterpret_cast<char*>(&signature), 4);
        in.read(reinterpret_cast<char*>(&version), 4);
        in.read(reinterpret_cast<char*>(&treeSize), 4);

        if (!in.good() || signature != 0x55aa1234 || (version != 1 && version != 2)) {
            return false;
        }
        // Защита от повреждённого treeSize, превышающего размер файла
        if (treeSize == 0 || static_cast<uintmax_t>(treeSize) > fileSize) {
            return false;
        }
        outVersion = version;

        if (version == 2) {
            if (fileSize < 28) return false;
            in.seekg(16, std::ios::cur);
            if (!in.good()) return false;
        }

        // Безопасное чтение C-строки с лимитом длины, чтобы битый файл не вызвал OOM
        auto readNullStr = [&in](std::string& str, size_t maxLen = 512) -> bool {
            str.clear();
            char ch = 0;
            while (in.get(ch)) {
                if (ch == '\0') return true;
                if (str.size() >= maxLen) return false;
                str.push_back(ch);
            }
            return false;
        };

        constexpr size_t kMaxEntries = 250000;
        while (in.good()) {
            std::string ext;
            if (!readNullStr(ext, 64) || ext.empty()) break;

            while (in.good()) {
                std::string dir;
                if (!readNullStr(dir, 512) || dir.empty()) break;

                while (in.good()) {
                    std::string fileName;
                    if (!readNullStr(fileName, 256) || fileName.empty()) break;

                    if (outEntries.size() >= kMaxEntries) {
                        return false;
                    }

                    VpkEntry entry;
                    uint16_t terminator = 0;
                    in.read(reinterpret_cast<char*>(&entry.crc), 4);
                    in.read(reinterpret_cast<char*>(&entry.preloadBytes), 2);
                    in.read(reinterpret_cast<char*>(&entry.archiveIndex), 2);
                    in.read(reinterpret_cast<char*>(&entry.entryOffset), 4);
                    in.read(reinterpret_cast<char*>(&entry.entryLength), 4);
                    in.read(reinterpret_cast<char*>(&terminator), 2);

                    if (!in.good() || terminator != 0xffff) return false;

                    if (entry.preloadBytes > 0) {
                        if (static_cast<uintmax_t>(entry.preloadBytes) > fileSize) return false;
                        entry.preloadData.resize(entry.preloadBytes);
                        in.read(reinterpret_cast<char*>(entry.preloadData.data()), entry.preloadBytes);
                        if (!in.good()) return false;
                    }

                    std::string cleanDir = (dir == " ") ? "" : (dir + "/");
                    std::string cleanExt = (ext == " ") ? "" : ("." + ext);
                    entry.fullPath = cleanDir + fileName + cleanExt;
                    outEntries.push_back(std::move(entry));
                }
            }
        }
        return true;
    }

    static int extractMatching(const fs::path& vpkPath,
                               const std::string& filterSubstr,
                               const fs::path& outDir) {
        std::vector<VpkEntry> entries;
        uint32_t version = 0;
        if (!readDirectory(vpkPath, entries, version)) return -1;

        std::string needle = VfV2::toLower(filterSubstr);
        int extracted = 0;

        for (const auto& e : entries) {
            if (!needle.empty() && VfV2::toLower(e.fullPath).find(needle) == std::string::npos) {
                continue;
            }
            // Защита от path traversal ("..") внутри повреждённого/злонамеренного VPK
            if (e.fullPath.find("..") != std::string::npos) {
                continue;
            }

            fs::path target = outDir / e.fullPath;
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);
            std::ofstream out(target, std::ios::binary);
            if (!out.is_open()) continue;

            if (!e.preloadData.empty()) {
                out.write(reinterpret_cast<const char*>(e.preloadData.data()),
                          static_cast<std::streamsize>(e.preloadData.size()));
            }

            if (e.entryLength > 0 && e.entryLength < 512u * 1024u * 1024u) {
                fs::path dataVpk = resolveDataArchivePath(vpkPath, e.archiveIndex);
                uintmax_t archSize = fs::file_size(dataVpk, ec);
                if (!ec && static_cast<uintmax_t>(e.entryOffset) + e.entryLength <= archSize) {
                    std::ifstream dataIn(dataVpk, std::ios::binary);
                    if (dataIn.is_open()) {
                        dataIn.seekg(static_cast<std::streamoff>(e.entryOffset), std::ios::beg);
                        std::vector<char> buf(e.entryLength);
                        dataIn.read(buf.data(), static_cast<std::streamsize>(e.entryLength));
                        if (dataIn.gcount() > 0) {
                            out.write(buf.data(), dataIn.gcount());
                        }
                    }
                }
            }
            ++extracted;
        }
        return extracted;
    }

private:
    static fs::path resolveDataArchivePath(const fs::path& dirVpkPath, uint16_t archiveIndex) {
        if (archiveIndex == 0x7fff) return dirVpkPath;
        std::string stem = dirVpkPath.stem().string();
        if (stem.size() >= 4 && stem.substr(stem.size() - 4) == "_dir") {
            stem = stem.substr(0, stem.size() - 4);
        }
        std::ostringstream oss;
        oss << stem << "_" << std::setw(3) << std::setfill('0') << archiveIndex << ".vpk";
        return dirVpkPath.parent_path() / oss.str();
    }
};

// ============================================================================
// 2. MATERIAL DOCTOR & PACK MANIFEST BUILDER
// ============================================================================
struct MaterialFixReport {
    int normalizedKeys = 0;
    std::vector<std::string> missingTextures;
};

class MaterialDoctor {
public:
    static MaterialFixReport healAndCheckVmt(const fs::path& vmtInPath,
                                             const fs::path& vmtOutPath,
                                             const std::vector<fs::path>& searchDirs) {
        MaterialFixReport rep;
        std::ifstream in(vmtInPath);
        if (!in.is_open()) return rep;

        std::vector<std::string> lines;
        std::set<std::string> texRefs;
        std::string line;

        while (std::getline(in, line)) {
            std::string t = VfV2::trim(line);
            std::string low = VfV2::toLower(t);

            bool isTexParam = false;
            for (const std::string& key : {"$basetexture", "$bumpmap", "$normalmap", "$detail", "$envmapmask"}) {
                if (low.find(key) != std::string::npos) {
                    isTexParam = true;
                    break;
                }
            }

            if (isTexParam) {
                std::string fixed = line;
                std::replace(fixed.begin(), fixed.end(), '\\', '/');
                size_t q1 = fixed.find('"');
                size_t q2 = (q1 != std::string::npos) ? fixed.find('"', q1 + 1) : std::string::npos;
                size_t q3 = (q2 != std::string::npos) ? fixed.find('"', q2 + 1) : std::string::npos;
                size_t q4 = (q3 != std::string::npos) ? fixed.find('"', q3 + 1) : std::string::npos;

                if (q3 != std::string::npos && q4 != std::string::npos && q4 > q3 + 1) {
                    std::string texVal = VfV2::toLower(fixed.substr(q3 + 1, q4 - q3 - 1));
                    fixed.replace(q3 + 1, q4 - q3 - 1, texVal);
                    if (texVal != "env_cubemap") {
                        texRefs.insert("materials/" + texVal + ".vtf");
                    }
                    ++rep.normalizedKeys;
                }
                lines.push_back(fixed);
            } else {
                lines.push_back(line);
            }
        }

        if (!vmtOutPath.empty()) {
            std::error_code ec;
            fs::create_directories(vmtOutPath.parent_path(), ec);
            std::ofstream out(vmtOutPath);
            for (const auto& l : lines) out << l << "\n";
        }

        if (!searchDirs.empty()) {
            rep.missingTextures = VfV2::ExtraTools::checkMissingAssets(texRefs, searchDirs);
        }
        return rep;
    }

    static bool packWithManifest(const fs::path& sourceFile,
                                 const std::vector<fs::path>& searchDirs,
                                 const fs::path& packOutDir,
                                 bool dryRun) {
        std::error_code ec;
        if (!fs::exists(sourceFile, ec)) return false;

        auto refs = VfV2::DepAndMapTools::collectAssetRefs(sourceFile);
        auto missing = VfV2::ExtraTools::checkMissingAssets(refs, searchDirs);

        int copied = 0;
        uintmax_t totalBytes = 0;
        std::vector<std::string> packedList;

        for (const auto& rawRef : refs) {
            std::string norm = rawRef;
            std::replace(norm.begin(), norm.end(), '\\', '/');
            while (!norm.empty() && norm.front() == '/') norm.erase(norm.begin());

            for (const auto& dir : searchDirs) {
                if (dir.empty() || !fs::exists(dir, ec)) continue;

                fs::path foundSrc;
                fs::path relDest;
                if (fs::exists(dir / norm, ec)) {
                    foundSrc = dir / norm;
                    relDest = norm;
                } else if (fs::exists(dir / "materials" / norm, ec)) {
                    foundSrc = dir / "materials" / norm;
                    relDest = fs::path("materials") / norm;
                } else if (fs::exists(dir / "models" / norm, ec)) {
                    foundSrc = dir / "models" / norm;
                    relDest = fs::path("models") / norm;
                }

                if (!foundSrc.empty()) {
                    totalBytes += fs::file_size(foundSrc, ec);
                    packedList.push_back(relDest.generic_string());
                    if (!dryRun) {
                        fs::path dest = packOutDir / relDest;
                        fs::create_directories(dest.parent_path(), ec);
                        fs::copy_file(foundSrc, dest, fs::copy_options::overwrite_existing, ec);
                    }
                    ++copied;
                    break;
                }
            }
        }

        fs::create_directories(packOutDir, ec);
        std::ofstream man(packOutDir / "manifest.json");
        if (!man.is_open()) return false;

        man << "{\n"
            << "  \"source\": \"" << VfV2::jsonEscape(sourceFile.filename().string()) << "\",\n"
            << "  \"dryRun\": " << (dryRun ? "true" : "false") << ",\n"
            << "  \"totalReferences\": " << refs.size() << ",\n"
            << "  \"packedCount\": " << copied << ",\n"
            << "  \"missingCount\": " << missing.size() << ",\n"
            << "  \"totalBytes\": " << totalBytes << ",\n"
            << "  \"packedFiles\": [";

        for (size_t i = 0; i < packedList.size(); ++i) {
            man << (i == 0 ? "\n    " : ",\n    ")
                << "\"" << VfV2::jsonEscape(packedList[i]) << "\"";
        }
        man << (packedList.empty() ? "" : "\n  ") << "],\n"
            << "  \"missingFiles\": [";

        for (size_t i = 0; i < missing.size(); ++i) {
            man << (i == 0 ? "\n    " : ",\n    ")
                << "\"" << VfV2::jsonEscape(missing[i]) << "\"";
        }
        man << (missing.empty() ? "" : "\n  ") << "]\n}\n";
        return man.good();
    }
};

// ============================================================================
// 3. DEMO ULTRA TOOLS
// ============================================================================
struct DemoHeaderInfo {
    bool valid = false;
    int32_t demoProtocol = 0;
    int32_t networkProtocol = 0;
    std::string serverName;
    std::string clientName;
    std::string mapName;
    std::string gameDirectory;
    float playbackTime = 0.0f;
    int32_t playbackTicks = 0;
    int32_t playbackFrames = 0;
};

class DemoUltraTools {
public:
    static DemoHeaderInfo parseHeader(const fs::path& demPath) {
        DemoHeaderInfo info{};
        std::error_code ec;
        if (fs::file_size(demPath, ec) < 1072) return info;

        std::ifstream in(demPath, std::ios::binary);
        if (!in.is_open()) return info;

        char magic[8] = {};
        in.read(magic, 8);
        if (in.gcount() != 8 || std::strncmp(magic, "HL2DEMO", 7) != 0) {
            return info;
        }

        auto readFixedStr = [&in](size_t len) -> std::string {
            std::vector<char> buf(len + 1, '\0');
            in.read(buf.data(), static_cast<std::streamsize>(len));
            return std::string(buf.data());
        };

        in.read(reinterpret_cast<char*>(&info.demoProtocol), 4);
        in.read(reinterpret_cast<char*>(&info.networkProtocol), 4);
        info.serverName = readFixedStr(260);
        info.clientName = readFixedStr(260);
        info.mapName = readFixedStr(260);
        info.gameDirectory = readFixedStr(260);
        in.read(reinterpret_cast<char*>(&info.playbackTime), 4);
        in.read(reinterpret_cast<char*>(&info.playbackTicks), 4);
        in.read(reinterpret_cast<char*>(&info.playbackFrames), 4);

        info.valid = in.good();
        return info;
    }

    static bool exportTimelineMarkers(const fs::path& demPath,
                                      const std::vector<VfV2::ClipRange>& clips,
                                      double tickRate,
                                      const fs::path& outDir) {
        if (tickRate <= 0.0) tickRate = 64.0;
        std::error_code ec;
        fs::create_directories(outDir, ec);

        std::ofstream cfg(outDir / (demPath.stem().string() + "_bookmarks.cfg"));
        std::ofstream yt(outDir / (demPath.stem().string() + "_chapters.txt"));
        if (!cfg.is_open() || !yt.is_open()) return false;

        cfg << "// Закладки тиков для демки: " << demPath.filename().string() << "\n"
            << "playdemo " << demPath.stem().string() << "\n\n";
        yt << "Таймкоды клипов (" << demPath.filename().string() << "):\n";

        for (size_t i = 0; i < clips.size(); ++i) {
            int tick = static_cast<int>(clips[i].startSec * tickRate);
            int totalSec = static_cast<int>(clips[i].startSec);
            int mm = totalSec / 60;
            int ss = totalSec % 60;

            cfg << "alias clip_" << (i + 1) << " \"demo_gototick " << tick << "; echo Jump to " << clips[i].name << "\"\n";
            yt << std::setw(2) << std::setfill('0') << mm << ":"
               << std::setw(2) << std::setfill('0') << ss << " - " << clips[i].name
               << " (tick " << tick << ")\n";
        }
        return cfg.good() && yt.good();
    }
};

// ============================================================================
// 4. PROJECT CONFIG (С ПОДДЕРЖКОЙ ЯЗЫКА И ТЕМЫ)
// ============================================================================
struct ProjectConfig {
    std::string gameDir = "./game";
    std::string customDir = "./custom";
    std::string outputDir = "./vortex_output";
    double defaultTickRate = 64.0;
    UiLang lang = UiLang::RU;
    bool darkTheme = false;
    fs::path sourcePath = "vortexforge.cfg";

    void save() const {
        std::ofstream out(sourcePath);
        if (out.is_open()) {
            out << "# Конфигурация VortexForge v" << VORTEXFORGE_VERSION << "\n"
                << "game_dir=" << gameDir << "\n"
                << "custom_dir=" << customDir << "\n"
                << "output_dir=" << outputDir << "\n"
                << "tickrate=" << defaultTickRate << "\n"
                << "lang=" << (lang == UiLang::EN ? "en" : "ru") << "\n"
                << "dark_theme=" << (darkTheme ? "1" : "0") << "\n";
        }
    }

    static ProjectConfig loadOrCreate(const fs::path& cfgPath) {
        ProjectConfig cfg;
        cfg.sourcePath = cfgPath;
        if (!fs::exists(cfgPath)) {
            cfg.save();
            return cfg;
        }

        std::ifstream in(cfgPath);
        std::string line;
        while (std::getline(in, line)) {
            std::string t = VfV2::trim(line);
            if (t.empty() || t[0] == '#') continue;
            size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string k = VfV2::toLower(VfV2::trim(t.substr(0, eq)));
            std::string v = VfV2::trim(t.substr(eq + 1));

            if (k == "game_dir") cfg.gameDir = v;
            else if (k == "custom_dir") cfg.customDir = v;
            else if (k == "output_dir") cfg.outputDir = v;
            else if (k == "lang") cfg.lang = (VfV2::toLower(v) == "en") ? UiLang::EN : UiLang::RU;
            else if (k == "dark_theme") cfg.darkTheme = (v == "1" || VfV2::toLower(v) == "true");
            else if (k == "tickrate") {
                try { cfg.defaultTickRate = std::stod(v); } catch (...) {}
            }
        }
        return cfg;
    }
};

// ============================================================================
// 5. DEAR IMGUI SHELL
// ============================================================================
class GuiShell {
public:
    static int run(VfCore::LogSink& logger, ProjectConfig& cfg) {
#if !defined(BUILD_WITH_IMGUI)
        (void)cfg;
        logger.write(VfCore::Level::Warn,
            "GUI собран без Dear ImGui. Пересоберите проект с флагом: cmake -S . -B build -DBUILD_WITH_IMGUI=ON");
        return 1;
#else
        if (!glfwInit()) {
            logger.write(VfCore::Level::Error, "Не удалось инициализировать GLFW.");
            return 1;
        }

        std::string winTitle = std::string("VortexForge v") + VORTEXFORGE_VERSION;
        GLFWwindow* window = glfwCreateWindow(820, 680, winTitle.c_str(), nullptr, nullptr);
        if (!window) {
            glfwTerminate();
            return 1;
        }

        int iconW = 0, iconH = 0, iconChannels = 0;
        unsigned char* iconPixels = nullptr;

        std::vector<fs::path> candidatePaths = {
            "assets/icon.png",
            "icon.png",
            "assets/icon.ico",
            "icon.ico"
        };

#ifdef _WIN32
        char exePathBuf[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, exePathBuf, MAX_PATH) > 0) {
            fs::path exeDir = fs::path(exePathBuf).parent_path();
            candidatePaths.push_back(exeDir / "assets/icon.png");
            candidatePaths.push_back(exeDir / "icon.png");
            candidatePaths.push_back(exeDir / "assets/icon.ico");
            candidatePaths.push_back(exeDir / "icon.ico");
        }
#endif

        for (const auto& path : candidatePaths) {
            std::error_code ec;
            if (fs::exists(path, ec)) {
                iconPixels = stbi_load(path.string().c_str(), &iconW, &iconH, &iconChannels, 4);
                if (iconPixels) break;
                logger.write(
                    VfCore::Level::Warn,
                    std::string("Не удалось декодировать иконку '") + path.string() + "': " + stbi_failure_reason()
                );
            }
        }

        if (iconPixels && iconW > 0 && iconH > 0) {
            GLFWimage winIcon;
            winIcon.width = iconW;
            winIcon.height = iconH;
            winIcon.pixels = iconPixels;
            glfwSetWindowIcon(window, 1, &winIcon);
            stbi_image_free(iconPixels);
        } else {
            logger.write(VfCore::Level::Warn, "Иконка окна не найдена или не загружена.");
        }

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        ImFontConfig fontCfg;
        fontCfg.OversampleH = 2;
        fontCfg.OversampleV = 2;

        const char* fontCandidates[] = {
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/tahoma.ttf"
        };

        bool loadedCyrFont = false;
        for (const char* fontPath : fontCandidates) {
            std::error_code ec;
            if (fs::exists(fontPath, ec)) {
                io.Fonts->AddFontFromFileTTF(fontPath, 16.0f, &fontCfg, io.Fonts->GetGlyphRangesCyrillic());
                loadedCyrFont = true;
                break;
            }
        }
        if (!loadedCyrFont) {
            io.Fonts->AddFontDefault();
        }
        (void)io;

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 130");

        UiLang lang = cfg.lang;
        bool darkTheme = cfg.darkTheme;
        auto applyTheme = [&darkTheme]() {
            if (darkTheme) ImGui::StyleColorsDark();
            else ImGui::StyleColorsLight();
        };
        applyTheme();

        UpdateCheckState updater;
        updater.startAsyncCheck();

        std::vector<std::string> queueItems;
        std::vector<std::string> guiLog;
        int selectedIndex = -1;
        int gameProfileIdx = 0;
        int presetIdx = 0;
        bool dryRunPack = false;
        bool showLogPanel = true;
        double tickRate = cfg.defaultTickRate;

        char filterBuf[256] = {};
        char addPathBuf[512] = {};
        char gameDirBuf[512] = {};
        char vmtPrefixBuf[256] = {};
        std::snprintf(gameDirBuf, sizeof(gameDirBuf), "%s", cfg.gameDir.c_str());

        std::string previewText = Loc::tr(lang,
            "Выберите файл или папку в списке, чтобы увидеть сводку.",
            "Select a file or folder in the list to inspect its summary.");
        std::string statusText = Loc::tr(lang,
            "Подсказка: перетащите файлы или папки прямо в окно программы.",
            "Tip: drag and drop files or folders onto the window.");

        auto pushLog = [&logger, &guiLog, &statusText](VfCore::Level lvl, const std::string& msg) {
            logger.write(lvl, msg);
            guiLog.push_back(msg);
            if (guiLog.size() > 200) {
                guiLog.erase(guiLog.begin());
            }
            statusText = msg;
        };

        auto safeRun = [&pushLog, &lang](const char* opName, auto&& fn) {
            try {
                fn();
            } catch (const std::exception& ex) {
                pushLog(VfCore::Level::Error,
                    std::string(Loc::tr(lang, "Ошибка в операции [", "Error in operation [")) + opName + "]: " + ex.what());
            } catch (...) {
                pushLog(VfCore::Level::Error,
                    std::string(Loc::tr(lang, "Неизвестный сбой при обработке [", "Unknown error while processing [")) + opName + "]");
            }
        };

        auto buildPreview = [&lang](const fs::path& p) -> std::string {
            try {
                if (p.empty()) return Loc::tr(lang, "Пустой путь.", "Empty path.");
                std::error_code ec;
                if (!fs::exists(p, ec)) {
                    return std::string(Loc::tr(lang, "Файл или папка не найдены на диске:\n", "File or folder not found on disk:\n")) + p.string();
                }
                if (fs::is_directory(p, ec)) {
                    size_t count = 0;
                    for (const auto& entry : fs::recursive_directory_iterator(p, ec)) {
                        if (entry.is_regular_file(ec)) ++count;
                    }
                    return std::string(Loc::tr(lang, "Папка: ", "Folder: ")) + p.filename().string() +
                           Loc::tr(lang, "\nФайлов внутри: ", "\nFiles inside: ") + std::to_string(count);
                }

                // Проверка через расширяемый реестр (включая форматы Source 2)
                auto extRes = VfExt::FormatRegistry::instance().tryInspect(p);
                if (extRes.handled) {
                    return extRes.summary;
                }

                std::string ext = VfV2::toLower(p.extension().string());
                if (ext == ".vtf") {
                    auto info = VfV2::ExtraTools::inspectTextureInfo(p);
                    if (!info.valid) {
                        return "VTF: " + p.filename().string() +
                               Loc::tr(lang, " (повреждённый или неподдерживаемый заголовок)", " (corrupted or unsupported header)");
                    }
                    return "VTF: " + p.filename().string() +
                           Loc::tr(lang, "\nВерсия: ", "\nVersion: ") + std::to_string(info.versionMajor) + "." + std::to_string(info.versionMinor) +
                           Loc::tr(lang, "\nРазмер: ", "\nSize: ") + std::to_string(info.width) + "x" + std::to_string(info.height) +
                           Loc::tr(lang, "\nMIP-уровней: ", "\nMIP levels: ") + std::to_string(info.mipCount) +
                           "\nPower-of-Two: " + (info.isPowerOfTwo ? Loc::tr(lang, "Да", "Yes") : Loc::tr(lang, "Нет", "No"));
                }
                if (ext == ".vmf") {
                    fs::path tmpStats = fs::temp_directory_path() / "vf_gui_preview_stats.json";
                    bool ok = VfV2::ExtraTools::exportMapStats(p, tmpStats);
                    return ok ? (std::string(Loc::tr(lang, "Карта VMF: ", "VMF Map: ")) + p.filename().string() +
                                 Loc::tr(lang, "\nСтатистика сущностей собрана (готов экспорт JSON/CSV/OBJ).",
                                               "\nEntity statistics collected (ready for JSON/CSV/OBJ export)."))
                              : (std::string(Loc::tr(lang, "Карта VMF: ", "VMF Map: ")) + p.filename().string());
                }
                if (ext == ".vmt") {
                    fs::path tmpJson = fs::temp_directory_path() / "vf_gui_preview_vmt.json";
                    bool ok = VfV2::MatAndDemoV2::convertVmtToJson(p, tmpJson);
                    return ok ? (std::string(Loc::tr(lang, "Материал VMT: ", "VMT Material: ")) + p.filename().string() +
                                 Loc::tr(lang, "\nПараметры разобраны.", "\nParameters parsed."))
                              : (std::string(Loc::tr(lang, "Материал VMT: ", "VMT Material: ")) + p.filename().string());
                }
                if (ext == ".dem") {
                    auto dh = DemoUltraTools::parseHeader(p);
                    if (!dh.valid) return std::string(Loc::tr(lang, "Демка DEM (битый заголовок): ", "DEM Demo (invalid header): ")) + p.filename().string();
                    return std::string(Loc::tr(lang, "Демка DEM: ", "DEM Demo: ")) + p.filename().string() +
                           Loc::tr(lang, "\nКарта: ", "\nMap: ") + dh.mapName +
                           Loc::tr(lang, "\nИгрок: ", "\nPlayer: ") + dh.clientName +
                           Loc::tr(lang, "\nТиков: ", "\nTicks: ") + std::to_string(dh.playbackTicks) +
                           " (" + std::to_string(dh.playbackTime) + " s)";
                }
                if (ext == ".vpk") {
                    std::vector<VpkEntry> entries;
                    uint32_t ver = 0;
                    bool ok = VpkInspector::readDirectory(p, entries, ver);
                    return ok ? (std::string(Loc::tr(lang, "Архив VPK v", "VPK Archive v")) + std::to_string(ver) + ": " + p.filename().string() +
                                 Loc::tr(lang, "\nФайлов в каталоге: ", "\nFiles in directory: ") + std::to_string(entries.size()))
                              : (std::string(Loc::tr(lang, "Архив VPK (повреждён или не *_dir.vpk): ", "VPK Archive (corrupted or not *_dir.vpk): ")) + p.filename().string());
                }
                return std::string(Loc::tr(lang, "Файл: ", "File: ")) + p.filename().string() +
                       Loc::tr(lang, "\nРасширение: ", "\nExtension: ") + (ext.empty() ? "-" : ext);
            } catch (const std::exception& ex) {
                return std::string(Loc::tr(lang, "Ошибка предпросмотра: ", "Preview error: ")) + ex.what();
            }
        };

        auto applyGameProfile = [&gameProfileIdx, &tickRate, &presetIdx, &gameDirBuf, &pushLog, &lang]() {
            if (gameProfileIdx == 1) {
                tickRate = 64.0;
                presetIdx = 0;
                std::snprintf(gameDirBuf, 512, "./csgo");
                pushLog(VfCore::Level::Info, Loc::tr(lang, "Применён профиль: CS:GO / CS:S (tickrate=64)", "Applied profile: CS:GO / CS:S (tickrate=64)"));
            } else if (gameProfileIdx == 2) {
                tickRate = 66.67;
                presetIdx = 0;
                std::snprintf(gameDirBuf, 512, "./tf");
                pushLog(VfCore::Level::Info, Loc::tr(lang, "Применён профиль: Team Fortress 2 (tickrate=66.67)", "Applied profile: Team Fortress 2 (tickrate=66.67)"));
            } else if (gameProfileIdx == 3) {
                tickRate = 66.0;
                presetIdx = 1;
                std::snprintf(gameDirBuf, 512, "./garrysmod");
                pushLog(VfCore::Level::Info, Loc::tr(lang, "Применён профиль: Garry's Mod (tickrate=66)", "Applied profile: Garry's Mod (tickrate=66)"));
            } else {
                tickRate = 64.0;
                std::snprintf(gameDirBuf, 512, "./game");
                pushLog(VfCore::Level::Info, Loc::tr(lang, "Применён базовый профиль: Custom", "Applied base profile: Custom"));
            }
        };

        struct DropCtx {
            std::vector<std::string>* items;
            std::vector<std::string>* logs;
        };
        DropCtx dropCtx{&queueItems, &guiLog};

        glfwSetWindowUserPointer(window, &dropCtx);
        glfwSetDropCallback(window, [](GLFWwindow* win, int count, const char** paths) {
            auto* ctx = static_cast<DropCtx*>(glfwGetWindowUserPointer(win));
            if (!ctx || !ctx->items) return;
            for (int i = 0; i < count; ++i) {
                if (paths[i] && paths[i][0] != '\0') {
                    ctx->items->emplace_back(paths[i]);
                    if (ctx->logs) {
                        ctx->logs->push_back(std::string("+ ") + paths[i]);
                    }
                }
            }
        });

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            int winW = 0, winH = 0;
            glfwGetWindowSize(window, &winW, &winH);

            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(static_cast<float>(winW), static_cast<float>(winH)));
            ImGui::Begin("##VortexForgeMain", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

            const float bottomBarHeight = 44.0f;
            const float logPanelHeight = showLogPanel ? 115.0f : 0.0f;
            ImGui::BeginChild("##ContentArea", ImVec2(0.0f, -(bottomBarHeight + logPanelHeight + 8.0f)), false);

            const char* gameProfiles[] = { "Custom", "CS:GO / CS:S", "TF2", "Garry's Mod" };
            ImGui::SetNextItemWidth(175.0f);
            if (ImGui::Combo(Loc::tr(lang, "Профиль игры", "Game profile"), &gameProfileIdx, gameProfiles, IM_ARRAYSIZE(gameProfiles))) {
                applyGameProfile();
            }
            ImGui::SameLine();
            ImGui::Text(Loc::tr(lang, "В очереди: %zu", "In queue: %zu"), queueItems.size());

            // Баннер о новой версии (если найдена на GitHub)
            if (updater.done && updater.updateAvailable) {
                std::lock_guard<std::mutex> lock(updater.mtx);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.15f, 0.65f, 0.25f, 1.0f), "[New: v%s]", updater.latestVersion.c_str());
            }

            if (ImGui::BeginTabBar("##MainTabs")) {
                // 1. Вкладка Files
                if (ImGui::BeginTabItem(Loc::tr(lang, "Файлы##Files", "Files##Files"))) {
                    ImGui::SetNextItemWidth(-145.0f);
                    ImGui::InputTextWithHint("##FilterQueue",
                        Loc::tr(lang, "Фильтр по имени или расширению (.vmf, .vtf, .vpk, .vtex_c)...",
                                      "Filter by name or extension (.vmf, .vtf, .vpk, .vtex_c)..."),
                        filterBuf, sizeof(filterBuf));
                    ImGui::SameLine();
                    if (ImGui::Button(Loc::tr(lang, "Сброс", "Reset"), ImVec2(-1.0f, 0.0f))) {
                        filterBuf[0] = '\0';
                    }

                    std::string filterLower = VfV2::toLower(filterBuf);
                    float rightPanelW = 140.0f;
                    float listH = (std::max)(140.0f, ImGui::GetContentRegionAvail().y * 0.62f);

                    ImGui::BeginChild("##QueueBox", ImVec2(-rightPanelW - 10.0f, listH), true);
                    for (int i = 0; i < static_cast<int>(queueItems.size()); ++i) {
                        if (!filterLower.empty() && VfV2::toLower(queueItems[i]).find(filterLower) == std::string::npos) {
                            continue;
                        }
                        bool isSelected = (selectedIndex == i);
                        if (ImGui::Selectable(queueItems[i].c_str(), isSelected)) {
                            selectedIndex = i;
                            previewText = buildPreview(fs::path(queueItems[i]));
                        }
                    }
                    ImGui::EndChild();

                    ImGui::SameLine();
                    ImGui::BeginChild("##QueueButtons", ImVec2(rightPanelW, listH), false);
                    ImGui::InputTextWithHint("##QuickAdd", Loc::tr(lang, "Путь...", "Path..."), addPathBuf, sizeof(addPathBuf));
                    if (ImGui::Button(Loc::tr(lang, "Добавить", "Add"), ImVec2(-1.0f, 0.0f))) {
                        if (addPathBuf[0] != '\0') {
                            queueItems.emplace_back(addPathBuf);
                            selectedIndex = static_cast<int>(queueItems.size()) - 1;
                            previewText = buildPreview(fs::path(queueItems[selectedIndex]));
                            pushLog(VfCore::Level::Info, std::string(Loc::tr(lang, "Добавлен: ", "Added: ")) + addPathBuf);
                            addPathBuf[0] = '\0';
                        }
                    }
                    if (ImGui::Button(Loc::tr(lang, "Удалить", "Remove"), ImVec2(-1.0f, 0.0f))) {
                        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(queueItems.size())) {
                            std::string removed = queueItems[selectedIndex];
                            queueItems.erase(queueItems.begin() + selectedIndex);
                            if (selectedIndex >= static_cast<int>(queueItems.size())) {
                                selectedIndex = static_cast<int>(queueItems.size()) - 1;
                            }
                            previewText = (selectedIndex >= 0) ? buildPreview(fs::path(queueItems[selectedIndex]))
                                                               : Loc::tr(lang, "Выберите файл или папку в списке.", "Select a file or folder in the list.");
                            pushLog(VfCore::Level::Info, std::string(Loc::tr(lang, "Удалён: ", "Removed: ")) + removed);
                        }
                    }
                    if (ImGui::Button(Loc::tr(lang, "Очистить всё", "Remove all"), ImVec2(-1.0f, 0.0f))) {
                        queueItems.clear();
                        selectedIndex = -1;
                        previewText = Loc::tr(lang, "Очередь очищена.", "Queue cleared.");
                        pushLog(VfCore::Level::Info, Loc::tr(lang, "Очередь полностью очищена.", "Queue completely cleared."));
                    }
                    ImGui::EndChild();

                    ImGui::TextWrapped("%s", statusText.c_str());
                    ImGui::Separator();
                    ImGui::BeginChild("##PreviewBox", ImVec2(0.0f, -1.0f), true);
                    ImGui::TextUnformatted(Loc::tr(lang, "Предпросмотр выбранного элемента:", "Selected item preview:"));
                    ImGui::Separator();
                    ImGui::TextWrapped("%s", previewText.c_str());
                    ImGui::EndChild();

                    ImGui::EndTabItem();
                }

                // 2. Вкладка World
                if (ImGui::BeginTabItem(Loc::tr(lang, "Карты##World", "World##World"))) {
                    ImGui::TextUnformatted(Loc::tr(lang, "Инструменты карт (.vmf):", "Map tools (.vmf):"));
                    if (ImGui::Button(Loc::tr(lang, "Экспорт сущностей в JSON", "Export entities to JSON")) && selectedIndex >= 0) {
                        safeRun("VMF->JSON", [&]() {
                            fs::path p(queueItems[selectedIndex]);
                            fs::path dst = fs::path(cfg.outputDir) / (p.stem().string() + "_entities.json");
                            bool ok = VfV2::DepAndMapTools::exportEntitiesToJson(p, dst);
                            pushLog(ok ? VfCore::Level::Info : VfCore::Level::Error,
                                ok ? ("VMF -> JSON: " + dst.string()) : Loc::tr(lang, "Ошибка экспорта VMF -> JSON", "VMF -> JSON export failed"));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Экспорт статистики в JSON", "Export map stats to JSON")) && selectedIndex >= 0) {
                        safeRun("VMF-Stats", [&]() {
                            fs::path p(queueItems[selectedIndex]);
                            fs::path dst = fs::path(cfg.outputDir) / (p.stem().string() + "_stats.json");
                            bool ok = VfV2::ExtraTools::exportMapStats(p, dst);
                            pushLog(ok ? VfCore::Level::Info : VfCore::Level::Error,
                                ok ? ("Stats: " + dst.string()) : Loc::tr(lang, "Ошибка анализа карты", "Map stats analysis failed"));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Экспорт маркеров в OBJ", "Export entity markers to OBJ")) && selectedIndex >= 0) {
                        safeRun("VMF->OBJ", [&]() {
                            fs::path p(queueItems[selectedIndex]);
                            fs::path dst = fs::path(cfg.outputDir) / (p.stem().string() + "_entities.obj");
                            bool ok = VfV2::ExtraTools::exportEntitiesToObj(p, dst, 16.0);
                            pushLog(ok ? VfCore::Level::Info : VfCore::Level::Error,
                                ok ? ("OBJ: " + dst.string()) : Loc::tr(lang, "Ошибка экспорта в OBJ", "OBJ export failed"));
                        });
                    }
                    ImGui::EndTabItem();
                }

                // 3. Вкладка Entities
                if (ImGui::BeginTabItem(Loc::tr(lang, "Сущности##Entities", "Entities##Entities"))) {
                    ImGui::InputText(Loc::tr(lang, "Папка игры / ассетов", "Game / assets folder"), gameDirBuf, sizeof(gameDirBuf));
                    if (ImGui::Button(Loc::tr(lang, "Экспорт сущностей в CSV", "Export entities to CSV")) && selectedIndex >= 0) {
                        safeRun("VMF->CSV", [&]() {
                            fs::path p(queueItems[selectedIndex]);
                            fs::path dst = fs::path(cfg.outputDir) / (p.stem().string() + "_entities.csv");
                            bool ok = VfV2::ExtraTools::exportEntitiesSummaryCsv(p, dst);
                            pushLog(ok ? VfCore::Level::Info : VfCore::Level::Error,
                                ok ? ("VMF -> CSV: " + dst.string()) : Loc::tr(lang, "Ошибка экспорта в CSV", "CSV export failed"));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Проверить зависимости", "Check dependencies")) && selectedIndex >= 0) {
                        safeRun("CheckDeps", [&]() {
                            fs::path p(queueItems[selectedIndex]);
                            auto refs = VfV2::DepAndMapTools::collectAssetRefs(p);
                            auto missing = VfV2::ExtraTools::checkMissingAssets(refs, {gameDirBuf});
                            pushLog(VfCore::Level::Info,
                                std::string(Loc::tr(lang, "Ссылок: ", "Refs: ")) + std::to_string(refs.size()) +
                                Loc::tr(lang, ", отсутствует: ", ", missing: ") + std::to_string(missing.size()));
                        });
                    }
                    ImGui::EndTabItem();
                }

                // 4. Вкладка Textures
                if (ImGui::BeginTabItem(Loc::tr(lang, "Текстуры##Textures", "Textures##Textures"))) {
                    if (ImGui::Button(Loc::tr(lang, "Инфо о VTF", "Inspect VTF")) && selectedIndex >= 0) {
                        safeRun("VTF-Info", [&]() {
                            auto info = VfV2::ExtraTools::inspectTextureInfo(queueItems[selectedIndex]);
                            pushLog(info.valid ? VfCore::Level::Info : VfCore::Level::Error,
                                info.valid ? ("VTF " + std::to_string(info.width) + "x" + std::to_string(info.height))
                                           : Loc::tr(lang, "Некорректный или битый VTF", "Invalid or corrupted VTF"));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Конвертировать VTF", "Convert VTF")) && selectedIndex >= 0) {
                        safeRun("VTF-Convert", [&]() {
                            VfTexForge::TextureProcessor proc(logger);
                            bool ok = proc.convertVtf(queueItems[selectedIndex], cfg.outputDir);
                            pushLog(ok ? VfCore::Level::Info : VfCore::Level::Error,
                                ok ? Loc::tr(lang, "Текстура VTF конвертирована", "VTF texture converted")
                                   : Loc::tr(lang, "Ошибка конвертации VTF", "VTF conversion failed"));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Починить VMT", "Heal VMT")) && selectedIndex >= 0) {
                        safeRun("VMT-Heal", [&]() {
                            fs::path p(queueItems[selectedIndex]);
                            fs::path dst = fs::path(cfg.outputDir) / p.filename();
                            auto rep = MaterialDoctor::healAndCheckVmt(p, dst, {gameDirBuf});
                            pushLog(VfCore::Level::Info,
                                "VMT -> " + dst.string() + " (" + std::to_string(rep.normalizedKeys) + ")");
                        });
                    }
                    ImGui::EndTabItem();
                }

                // 5. Вкладка Other
                if (ImGui::BeginTabItem(Loc::tr(lang, "Утилиты##Other", "Other##Other"))) {
                    ImGui::InputDouble("Tickrate", &tickRate, 1.0, 16.0, "%.2f");
                    const char* presets[] = { "YouTube 1080p60", "High Quality MP4", "Fast Preview" };
                    ImGui::Combo(Loc::tr(lang, "Пресет видео", "Video preset"), &presetIdx, presets, IM_ARRAYSIZE(presets));
                    ImGui::Checkbox(Loc::tr(lang, "Dry-run при сборке пака", "Dry-run when packing"), &dryRunPack);
                    ImGui::InputTextWithHint(Loc::tr(lang, "Префикс материала", "Material prefix"),
                                             "props/custom", vmtPrefixBuf, sizeof(vmtPrefixBuf));

                    ImGui::Separator();
                    if (ImGui::Button(Loc::tr(lang, "Сгенерировать базовый VMT из текстуры", "Generate basic VMT from texture")) && selectedIndex >= 0) {
                        safeRun("GenVMT", [&]() {
                            fs::path p(queueItems[selectedIndex]);
                            fs::path dst = fs::path(cfg.outputDir) / (p.stem().string() + ".vmt");
                            bool ok = VfV2::ExtraTools::generateBasicVmt(p, dst, vmtPrefixBuf);
                            pushLog(ok ? VfCore::Level::Info : VfCore::Level::Error,
                                ok ? ("VMT: " + dst.string()) : Loc::tr(lang, "Ошибка генерации VMT", "VMT generation failed"));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Создать .cfg для записи демок", "Create demo recording .cfg")) && selectedIndex >= 0) {
                        safeRun("DemoCfg", [&]() {
                            fs::path dst = fs::path(cfg.outputDir) / ("vortex_movie_" + std::to_string(gameProfileIdx) + ".cfg");
                            bool ok = VfV2::ExtraTools::writeGameLaunchPreset(dst, gameProfiles[gameProfileIdx], static_cast<int>(tickRate));
                            pushLog(ok ? VfCore::Level::Info : VfCore::Level::Error,
                                ok ? ("CFG: " + dst.string()) : Loc::tr(lang, "Ошибка создания .cfg", "Failed to create .cfg"));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Экспорт маркеров и demo_gototick", "Export timeline & demo_gototick")) && selectedIndex >= 0) {
                        safeRun("DemoMarkers", [&]() {
                            std::vector<VfV2::ClipRange> sampleClips = {{"clip_01", 10.0, 20.0}, {"clip_02", 35.0, 45.0}};
                            bool ok = DemoUltraTools::exportTimelineMarkers(queueItems[selectedIndex], sampleClips, tickRate, cfg.outputDir);
                            pushLog(ok ? VfCore::Level::Info : VfCore::Level::Error,
                                ok ? Loc::tr(lang, "Маркеры глав сохранены", "Timeline markers saved")
                                   : Loc::tr(lang, "Ошибка экспорта маркеров", "Marker export failed"));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Проверить пути (Lint)", "Lint asset paths")) && selectedIndex >= 0) {
                        safeRun("Lint", [&]() {
                            auto refs = VfV2::DepAndMapTools::collectAssetRefs(queueItems[selectedIndex]);
                            auto warns = VfV2::ExtraTools::lintAssetPaths(refs);
                            pushLog(VfCore::Level::Info, "Lint: refs=" + std::to_string(refs.size()) + ", warnings=" + std::to_string(warns.size()));
                        });
                    }
                    if (ImGui::Button(Loc::tr(lang, "Найти неиспользуемые ассеты", "Find unused assets")) && selectedIndex >= 0) {
                        safeRun("Unused", [&]() {
                            auto refs = VfV2::DepAndMapTools::collectAssetRefs(queueItems[selectedIndex]);
                            auto unused = VfV2::ExtraTools::findUnusedAssets(gameDirBuf, refs);
                            pushLog(VfCore::Level::Info, "Unused files: " + std::to_string(unused.size()));
                        });
                    }

                    ImGui::Separator();
                    if (ImGui::Button(Loc::tr(lang, "Проверить обновления (GitHub)", "Check for updates (GitHub)"))) {
                        updater.startAsyncCheck();
                        pushLog(VfCore::Level::Info, Loc::tr(lang, "Проверка обновлений...", "Checking for updates..."));
                    }
                    if (updater.done) {
                        std::lock_guard<std::mutex> lock(updater.mtx);
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", updater.statusMessage.c_str());
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::EndChild();

            // Живой лог
            ImGui::Checkbox(Loc::tr(lang, "Показывать лог", "Show log"), &showLogPanel);
            if (showLogPanel) {
                ImGui::SameLine();
                if (ImGui::SmallButton(Loc::tr(lang, "Очистить лог", "Clear log"))) {
                    guiLog.clear();
                }
                ImGui::BeginChild("##LiveLogBox", ImVec2(0.0f, logPanelHeight - 24.0f), true);
                for (const auto& entry : guiLog) {
                    ImGui::TextWrapped("%s", entry.c_str());
                }
                if (!guiLog.empty()) {
                    ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();
            }

            ImGui::Separator();

            // Нижняя панель: Defaults, Dark theme, Переключатель языка RU/EN и Run / Pack
            if (ImGui::Button(Loc::tr(lang, "Сброс", "Defaults"))) {
                darkTheme = false;
                applyTheme();
                gameProfileIdx = 0;
                presetIdx = 0;
                dryRunPack = false;
                tickRate = 64.0;
                std::snprintf(gameDirBuf, sizeof(gameDirBuf), "%s", "./game");
                cfg.darkTheme = darkTheme;
                cfg.save();
                pushLog(VfCore::Level::Info, Loc::tr(lang, "Настройки сброшены.", "Settings reset to defaults."));
            }

            ImGui::SameLine();
            if (ImGui::Checkbox(Loc::tr(lang, "Тёмная тема", "Dark theme"), &darkTheme)) {
                applyTheme();
                cfg.darkTheme = darkTheme;
                cfg.save();
                pushLog(VfCore::Level::Info, darkTheme ? "Dark theme ON" : "Light theme ON");
            }

            ImGui::SameLine();
            const char* langItems[] = { "RU", "EN" };
            int langIdx = (lang == UiLang::EN) ? 1 : 0;
            ImGui::SetNextItemWidth(60.0f);
            if (ImGui::Combo("##LangSwitch", &langIdx, langItems, 2)) {
                lang = (langIdx == 1) ? UiLang::EN : UiLang::RU;
                cfg.lang = lang;
                cfg.save();
                previewText = (selectedIndex >= 0 && selectedIndex < static_cast<int>(queueItems.size()))
                    ? buildPreview(fs::path(queueItems[selectedIndex]))
                    : Loc::tr(lang, "Выберите файл или папку в списке, чтобы увидеть сводку.",
                                    "Select a file or folder in the list to inspect its summary.");
            }

            float actionBtnW = 135.0f;
            ImGui::SameLine((std::max)(10.0f, ImGui::GetContentRegionAvail().x - actionBtnW));
            if (ImGui::Button("Run / Pack", ImVec2(actionBtnW, 0.0f))) {
                if (queueItems.empty()) {
                    pushLog(VfCore::Level::Warn, Loc::tr(lang, "Очередь пуста.", "Queue is empty."));
                } else {
                    safeRun("Run/Pack", [&]() {
                        int processed = 0;
                        for (const auto& item : queueItems) {
                            fs::path p(item);
                            std::string ext = VfV2::toLower(p.extension().string());
                            if (ext == ".vmt" || ext == ".vmf") {
                                if (MaterialDoctor::packWithManifest(p, {gameDirBuf}, fs::path(cfg.outputDir) / "pack", dryRunPack)) {
                                    ++processed;
                                }
                            } else if (ext == ".vtf") {
                                VfTexForge::TextureProcessor proc(logger);
                                if (proc.convertVtf(p, cfg.outputDir)) ++processed;
                            } else if (fs::is_directory(p)) {
                                VfV2::SmartBatchQueue batch;
                                batch.setAllowedExtensions({".vmt", ".vmf", ".bsp"});
                                batch.addFromPath(p, cfg.outputDir);
                                batch.run(true);
                                ++processed;
                            }
                        }
                        pushLog(VfCore::Level::Info,
                            std::string(Loc::tr(lang, "Обработано: ", "Processed: ")) +
                            std::to_string(processed) + " / " + std::to_string(queueItems.size()));
                    });
                }
            }

            ImGui::End();

            ImGui::Render();
            int displayW = 0, displayH = 0;
            glfwGetFramebufferSize(window, &displayW, &displayH);
            glViewport(0, 0, displayW, displayH);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
#endif
    }
};

}