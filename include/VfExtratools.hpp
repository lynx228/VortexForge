#pragma once

#include "VfModules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace VfV2 {

namespace fs = std::filesystem;

struct TextureInspectResult {
    bool valid = false;
    uint32_t versionMajor = 0;
    uint32_t versionMinor = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t mipCount = 0;
    int32_t highResFormat = -1;
    bool isPowerOfTwo = false;
};

struct TickClipInfo {
    std::string name;
    int startTick = 0;
    int endTick = 0;
    double durationSec = 0.0;
    bool overlapsPrevious = false;
};

struct MapDiffReport {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> moved;
};

class ExtraTools {
public:
    // 1. Проверка недостающих ассетов по списку ссылок и директориям поиска
    static std::vector<std::string> checkMissingAssets(const std::set<std::string>& refs,
                                                       const std::vector<fs::path>& searchDirs) {
        std::vector<std::string> missing;
        for (const auto& rawRef : refs) {
            std::string normalized = normalizeRelPath(rawRef);
            if (!resolveAssetFile(normalized, searchDirs).has_value()) {
                missing.push_back(normalized);
            }
        }
        return missing;
    }

    // 2. Копирование найденных ассетов в единую папку с сохранением структуры
    static int packFoundAssets(const std::set<std::string>& refs,
                               const std::vector<fs::path>& searchDirs,
                               const fs::path& packOutDir) {
        int copied = 0;
        for (const auto& rawRef : refs) {
            std::string normalized = normalizeRelPath(rawRef);
            auto resolved = resolveAssetFile(normalized, searchDirs);
            if (!resolved.has_value()) continue;

            fs::path destPath = packOutDir / resolved->second;
            std::error_code ec;
            fs::create_directories(destPath.parent_path(), ec);
            if (fs::copy_file(resolved->first, destPath, fs::copy_options::overwrite_existing, ec)) {
                ++copied;
            }
        }
        return copied;
    }

