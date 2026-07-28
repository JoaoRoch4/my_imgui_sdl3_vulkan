#include "pch.hpp" // NOLINT

#include "playback_clock.hpp"

void PlaybackClock::load(std::uint32_t seed, double duration, std::string title)
{
    m_seed     = seed;
    m_duration = std::max(duration, 0.0);
    m_title    = std::move(title);
    m_position = 0.0;
    m_paused   = false;
}

void PlaybackClock::tick(float dt)
{
    if (m_paused || m_duration <= 0.0)
        return;

    m_position += static_cast<double>(dt) * m_speed;
    if (m_position < m_duration)
        return;

    if (m_loop) {
        // fmod rather than a reset to 0 so a long frame (or a >1x speed) does not
        // silently drop the overshoot and drift the clock.
        m_position = std::fmod(m_position, m_duration);
    } else {
        m_position = m_duration;
        m_paused   = true;
    }
}

void PlaybackClock::toggle_pause()
{
    m_paused = !m_paused;
}

void PlaybackClock::seek_to(double seconds)
{
    m_position = std::clamp(seconds, 0.0, m_duration);
}

void PlaybackClock::seek_by(double seconds)
{
    seek_to(m_position + seconds);
}

float PlaybackClock::fraction() const
{
    if (m_duration <= 0.0)
        return 0.0f;
    return static_cast<float>(std::clamp(m_position / m_duration, 0.0, 1.0));
}

std::string PlaybackClock::format_time(double seconds)
{
    const int total = static_cast<int>(std::max(seconds, 0.0));
    std::array<char, 16> buf{};
    std::snprintf(buf.data(), buf.size(), "%d:%02d", total / 60, total % 60);
    return {buf.data()};
}
