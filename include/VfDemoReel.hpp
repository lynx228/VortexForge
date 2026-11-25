// Назначение: анализ заголовков .dem (Source 1 / Source 2), перевод таймкодов в тики,
// генерация .cfg для записи кадров из игры и сборка видео через FFmpeg.
#pragma once

#include "VfCore.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace VfDemoReel {

namespace fs = std::filesystem;

#pragma pack(push, 1)
struct Source1DemoHeader {
    char demoProtocol[8];
    int32_t netProtocol;
    char serverName[260];
    char clientName[260];
    char mapName[260];
    char gameDirectory[260];
    float playbackTime;
    int32_t playbackTicks;
    int32_t playbackFrames;
    int32_t signOnLength;
};
#pragma pack(pop)

struct DemoSummary {
    std::string engineTag;
    std::string mapName;
    float durationSec = 0.0f;
    int32_t totalTicks = 0;
    float tickRate = 64.0f;

    int secToTick(double seconds) const {
        return std::max(0, static_cast<int>(seconds * tickRate));
    }

    static std::string formatTimecode(double seconds) {
        if (seconds < 0.0) seconds = 0.0;
        int totalMs = static_cast<int>(seconds * 1000.0 + 0.5);
        int mins = totalMs / 60000;
        int secs = (totalMs % 60000) / 1000;
        int ms = totalMs % 1000;
        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(2) << mins << ":"
            << std::setw(2) << secs << "."
            << std::setw(3) << ms;
        return oss.str();
    }
};

struct ReelSettings {
    fs::path gameExe = "csgo.exe";
    fs::path gameCfgDir = "./cfg";
    fs::path demoPath;
    fs::path framesPattern = "./frames/frame_%05d.tga";
    fs::path outputVideo;
    int fps = 60;
    int crf = 18;
    bool runExternal = false;
};

class DemoPipeline {
public:
    explicit DemoPipeline(VfCore::LogSink& log) : log_(log) {}

    bool inspectDemo(const fs::path& demPath, DemoSummary& outSummary) {
        std::ifstream in(demPath, std::ios::binary);
        if (!in.is_open()) {
            log_.write(VfCore::Level::Error, "VfDemoReel: не открыть демку " + demPath.string());
            return false;
        }

        char magic[8]{};
        in.read(magic, 8);
        std::string sig(magic, 8);

        if (sig.rfind("HL2DEMO", 0) == 0) {
            in.seekg(0, std::ios::beg);
            Source1DemoHeader hdr{};
            in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));

            outSummary.engineTag = "Source 1 (HL2DEMO)";
            outSummary.mapName = hdr.mapName;
            outSummary.durationSec = hdr.playbackTime;
            outSummary.totalTicks = hdr.playbackTicks;
            outSummary.tickRate = (hdr.playbackTime > 0.01f && hdr.playbackTicks > 0)
                ? static_cast<float>(hdr.playbackTicks) / hdr.playbackTime
                : 64.0f;

            log_.write(VfCore::Level::Info,
                "VfDemoReel [" + outSummary.engineTag + "]: карта='" + outSummary.mapName +
                "', длительность=" + DemoSummary::formatTimecode(outSummary.durationSec) +
                ", тиков=" + std::to_string(outSummary.totalTicks));
            return true;
        }

        if (sig.rfind("PBDEMS2", 0) == 0) {
            outSummary.engineTag = "Source 2 (PBDEMS2)";
            outSummary.mapName = "(внутри protobuf-пакетов)";
            outSummary.durationSec = 0.0f;
            outSummary.totalTicks = 0;
            log_.write(VfCore::Level::Info,
                "VfDemoReel [Source 2 PBDEMS2]: обнаружен заголовок демки " + demPath.filename().string());
            return true;
        }

        log_.write(VfCore::Level::Warn,
            "VfDemoReel: неизвестная сигнатура демки в " + demPath.filename().string());
        return false;
    }

    bool render(const ReelSettings& cfg) {
        fs::create_directories(cfg.gameCfgDir);
        fs::path autoCfg = cfg.gameCfgDir / "vf_demoreel.cfg";
        {
            std::ofstream f(autoCfg);
            if (!f.is_open()) {
                log_.write(VfCore::Level::Error, "VfDemoReel: не создать конфиг " + autoCfg.string());
                return false;
            }
            f << "sv_cheats 1\n";
            f << "host_framerate " << cfg.fps << "\n";
            f << "startmovie \"" << cfg.framesPattern.parent_path().string() << "/frame_\" tga\n";
        }

        fs::path outFile = cfg.outputVideo.empty()
            ? fs::path("./output") / (cfg.demoPath.stem().string() + ".mp4")
            : cfg.outputVideo;
        fs::create_directories(outFile.parent_path());

        std::string launchCmd = "\"" + cfg.gameExe.string() + "\" -novid +playdemo \"" +
                                cfg.demoPath.stem().string() + "\" +exec vf_demoreel.cfg";
        std::string ffmpegCmd = "ffmpeg -y -framerate " + std::to_string(cfg.fps) +
                                " -i \"" + cfg.framesPattern.string() + "\"" +
                                " -c:v libx264 -crf " + std::to_string(cfg.crf) +
                                " -pix_fmt yuv420p \"" + outFile.string() + "\"";

        log_.write(VfCore::Level::Info, "VfDemoReel: конфиг -> " + autoCfg.string());
        log_.write(VfCore::Level::Info, "VfDemoReel: запуск игры -> " + launchCmd);
        log_.write(VfCore::Level::Info, "VfDemoReel: сборка видео -> " + ffmpegCmd);

        if (cfg.runExternal) {
            if (std::system(launchCmd.c_str()) != 0) {
                log_.write(VfCore::Level::Error, "VfDemoReel: процесс игры завершился с ошибкой.");
                return false;
            }
            if (std::system(ffmpegCmd.c_str()) != 0) {
                log_.write(VfCore::Level::Error, "VfDemoReel: FFmpeg завершился с ошибкой.");
                return false;
            }
        }
        return true;
    }

private:
    VfCore::LogSink& log_;
};

}