#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mojorecomp::config {

enum class DisplayMode {
    Windowed,
    Fullscreen,
};

enum class AspectRatio {
    Native16x9,
    Ultrawide21x9,
    SuperUltrawide32x9,
    Ratio16x10,
    Ratio4x3,
};

enum class AntiAliasing {
    Off,
    Fxaa,
    FxaaExtreme,
};

// Final scaling from the internal (guest x resolution scale) image to the
// output. Bilinear is the presenter's existing path and stays the default.
enum class Upscaler {
    Bilinear,
    // AMD FidelityFX Super Resolution 1: EASU when the internal image is smaller
    // than the output, then RCAS sharpening.
    Fsr1,
    // RCAS sharpening only, on the existing scaled image.
    Rcas,
};

enum class GuestProfileMode {
    Off,
    // Samples the main guest thread only.
    MainThread,
    // Samples every registered guest thread.
    AllThreads,
};

inline constexpr float kDefaultFsrSharpness = 0.8f;

struct RuntimeConfig {
    uint32_t schemaVersion = 1;
    std::string localizationProfile = "en";
    uint32_t xboxLanguage = 1;
    DisplayMode displayMode = DisplayMode::Windowed;
    std::string monitor = "primary";
    std::string outputResolution = "desktop";
    uint32_t resolutionScale = 1;
    AspectRatio aspectRatio = AspectRatio::Native16x9;
    // Preserve the current timing-safe COT baseline when no launcher config exists.
    bool vsync = false;
    AntiAliasing antiAliasing = AntiAliasing::FxaaExtreme;
    // 0 means Default. Other accepted values are 1, 2, 4, 8 and 16.
    uint32_t textureFiltering = 8;
    // Phase 4 options. The official launcher 1.1.0 rewrites settings.toml from
    // its own fields, so these are also read from custom-settings.toml beside it
    // and from MOJORECOMP_* environment variables. Defaults keep 1.1.0 behaviour.
    bool motionBlur = true;
    Upscaler upscaler = Upscaler::Bilinear;
    // 0..1, higher is sharper. RCAS receives 2 * (1 - value) stops.
    float fsrSharpness = kDefaultFsrSharpness;
    GuestProfileMode guestProfile = GuestProfileMode::Off;
    bool gpuProfile = false;
};

struct RuntimeConfigOverrides {
    std::optional<DisplayMode> displayMode;
    std::optional<uint32_t> resolutionScale;
    std::optional<AspectRatio> aspectRatio;
    std::optional<bool> vsync;
    std::optional<AntiAliasing> antiAliasing;
    std::optional<uint32_t> textureFiltering;
};

bool LoadFile(const std::filesystem::path& path, RuntimeConfig& out, std::string& error);
// Applies only the keys the official launcher does not manage ([graphics]
// motion_blur, upscaler, fsr_sharpness and [profiling] guest_profile,
// gpu_profile) on top of `config`. Other keys in the file are rejected so a
// typo cannot silently do nothing.
bool LoadCustomSettingsFile(const std::filesystem::path& path, RuntimeConfig& config,
                            std::string& error);
// custom-settings.toml in the same folder as settings.toml, or
// MOJORECOMP_CUSTOM_SETTINGS when it is set.
std::filesystem::path CustomSettingsPathFor(const std::filesystem::path& settingsPath);
bool ApplyEnvironmentOverrides(RuntimeConfig& config, std::string& error);
void ApplyOverrides(RuntimeConfig& config, const RuntimeConfigOverrides& overrides);

void Set(const RuntimeConfig& config);
const RuntimeConfig& Get();

bool ParseDisplayMode(const std::string& value, DisplayMode& out);
bool ParseAspectRatio(const std::string& value, AspectRatio& out);
bool ParseAntiAliasing(const std::string& value, AntiAliasing& out);
bool ParseTextureFiltering(const std::string& value, uint32_t& out);
bool ParseBool(const std::string& value, bool& out);
bool ParseUpscaler(const std::string& value, Upscaler& out);
bool ParseFsrSharpness(const std::string& value, float& out);
bool ValidFsrSharpness(double value);
bool ParseGuestProfileMode(const std::string& value, GuestProfileMode& out);
bool ParseLocalizationProfile(const std::string& value, std::string& profile,
                              uint32_t& xboxLanguage);

const char* ToString(DisplayMode value);
const char* ToString(AspectRatio value);
const char* ToString(AntiAliasing value);
const char* ToString(Upscaler value);
const char* ToString(GuestProfileMode value);
const char* TextureFilteringToString(uint32_t value);

} // namespace mojorecomp::config
