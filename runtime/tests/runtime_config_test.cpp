#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../config/runtime_config.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

void SetEnv(const char* name, const char* value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

void WriteText(const std::filesystem::path& path, const char* text)
{
    std::ofstream file(path, std::ios::trunc | std::ios::binary);
    file << text;
}

// Options the official launcher 1.1.0 does not manage: defaults, settings.toml
// keys for a later launcher, custom-settings.toml on top, then environment.
int TestPhase4Settings()
{
    namespace cfg = mojorecomp::config;
    const cfg::RuntimeConfig defaults{};
    if (!defaults.motionBlur || defaults.upscaler != cfg::Upscaler::Bilinear ||
        defaults.fsrSharpness != cfg::kDefaultFsrSharpness ||
        defaults.guestProfile != cfg::GuestProfileMode::Off || defaults.gpuProfile)
        return Fail("phase 4 defaults are not conservative");

    const auto dir = std::filesystem::temp_directory_path() / "mojorecomp-config-phase4-test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto settings = dir / "settings.toml";
    const auto custom = dir / "custom-settings.toml";

    // A settings.toml written by the launcher 1.1.0 has none of the new keys.
    WriteText(settings, "schema_version = 1\n[graphics]\nanti_aliasing = \"fxaa\"\n");
    cfg::RuntimeConfig config{};
    std::string error;
    if (!cfg::LoadFile(settings, config, error))
        return Fail(error.c_str());
    if (!config.motionBlur || config.upscaler != cfg::Upscaler::Bilinear)
        return Fail("launcher 1.1.0 settings changed the phase 4 defaults");

    // A later launcher may write them into settings.toml directly.
    WriteText(settings, "schema_version = 1\n[graphics]\nmotion_blur = false\n"
                        "upscaler = \"rcas\"\nfsr_sharpness = 0.5\n");
    if (!cfg::LoadFile(settings, config, error))
        return Fail(error.c_str());
    if (config.motionBlur || config.upscaler != cfg::Upscaler::Rcas ||
        config.fsrSharpness != 0.5f)
        return Fail("settings.toml phase 4 keys were not parsed");
    WriteText(settings, "schema_version = 1\n[graphics]\nfsr_sharpness = 1.5\n");
    if (cfg::LoadFile(settings, config, error))
        return Fail("settings.toml accepted fsr_sharpness outside 0..1");
    WriteText(settings, "schema_version = 1\n[graphics]\nupscaler = \"fsr3\"\n");
    if (cfg::LoadFile(settings, config, error))
        return Fail("settings.toml accepted an unknown upscaler");

    // custom-settings.toml overrides only its own keys.
    if (cfg::CustomSettingsPathFor(settings) != custom)
        return Fail("custom-settings.toml is not looked up beside settings.toml");
    config = {};
    config.antiAliasing = cfg::AntiAliasing::Fxaa;
    config.resolutionScale = 2;
    WriteText(custom, "# local options\n[graphics]\nmotion_blur = false\nupscaler = \"fsr1\"\n"
                      "depth_of_field = false\n"
                      "fsr_sharpness = 1\n[profiling]\nguest_profile = \"all\"\n"
                      "gpu_profile = true\n");
    if (!cfg::LoadCustomSettingsFile(custom, config, error))
        return Fail(error.c_str());
    if (config.motionBlur || config.depthOfField || config.upscaler != cfg::Upscaler::Fsr1 ||
        config.fsrSharpness != 1.0f ||
        config.guestProfile != cfg::GuestProfileMode::AllThreads || !config.gpuProfile)
        return Fail("custom-settings.toml values were not applied");
    if (config.antiAliasing != cfg::AntiAliasing::Fxaa || config.resolutionScale != 2)
        return Fail("custom-settings.toml changed a launcher-managed setting");

    const char* rejected[] = {
        "[graphics]\nanti_aliasing = \"off\"\n",   // launcher-managed key
        "[graphics]\nmotion_blurr = false\n",       // typo
        "[display]\nvsync = true\n",                // launcher-managed table
        "[graphics]\nmotion_blur = 0\n",            // not a boolean
        "[graphics]\nfsr_sharpness = -0.1\n",
        "[profiling]\nguest_profile = \"some\"\n",
        "schema_version = 2\n",
    };
    for (const char* text : rejected)
    {
        cfg::RuntimeConfig untouched{};
        WriteText(custom, text);
        if (cfg::LoadCustomSettingsFile(custom, untouched, error))
        {
            std::fprintf(stderr, "accepted:\n%s", text);
            return Fail("invalid custom-settings.toml was accepted");
        }
        if (!untouched.motionBlur || untouched.upscaler != cfg::Upscaler::Bilinear)
            return Fail("a rejected custom-settings.toml still changed the config");
    }

    // MOJORECOMP_CUSTOM_SETTINGS points elsewhere.
    SetEnv("MOJORECOMP_CUSTOM_SETTINGS", (dir / "elsewhere.toml").string().c_str());
    if (cfg::CustomSettingsPathFor(settings) != dir / "elsewhere.toml")
        return Fail("MOJORECOMP_CUSTOM_SETTINGS was ignored");
    SetEnv("MOJORECOMP_CUSTOM_SETTINGS", nullptr);

    // Environment variables win over both files.
    config = {};
    config.motionBlur = false;
    SetEnv("MOJORECOMP_MOTION_BLUR", "1");
    SetEnv("MOJORECOMP_UPSCALER", "FSR1");
    SetEnv("MOJORECOMP_FSR_SHARPNESS", "0.25");
    SetEnv("MOJORECOMP_GUEST_PROFILE", "1");
    SetEnv("MOJORECOMP_GPU_PROFILE", "1");
    const bool applied = cfg::ApplyEnvironmentOverrides(config, error);
    if (!applied || !config.motionBlur || config.upscaler != cfg::Upscaler::Fsr1 ||
        config.fsrSharpness != 0.25f ||
        config.guestProfile != cfg::GuestProfileMode::MainThread || !config.gpuProfile)
        return Fail("phase 4 environment overrides were not applied");
    SetEnv("MOJORECOMP_FSR_SHARPNESS", "2");
    if (cfg::ApplyEnvironmentOverrides(config, error))
        return Fail("MOJORECOMP_FSR_SHARPNESS=2 was accepted");
    SetEnv("MOJORECOMP_FSR_SHARPNESS", "nan");
    if (cfg::ApplyEnvironmentOverrides(config, error))
        return Fail("MOJORECOMP_FSR_SHARPNESS=nan was accepted");
    for (const char* name : {"MOJORECOMP_MOTION_BLUR", "MOJORECOMP_UPSCALER",
                             "MOJORECOMP_FSR_SHARPNESS", "MOJORECOMP_GUEST_PROFILE",
                             "MOJORECOMP_GPU_PROFILE"})
        SetEnv(name, nullptr);

    cfg::GuestProfileMode mode{};
    if (!cfg::ParseGuestProfileMode("all", mode) || mode != cfg::GuestProfileMode::AllThreads ||
        !cfg::ParseGuestProfileMode("0", mode) || mode != cfg::GuestProfileMode::Off)
        return Fail("guest profile modes parsed incorrectly");
    if (std::string(cfg::ToString(cfg::Upscaler::Fsr1)) != "fsr1" ||
        std::string(cfg::ToString(cfg::Upscaler::Bilinear)) != "bilinear" ||
        std::string(cfg::ToString(cfg::Upscaler::Rcas)) != "rcas")
        return Fail("upscaler names changed");

    std::filesystem::remove_all(dir, ec);
    return 0;
}

} // namespace

