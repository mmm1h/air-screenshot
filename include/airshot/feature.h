#pragma once

#include "airshot/config.h"

#include <memory>

namespace airshot {

class IFeatureModule {
public:
    virtual ~IFeatureModule() = default;
    [[nodiscard]] virtual std::wstring_view id() const noexcept = 0;
};

class FeatureRegistry {
public:
    FeatureRegistry();
    [[nodiscard]] bool enabled(std::wstring_view id, const AppConfig& config) const;
    [[nodiscard]] std::vector<std::pair<std::wstring, bool>> list(const AppConfig& config) const;
    IFeatureModule* activate(std::wstring_view id, const AppConfig& config);
    void unload_disabled(const AppConfig& config);
    [[nodiscard]] bool loaded(std::wstring_view id) const;

private:
    using Enabled = bool AppConfig::*;
    using Factory = std::unique_ptr<IFeatureModule> (*)(std::wstring_view);

    struct Entry {
        std::wstring id;
        Enabled enabled;
        Factory factory;
        std::unique_ptr<IFeatureModule> instance;
    };

    std::vector<Entry> entries_;
};

}  // namespace airshot
