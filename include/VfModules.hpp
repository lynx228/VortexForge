#pragma once

#include "VfCore.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace VfV2 {

namespace fs = std::filesystem;

inline std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

inline std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

struct ClipRange {
    std::string name;
    double startSec = 0.0;
    double endSec = 0.0;
};

enum class VideoPreset {
    YouTube1080p60,
    HighQualityMp4,
    FastPreview
};

struct BatchSummary {
    int done = 0;
    int skipped = 0;
    int failed = 0;
};

class DepAndMapTools {
public:
    static std::set<std::string> collectAssetRefs(const fs::path& filePath) {
        std::set<std::string> refs;
        std::ifstream in(filePath);
        if (!in.is_open()) return refs;

        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string lower = toLower(content);

        const std::vector<std::string> exts = {".vmt", ".vtf", ".mdl", ".vmat", ".vtex", ".vmdl", ".wav"};
        for (const auto& ext : exts) {
            size_t pos = 0;
            while ((pos = lower.find(ext, pos)) != std::string::npos) {
                size_t start = pos;
                while (start > 0) {
                    char ch = content[start - 1];
                    if (ch == '"' || ch == '\'' || ch == '\n' || ch == '\r' || ch == '\t') break;
                    --start;
                }
                std::string token = trim(content.substr(start, (pos + ext.size()) - start));
                std::replace(token.begin(), token.end(), '\\', '/');
                while (!token.empty() && token.front() == '/') token.erase(token.begin());
                if (!token.empty() && token.size() < 260) {
                    refs.insert(token);
                }
                pos += ext.size();
            }
        }
        return refs;
    }

    static bool exportEntitiesToJson(const fs::path& vmfPath, const fs::path& jsonOutPath) {
        std::ifstream in(vmfPath);
        if (!in.is_open()) return false;

        std::vector<std::map<std::string, std::string>> entities;
        std::map<std::string, std::string> cur;
        bool inEnt = false;
        int depth = 0;
        std::string line;

        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty()) continue;
            if (depth == 0 && toLower(t).find("\"entity\"") == 0) {
                inEnt = true;
                cur.clear();
                continue;
            }
            if (t == "{") { ++depth; continue; }
            if (t == "}") {
                --depth;
                if (depth <= 0 && inEnt) {
                    if (!cur.empty()) entities.push_back(cur);
                    inEnt = false;
                    depth = 0;
                }
                continue;
            }
            if (inEnt && depth == 1) {
                std::vector<std::string> toks;
                std::string buf;
                bool q = false;
                for (char c : t) {
                    if (c == '"') {
                        q = !q;
                        if (!q) { toks.push_back(buf); buf.clear(); }
                    } else if (q) {
                        buf.push_back(c);
                    }
                }
                if (toks.size() >= 2) {
                    cur[toks[0]] = toks[1];
                }
            }
        }

        fs::create_directories(jsonOutPath.parent_path());
        std::ofstream out(jsonOutPath);
        if (!out.is_open()) return false;

        out << "[\n";
        for (size_t i = 0; i < entities.size(); ++i) {
            out << "  {";
            size_t kIdx = 0;
            for (const auto& [k, v] : entities[i]) {
                out << (kIdx == 0 ? "" : ", ")
                    << "\"" << jsonEscape(k) << "\": \"" << jsonEscape(v) << "\"";
                ++kIdx;
            }
            out << "}" << (i + 1 < entities.size() ? "," : "") << "\n";
        }
        out << "]\n";
        return out.good();
    }
};

