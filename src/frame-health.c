/* SPDX-License-Identifier: AGPL-3.0-only */
#include "frame-health.h"
#include <obs-module.h>
#include <stdatomic.h>
#include <util/bmem.h>
#include <util/platform.h>

static const char *name(void *unused) {
  (void)unused;
  return "Capture frame health";
}

static void read_health(void *data, calldata_t *call) {
  struct frame_health *health = data;
  calldata_set_int(call, "received_ns", atomic_load(&health->received));
}

static void *create(obs_data_t *settings, obs_source_t *source) {
  (void)settings;
  struct frame_health *health = bzalloc(sizeof(*health));
  atomic_init(&health->received, 0);
  proc_handler_add(obs_source_get_proc_handler(source),
                   "void get_frame_health(out int received_ns)", read_health,
                   health);
  return health;
}

static struct obs_source_frame *observe(void *data,
                                        struct obs_source_frame *frame) {
  struct frame_health *health = data;
  if (frame)
    frame_health_observe(health, frame->timestamp, os_gettime_ns());
  return frame;
}

void capture_frame_health_register(void) {
  struct obs_source_info info = {
      .id = "capture_card_frame_health",
      .type = OBS_SOURCE_TYPE_FILTER,
      .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CAP_DISABLED,
      .get_name = name,
      .create = create,
      .destroy = bfree,
      .filter_video = observe,
  };
  obs_register_source(&info);
}
