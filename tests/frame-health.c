/* SPDX-License-Identifier: AGPL-3.0-only */
#include "../src/frame-health.h"
#include <assert.h>

int main(void) {
  struct frame_health health = {0};
  atomic_init(&health.received, 0);
  uint64_t started = UINT64_C(1000000000);
  assert(!frame_is_fresh(started, 0));
  assert(!capture_is_stalled(started + CAPTURE_STALL_NS - 1, started, 0));
  assert(capture_is_stalled(started + CAPTURE_STALL_NS, started, 0));
  frame_health_observe(&health, 0, started);
  assert(frame_is_fresh(started, atomic_load(&health.received)));
  frame_health_observe(&health, 0, started + CAPTURE_STALL_NS);
  assert(
      !frame_is_fresh(started + FRAME_FRESH_NS, atomic_load(&health.received)));
  assert(capture_is_stalled(started + CAPTURE_STALL_NS, started,
                            atomic_load(&health.received)));
  frame_health_observe(&health, 1, started + CAPTURE_STALL_NS);
  assert(frame_is_fresh(started + CAPTURE_STALL_NS,
                        atomic_load(&health.received)));
  return 0;
}