class MatAndDemoV2 {
public:
    static bool convertVmtToJson(const fs::path& vmtPath, const fs::path& jsonOutPath) {
        std::ifstream in(vmtPath);
        if (!in.is_open()) return false;

        std::string shader;
        std::map<std::string, std::string> props;
        std::string line;
        while (std::getline(in, line)) {
            std::string t = trim(line);
            if (t.empty() || t == "{" || t == "}" || t.rfind("//", 0) == 0) continue;

            std::vector<std::string> toks;
            std::string buf;
            bool q = false;
            for (char c : t) {
                if (c == '"') {
                    q = !q;
                    if (!q) { toks.push_back(buf); buf.clear(); }
                } else if (q) {
                    buf.push_back(c);
                }
            }
            if (toks.size() == 1 && shader.empty()) {
                shader = toks[0];
            } else if (toks.size() >= 2) {
                props[toLower(toks[0])] = toks[1];
            }
        }

        fs::create_directories(jsonOutPath.parent_path());
        std::ofstream out(jsonOutPath);
        if (!out.is_open()) return false;

        out << "{\n"
            << "  \"shader\": \"" << jsonEscape(shader) << "\",\n"
            << "  \"properties\": {";
        size_t idx = 0;
        for (const auto& [k, v] : props) {
            out << (idx == 0 ? "\n    " : ",\n    ")
                << "\"" << jsonEscape(k) << "\": \"" << jsonEscape(v) << "\"";
            ++idx;
        }
        out << (props.empty() ? "" : "\n  ") << "}\n}\n";
        return out.good();
    }

    static std::string buildBatchClipsScript(const fs::path& demPath,
                                             const std::vector<ClipRange>& clips,
                                             VideoPreset preset,
                                             const std::string& containerExt,
                                             const fs::path& outDir) {
        std::string crf = "18";
        std::string fps = "60";
        if (preset == VideoPreset::HighQualityMp4) { crf = "15"; fps = "60"; }
        else if (preset == VideoPreset::FastPreview) { crf = "26"; fps = "30"; }

        std::ostringstream oss;
        oss << "# Demo source: " << demPath.string() << "\n"
            << "mkdir -p \"" << outDir.generic_string() << "\"\n";
        for (const auto& c : clips) {
            double dur = std::max(0.1, c.endSec - c.startSec);
            fs::path outFile = outDir / (c.name + containerExt);
            oss << "ffmpeg -y -ss " << c.startSec << " -t " << dur
                << " -i \"" << demPath.generic_string() << "\" "
                << "-r " << fps << " -c:v libx264 -crf " << crf << " -pix_fmt yuv420p "
                << "\"" << outFile.generic_string() << "\"\n";
        }
        return oss.str();
    }
};

class SmartBatchQueue {
public:
    void setAllowedExtensions(const std::set<std::string>& exts) {
        allowedExts_.clear();
        for (const auto& e : exts) allowedExts_.insert(toLower(e));
    }

    void addFromPath(const fs::path& inputPath, const fs::path& outDir) {
        if (!fs::exists(inputPath)) return;
        if (fs::is_regular_file(inputPath)) {
            items_.push_back({inputPath, outDir});
            return;
        }
        for (const auto& entry : fs::recursive_directory_iterator(inputPath)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = toLower(entry.path().extension().string());
            if (allowedExts_.empty() || allowedExts_.count(ext)) {
                fs::path rel = fs::relative(entry.path(), inputPath);
                items_.push_back({entry.path(), outDir / rel.parent_path()});
            }
        }
    }

    BatchSummary run(bool smartSkip) {
        BatchSummary sum;
        for (const auto& item : items_) {
            std::string ext = toLower(item.src.extension().string());
            fs::path dst = item.dstDir / (item.src.stem().string() + ".json");

            if (smartSkip && fs::exists(dst)) {
                std::error_code ec1, ec2;
                auto tSrc = fs::last_write_time(item.src, ec1);
                auto tDst = fs::last_write_time(dst, ec2);
                if (!ec1 && !ec2 && tDst >= tSrc) {
                    ++sum.skipped;
                    continue;
                }
            }

            bool ok = false;
            if (ext == ".vmt") ok = MatAndDemoV2::convertVmtToJson(item.src, dst);
            else if (ext == ".vmf") ok = DepAndMapTools::exportEntitiesToJson(item.src, dst);
            else ok = true;

            if (ok) ++sum.done;
            else ++sum.failed;
        }
        return sum;
    }

private:
    struct Item {
        fs::path src;
        fs::path dstDir;
    };
    std::set<std::string> allowedExts_;
    std::vector<Item> items_;
};

}