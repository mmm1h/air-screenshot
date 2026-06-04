#include "airshot/feature.h"

namespace airshot {
namespace {

class ConfigFeature final : public IFeatureModule {
public:
    explicit ConfigFeature(std::wstring module_id) : id_(std::move(module_id)) {}

    std::wstring_view id() const noexcept override { return id_; }

private:
    std::wstring id_;
};

std::unique_ptr<IFeatureModule> make_feature(std::wstring_view id) {
    return std::make_unique<ConfigFeature>(std::wstring(id));
}

}  // namespace

FeatureRegistry::FeatureRegistry() {
    entries_.push_back({L"annotation", &AppConfig::annotation_enabled, make_feature, {}});
    entries_.push_back({L"ocr", &AppConfig::ocr_enabled, make_feature, {}});
    entries_.push_back({L"shell", &AppConfig::shell_enabled, make_feature, {}});
}

bool FeatureRegistry::enabled(std::wstring_view id, const AppConfig& config) const {
    const auto found = std::ranges::find_if(entries_, [id](const Entry& entry) { return entry.id == id; });
    return found != entries_.end() && config.*found->enabled;
}

std::vector<std::pair<std::wstring, bool>> FeatureRegistry::list(const AppConfig& config) const {
    std::vector<std::pair<std::wstring, bool>> result;
    for (const auto& entry : entries_) {
        result.emplace_back(entry.id, config.*entry.enabled);
    }
    return result;
}

IFeatureModule* FeatureRegistry::activate(std::wstring_view id, const AppConfig& config) {
    const auto found = std::ranges::find_if(entries_, [id](const Entry& entry) { return entry.id == id; });
    if (found == entries_.end() || !(config.*found->enabled)) {
        return nullptr;
    }
    if (!found->instance) {
        found->instance = found->factory(found->id);
    }
    return found->instance.get();
}

void FeatureRegistry::unload_disabled(const AppConfig& config) {
    for (auto& entry : entries_) {
        if (!(config.*entry.enabled)) {
            entry.instance.reset();
        }
    }
}

bool FeatureRegistry::loaded(std::wstring_view id) const {
    const auto found = std::ranges::find_if(entries_, [id](const Entry& entry) { return entry.id == id; });
    return found != entries_.end() && found->instance != nullptr;
}

}  // namespace airshot
