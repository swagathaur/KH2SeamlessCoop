// Smooths a remote player's state for display: snapshots are kept with their
// arrival time and the puppet is drawn a little in the past (delay), between
// the two snapshots around that moment. Short gaps extrapolate briefly; a
// teleport (big jump or room change) snaps.
#pragma once

#include <cmath>
#include <deque>

#include "protocol.h"

namespace kh2net
{
class Interpolator
{
public:
    explicit Interpolator(double delay = 0.1) : m_delay(delay) {}

    void push(const PlayerState &s, double now)
    {
        if (!m_buf.empty() && static_cast<int32_t>(s.tick - m_buf.back().state.tick) <= 0)
            return; // out of order or duplicate
        // Arrival times jitter; space them by the sender's ticks instead, anchored
        // so the newest is never later than now.
        double t = now;
        if (!m_buf.empty())
        {
            const double byTick = m_buf.back().time + (s.tick - m_buf.back().state.tick) / 60.0;
            t = std::fabs(byTick - now) < 0.25 ? std::min(byTick, now) : now;
        }
        m_buf.push_back({s, t});
        while (m_buf.size() > 32)
            m_buf.pop_front();
    }

    bool empty() const { return m_buf.empty(); }
    double lastArrival() const { return m_buf.empty() ? -1e9 : m_buf.back().time; }

    // The state to show at `now`. False when nothing has arrived yet.
    bool sample(double now, PlayerState &out) const
    {
        if (m_buf.empty())
            return false;
        const double t = now - m_delay;
        if (m_buf.size() == 1 || t <= m_buf.front().time)
        {
            out = m_buf.front().state;
            if (m_buf.size() == 1)
                out = m_buf.back().state;
            return true;
        }
        for (size_t i = 1; i < m_buf.size(); ++i)
        {
            const Entry &a = m_buf[i - 1], &b = m_buf[i];
            if (t <= b.time)
            {
                const double span = b.time - a.time;
                const float f = span > 1e-6 ? static_cast<float>((t - a.time) / span) : 1.0f;
                out = blend(a.state, b.state, f);
                return true;
            }
        }
        // Past the newest: extrapolate up to 150 ms from the last two, then hold.
        const Entry &a = m_buf[m_buf.size() - 2], &b = m_buf.back();
        const double span = b.time - a.time;
        const double over = std::min(t - b.time, 0.15);
        const float f = span > 1e-6 ? static_cast<float>(1.0 + over / span) : 1.0f;
        out = blend(a.state, b.state, f);
        return true;
    }

private:
    struct Entry
    {
        PlayerState state;
        double time;
    };

    static float lerpAngle(float a, float b, float f)
    {
        const float twoPi = 6.28318530718f;
        float d = std::fmod(b - a, twoPi);
        if (d > twoPi / 2)
            d -= twoPi;
        if (d < -twoPi / 2)
            d += twoPi;
        return a + d * f;
    }

    static PlayerState blend(const PlayerState &a, const PlayerState &b, float f)
    {
        PlayerState s = f < 0.5f ? a : b;
        if (a.world != b.world || a.room != b.room)
            return b; // changed rooms: no in-between
        const float dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
        if (dx * dx + dy * dy + dz * dz > 500.0f * 500.0f)
            return b; // teleported
        for (int i = 0; i < 3; ++i)
            s.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * f;
        s.rotY = lerpAngle(a.rotY, b.rotY, f > 1 ? 1 : f);
        if (a.anim == b.anim)
            s.animTime = a.animTime + (b.animTime - a.animTime) * f;
        return s;
    }

    double m_delay;
    std::deque<Entry> m_buf;
};
} // namespace kh2net
