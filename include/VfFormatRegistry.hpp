#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace VfExt {

namespace fs = std::filesystem;

struct InspectResult {
    bool handled = false;
    bool valid = false;
    std::string engineLabel; // "Source 1" / "Source 2"
    std::string summary;
};

class IFormatHandler {
public:
    virtual ~IFormatHandler() = default;
    virtual std::string name() const = 0;
    virtual bool canHandle(const std::string& lowerExt) const = 0;
    virtual InspectResult inspect(const fs::path& filePath) const = 0;
};

// ============================================================================
// Обработчик скомпилированных ресурсов Source 2 (Valve Resource Format: *_c)
// ============================================================================
class Source2ResourceHandler final : public IFormatHandler {
public:
    std::string name() const override {
        return "Source 2 Compiled Resource Inspector";
    }

    bool canHandle(const std::string& lowerExt) const override {
        return lowerExt == ".vtex_c" || lowerExt == ".vmat_c" ||
               lowerExt == ".vmdl_c" || lowerExt == ".vmap_c" ||
               lowerExt == ".vsnd_c" || lowerExt == ".vpcf_c";
    }

    InspectResult inspect(const fs::path& filePath) const override {
        InspectResult res;
        res.handled = true;
        res.engineLabel = "Source 2";

        std::error_code ec;
        uintmax_t fileSize = fs::file_size(filePath, ec);
        if (ec || fileSize < 16) {
            res.valid = false;
            res.summary = "Source 2: файл повреждён или слишком мал (< 16 байт).";
            return res;
        }

        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open()) {
            res.valid = false;
            res.summary = "Source 2: не удалось открыть файл.";
            return res;
        }

        uint32_t declaredSize = 0;
        uint16_t headerVersion = 0;
        uint16_t resourceVersion = 0;
        uint32_t blockOffset = 0;
        uint32_t blockCount = 0;

        in.read(reinterpret_cast<char*>(&declaredSize), 4);
        in.read(reinterpret_cast<char*>(&headerVersion), 2);
        in.read(reinterpret_cast<char*>(&resourceVersion), 2);
        in.read(reinterpret_cast<char*>(&blockOffset), 4);
        in.read(reinterpret_cast<char*>(&blockCount), 4);

        if (!in.good() || blockCount > 256 || declaredSize > fileSize * 2) {
            res.valid = false;
            res.summary = "Source 2: некорректный заголовок Valve Resource.";
            return res;
        }

        res.valid = true;
        res.summary = "Source 2 Resource: " + filePath.filename().string() +
                      "\nHeader v" + std::to_string(headerVersion) +
                      " | Resource v" + std::to_string(resourceVersion) +
                      "\nБлоков данных: " + std::to_string(blockCount) +
                      "\nРазмер: " + std::to_string(fileSize) + " байт";
        return res;
    }
};

class FormatRegistry {
public:
    static FormatRegistry& instance() {
        static FormatRegistry reg;
        return reg;
    }

    void registerHandler(std::shared_ptr<IFormatHandler> handler) {
        if (handler) {
            handlers_.push_back(std::move(handler));
        }
    }

    InspectResult tryInspect(const fs::path& filePath) const {
        std::string ext = filePath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        for (const auto& h : handlers_) {
            if (h->canHandle(ext)) {
                return h->inspect(filePath);
            }
        }
        return {};
    }

private:
    FormatRegistry() {
        registerHandler(std::make_shared<Source2ResourceHandler>());
    }
    std::vector<std::shared_ptr<IFormatHandler>> handlers_;
};

}