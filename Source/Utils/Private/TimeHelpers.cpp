#include <iomanip>

#include "Utils/TimeHelpers.hpp"

#include "Utils/Helpers.hpp"
#include "Utils/Logger.hpp"

using namespace std::chrono;

namespace Details
{
    static const TimePoint globalStartTimePoint = high_resolution_clock::now();

    static uint64_t GetNanosecondCount(const TimePoint& start, const TimePoint& end)
    {
        return duration_cast<nanoseconds>(end - start).count();
    }

    static float GetDeltaSeconds(const TimePoint& start, const TimePoint& end)
    {
        const uint64_t nanosecondCount = GetNanosecondCount(start, end);

        return static_cast<float>(nanosecondCount) * Numbers::kNano;
    }

    static float GetDeltaMiliseconds(const TimePoint& start, const TimePoint& end)
    {
        const uint64_t nanosecondCount = GetNanosecondCount(start, end);

        return static_cast<float>(nanosecondCount) * Numbers::kNano / Numbers::kMili;
    }
}

float Timer::GetGlobalSeconds()
{
    const TimePoint now = high_resolution_clock::now();
    const uint64_t nanosecondCount = duration_cast<nanoseconds>(now - Details::globalStartTimePoint).count();
    return static_cast<float>(nanosecondCount) * Numbers::kNano;
}

float Timer::GetDeltaSeconds()
{
    float deltaSeconds = 0.0f;

    const TimePoint now = high_resolution_clock::now();

    if (lastTimePoint.has_value())
    {
        deltaSeconds = Details::GetDeltaSeconds(lastTimePoint.value(), now);
    }

    lastTimePoint = now;

    return deltaSeconds;
}