    // 3. Поиск неиспользуемых ("осиротевших") ассетов в папке
    static std::vector<fs::path> findUnusedAssets(const fs::path& assetsDir,
                                                  const std::set<std::string>& usedRefs) {
        std::vector<fs::path> unused;
        if (!fs::exists(assetsDir) || !fs::is_directory(assetsDir)) return unused;

        std::set<std::string> normalizedUsed;
        for (const auto& r : usedRefs) {
            std::string n = toLower(normalizeRelPath(r));
            normalizedUsed.insert(n);
            if (n.rfind("materials/", 0) == 0) normalizedUsed.insert(n.substr(10));
            if (n.rfind("models/", 0) == 0) normalizedUsed.insert(n.substr(7));
        }

        const std::set<std::string> trackExts = {".vmt", ".vtf", ".mdl", ".vmat", ".vtex", ".vmdl"};
        for (const auto& entry : fs::recursive_directory_iterator(assetsDir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = toLower(entry.path().extension().string());
            if (trackExts.find(ext) == trackExts.end()) continue;

            std::error_code ec;
            fs::path rel = fs::relative(entry.path(), assetsDir, ec);
            std::string relNorm = toLower(normalizeRelPath(ec ? entry.path().filename().string() : rel.string()));

            bool isUsed = (normalizedUsed.find(relNorm) != normalizedUsed.end());
            if (!isUsed && relNorm.rfind("materials/", 0) == 0) {
                isUsed = normalizedUsed.find(relNorm.substr(10)) != normalizedUsed.end();
            }
            if (!isUsed && relNorm.rfind("models/", 0) == 0) {
                isUsed = normalizedUsed.find(relNorm.substr(7)) != normalizedUsed.end();
            }

            if (!isUsed) {
                unused.push_back(entry.path());
            }
        }
        return unused;
    }

    // 4. Линтер путей ассетов (проверка пробелов, не-ASCII символов и заглавных букв)
    static std::vector<std::string> lintAssetPaths(const std::set<std::string>& refs) {
        std::vector<std::string> warnings;
        for (const auto& rawRef : refs) {
            std::string p = normalizeRelPath(rawRef);
            bool hasSpace = false;
            bool hasNonAscii = false;
            bool hasUpper = false;

            for (unsigned char c : p) {
                if (c == ' ') hasSpace = true;
                if (c >= 128) hasNonAscii = true;
                if (std::isupper(c)) hasUpper = true;
            }

            if (hasSpace || hasNonAscii || hasUpper) {
                std::string reason;
                if (hasSpace) reason += "[пробелы] ";
                if (hasNonAscii) reason += "[не-ASCII/кириллица] ";
                if (hasUpper) reason += "[заглавные буквы] ";
                warnings.push_back(reason + "-> " + p);
            }
        }
        return warnings;
    }

    // 5. Генерация базового .vmt из текстуры (с автопоиском _normal / _bump)
    static bool generateBasicVmt(const fs::path& texturePath,
                                 const fs::path& vmtOutPath,
                                 const std::string& materialPrefix = "") {
        std::string texName = texturePath.stem().string();
        std::string relTex = materialPrefix.empty()
            ? texName
            : (normalizeRelPath(materialPrefix) + "/" + texName);

        fs::path parent = texturePath.parent_path();
        std::string bumpRel;
        for (const std::string& suffix : {"_normal", "_bump", "_n"}) {
            for (const std::string& ext : {".vtf", ".png", ".tga"}) {
                if (!parent.empty() && fs::exists(parent / (texName + suffix + ext))) {
                    bumpRel = relTex + suffix;
                    break;
                }
            }
            if (!bumpRel.empty()) break;
        }

        fs::create_directories(vmtOutPath.parent_path());
        std::ofstream out(vmtOutPath);
        if (!out.is_open()) return false;

        out << "\"LightmappedGeneric\"\n{\n"
            << "\t\"$basetexture\" \"" << relTex << "\"\n";
        if (!bumpRel.empty()) {
            out << "\t\"$bumpmap\" \"" << bumpRel << "\"\n";
        }
        out << "\t\"$surfaceprop\" \"default\"\n}\n";
        return out.good();
    }

    // 6. Инспекция заголовка VTF и проверка кратности степени двойки (Power of Two)
    static TextureInspectResult inspectTextureInfo(const fs::path& vtfPath) {
        TextureInspectResult res{};
        std::ifstream in(vtfPath, std::ios::binary);
        if (!in.is_open()) return res;

        char sig[4] = {};
        in.read(sig, 4);
        if (in.gcount() != 4 || std::string(sig, 4) != std::string("VTF\0", 4)) {
            return res;
        }

        uint32_t headerSize = 0;
        in.read(reinterpret_cast<char*>(&res.versionMajor), 4);
        in.read(reinterpret_cast<char*>(&res.versionMinor), 4);
        in.read(reinterpret_cast<char*>(&headerSize), 4);
        in.read(reinterpret_cast<char*>(&res.width), 2);
        in.read(reinterpret_cast<char*>(&res.height), 2);

        in.seekg(32, std::ios::cur);
        in.read(reinterpret_cast<char*>(&res.highResFormat), 4);
        in.read(reinterpret_cast<char*>(&res.mipCount), 1);

        if (!in.good()) return res;

        auto isPow2 = [](uint16_t v) { return v > 0 && (v & (v - 1)) == 0; };
        res.valid = true;
        res.isPowerOfTwo = isPow2(res.width) && isPow2(res.height);
        return res;
    }

    // 7. Тик-точная валидация клипов и сводка хронометража
    static std::string buildTickAccurateClipList(const std::vector<ClipRange>& clips, double tickRate = 64.0) {
        if (tickRate <= 0.0) tickRate = 64.0;

        std::vector<TickClipInfo> items;
        items.reserve(clips.size());
        double totalSec = 0.0;

        for (size_t i = 0; i < clips.size(); ++i) {
            int sTick = static_cast<int>(std::llround(std::max(0.0, clips[i].startSec) * tickRate));
            int eTick = static_cast<int>(std::llround(std::max(clips[i].startSec, clips[i].endSec) * tickRate));
            if (eTick <= sTick) eTick = sTick + 1;

            double dur = static_cast<double>(eTick - sTick) / tickRate;
            bool overlap = (!items.empty() && sTick < items.back().endTick);

            items.push_back({clips[i].name, sTick, eTick, dur, overlap});
            totalSec += dur;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "Сводка клипов (tickrate=" << tickRate << "):\n";
        for (const auto& it : items) {
            oss << "  - " << it.name
                << ": тики [" << it.startTick << " .. " << it.endTick << "]"
                << ", длит. " << it.durationSec << " с"
                << (it.overlapsPrevious ? " [ВНИМАНИЕ: пересекается с предыдущим]" : "")
                << "\n";
        }
        oss << "Суммарная длительность: " << totalSec << " с\n";
        return oss.str();
    }

    // 8. Генерация .cfg-пресета движка Source для записи демок и рендера кадров
    static bool writeGameLaunchPreset(const fs::path& cfgOutPath,
                                      const std::string& gameProfile = "csgo",
                                      int hostFramerate = 60) {
        fs::create_directories(cfgOutPath.parent_path());
        std::ofstream out(cfgOutPath);
        if (!out.is_open()) return false;

        std::string g = toLower(gameProfile);
        out << "// VortexForge Movie-Making / Demo Recording Config (" << g << ")\n"
            << "sv_cheats 1\n"
            << "host_framerate " << std::max(1, hostFramerate) << "\n"
            << "fps_max 0\n"
            << "mat_queue_mode 2\n"
            << "snd_mute_losefocus 0\n";

        if (g == "csgo" || g == "css") {
            out << "cl_draw_only_deathnotices 1\n"
                << "net_graph 0\n"
                << "cl_showpos 0\n";
        } else if (g == "tf2") {
            out << "cl_drawhud 0\n"
                << "hud_saytext_time 0\n";
        } else if (g == "gmod") {
            out << "cl_drawhud 0\n"
                << "gmod_physiterations 4\n";
        }
        out << "// Для старта записи кадров в игре: startmovie <имя_клипа> tga wav\n"
            << "// Для остановки: endmovie\n";
        return out.good();
    }

    // 9. Экспорт координат сущностей карты VMF в 3D-файл Wavefront .obj (для Blender)
    static bool exportEntitiesToObj(const fs::path& vmfPath, const fs::path& objOutPath, double markerRadius = 16.0) {
        auto entities = parseVmfEntities(vmfPath);
        if (entities.empty()) return false;

        fs::create_directories(objOutPath.parent_path());
        std::ofstream out(objOutPath);
        if (!out.is_open()) return false;

        out << "# VortexForge VMF Entity Markers OBJ\n";
        size_t vOffset = 1;
        int exported = 0;

        for (size_t i = 0; i < entities.size(); ++i) {
            auto it = entities[i].find("origin");
            if (it == entities[i].end()) continue;

            std::istringstream iss(it->second);
            double x = 0.0, y = 0.0, z = 0.0;
            if (!(iss >> x >> y >> z)) continue;

            std::string cls = entities[i].count("classname") ? entities[i].at("classname") : "entity";
            out << "o " << cls << "_" << i << "\n";

            // Создаём 3D-октаэдр (6 вершин, 8 граней) вокруг точки origin
            double r = std::max(1.0, markerRadius);
            out << "v " << (x + r) << " " << y << " " << z << "\n"
                << "v " << (x - r) << " " << y << " " << z << "\n"
                << "v " << x << " " << (y + r) << " " << z << "\n"
                << "v " << x << " " << (y - r) << " " << z << "\n"
                << "v " << x << " " << y << " " << (z + r) << "\n"
                << "v " << x << " " << y << " " << (z - r) << "\n";

            size_t p1 = vOffset, p2 = vOffset + 1, p3 = vOffset + 2;
            size_t p4 = vOffset + 3, p5 = vOffset + 4, p6 = vOffset + 5;
            out << "f " << p1 << " " << p3 << " " << p5 << "\n"
                << "f " << p3 << " " << p2 << " " << p5 << "\n"
                << "f " << p2 << " " << p4 << " " << p5 << "\n"
                << "f " << p4 << " " << p1 << " " << p5 << "\n"
                << "f " << p3 << " " << p1 << " " << p6 << "\n"
                << "f " << p2 << " " << p3 << " " << p6 << "\n"
                << "f " << p4 << " " << p2 << " " << p6 << "\n"
                << "f " << p1 << " " << p4 << " " << p6 << "\n";

            vOffset += 6;
            ++exported;
        }
        return exported > 0 && out.good();
    }

    // 10. Сравнение сущностей двух версий карты .vmf (diff)
    static MapDiffReport diffMapEntities(const fs::path& oldVmfPath, const fs::path& newVmfPath) {
        MapDiffReport report;
        auto oldList = parseVmfEntities(oldVmfPath);
        auto newList = parseVmfEntities(newVmfPath);

        auto buildKey = [](const std::map<std::string, std::string>& kv, size_t idx) {
            std::string cls = kv.count("classname") ? kv.at("classname") : "unknown";
            std::string tname = kv.count("targetname") ? kv.at("targetname") : "";
            if (!tname.empty()) return cls + "#" + tname;
            return cls + "@idx_" + std::to_string(idx);
        };

        std::map<std::string, std::string> oldMap;
        std::map<std::string, std::string> newMap;

        for (size_t i = 0; i < oldList.size(); ++i) {
            std::string origin = oldList[i].count("origin") ? oldList[i].at("origin") : "";
            oldMap[buildKey(oldList[i], i)] = origin;
        }
        for (size_t i = 0; i < newList.size(); ++i) {
            std::string origin = newList[i].count("origin") ? newList[i].at("origin") : "";
            newMap[buildKey(newList[i], i)] = origin;
        }

        for (const auto& [k, newOrig] : newMap) {
            auto it = oldMap.find(k);
            if (it == oldMap.end()) {
                report.added.push_back(k);
            } else if (it->second != newOrig) {
                report.moved.push_back(k + " (" + it->second + " -> " + newOrig + ")");
            }
        }
        for (const auto& [k, oldOrig] : oldMap) {
            if (newMap.find(k) == newMap.end()) {
                report.removed.push_back(k);
            }
        }
        return report;
    }

    // 11. Экспорт статистики сущностей карты VMF в JSON
    static bool exportMapStats(const fs::path& vmfPath, const fs::path& statsJsonOutPath) {
        std::ifstream in(vmfPath);
        if (!in.is_open()) return false;

        std::map<std::string, int> byClassname;
        int totalEntities = 0;
        int totalSolids = 0;

        bool inEntity = false;
        int braceDepth = 0;
        std::string currentClass;

        std::string line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty()) continue;

            std::string low = toLower(t);
            if (braceDepth == 0 && low.find("\"entity\"") == 0) {
                inEntity = true;
                currentClass = "unknown";
                continue;
            }
            if (low.find("\"solid\"") == 0) {
                ++totalSolids;
            }

            if (t == "{") {
                ++braceDepth;
                continue;
            }
            if (t == "}") {
                --braceDepth;
                if (braceDepth <= 0 && inEntity) {
                    ++totalEntities;
                    ++byClassname[currentClass];
                    inEntity = false;
                    braceDepth = 0;
                }
                continue;
            }

            if (inEntity && braceDepth == 1) {
                auto parts = parseQuotedTokens(t);
                if (parts.size() >= 2 && toLower(parts[0]) == "classname") {
                    currentClass = parts[1];
                }
            }
        }

        fs::create_directories(statsJsonOutPath.parent_path());
        std::ofstream out(statsJsonOutPath);
        if (!out.is_open()) return false;

        out << "{\n"
            << "  \"mapFile\": \"" << jsonEscape(vmfPath.filename().string()) << "\",\n"
            << "  \"totalEntities\": " << totalEntities << ",\n"
            << "  \"totalSolids\": " << totalSolids << ",\n"
            << "  \"entitiesByClassname\": {";

        size_t idx = 0;
        for (const auto& [cls, cnt] : byClassname) {
            out << (idx == 0 ? "\n    " : ",\n    ")
                << "\"" << jsonEscape(cls) << "\": " << cnt;
            ++idx;
        }
        out << (byClassname.empty() ? "" : "\n  ") << "}\n}\n";
        return out.good();
    }

