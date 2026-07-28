#pragma once

#include "pch.hpp"

/// Transport state for the concept: position, duration, pause, speed, volume, loop.
///
/// Stands in for the observed mpv properties the real player reads out of
/// MpvAsyncState. The interface is deliberately the same shape — the UI asks this
/// object for values and tells it about intent, it never reaches into a decoder — so
/// swapping in the real async state is a change of type, not of call sites.
class PlaybackClock {
public:
    /// Advances the position by @p dt scaled by the current speed. No-op while paused.
    /// Wraps to 0 at the end when looping, otherwise stops and pauses at the end.
    void tick(float dt);

    void load(std::uint32_t seed, double duration, std::string title);

    void toggle_pause();
    void set_paused(bool paused) { m_paused = paused; }
    void seek_to(double seconds);
    void seek_by(double seconds);

    void set_speed(double speed) { m_speed = std::clamp(speed, 0.25, 4.0); }
    void set_volume(int volume) { m_volume = std::clamp(volume, 0, 150); }
    void set_loop(bool loop) { m_loop = loop; }
    void toggle_loop() { m_loop = !m_loop; }

    [[nodiscard]] double position() const { return m_position; }
    [[nodiscard]] double duration() const { return m_duration; }
    [[nodiscard]] double speed() const { return m_speed; }
    [[nodiscard]] bool   paused() const { return m_paused; }
    [[nodiscard]] bool   loop() const { return m_loop; }
    [[nodiscard]] int    volume() const { return m_volume; }
    [[nodiscard]] std::uint32_t seed() const { return m_seed; }
    [[nodiscard]] const std::string &title() const { return m_title; }

    /// 0..1 progress; 0 when the duration is unknown.
    [[nodiscard]] float fraction() const;

    /// "m:ss" for @p seconds. Static because the scene strip and the folder shelf
    /// format timestamps that are not this clock's position.
    [[nodiscard]] static std::string format_time(double seconds);

private:
    std::uint32_t m_seed     = 0;
    std::string   m_title;
    double        m_position = 0.0;
    double        m_duration = 0.0;
    double        m_speed    = 1.0;
    bool          m_paused   = false;
    bool          m_loop     = true;
    int           m_volume   = 85;
};
