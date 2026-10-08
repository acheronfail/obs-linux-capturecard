/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define FRAME_FRESH_NS UINT64_C(2000000000)
#define CAPTURE_STALL_NS UINT64_C(5000000000)
#define CAPTURE_RETRY_NS UINT64_C(10000000000)

static inline bool frame_is_fresh(uint64_t now, uint64_t received) {
  return received && now >= received && now - received < FRAME_FRESH_NS;
}

static inline bool capture_is_stalled(uint64_t now, uint64_t started,
                                      uint64_t received) {
  uint64_t anchor = received ? received : started;
  return now >= anchor && now - anchor >= CAPTURE_STALL_NS;
}

struct frame_health {
  atomic_uint_fast64_t received;
  uint64_t timestamp;
  bool seen;
};

static inline void frame_health_observe(struct frame_health *health,
                                        uint64_t timestamp, uint64_t now) {
  if (!health->seen || timestamp != health->timestamp) {
    health->seen = true;
    health->timestamp = timestamp;
    atomic_store(&health->received, now);
  }
}