    // 12. Экспорт сущностей .vmf в CSV с ключевыми полями (classname, targetname, origin, angles)
    static bool exportEntitiesSummaryCsv(const fs::path& vmfPath, const fs::path& csvOutPath) {
        auto entities = parseVmfEntities(vmfPath);
        if (entities.empty() && !fs::exists(vmfPath)) return false;

        fs::create_directories(csvOutPath.parent_path());
        std::ofstream out(csvOutPath);
        if (!out.is_open()) return false;

        auto csvEscape = [](const std::string& val) {
            std::string escaped = "\"";
            for (char c : val) {
                if (c == '"') escaped += "\"\"";
                else escaped.push_back(c);
            }
            escaped.push_back('"');
            return escaped;
        };

        out << "classname,targetname,origin,angles\n";
        for (const auto& kv : entities) {
            std::string cls = kv.count("classname") ? kv.at("classname") : "";
            std::string tname = kv.count("targetname") ? kv.at("targetname") : "";
            std::string orig = kv.count("origin") ? kv.at("origin") : "";
            std::string ang = kv.count("angles") ? kv.at("angles") : "";

            if (!cls.empty() || !tname.empty() || !orig.empty()) {
                out << csvEscape(cls) << ","
                    << csvEscape(tname) << ","
                    << csvEscape(orig) << ","
                    << csvEscape(ang) << "\n";
            }
        }
        return true;
    }

