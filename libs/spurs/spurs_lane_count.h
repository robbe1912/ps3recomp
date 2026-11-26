/* Optional lane count override for each SPURS instance.
 *
 * SPURS_LANES=N replaces the lane count that the game requests with N. N is a
 * decimal number from 1 to 6. The Cell design has 6 SPUs for an application.
 * Any other value, or no setting, keeps the count of the game. The game sees
 * the effective count: spurs_initialize_common writes it to the instance and
 * cellSpursGetInfo returns it. More lanes shorten a wait only when jobs are
 * ready and no lane is free.
 */
#ifndef PS3_SPURS_LANE_COUNT_H
#define PS3_SPURS_LANE_COUNT_H

#include <stdint.h>

#define SPURS_LANES_MAX 6u

/* The lane count to use. `setting` is the value of SPURS_LANES, or NULL. */
static inline uint32_t spurs_lane_count(uint32_t requested, const char *setting)
{
    if (!setting || !*setting) return requested;
    uint32_t n = 0;
    for (const char *p = setting; *p; ++p) {
        /* The guard in the loop also stops an overflow of a long number. */
        if (*p < '0' || *p > '9' || n > SPURS_LANES_MAX) return requested;
        n = n * 10u + (uint32_t)(*p - '0');
    }
    return n >= 1u && n <= SPURS_LANES_MAX ? n : requested;
}

#endif
