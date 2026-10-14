#include "LockScreen.hpp"
#include "BatteryDrawing.hpp"
#include "services/ClockService.hpp"
#include <nxui/core/GpuDevice.hpp>
#include <nxui/core/I18n.hpp>
#include <nxui/core/Renderer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr float kScreenW = 1280.f;
constexpr float kScreenH = 720.f;
constexpr float kMargin = 56.f;
constexpr float kCurtainTravel = 740.f;
constexpr float kInputGuard = 0.35f; // ignore the press that woke the console
constexpr int kTimeFontPt = 200;

std::string replaceAll(std::string text, const std::string& token, const std::string& value) {
    for (std::size_t pos = text.find(token); pos != std::string::npos;
         pos = text.find(token, pos + value.size()))
        text.replace(pos, token.size(), value);
    return text;
}

std::string firstGlyph(const std::string& name) {
    if (name.empty()) return "?";
    unsigned char lead = static_cast<unsigned char>(name[0]);
    std::size_t len = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
    return name.substr(0, std::min(len, name.size()));
}
}

LockScreen::LockScreen() {
    setRect({0.f, 0.f, kScreenW, kScreenH});
    setVisible(false);
    setFocusable(true);
    setLiquidGlassEnabled(false);
    setForceLiquidGlass(false);
    setLiquidGlassShaderEnabled(false);
    setBlurEnabled(false);
    setFrameworkTouchEnabled(false);
    wireActions();
}

void LockScreen::wireActions() {
    clearActions();
    // Sticks are left out on purpose: drift would unlock the console by itself.
    for (auto button : {nxui::Button::A, nxui::Button::B, nxui::Button::X, nxui::Button::Y,
                        nxui::Button::L, nxui::Button::R, nxui::Button::ZL, nxui::Button::ZR,
                        nxui::Button::Plus, nxui::Button::Minus,
                        nxui::Button::DUp, nxui::Button::DDown,
                        nxui::Button::DLeft, nxui::Button::DRight}) {
        addAction(static_cast<std::uint64_t>(button), [this]() { unlock(); });
    }
}

void LockScreen::show(const std::string& userName, const std::string& suspendedGame, bool instant) {
    auto& i18n = nxui::I18n::instance();
    m_userName = userName;
    m_suspendedGame = suspendedGame;
    m_enterText = i18n.tr("lock.enter", "Press A to enter");
    m_hintText = i18n.tr("lock.hint", "Any other button, or a tap, also works");
    m_pausedText = i18n.tr("lock.game_paused", "paused");

    // Previous artwork and fonts are dropped and the new ones created here: no
    // frame is in flight (just woke up / about to sleep), so freeing is safe.
    releaseArt();
    if (m_gpu && m_ren && !m_timeFontPath.empty()) {
        auto font = std::make_unique<nxui::Font>();
        if (font->load(*m_gpu, *m_ren, m_timeFontPath, kTimeFontPt)) {
            m_timeFont = std::move(font);
            m_timeFontLoaded = true;
        }
    }
    if (m_gpu && m_ren) {
        // The text needs GPU memory too (several small textures plus the large time
        // one), so the artwork is only uploaded when there is plenty to spare, and at
        // quarter size: it sits under a dark scrim, detail would be wasted.
        constexpr std::uint64_t kTextReserve = 6u * 1024u * 1024u;
        if (!m_pendingHero.empty() && m_pendingHero.size() == 1280u * 720u * 4u &&
            m_gpu->imageMemoryAvailable() > kTextReserve + 640u * 360u * 4u) {
            std::vector<std::uint8_t> small(640u * 360u * 4u);
            for (unsigned y = 0; y < 360u; ++y) {
                for (unsigned x = 0; x < 640u; ++x) {
                    for (unsigned c = 0; c < 4u; ++c) {
                        const std::uint8_t* src = m_pendingHero.data() + ((y * 2u) * 1280u + x * 2u) * 4u + c;
                        small[(y * 640u + x) * 4u + c] = static_cast<std::uint8_t>(
                            (src[0] + src[4] + src[1280u * 4u] + src[1280u * 4u + 4]) / 4u);
                    }
                }
            }
            m_hasHero = m_heroTex.loadFromPixels(*m_gpu, *m_ren, small.data(), 640, 360);
        }
        if (!m_pendingIcon.empty() && m_gpu->imageMemoryAvailable() > kTextReserve)
            m_hasIcon = m_iconTex.loadFromMemory(*m_gpu, *m_ren, m_pendingIcon.data(),
                                                 m_pendingIcon.size(), 160);
    }
    m_pendingHero.clear();
    m_pendingHero.shrink_to_fit();
    m_pendingIcon.clear();
    m_releaseTimer = 0.f;

    m_active = true;
    m_unlocking = false;
    m_age = 0.f;
    m_clockTimer = 0.f;
    m_lastMinute = -1;
    m_curtain.setImmediate(0.f);
    // The opaque background covers HOME at once; text and shapes fade in so the
    // glyph textures are created while still invisible instead of popping in.
    if (instant) {
        m_fade.setImmediate(1.f);
    } else {
        m_fade.setImmediate(0.f);
        m_fade.set(1.f, 0.30f, nxui::Easing::outCubic);
    }
    refreshClock();
    setVisible(true);
}