    // 13. Генерация готового скрипта рендера клипов (.bat для Windows или .sh для Unix)
    static bool writeClipRunnerScript(const fs::path& scriptPath,
                                      const fs::path& demPath,
                                      const std::vector<ClipRange>& clips,
                                      VideoPreset preset,
                                      const std::string& containerExt = ".mp4",
                                      const fs::path& outDir = "./clips_out",
                                      bool windowsBatch = true) {
        fs::create_directories(scriptPath.parent_path());
        std::ofstream out(scriptPath);
        if (!out.is_open()) return false;

        std::string commands = MatAndDemoV2::buildBatchClipsScript(
            demPath, clips, preset, containerExt, outDir);

        if (windowsBatch) {
            out << "@echo off\n"
                << "chcp 65001 >nul\n"
                << "where ffmpeg >nul 2>nul\n"
                << "if errorlevel 1 (\n"
                << "    echo [ERROR] ffmpeg не найден в PATH!\n"
                << "    exit /b 1\n"
                << ")\n\n";

            std::istringstream iss(commands);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.empty()) {
                    out << "\n";
                } else if (line.rfind("#", 0) == 0) {
                    out << "REM " << line.substr(1) << "\n";
                } else {
                    out << line << "\n"
                        << "if errorlevel 1 exit /b 1\n";
                }
            }
        } else {
            out << "#!/usr/bin/env bash\n"
                << "set -euo pipefail\n"
                << "if ! command -v ffmpeg >/dev/null 2>&1; then\n"
                << "    echo \"[ERROR] ffmpeg не найден в PATH!\" >&2\n"
                << "    exit 1\n"
                << "fi\n\n"
                << commands;
        }

        return out.good();
    }

