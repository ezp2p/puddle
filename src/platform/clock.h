#ifndef PUDDLE_CLOCK_H
#define PUDDLE_CLOCK_H

/* The two clocks the Puddle reads, the same on every OS (std::chrono underneath). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UTC milliseconds since 1970: what events and records are stamped with. */
int64_t puddle_wall_ms(void);

/* Milliseconds from an arbitrary start that never goes backwards: for timeouts, whatever happens to the wall clock. */
int64_t puddle_mono_ms(void);

#ifdef __cplusplus
}
#endif
#endif
