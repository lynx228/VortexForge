#include "VfCore.hpp"
#include "VfTexForge.hpp"
#include "VfDemoReel.hpp"
#include "VfModules.hpp"
#include "VfExtratools.hpp"
#include "VfUltraSuite.hpp"

#include <clocale>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void printUsage(VfCore::LogSink& logger) {
    logger.write(VfCore::Level::Info,
        "VortexForge Ultra Multi-Tool — комбайн ассетов Source 1 / Source 2\n"
        "Доступные режимы запуска:\n"
        "  [Текстуры и материалы]\n"
        "  1)  valve_converter --vtf <файл.vtf> [папка_вывода]\n"
        "  2)  valve_converter --vtf-info <файл.vtf>\n"
        "  3)  valve_converter --gen-vmt <текстура.vtf/.png> [папка_вывода] [префикс]\n"
        "  4)  valve_converter --vmt <файл.vmt> [папка_вывода]\n"
        "  5)  valve_converter --vmt-fix <файл.vmt> [папка_игры] [папка_вывода]\n"
        "  [Архивы VPK]\n"
        "  6)  valve_converter --vpk-list <архив.vpk> [фильтр_строки]\n"
        "  7)  valve_converter --vpk-extract <архив.vpk> [фильтр_строки] [папка_вывода]\n"
        "  [Демки и видео]\n"
        "  8)  valve_converter --dem <файл.dem> [папка_вывода]\n"
        "  9)  valve_converter --dem-info <файл.dem>\n"
        "  10) valve_converter --dem-batch <файл.dem> [папка_вывода]\n"
        "  11) valve_converter --dem-markers <файл.dem> [папка_вывода]\n"
        "  12) valve_converter --game-cfg <csgo|css|tf2|gmod> [папка_вывода]\n"
        "  [Карты VMF]\n"
        "  13) valve_converter --vmf-json <файл.vmf> [папка_вывода]\n"
        "  14) valve_converter --vmf-csv <файл.vmf> [папка_вывода]\n"
        "  15) valve_converter --vmf-obj <файл.vmf> [папка_вывода]\n"
        "  16) valve_converter --vmf-stats <файл.vmf> [папка_вывода]\n"
        "  17) valve_converter --vmf-diff <старый.vmf> <новый.vmf>\n"
        "  [Зависимости, упаковка и пакетная обработка]\n"
        "  18) valve_converter --check-deps <файл.vmf/.vmt> <папка_игры> [доп_папка]\n"
        "  19) valve_converter --pack-deps <файл.vmf/.vmt> <папка_игры> [папка_сборки]\n"
        "  20) valve_converter --pack-manifest <файл.vmf/.vmt> <папка_игры> [папка_сборки]\n"
        "  21) valve_converter --find-unused <файл.vmf/.vmt> <папка_ассетов>\n"
        "  22) valve_converter --lint-paths <файл.vmf/.vmt>\n"
        "  23) valve_converter --batch <папка_входа> [папка_вывода]\n"
        "  24) valve_converter --gui");
}

}