int main()
{
    const mojorecomp::config::RuntimeConfig defaults{};
    if (defaults.resolutionScale != 1 ||
        defaults.aspectRatio != mojorecomp::config::AspectRatio::Native16x9 ||
        defaults.vsync ||
        defaults.antiAliasing != mojorecomp::config::AntiAliasing::FxaaExtreme ||
        defaults.textureFiltering != 8)
        return Fail("balanced graphics defaults changed unexpectedly");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "mojorecomp-runtime-config-test.toml";
    {
        std::ofstream file(path, std::ios::trunc);
        file << "schema_version = 1\n"
                "[localization]\n"
                "profile = \"pt-BR\"\n"
                "xbox_language = 1\n"
                "[display]\n"
                "mode = \"fullscreen\"\n"
                "monitor = \"primary\"\n"
                "output_resolution = \"desktop\"\n"
                "resolution_scale = 3\n"
                "aspect_ratio = \"21:9\"\n"
                "vsync = true\n"
                "[graphics]\n"
                "anti_aliasing = \"fxaa_extreme\"\n"
                "texture_filtering = \"8x\"\n";
    }

    mojorecomp::config::RuntimeConfig config{};
    std::string error;
    const bool loaded = mojorecomp::config::LoadFile(path, config, error);
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    if (!loaded)
        return Fail(error.c_str());
    if (config.displayMode != mojorecomp::config::DisplayMode::Fullscreen)
        return Fail("display mode was not parsed");
    if (config.localizationProfile != "pt-BR" || config.xboxLanguage != 1)
        return Fail("localization profile was not parsed");
    if (config.resolutionScale != 3)
        return Fail("resolution scale was not parsed");
    if (config.aspectRatio != mojorecomp::config::AspectRatio::Ultrawide21x9)
        return Fail("aspect ratio was not parsed");
    if (!config.vsync)
        return Fail("vsync was not parsed");
    if (config.antiAliasing != mojorecomp::config::AntiAliasing::FxaaExtreme)
        return Fail("anti-aliasing was not parsed");
    if (config.textureFiltering != 8)
        return Fail("texture filtering was not parsed");

    mojorecomp::config::RuntimeConfig invalid{};
    if (mojorecomp::config::ParseTextureFiltering("3x", invalid.textureFiltering))
        return Fail("invalid anisotropy level was accepted");

    std::string profile;
    uint32_t language = 0;
    if (!mojorecomp::config::ParseLocalizationProfile("pt-br", profile, language) ||
        profile != "pt-BR" || language != 1)
        return Fail("PT-BR localization profile did not map to English Xbox language");
    if (mojorecomp::config::ParseLocalizationProfile("de", profile, language))
    {
        if (profile != "de" || language != 3)
            return Fail("German localization profile mapped incorrectly");
    }
    else
        return Fail("German localization profile was rejected");
    if (!mojorecomp::config::ParseLocalizationProfile("nl", profile, language) ||
        profile != "nl" || language != 16)
        return Fail("Dutch localization profile mapped incorrectly");
    if (!mojorecomp::config::ParseLocalizationProfile("fr", profile, language) ||
        profile != "fr" || language != 4)
        return Fail("French localization profile mapped incorrectly");
    if (!mojorecomp::config::ParseLocalizationProfile("es", profile, language) ||
        profile != "es" || language != 5)
        return Fail("Spanish localization profile mapped incorrectly");
    if (!mojorecomp::config::ParseLocalizationProfile("it", profile, language) ||
        profile != "it" || language != 6)
        return Fail("Italian localization profile mapped incorrectly");
    if (mojorecomp::config::ParseLocalizationProfile("ja", profile, language))
        return Fail("unsupported localization profile was accepted");

    if (const int result = TestPhase4Settings())
        return result;

    std::puts("OK: runtime config schema and public graphics settings parsed correctly.");
    return 0;
}
