#include "runtime_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include <toml++/toml.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace mojorecomp::config {
namespace {

RuntimeConfig g_config{};
std::mutex g_configMutex;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ValidateScale(uint32_t value)
{
    return value >= 1 && value <= 3;
}

bool ValidateFiltering(uint32_t value)
{
    return value == 0 || value == 1 || value == 2 || value == 4 ||
           value == 8 || value == 16;
}

template <typename T>
bool ReadString(const toml::table& table, std::string_view key, T& out,
                bool (*parser)(const std::string&, T&), std::string& error)
{
    const auto value = table[key].value<std::string>();
    if (!value)
        return true;
    if (!parser(*value, out))
    {
        error = "Invalid value for '" + std::string(key) + "': " + *value;
        return false;
    }
    return true;
}

bool ApplyEnvString(const char* name, auto parser, auto& target, std::string& error)
{
    const char* value = std::getenv(name);
    if (!value || !*value)
        return true;
    using Value = std::remove_reference_t<decltype(target)>;
    Value parsed{};
    if (!parser(std::string(value), parsed))
    {
        error = std::string("Invalid ") + name + " value: " + value;
        return false;
    }
    target = parsed;
    return true;
}

bool ReadGraphicsExtensions(const toml::table& graphics, RuntimeConfig& config,
                            const char* source, std::string& error)
{
    const std::string prefix = std::string(source) + ": ";
    if (const auto* node = graphics.get("motion_blur"))
    {
        const auto value = node->value<bool>();
        if (!value || !node->is_boolean())
        {
            error = prefix + "graphics.motion_blur must be true or false";
            return false;
        }
        config.motionBlur = *value;
    }
    if (const auto* node = graphics.get("depth_of_field"))
    {
        const auto value = node->value<bool>();
        if (!value || !node->is_boolean())
        {
            error = prefix + "graphics.depth_of_field must be true or false";
            return false;
        }
        config.depthOfField = *value;
    }
    if (const auto* node = graphics.get("upscaler"))
    {
        const auto value = node->value<std::string>();
        if (!value || !ParseUpscaler(*value, config.upscaler))
        {
            error = prefix + "graphics.upscaler must be \"bilinear\", \"fsr1\" or \"rcas\"";
            return false;
        }
    }
    if (const auto* node = graphics.get("fsr_sharpness"))
    {
        std::optional<double> value;
        if (node->is_floating_point())
            value = node->value<double>();
        else if (node->is_integer())
            value = static_cast<double>(*node->value<int64_t>());
        if (!value || !ValidFsrSharpness(*value))
        {
            error = prefix + "graphics.fsr_sharpness must be a number from 0.0 to 1.0";
            return false;
        }
        config.fsrSharpness = static_cast<float>(*value);
    }
    return true;
}

bool ReadProfiling(const toml::table& profiling, RuntimeConfig& config,
                   const char* source, std::string& error)
{
    const std::string prefix = std::string(source) + ": ";
    if (const auto* node = profiling.get("guest_profile"))
    {
        bool ok = false;
        if (node->is_boolean())
        {
            config.guestProfile = *node->value<bool>()
                ? GuestProfileMode::MainThread : GuestProfileMode::Off;
            ok = true;
        }
        else if (const auto value = node->value<std::string>())
            ok = ParseGuestProfileMode(*value, config.guestProfile);
        if (!ok)
        {
            error = prefix + "profiling.guest_profile must be \"off\", \"main\", \"all\" or a boolean";
            return false;
        }
    }
    if (const auto* node = profiling.get("gpu_profile"))
    {
        if (!node->is_boolean())
        {
            error = prefix + "profiling.gpu_profile must be true or false";
            return false;
        }
        config.gpuProfile = *node->value<bool>();
    }
    return true;
}

bool RejectUnknownKeys(const toml::table& table, std::initializer_list<std::string_view> known,
                       const std::string& where, std::string& error)
{
    for (const auto& [key, node] : table)
    {
        (void)node;
        const std::string_view name = key.str();
        if (std::find(known.begin(), known.end(), name) == known.end())
        {
            error = where + "unknown key '" + std::string(name) + "'";
            return false;
        }
    }
    return true;
}

} // namespace

bool ParseDisplayMode(const std::string& value, DisplayMode& out)
{
    const std::string v = Lower(value);
    if (v == "windowed")
    {
        out = DisplayMode::Windowed;
        return true;
    }
    if (v == "fullscreen" || v == "borderless" || v == "borderless_fullscreen")
    {
        out = DisplayMode::Fullscreen;
        return true;
    }
    return false;
}