void LockScreen::releaseArt() {
    if (m_hasHero) m_heroTex = nxui::Texture();
    if (m_hasIcon) m_iconTex = nxui::Texture();
    m_hasHero = false;
    m_hasIcon = false;
    m_timeFont.reset();
    m_timeFontLoaded = false;
}

void LockScreen::tickRelease(float dt) {
    if (m_active || (!m_hasHero && !m_hasIcon && !m_timeFont))
        return;
    // A second and a half is far more than the frames the GPU keeps in flight.
    m_releaseTimer += dt;
    if (m_releaseTimer >= 1.5f)
        releaseArt();
}

void LockScreen::unlock() {
    if (!m_active || m_unlocking || m_age < kInputGuard)
        return;
    m_unlocking = true;
    if (m_holdOnUnlock) {
        // Keep the lock screen on display while the game comes back.
        if (m_onDismissed)
            m_onDismissed();
        return;
    }
    m_curtain.set(1.f, 0.62f, nxui::Easing::inOutCubic);
}

void LockScreen::handleTouch(nxui::Input& input) {
    if (m_active && input.touchDown())
        unlock();
}

void LockScreen::refreshClock() {
    if (!m_clock)
        return;
    const auto snapshot = m_clock->refresh();
    if (!snapshot.valid)
        return;
    const auto& cal = snapshot.calendar;
    if (static_cast<int>(cal.hour) * 60 + cal.minute == m_lastMinute)
        return;
    m_lastMinute = static_cast<int>(cal.hour) * 60 + cal.minute;

    char buf[32];
    if (m_use12Hour) {
        int hour = cal.hour % 12;
        if (hour == 0) hour = 12;
        std::snprintf(buf, sizeof(buf), "%d:%02d", hour, cal.minute);
    } else {
        std::snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(cal.hour),
                      static_cast<unsigned>(cal.minute));
    }
    m_timeStr = buf;

    auto& i18n = nxui::I18n::instance();
    static const char* const kWeekdayFallback[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char* const kMonthFallback[12] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
    const unsigned wday = std::min<unsigned>(snapshot.additional.wday, 6u);
    const unsigned month = std::clamp<unsigned>(cal.month, 1u, 12u);
    std::string weekday = i18n.tr("lock.weekday." + std::to_string(wday), kWeekdayFallback[wday]);
    std::string monthName = i18n.tr("lock.month." + std::to_string(month), kMonthFallback[month - 1]);
    std::string out = i18n.tr("lock.date_format", "{weekday}, {month} {day}");
    out = replaceAll(out, "{weekday}", weekday);
    out = replaceAll(out, "{month}", monthName);
    out = replaceAll(out, "{day}", std::to_string(static_cast<unsigned>(cal.day)));
    out = replaceAll(out, "{year}", std::to_string(static_cast<unsigned>(cal.year)));
    m_dateStr = out;
}

void LockScreen::update(float dt) {
    if (!m_active)
        return;
    m_age += dt;
    m_curtain.update(dt);
    m_fade.update(dt);
    m_clockTimer += dt;
    if (m_clockTimer >= 1.f) {
        m_clockTimer = 0.f;
        refreshClock();
    }
    if (m_unlocking && m_curtain.value() >= 0.999f) {
        m_active = false;
        m_unlocking = false;
        setVisible(false);
        if (m_onDismissed)
            m_onDismissed();
    }
}

