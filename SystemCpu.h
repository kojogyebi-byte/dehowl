#pragma once

#include <juce_core/juce_core.h>

#if JUCE_MAC
 #include <mach/mach.h>
#elif JUCE_LINUX
 #include <cstdio>
#endif

//==============================================================================
// Whole-computer CPU usage in percent (all cores combined).
// Call getPercent() periodically (e.g. a few times per second); it measures
// the load between consecutive calls.
class SystemCpu
{
public:
    double getPercent()
    {
       #if JUCE_MAC
        host_cpu_load_info_data_t info;
        mach_msg_type_number_t    count = HOST_CPU_LOAD_INFO_COUNT;

        if (host_statistics (mach_host_self(), HOST_CPU_LOAD_INFO,
                             (host_info_t) &info, &count) != KERN_SUCCESS)
            return lastPct;

        std::uint64_t idle = info.cpu_ticks[CPU_STATE_IDLE];
        std::uint64_t total = 0;
        for (int i = 0; i < CPU_STATE_MAX; ++i)
            total += info.cpu_ticks[i];

        const auto dTotal = (double) (total - prevTotal);
        const auto dIdle  = (double) (idle  - prevIdle);
        if (prevTotal != 0 && dTotal > 0.0)
            lastPct = juce::jlimit (0.0, 100.0, 100.0 * (1.0 - dIdle / dTotal));

        prevTotal = total;
        prevIdle  = idle;
        return lastPct;

       #elif JUCE_LINUX
        if (FILE* f = std::fopen ("/proc/stat", "r"))
        {
            unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, irq = 0, sirq = 0;
            const int got = std::fscanf (f, "cpu %llu %llu %llu %llu %llu %llu %llu",
                                         &u, &n, &s, &i, &w, &irq, &sirq);
            std::fclose (f);

            if (got >= 4)
            {
                const std::uint64_t idle  = i + w;
                const std::uint64_t total = u + n + s + i + w + irq + sirq;
                const auto dTotal = (double) (total - prevTotal);
                const auto dIdle  = (double) (idle  - prevIdle);
                if (prevTotal != 0 && dTotal > 0.0)
                    lastPct = juce::jlimit (0.0, 100.0, 100.0 * (1.0 - dIdle / dTotal));
                prevTotal = total;
                prevIdle  = idle;
            }
        }
        return lastPct;

       #else
        return 0.0;
       #endif
    }

private:
    std::uint64_t prevTotal = 0, prevIdle = 0;
    double lastPct = 0.0;
};