private:
    static std::string normalizeRelPath(std::string p) {
        std::replace(p.begin(), p.end(), '\\', '/');
        while (!p.empty() && p.front() == '/') p.erase(p.begin());
        while (!p.empty() && p.back() == '/') p.pop_back();
        return p;
    }

    static std::optional<std::pair<fs::path, fs::path>> resolveAssetFile(
        const std::string& normalized,
        const std::vector<fs::path>& searchDirs) {
        for (const auto& dir : searchDirs) {
            if (dir.empty() || !fs::exists(dir)) continue;

            fs::path direct = dir / normalized;
            if (fs::exists(direct)) return std::make_pair(direct, fs::path(normalized));

            fs::path mat = dir / "materials" / normalized;
            if (fs::exists(mat)) return std::make_pair(mat, fs::path("materials") / normalized);

            fs::path mdl = dir / "models" / normalized;
            if (fs::exists(mdl)) return std::make_pair(mdl, fs::path("models") / normalized);
        }
        return std::nullopt;
    }

    static std::vector<std::string> parseQuotedTokens(const std::string& t) {
        std::vector<std::string> parts;
        std::string cur;
        bool q = false;
        for (char c : t) {
            if (c == '"') {
                q = !q;
                if (!q) {
                    parts.push_back(cur);
                    cur.clear();
                }
            } else if (q) {
                cur.push_back(c);
            }
        }
        return parts;
    }

    static std::vector<std::map<std::string, std::string>> parseVmfEntities(const fs::path& vmfPath) {
        std::vector<std::map<std::string, std::string>> result;
        std::ifstream in(vmfPath);
        if (!in.is_open()) return result;

        std::map<std::string, std::string> current;
        bool inEntity = false;
        int braceDepth = 0;
        std::string line;

        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty()) continue;

            if (braceDepth == 0 && toLower(t).find("\"entity\"") == 0) {
                inEntity = true;
                current.clear();
                continue;
            }
            if (t == "{") {
                ++braceDepth;
                continue;
            }
            if (t == "}") {
                --braceDepth;
                if (braceDepth <= 0 && inEntity) {
                    if (!current.empty()) result.push_back(current);
                    inEntity = false;
                    braceDepth = 0;
                }
                continue;
            }
            if (inEntity && braceDepth == 1) {
                auto parts = parseQuotedTokens(t);
                if (parts.size() >= 2) {
                    current[toLower(parts[0])] = parts[1];
                }
            }
        }
        return result;
    }
};

}