void LockScreen::render(nxui::Renderer& ren) {
    if (!m_active || !m_theme || !m_font)
        return;

    const float dy = -m_curtain.value() * kCurtainTravel;
    if (dy <= -kScreenH)
        return;
    auto at = [dy](nxui::Rect r) { r.y += dy; return r; };
    auto pt = [dy](float x, float y) { return nxui::Vec2{x, y + dy}; };
    const nxui::Theme& th = *m_theme;

    // Background: the game's artwork when there is one, otherwise the theme base;
    // accent glows and drifting theme shapes on top either way.
    ren.drawRect(at({0.f, 0.f, kScreenW, kScreenH}), th.background.withAlpha(1.f));
    const float fade = std::clamp(m_fade.value(), 0.f, 1.f);
    if (m_hasHero) {
        ren.drawTexture(&m_heroTex, at({0.f, 0.f, kScreenW, kScreenH}));
        // Scrim keeps the text readable over any artwork and any theme.
        ren.drawGradientRect(at({0.f, 0.f, kScreenW, kScreenH}),
                             nxui::Color(0.f, 0.f, 0.f, 0.40f), nxui::Color(0.f, 0.f, 0.f, 0.80f));
        ren.drawRect(at({0.f, 0.f, kScreenW, kScreenH}), th.backgroundAccent.withAlpha(0.16f));
    }
    ren.drawGradientRect(at({0.f, 0.f, kScreenW, 340.f}),
                         th.backgroundAccent.withAlpha(0.60f * fade), th.backgroundAccent.withAlpha(0.f));
    ren.drawGradientRect(at({0.f, 400.f, kScreenW, 320.f}),
                         th.backgroundAccent.withAlpha(0.f), th.backgroundAccent.withAlpha(0.50f * fade));
    // Static on purpose: the screen redraws at a low rate while idle, and slow
    // motion would only look choppy and cost power.
    const float t = 0.f;
    const nxui::Color shape = th.shapeColor.withAlpha(0.17f * fade);
    ren.drawTriangle(pt(1010.f + 10.f * std::sin(t * 0.21f), 60.f + 8.f * std::cos(t * 0.17f)),
                     pt(1130.f + 10.f * std::sin(t * 0.21f), 110.f + 8.f * std::cos(t * 0.17f)),
                     pt(1040.f + 10.f * std::sin(t * 0.21f), 190.f + 8.f * std::cos(t * 0.17f)), shape);
    ren.drawTriangle(pt(160.f + 9.f * std::cos(t * 0.19f), 470.f + 7.f * std::sin(t * 0.23f)),
                     pt(260.f + 9.f * std::cos(t * 0.19f), 520.f + 7.f * std::sin(t * 0.23f)),
                     pt(170.f + 9.f * std::cos(t * 0.19f), 600.f + 7.f * std::sin(t * 0.23f)), shape);
    ren.drawCircle(pt(1170.f + 8.f * std::sin(t * 0.15f), 560.f + 6.f * std::cos(t * 0.2f)), 62.f, shape);
    ren.drawCircle(pt(560.f + 7.f * std::cos(t * 0.18f), 120.f + 6.f * std::sin(t * 0.14f)), 34.f, shape);

    const nxui::Color ink = (m_hasHero ? nxui::Color(1.f, 1.f, 1.f, 1.f) : th.textPrimary).withAlpha(fade);
    const nxui::Color inkSoft = (m_hasHero ? nxui::Color(1.f, 1.f, 1.f, 1.f) : th.textSecondary)
                                    .withAlpha((m_hasHero ? 0.88f : 0.92f) * fade);
    nxui::Font* small = m_smallFont ? m_smallFont : m_font;

    // Top left: profile initial + name.
    if (!m_userName.empty()) {
        const nxui::Vec2 c = pt(kMargin + 27.f, 70.f);
        ren.drawCircle(c, 27.f, th.cursorNormal.withAlpha(fade), 40);
        const std::string initial = firstGlyph(m_userName);
        const nxui::Vec2 isz = m_font->measure(initial);
        ren.drawText(initial, {c.x - isz.x * 0.5f, c.y - isz.y * 0.5f}, m_font, th.background.withAlpha(fade), 1.f);
        ren.drawText(m_userName, pt(kMargin + 68.f, 54.f), m_font, inkSoft, 0.9f);
    }

    // Top right: battery.
    if (m_battery >= 0) {
        const std::string pct = std::to_string(m_battery) + " %";
        const nxui::Vec2 psz = small->measure(pct);
        const float textW = psz.x * 1.0f;
        const float right = kScreenW - kMargin;
        ren.drawText(pct, pt(right - textW, 52.f), small, inkSoft, 1.f);
        const nxui::Rect body = at({right - textW - 58.f, 56.f, 44.f, 22.f});
        ren.drawRoundedRectOutline(body, inkSoft, 5.f, 2.f);
        ren.drawRect({body.right() + 2.f, body.y + 6.f, 4.f, 10.f}, inkSoft);
        const float fill = std::clamp(m_battery / 100.f, 0.f, 1.f);
        ren.drawRoundedRect({body.x + 3.f, body.y + 3.f, (body.width - 6.f) * fill, body.height - 6.f},
                            th.cursorNormal.withAlpha(fade), 3.f);
        if (m_charging)
            switchu::battery_drawing::drawLightningBolt(
                ren, {body.x + 12.f, body.y - 1.f, 20.f, 24.f}, ink, th.background.withAlpha(fade), 1.f);
    }

    // Clock: the time is the one big element, bottom left.
    if (m_timeFont) {
        const nxui::Vec2 tsz = m_timeFont->measure(m_timeStr);
        ren.drawText(m_timeStr, pt(kMargin, 268.f - (tsz.y - 200.f) * 0.5f), m_timeFont.get(), ink, 1.f);
    } else {
        ren.drawText(m_timeStr, pt(kMargin, 300.f), m_font, ink, 4.f);
    }
    ren.drawText(m_dateStr, pt(kMargin + 8.f, 494.f), m_font, inkSoft, 1.2f);

    // Bottom: unlock prompt (left) and suspended game (right).
    const float pillH = 66.f;
    const float pillY = kScreenH - 46.f - pillH;
    auto drawPill = [&](float x, const std::string& glyph, const std::string& text,
                        float textScale, bool filledGlyph) -> float {
        const float glyphD = 40.f;
        const nxui::Vec2 tsz = m_font->measure(text);
        const float w = 14.f + glyphD + 16.f + tsz.x * textScale + 30.f;
        const nxui::Rect r = at({x, pillY, w, pillH});
        ren.drawRoundedRect(r, th.panelBase.withAlpha(0.60f * fade), pillH * 0.5f);
        ren.drawRoundedRectOutline(r, th.panelBorder.withAlpha(0.45f * fade), pillH * 0.5f, 1.2f);
        const nxui::Vec2 gc{r.x + 14.f + glyphD * 0.5f, r.y + pillH * 0.5f};
        if (filledGlyph) ren.drawCircle(gc, glyphD * 0.5f, ink, 32);
        else ren.drawRoundedRectOutline({gc.x - glyphD * 0.5f, gc.y - glyphD * 0.5f, glyphD, glyphD},
                                        inkSoft, glyphD * 0.5f, 2.f);
        const nxui::Vec2 gsz = small->measure(glyph);
        ren.drawText(glyph, {gc.x - gsz.x * (filledGlyph ? 0.5f : 0.36f),
                             gc.y - gsz.y * (filledGlyph ? 0.5f : 0.36f)},
                     small, filledGlyph ? th.background.withAlpha(fade) : ink, filledGlyph ? 1.f : 0.72f);
        ren.drawText(text, {r.x + 14.f + glyphD + 16.f, r.y + (pillH - tsz.y * textScale) * 0.5f},
                     m_font, filledGlyph ? ink : inkSoft, textScale);
        return w;
    };
    drawPill(kMargin, "A", m_enterText, 1.0f, true);

    float rightEdge = kScreenW - kMargin;
    if (!m_suspendedGame.empty()) {
        std::string label = m_suspendedGame;
        const float maxW = 420.f;
        while (label.size() > 3 && m_font->measure(label + "...").x * 0.9f > maxW)
            label.pop_back();
        if (label != m_suspendedGame) label += "...";
        const std::string text = label + " " + m_pausedText;
        const float w = 14.f + 40.f + 16.f + m_font->measure(text).x * 0.9f + 30.f;
        drawPill(rightEdge - w, "HOME", text, 0.9f, false);
    }

    if (m_hasIcon && !m_suspendedGame.empty()) {
        constexpr float kIcon = 132.f;
        const nxui::Rect icon = at({rightEdge - kIcon, pillY - kIcon - 18.f, kIcon, kIcon});
        ren.drawTextureRounded(&m_iconTex, icon, 26.f, nxui::Color(1.f, 1.f, 1.f, fade));
        ren.drawRoundedRectOutline(icon, th.panelBorder.withAlpha(0.55f * fade), 26.f, 1.5f);
    }
    ren.drawText(m_hintText, pt(kMargin + 8.f, pillY - 34.f), small, inkSoft, 0.78f);

    if (m_blackout)
        ren.drawRect(at({0.f, 0.f, kScreenW, kScreenH}), nxui::Color(0.f, 0.f, 0.f, 1.f));
}