int main(int argc, char* argv[]) {
    std::setlocale(LC_ALL, "Russian");

    VfCore::LogSink logger("vortex_forge.log");
    VfUltra::ProjectConfig projCfg = VfUltra::ProjectConfig::loadOrCreate("vortexforge.cfg");

    bool launchGui = (argc == 1);
    for (int i = 1; i < argc; ++i) {
        if (VfCore::toLower(argv[i]) == "--gui") {
            launchGui = true;
            break;
        }
    }
    
    if (launchGui) {
        return VfUltra::GuiShell::run(logger, projCfg);
    }

    if (argc < 3) {
        printUsage(logger);
        return 0;
    }

    const std::string mode = VfCore::toLower(argv[1]);
    const fs::path inputPath = argv[2];
    const fs::path outDir = (argc >= 4) ? fs::path(argv[3]) : fs::path(projCfg.outputDir);

    if (mode == "--vtf") {
        VfTexForge::TextureProcessor texProc(logger);
        return texProc.convertVtf(inputPath, outDir) ? 0 : 1;

    } else if (mode == "--vtf-info") {
        auto info = VfV2::ExtraTools::inspectTextureInfo(inputPath);
        if (!info.valid) {
            logger.write(VfCore::Level::Error, "Не удалось прочитать заголовок VTF: " + inputPath.string());
            return 1;
        }
        logger.write(VfCore::Level::Info,
            "VTF v" + std::to_string(info.versionMajor) + "." + std::to_string(info.versionMinor) +
            " | Размер: " + std::to_string(info.width) + "x" + std::to_string(info.height) +
            " | MIP: " + std::to_string(info.mipCount) +
            " | FormatID: " + std::to_string(info.highResFormat) +
            " | PowerOfTwo: " + std::string(info.isPowerOfTwo ? "Да" : "Нет"));

    } else if (mode == "--gen-vmt") {
        std::string prefix = (argc >= 5) ? argv[4] : "";
        fs::path vmtOut = outDir / (inputPath.stem().string() + ".vmt");
        bool ok = VfV2::ExtraTools::generateBasicVmt(inputPath, vmtOut, prefix);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("Базовый VMT создан: " + vmtOut.string()) : "Ошибка генерации VMT.");
        return ok ? 0 : 1;

    } else if (mode == "--vmt") {
        fs::path outJson = outDir / (inputPath.stem().string() + ".json");
        bool ok = VfV2::MatAndDemoV2::convertVmtToJson(inputPath, outJson);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("VMT конвертирован в JSON: " + outJson.string()) : "Ошибка конвертации VMT.");
        return ok ? 0 : 1;

    } else if (mode == "--vmt-fix") {
        fs::path gamePath = (argc >= 4) ? fs::path(argv[3]) : fs::path(projCfg.gameDir);
        fs::path saveDir = (argc >= 5) ? fs::path(argv[4]) : fs::path(projCfg.outputDir);
        fs::path vmtFixed = saveDir / inputPath.filename();

        auto rep = VfUltra::MaterialDoctor::healAndCheckVmt(inputPath, vmtFixed, {gamePath, projCfg.customDir});
        logger.write(VfCore::Level::Info,
            "VMT обработан (" + vmtFixed.string() + ") — нормализовано путей: " +
            std::to_string(rep.normalizedKeys) + ", битых ссылок: " + std::to_string(rep.missingTextures.size()));
        for (const auto& m : rep.missingTextures) {
            logger.write(VfCore::Level::Warn, "  [BROKEN TEX] " + m);
        }

    } else if (mode == "--vpk-list") {
        std::vector<VfUltra::VpkEntry> entries;
        uint32_t ver = 0;
        if (!VfUltra::VpkInspector::readDirectory(inputPath, entries, ver)) {
            logger.write(VfCore::Level::Error, "Не удалось прочитать VPK: " + inputPath.string());
            return 1;
        }
        std::string filter = (argc >= 4) ? VfCore::toLower(argv[3]) : "";
        int shown = 0;
        for (const auto& e : entries) {
            if (!filter.empty() && VfV2::toLower(e.fullPath).find(filter) == std::string::npos) continue;
            if (shown < 50) {
                logger.write(VfCore::Level::Info, "  " + e.fullPath + " (" + std::to_string(e.entryLength + e.preloadBytes) + " B)");
            }
            ++shown;
        }
        logger.write(VfCore::Level::Info,
            "VPK v" + std::to_string(ver) + ": всего файлов=" + std::to_string(entries.size()) +
            ", совпало по фильтру=" + std::to_string(shown));

    } else if (mode == "--vpk-extract") {
        std::string filter = (argc >= 4) ? argv[3] : "";
        fs::path extractDir = (argc >= 5) ? fs::path(argv[4]) : (fs::path(projCfg.outputDir) / "vpk_extracted");
        int count = VfUltra::VpkInspector::extractMatching(inputPath, filter, extractDir);
        if (count < 0) {
            logger.write(VfCore::Level::Error, "Ошибка распаковки VPK: " + inputPath.string());
            return 1;
        }
        logger.write(VfCore::Level::Info,
            "Извлечено файлов из VPK: " + std::to_string(count) + " в папку " + extractDir.string());

    } else if (mode == "--dem") {
        VfDemoReel::DemoPipeline demoPipe(logger);
        VfDemoReel::DemoSummary info;
        demoPipe.inspectDemo(inputPath, info);

        VfDemoReel::ReelSettings reel;
        reel.demoPath = inputPath;
        reel.outputVideo = outDir / (inputPath.stem().string() + ".mp4");
        reel.runExternal = false;
        return demoPipe.render(reel) ? 0 : 1;

    } else if (mode == "--dem-info") {
        auto dh = VfUltra::DemoUltraTools::parseHeader(inputPath);
        if (!dh.valid) {
            logger.write(VfCore::Level::Error, "Не удалось прочитать заголовок HL2DEMO: " + inputPath.string());
            return 1;
        }
        logger.write(VfCore::Level::Info,
            "DEM Протокол: " + std::to_string(dh.demoProtocol) +
            " | Игра: " + dh.gameDirectory +
            " | Карта: " + dh.mapName +
            " | Игрок: " + dh.clientName +
            " | Тиков: " + std::to_string(dh.playbackTicks) +
            " | Время: " + std::to_string(dh.playbackTime) + " с");

    } else if (mode == "--dem-batch") {
        std::vector<VfV2::ClipRange> clips = {
            {"clip_01", 10.0, 20.0},
            {"clip_02", 35.0, 48.0}
        };
        logger.write(VfCore::Level::Info, VfV2::ExtraTools::buildTickAccurateClipList(clips, projCfg.defaultTickRate));

#if defined(_WIN32)
        const bool isWindows = true;
        const fs::path scriptPath = outDir / "render_clips.bat";
#else
        const bool isWindows = false;
        const fs::path scriptPath = outDir / "render_clips.sh";
#endif
        bool ok = VfV2::ExtraTools::writeClipRunnerScript(
            scriptPath, inputPath, clips, VfV2::VideoPreset::YouTube1080p60, ".mp4", outDir / "clips", isWindows);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("Скрипт рендера клипов сохранён: " + scriptPath.string()) : "Ошибка создания скрипта клипов.");
        return ok ? 0 : 1;

    } else if (mode == "--dem-markers") {
        std::vector<VfV2::ClipRange> clips = {
            {"intro_frag", 8.0, 16.0},
            {"clutch_round", 42.0, 58.0}
        };
        bool ok = VfUltra::DemoUltraTools::exportTimelineMarkers(inputPath, clips, projCfg.defaultTickRate, outDir);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("Маркеры глав и demo_gototick сохранены в " + outDir.string()) : "Ошибка экспорта маркеров.");
        return ok ? 0 : 1;

    } else if (mode == "--game-cfg") {
        std::string profile = argv[2];
        fs::path cfgPath = outDir / ("vortex_movie_" + VfCore::toLower(profile) + ".cfg");
        bool ok = VfV2::ExtraTools::writeGameLaunchPreset(cfgPath, profile, 60);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("Конфиг для игры создан: " + cfgPath.string()) : "Ошибка создания .cfg-пресета.");
        return ok ? 0 : 1;

    } else if (mode == "--vmf-json") {
        fs::path outJson = outDir / (inputPath.stem().string() + "_entities.json");
        bool ok = VfV2::DepAndMapTools::exportEntitiesToJson(inputPath, outJson);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("Сущности VMF выгружены в JSON: " + outJson.string()) : "Ошибка экспорта VMF в JSON.");
        return ok ? 0 : 1;

    } else if (mode == "--vmf-csv") {
        fs::path outCsv = outDir / (inputPath.stem().string() + "_entities.csv");
        bool ok = VfV2::ExtraTools::exportEntitiesSummaryCsv(inputPath, outCsv);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("Сущности VMF выгружены в CSV: " + outCsv.string()) : "Ошибка экспорта VMF в CSV.");
        return ok ? 0 : 1;

    } else if (mode == "--vmf-obj") {
        fs::path outObj = outDir / (inputPath.stem().string() + "_entities.obj");
        bool ok = VfV2::ExtraTools::exportEntitiesToObj(inputPath, outObj, 16.0);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("3D-маркеры сущностей сохранены в OBJ: " + outObj.string()) : "Ошибка экспорта VMF в OBJ.");
        return ok ? 0 : 1;

    } else if (mode == "--vmf-stats") {
        fs::path outStats = outDir / (inputPath.stem().string() + "_stats.json");
        bool ok = VfV2::ExtraTools::exportMapStats(inputPath, outStats);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("Статистика карты сохранена: " + outStats.string()) : "Ошибка анализа статистики карты.");
        return ok ? 0 : 1;

    } else if (mode == "--vmf-diff") {
        if (argc < 4) {
            logger.write(VfCore::Level::Error, "Укажите два VMF-файла: --vmf-diff <старый.vmf> <новый.vmf>");
            return 1;
        }
        auto diff = VfV2::ExtraTools::diffMapEntities(inputPath, fs::path(argv[3]));
        logger.write(VfCore::Level::Info,
            "Сравнение карт: добавлено=" + std::to_string(diff.added.size()) +
            ", удалено=" + std::to_string(diff.removed.size()) +
            ", сдвинуто=" + std::to_string(diff.moved.size()));
        for (const auto& s : diff.added) logger.write(VfCore::Level::Info, "  [+] " + s);
        for (const auto& s : diff.removed) logger.write(VfCore::Level::Warn, "  [-] " + s);
        for (const auto& s : diff.moved) logger.write(VfCore::Level::Info, "  [~] " + s);

    } else if (mode == "--check-deps") {
        std::vector<fs::path> searchDirs = {
            (argc >= 4) ? fs::path(argv[3]) : fs::path(projCfg.gameDir),
            (argc >= 5) ? fs::path(argv[4]) : fs::path(projCfg.customDir)
        };
        auto refs = VfV2::DepAndMapTools::collectAssetRefs(inputPath);
        auto missing = VfV2::ExtraTools::checkMissingAssets(refs, searchDirs);

        logger.write(VfCore::Level::Info,
            "Ссылок найдено: " + std::to_string(refs.size()) +
            ", отсутствует: " + std::to_string(missing.size()));
        for (const auto& m : missing) {
            logger.write(VfCore::Level::Warn, "  [MISSING] " + m);
        }

    } else if (mode == "--pack-deps") {
        std::vector<fs::path> searchDirs = {
            (argc >= 4) ? fs::path(argv[3]) : fs::path(projCfg.gameDir),
            fs::path(projCfg.customDir)
        };
        fs::path packDir = (argc >= 5) ? fs::path(argv[4]) : (fs::path(projCfg.outputDir) / "packed_assets");

        auto refs = VfV2::DepAndMapTools::collectAssetRefs(inputPath);
        int copied = VfV2::ExtraTools::packFoundAssets(refs, searchDirs, packDir);
        logger.write(VfCore::Level::Info,
            "Упаковано ассетов: " + std::to_string(copied) + " из " + std::to_string(refs.size()) +
            " в папку " + packDir.string());

    } else if (mode == "--pack-manifest") {
        std::vector<fs::path> searchDirs = {
            (argc >= 4) ? fs::path(argv[3]) : fs::path(projCfg.gameDir),
            fs::path(projCfg.customDir)
        };
        fs::path packDir = (argc >= 5) ? fs::path(argv[4]) : (fs::path(projCfg.outputDir) / "packed_manifest");
        bool ok = VfUltra::MaterialDoctor::packWithManifest(inputPath, searchDirs, packDir, false);
        logger.write(ok ? VfCore::Level::Info : VfCore::Level::Error,
            ok ? ("Сборка пака и manifest.json завершена: " + packDir.string()) : "Ошибка сборки пака.");
        return ok ? 0 : 1;

    } else if (mode == "--find-unused") {
        fs::path assetsDir = (argc >= 4) ? fs::path(argv[3]) : fs::path(projCfg.gameDir);
        auto refs = VfV2::DepAndMapTools::collectAssetRefs(inputPath);
        auto unused = VfV2::ExtraTools::findUnusedAssets(assetsDir, refs);
        logger.write(VfCore::Level::Info, "Найдено неиспользуемых файлов: " + std::to_string(unused.size()));
        for (const auto& u : unused) {
            logger.write(VfCore::Level::Warn, "  [UNUSED] " + u.string());
        }

    } else if (mode == "--lint-paths") {
        auto refs = VfV2::DepAndMapTools::collectAssetRefs(inputPath);
        auto warnings = VfV2::ExtraTools::lintAssetPaths(refs);
        logger.write(VfCore::Level::Info,
            "Проверка путей завершена — всего ссылок: " + std::to_string(refs.size()) +
            ", замечаний: " + std::to_string(warnings.size()));
        for (const auto& w : warnings) {
            logger.write(VfCore::Level::Warn, "  [LINT] " + w);
        }

    } else if (mode == "--batch") {
        VfV2::SmartBatchQueue batch;
        batch.setAllowedExtensions({".vmt", ".vmf", ".bsp"});
        batch.addFromPath(inputPath, outDir);
        VfV2::BatchSummary summary = batch.run(true);

        logger.write(VfCore::Level::Info,
            "Пакетная обработка завершена — готово: " + std::to_string(summary.done) +
            ", пропущено: " + std::to_string(summary.skipped) +
            ", ошибок: " + std::to_string(summary.failed));
        return (summary.failed == 0) ? 0 : 1;

    } else {
        logger.write(VfCore::Level::Warn, "Неизвестный режим: " + std::string(argv[1]));
        printUsage(logger);
        return 1;
    }

    return 0;
}