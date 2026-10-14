#pragma once

#include <nxui/widgets/GlassWidget.hpp>
#include <nxui/core/Animation.hpp>
#include <nxui/core/Font.hpp>
#include <nxui/core/Input.hpp>
#include <nxui/Theme.hpp>
#include <nxui/core/Texture.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace switchu::services { class ClockService; }

/// Full-screen lock screen shown when the console wakes from sleep. A (or any
/// other button, or a tap) lifts it like a curtain and reveals the HOME menu.
/// Colors come from the active theme.
class LockScreen : public nxui::GlassWidget {
public:
    LockScreen();

    void setFonts(nxui::Font* body, nxui::Font* small) { m_font = body; m_smallFont = small; }
    void setTimeFontSource(nxui::GpuDevice* gpu, nxui::Renderer* ren, std::string path) {
        m_gpu = gpu; m_ren = ren; m_timeFontPath = std::move(path);
    }
    void setTheme(const nxui::Theme* theme) { m_theme = theme; }
    void setClockService(switchu::services::ClockService* clock) { m_clock = clock; }
    void setUse12HourClock(bool enabled) { m_use12Hour = enabled; m_lastMinute = -1; }
    void setBatteryStatus(int percent, bool charging) { m_battery = percent; m_charging = charging; }
    void setOnDismissed(std::function<void()> cb) { m_onDismissed = std::move(cb); }
    /// When set, unlocking does not lift the curtain: the callback runs at once and
    /// the lock screen stays drawn (used when a game is about to come back, so
    /// HOME is never shown in between).
    void setHoldOnUnlock(bool hold) { m_holdOnUnlock = hold; }
    /// While set, everything is drawn as usual (so textures get created) and then
    /// covered with black. Used for the frames presented just before sleeping, so
    /// the panel does not flash the lock screen while it switches off.
    void setBlackout(bool blackout) { m_blackout = blackout; }

    /// `suspendedGame` is empty when no game is suspended.
    /// `instant` skips the fade-in (used when the frame is drawn right before
    /// sleeping, so the last image the display keeps is the finished lock screen).
    void show(const std::string& userName, const std::string& suspendedGame, bool instant = false);
    /// Artwork of the suspended game, shown as background (1280x720 RGBA) and as
    /// an icon (compressed image). Either may be empty. Applied on the next show().
    void setGameArt(std::vector<std::uint8_t> iconData, std::vector<std::uint8_t> heroRgba) {
        m_pendingIcon = std::move(iconData);
        m_pendingHero = std::move(heroRgba);
    }
    /// Call every frame: frees the artwork textures a while after the screen
    /// is dismissed, once no frame can still be using them.
    void tickRelease(float dt);
    /// After a wake: ignore input for a moment and refresh the clock at once.
    void rearmInput() { m_age = 0.f; }
    void refreshNow() { m_lastMinute = -1; refreshClock(); }
    void handleTouch(nxui::Input& input);
    /// Same as pressing A: lifts the curtain (used for the HOME button).
    void requestUnlock() { unlock(); }

    bool isActive() const { return m_active; }


    void update(float dt) override;
    void render(nxui::Renderer& ren) override;

private:
    void wireActions();
    void releaseArt();
    void unlock();
    void refreshClock();

    nxui::Font* m_font = nullptr;
    nxui::Font* m_smallFont = nullptr;
    std::unique_ptr<nxui::Font> m_timeFont; // only alive while the screen (or its fade-out) needs it
    nxui::GpuDevice* m_gpu = nullptr;
    nxui::Renderer* m_ren = nullptr;
    std::string m_timeFontPath;
    bool m_timeFontLoaded = false;

    const nxui::Theme* m_theme = nullptr;
    switchu::services::ClockService* m_clock = nullptr;
    bool m_use12Hour = false;

    std::string m_userName;
    std::string m_enterText;
    std::string m_hintText;
    std::string m_pausedText;
    std::string m_suspendedGame;
    std::string m_timeStr = "--:--";
    std::string m_dateStr;
    int m_lastMinute = -1;
    int m_battery = -1;
    bool m_charging = false;

    nxui::Texture m_heroTex;
    nxui::Texture m_iconTex;
    std::vector<std::uint8_t> m_pendingIcon;
    std::vector<std::uint8_t> m_pendingHero;
    bool m_hasHero = false;
    bool m_hasIcon = false;
    float m_releaseTimer = 0.f;

    bool m_active = false;
    bool m_unlocking = false;
    bool m_holdOnUnlock = false;
    bool m_blackout = false;
    float m_age = 0.f;
    float m_clockTimer = 0.f;
    nxui::AnimatedFloat m_curtain{0.f};
    nxui::AnimatedFloat m_fade{0.f};
    std::function<void()> m_onDismissed;
};