bool ParseAspectRatio(const std::string& value, AspectRatio& out)
{
    const std::string v = Lower(value);
    if (v == "16:9" || v == "16x9" || v == "native" || v == "native16:9")
        out = AspectRatio::Native16x9;
    else if (v == "21:9" || v == "21x9" || v == "ultrawide")
        out = AspectRatio::Ultrawide21x9;
    else if (v == "32:9" || v == "32x9" || v == "super_ultrawide" || v == "superultrawide")
        out = AspectRatio::SuperUltrawide32x9;
    else if (v == "16:10" || v == "16x10")
        out = AspectRatio::Ratio16x10;
    else if (v == "4:3" || v == "4x3")
        out = AspectRatio::Ratio4x3;
    else
        return false;
    return true;
}

bool ParseAntiAliasing(const std::string& value, AntiAliasing& out)
{
    const std::string v = Lower(value);
    if (v == "off" || v == "none")
        out = AntiAliasing::Off;
    else if (v == "fxaa")
        out = AntiAliasing::Fxaa;
    else if (v == "fxaa_extreme" || v == "fxaa-extreme" || v == "fxaa extreme")
        out = AntiAliasing::FxaaExtreme;
    else
        return false;
    return true;
}

bool ParseTextureFiltering(const std::string& value, uint32_t& out)
{
    const std::string v = Lower(value);
    if (v == "default")
    {
        out = 0;
        return true;
    }
    std::string numeric = v;
    if (!numeric.empty() && numeric.back() == 'x')
        numeric.pop_back();
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(numeric.c_str(), &end, 10);
    if (end == numeric.c_str() || *end != '\0' || !ValidateFiltering(static_cast<uint32_t>(parsed)))
        return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseBool(const std::string& value, bool& out)
{
    const std::string v = Lower(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on")
    {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off")
    {
        out = false;
        return true;
    }
    return false;
}

bool ParseUpscaler(const std::string& value, Upscaler& out)
{
    const std::string v = Lower(value);
    if (v == "bilinear" || v == "default" || v == "off" || v == "none")
        out = Upscaler::Bilinear;
    else if (v == "fsr1" || v == "fsr" || v == "fsr_1" || v == "fsr-1")
        out = Upscaler::Fsr1;
    else if (v == "rcas" || v == "sharpen")
        out = Upscaler::Rcas;
    else
        return false;
    return true;
}

bool ValidFsrSharpness(double value)
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool ParseFsrSharpness(const std::string& value, float& out)
{
    if (value.empty())
        return false;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || !ValidFsrSharpness(parsed))
        return false;
    out = static_cast<float>(parsed);
    return true;
}

bool ParseGuestProfileMode(const std::string& value, GuestProfileMode& out)
{
    const std::string v = Lower(value);
    if (v == "0" || v == "off" || v == "false" || v == "no")
        out = GuestProfileMode::Off;
    else if (v == "1" || v == "on" || v == "true" || v == "yes" || v == "main")
        out = GuestProfileMode::MainThread;
    else if (v == "all" || v == "2")
        out = GuestProfileMode::AllThreads;
    else
        return false;
    return true;
}

bool ParseLocalizationProfile(const std::string& value, std::string& profile,
                              uint32_t& xboxLanguage)
{
    const std::string v = Lower(value);
    if (v == "en")
    {
        profile = "en";
        xboxLanguage = 1;
        return true;
    }
    if (v == "de")
    {
        profile = "de";
        xboxLanguage = 3;
        return true;
    }
    if (v == "fr")
    {
        profile = "fr";
        xboxLanguage = 4;
        return true;
    }
    if (v == "es")
    {
        profile = "es";
        xboxLanguage = 5;
        return true;
    }
    if (v == "it")
    {
        profile = "it";
        xboxLanguage = 6;
        return true;
    }
    if (v == "nl")
    {
        profile = "nl";
        xboxLanguage = 16;
        return true;
    }
    if (v == "pt-br" || v == "pt_br")
    {
        profile = "pt-BR";
        // COT's PT-BR localization pack replaces the English text resources while
        // deliberately retaining the English voice/content path.
        xboxLanguage = 1;
        return true;
    }
    return false;
}

bool LoadFile(const std::filesystem::path& path, RuntimeConfig& out, std::string& error)
{
    try
    {
        const toml::table root = toml::parse_file(path.string());
        RuntimeConfig loaded{};
        if (const auto schema = root["schema_version"].value<int64_t>())
        {
            if (*schema != 1)
            {
                error = "Unsupported config schema_version: " + std::to_string(*schema);
                return false;
            }
            loaded.schemaVersion = static_cast<uint32_t>(*schema);
        }

        if (const toml::table* localization = root["localization"].as_table())
        {
            if (const auto profile = (*localization)["profile"].value<std::string>())
            {
                if (!ParseLocalizationProfile(*profile, loaded.localizationProfile,
                                              loaded.xboxLanguage))
                {
                    error = "Unsupported localization.profile: " + *profile;
                    return false;
                }
            }
            if (const auto language = (*localization)["xbox_language"].value<int64_t>())
            {
                if (*language < 1 || *language > 16 ||
                    static_cast<uint32_t>(*language) != loaded.xboxLanguage)
                {
                    error = "localization.xbox_language does not match the selected profile";
                    return false;
                }
            }
        }

        if (const toml::table* display = root["display"].as_table())
        {
            if (!ReadString(*display, "mode", loaded.displayMode, ParseDisplayMode, error) ||
                !ReadString(*display, "aspect_ratio", loaded.aspectRatio, ParseAspectRatio, error))
                return false;
            if (const auto monitor = (*display)["monitor"].value<std::string>())
                loaded.monitor = *monitor;
            if (const auto resolution = (*display)["output_resolution"].value<std::string>())
                loaded.outputResolution = *resolution;
            if (const auto scale = (*display)["resolution_scale"].value<int64_t>())
            {
                if (*scale < 1 || *scale > 3)
                {
                    error = "display.resolution_scale must be 1, 2 or 3";
                    return false;
                }
                loaded.resolutionScale = static_cast<uint32_t>(*scale);
            }
            if (const auto vsync = (*display)["vsync"].value<bool>())
                loaded.vsync = *vsync;
        }

        if (const toml::table* graphics = root["graphics"].as_table())
        {
            if (!ReadString(*graphics, "anti_aliasing", loaded.antiAliasing,
                            ParseAntiAliasing, error))
                return false;
            if (const auto filtering = (*graphics)["texture_filtering"].value<std::string>())
            {
                if (!ParseTextureFiltering(*filtering, loaded.textureFiltering))
                {
                    error = "Invalid graphics.texture_filtering value: " + *filtering;
                    return false;
                }
            }
            else if (const auto filteringInt = (*graphics)["texture_filtering"].value<int64_t>())
            {
                if (*filteringInt < 0 ||
                    !ValidateFiltering(static_cast<uint32_t>(*filteringInt)))
                {
                    error = "graphics.texture_filtering must be default, 1, 2, 4, 8 or 16";
                    return false;
                }
                loaded.textureFiltering = static_cast<uint32_t>(*filteringInt);
            }
            // Not written by the launcher 1.1.0; a later launcher may write them.
            if (!ReadGraphicsExtensions(*graphics, loaded, "settings", error))
                return false;
        }

        if (const toml::table* profiling = root["profiling"].as_table())
        {
            if (!ReadProfiling(*profiling, loaded, "settings", error))
                return false;
        }

        out = std::move(loaded);
        return true;
    }
    catch (const toml::parse_error& parseError)
    {
        error = "Could not parse config TOML: " + std::string(parseError.description());
        return false;
    }
    catch (const std::exception& exception)
    {
        error = "Could not load config: " + std::string(exception.what());
        return false;
    }
}

bool LoadCustomSettingsFile(const std::filesystem::path& path, RuntimeConfig& config,
                            std::string& error)
{
    try
    {
        const toml::table root = toml::parse_file(path.string());
        if (!RejectUnknownKeys(root, {"schema_version", "graphics", "profiling"},
                               "custom-settings: ", error))
            return false;
        if (const auto* schema = root.get("schema_version"))
        {
            if (!schema->is_integer() || *schema->value<int64_t>() != 1)
            {
                error = "custom-settings: unsupported schema_version";
                return false;
            }
        }
        RuntimeConfig updated = config;
        if (const auto* node = root.get("graphics"))
        {
            const toml::table* graphics = node->as_table();
            if (!graphics)
            {
                error = "custom-settings: [graphics] must be a table";
                return false;
            }
            if (!RejectUnknownKeys(*graphics,
                                   {"motion_blur", "depth_of_field", "upscaler", "fsr_sharpness"},
                                   "custom-settings: [graphics] ", error) ||
                !ReadGraphicsExtensions(*graphics, updated, "custom-settings", error))
                return false;
        }
        if (const auto* node = root.get("profiling"))
        {
            const toml::table* profiling = node->as_table();
            if (!profiling)
            {
                error = "custom-settings: [profiling] must be a table";
                return false;
            }
            if (!RejectUnknownKeys(*profiling, {"guest_profile", "gpu_profile"},
                                   "custom-settings: [profiling] ", error) ||
                !ReadProfiling(*profiling, updated, "custom-settings", error))
                return false;
        }
        config = updated;
        return true;
    }
    catch (const toml::parse_error& parseError)
    {
        error = "Could not parse custom-settings TOML: " +
                std::string(parseError.description());
        return false;
    }
    catch (const std::exception& exception)
    {
        error = "Could not load custom-settings: " + std::string(exception.what());
        return false;
    }
}

std::filesystem::path CustomSettingsPathFor(const std::filesystem::path& settingsPath)
{
    if (const char* custom = std::getenv("MOJORECOMP_CUSTOM_SETTINGS"); custom && *custom)
        return std::filesystem::path(custom);
    return settingsPath.parent_path() / "custom-settings.toml";
}

bool ApplyEnvironmentOverrides(RuntimeConfig& config, std::string& error)
{
    if (!ApplyEnvString("MOJORECOMP_DISPLAY_MODE", ParseDisplayMode, config.displayMode, error) ||
        !ApplyEnvString("MOJORECOMP_ASPECT_RATIO", ParseAspectRatio, config.aspectRatio, error) ||
        !ApplyEnvString("MOJORECOMP_VSYNC", ParseBool, config.vsync, error) ||
        !ApplyEnvString("MOJORECOMP_ANTI_ALIASING", ParseAntiAliasing, config.antiAliasing, error) ||
        !ApplyEnvString("MOJORECOMP_TEXTURE_FILTERING", ParseTextureFiltering,
                        config.textureFiltering, error) ||
        !ApplyEnvString("MOJORECOMP_MOTION_BLUR", ParseBool, config.motionBlur, error) ||
        !ApplyEnvString("MOJORECOMP_DEPTH_OF_FIELD", ParseBool, config.depthOfField, error) ||
        !ApplyEnvString("MOJORECOMP_UPSCALER", ParseUpscaler, config.upscaler, error) ||
        !ApplyEnvString("MOJORECOMP_FSR_SHARPNESS", ParseFsrSharpness,
                        config.fsrSharpness, error) ||
        !ApplyEnvString("MOJORECOMP_GUEST_PROFILE", ParseGuestProfileMode,
                        config.guestProfile, error) ||
        !ApplyEnvString("MOJORECOMP_GPU_PROFILE", ParseBool, config.gpuProfile, error))
        return false;

    if (const char* value = std::getenv("MOJORECOMP_LOCALIZATION_PROFILE"); value && *value)
    {
        if (!ParseLocalizationProfile(value, config.localizationProfile, config.xboxLanguage))
        {
            error = std::string("Invalid MOJORECOMP_LOCALIZATION_PROFILE value: ") + value;
            return false;
        }
    }

    if (const char* value = std::getenv("MOJORECOMP_RESOLUTION_SCALE"); value && *value)
    {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0' || !ValidateScale(static_cast<uint32_t>(parsed)))
        {
            error = std::string("Invalid MOJORECOMP_RESOLUTION_SCALE value: ") + value;
            return false;
        }
        config.resolutionScale = static_cast<uint32_t>(parsed);
    }
    return true;
}

void ApplyOverrides(RuntimeConfig& config, const RuntimeConfigOverrides& overrides)
{
    if (overrides.displayMode) config.displayMode = *overrides.displayMode;
    if (overrides.resolutionScale) config.resolutionScale = *overrides.resolutionScale;
    if (overrides.aspectRatio) config.aspectRatio = *overrides.aspectRatio;
    if (overrides.vsync) config.vsync = *overrides.vsync;
    if (overrides.antiAliasing) config.antiAliasing = *overrides.antiAliasing;
    if (overrides.textureFiltering) config.textureFiltering = *overrides.textureFiltering;
}

void Set(const RuntimeConfig& config)
{
    std::lock_guard lock(g_configMutex);
    g_config = config;
}

const RuntimeConfig& Get()
{
    return g_config;
}

const char* ToString(DisplayMode value)
{
    return value == DisplayMode::Fullscreen ? "fullscreen" : "windowed";
}

const char* ToString(AspectRatio value)
{
    switch (value)
    {
        case AspectRatio::Ultrawide21x9: return "21:9";
        case AspectRatio::SuperUltrawide32x9: return "32:9";
        case AspectRatio::Ratio16x10: return "16:10";
        case AspectRatio::Ratio4x3: return "4:3";
        default: return "16:9";
    }
}

const char* ToString(AntiAliasing value)
{
    switch (value)
    {
        case AntiAliasing::Fxaa: return "fxaa";
        case AntiAliasing::FxaaExtreme: return "fxaa_extreme";
        default: return "off";
    }
}

const char* ToString(Upscaler value)
{
    switch (value)
    {
        case Upscaler::Fsr1: return "fsr1";
        case Upscaler::Rcas: return "rcas";
        default: return "bilinear";
    }
}

const char* ToString(GuestProfileMode value)
{
    switch (value)
    {
        case GuestProfileMode::MainThread: return "main";
        case GuestProfileMode::AllThreads: return "all";
        default: return "off";
    }
}

const char* TextureFilteringToString(uint32_t value)
{
    switch (value)
    {
        case 1: return "1x";
        case 2: return "2x";
        case 4: return "4x";
        case 8: return "8x";
        case 16: return "16x";
        default: return "default";
    }
}

} // namespace mojorecomp::config
