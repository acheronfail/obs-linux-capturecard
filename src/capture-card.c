/* SPDX-License-Identifier: AGPL-3.0-only */
#include "frame-health.h"
#include <fcntl.h>
#include <glob.h>
#include <linux/videodev2.h>
#include <obs-module.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/task.h>

OBS_DECLARE_MODULE()
static os_task_queue_t *lifecycle_queue;
void capture_frame_health_register(void);
void capture_toolbar_load(void);
void capture_toolbar_unload(void);
MODULE_EXPORT const char *obs_module_description(void) {
  return "Combined Linux capture card source with explicit capture-session "
         "restart";
}

struct capture {
  obs_source_t *source;
  /* Protects pointers during rendering. Never release a child under this lock.
   */
  pthread_mutex_t mutex;
  obs_source_t *video;
  obs_source_t *audio;
  obs_source_t *health_filter;
  struct stat device_identity;
  bool device_valid;
  bool health_pending;
  uint64_t session_started_ns, next_health_ns, retry_after_ns;
  uint64_t generation;
  uint64_t completed_generation;
  bool enabled;
  uint32_t width, height;
  atomic_uint_fast64_t audio_blocks;
};

struct request {
  struct capture *capture;
  obs_source_t
      *owner; /* Keeps the instance alive until both queue stages finish. */
  obs_data_t *settings;
  uint64_t generation;
  uint64_t queued_ns;
  bool recovery;
  const char *reason;
};

static obs_properties_t *audio_devices(void) {
  return obs_get_source_properties("pulse_input_capture");
}

/* Match the USB identity, including serial, rather than guessing by display
 * name. */
static char *resolve_audio(const char *video, const char *selection) {
  if (strcmp(selection, "@auto") != 0)
    return bstrdup(selection);
  const char *base = strrchr(video, '/');
  base = base ? base + 1 : video;
  const char *end = strstr(base, "-video-index");
  if (!end || strncmp(base, "usb-", 4) != 0)
    return bstrdup("");
  char prefix[512];
  snprintf(prefix, sizeof(prefix), "alsa_input.%.*s-", (int)(end - base), base);
  obs_properties_t *props = audio_devices();
  obs_property_t *list = props ? obs_properties_get(props, "device_id") : NULL;
  char *match = NULL;
  unsigned matches = 0;
  for (size_t i = 0; list && i < obs_property_list_item_count(list); i++) {
    const char *id = obs_property_list_item_string(list, i);
    if (strncmp(id, prefix, strlen(prefix)) == 0) {
      bfree(match);
      match = bstrdup(id);
      matches++;
    }
  }
  obs_properties_destroy(props);
  if (matches != 1) {
    bfree(match);
    match = bstrdup("");
    blog(LOG_WARNING, "[capture-card] No unique matching audio device; select "
                      "one explicitly");
  }
  return match;
}

static void forward_audio(void *param, obs_source_t *child,
                          const struct audio_data *audio, bool muted) {
  (void)child;
  struct capture *c = param;
  struct obs_audio_info info;
  if (muted || !obs_get_audio_info(&info))
    return;
  /* OBS capture callbacks provide resampled planar float at OBS's mix rate.
   * The private child has zero sync offset; preserve its capture timestamp. */
  struct obs_source_audio out = {
      .frames = audio->frames,
      .speakers = info.speakers,
      .format = AUDIO_FORMAT_FLOAT_PLANAR,
      .samples_per_sec = info.samples_per_sec,
      .timestamp = audio->timestamp,
  };
  for (size_t i = 0; i < MAX_AV_PLANES; i++)
    out.data[i] = audio->data[i];
  obs_source_output_audio(c->source, &out);
  atomic_fetch_add(&c->audio_blocks, 1);
}

static uint64_t last_frame_ns(struct capture *c) {
  if (!c->health_filter)
    return 0;
  calldata_t call;
  calldata_init(&call);
  proc_handler_call(obs_source_get_proc_handler(c->health_filter),
                    "get_frame_health", &call);
  uint64_t received = (uint64_t)calldata_int(&call, "received_ns");
  calldata_free(&call);
  return received;
}

static double elapsed_ms(uint64_t started) {
  return (os_gettime_ns() - started) / 1000000.0;
}

static void stop_capture(struct capture *c) {
  uint64_t started = os_gettime_ns();
  pthread_mutex_lock(&c->mutex);
  obs_source_t *video = c->video;
  obs_source_t *audio = c->audio;
  obs_source_t *health = c->health_filter;
  c->health_filter = NULL;
  c->device_valid = false;
  c->video = NULL;
  c->audio = NULL;
  pthread_mutex_unlock(&c->mutex);
  if (audio) {
    /* Removal waits for an in-flight callback before the instance can be freed.
     */
    uint64_t audio_started = os_gettime_ns();
    obs_source_remove_audio_capture_callback(audio, forward_audio, c);
    blog(LOG_INFO, "[capture-card] Audio callback detached in %.1f ms",
         elapsed_ms(audio_started));
    obs_source_release(audio);
  }
  if (video) {
    if (health)
      obs_source_filter_remove(video, health);
    obs_source_dec_showing(video);
    obs_source_release(video);
  }
  obs_source_release(health);
  atomic_store(&c->audio_blocks, 0);
  blog(LOG_INFO, "[capture-card] Backend references released in %.1f ms",
       elapsed_ms(started));
}

static void free_request(struct request *r) {
  pthread_mutex_lock(&r->capture->mutex);
  if (r->generation == r->capture->generation)
    r->capture->completed_generation = r->generation;
  pthread_mutex_unlock(&r->capture->mutex);
  obs_data_release(r->settings);
  obs_source_release(r->owner);
  bfree(r);
}

static bool current_request(struct request *r) {
  pthread_mutex_lock(&r->capture->mutex);
  bool current = r->generation == r->capture->generation;
  pthread_mutex_unlock(&r->capture->mutex);
  return current;
}

static void start_task(void *param) {
  struct request *r = param;
  struct capture *c = r->capture;
  const char *device = obs_data_get_string(r->settings, "video_device");
  if (!current_request(r) || !obs_data_get_bool(r->settings, "active") ||
      !*device) {
    free_request(r);
    return;
  }
  struct stat identity = {0};
  bool device_valid = stat(device, &identity) == 0;
  if (r->recovery && !device_valid) {
    free_request(r);
    return;
  }
  uint64_t started = os_gettime_ns();
  obs_data_t *vs = obs_get_source_defaults("v4l2_input");
  if (!vs) {
    blog(LOG_ERROR, "[capture-card] OBS linux-v4l2 backend is unavailable");
    free_request(r);
    return;
  }
  obs_data_set_string(vs, "device_id", device);
  obs_data_set_int(vs, "input", -1);
  obs_data_set_int(vs, "pixelformat",
                   obs_data_get_int(r->settings, "pixel_format"));
  int64_t width = obs_data_get_int(r->settings, "width");
  int64_t height = obs_data_get_int(r->settings, "height");
  int64_t fps = obs_data_get_int(r->settings, "fps");
  obs_data_set_int(vs, "resolution",
                   width > 0 && width <= 7680 && height > 0 && height <= 4320
                       ? (width << 32) | height
                       : -1);
  obs_data_set_int(vs, "framerate",
                   fps > 0 && fps <= 240 ? (INT64_C(1) << 32) | fps : -1);
  obs_data_set_bool(vs, "buffering",
                    obs_data_get_bool(r->settings, "buffering"));
  obs_data_set_bool(vs, "auto_reset",
                    obs_data_get_bool(r->settings, "auto_reset"));
  obs_source_t *video =
      obs_source_create_private("v4l2_input", "Capture card video backend", vs);
  obs_data_release(vs);
  obs_source_t *health = NULL;
  if (video) {
    health = obs_source_create_private("capture_card_frame_health",
                                       "Capture frame health", NULL);
    if (health)
      obs_source_filter_add(video, health);
    obs_source_inc_showing(video);
  }

  char *audio_id =
      resolve_audio(device, obs_data_get_string(r->settings, "audio_device"));
  obs_source_t *audio = NULL;
  if (*audio_id) {
    obs_data_t *as = obs_data_create();
    obs_data_set_string(as, "device_id", audio_id);
    audio = obs_source_create_private("pulse_input_capture",
                                      "Capture card audio backend", as);
    obs_data_release(as);
    if (audio) {
      /* Only the parent reaches OBS's mixer. Avoid a second audible path. */
      obs_source_set_audio_mixers(audio, 0);
      obs_source_add_audio_capture_callback(audio, forward_audio, c);
    }
  }
  blog(LOG_INFO, "[capture-card] Opened sessions: video=%s audio=%s", device,
       *audio_id ? audio_id : "none (select an audio device if needed)");
  bfree(audio_id);
  pthread_mutex_lock(&c->mutex);
  c->video = video;
  c->audio = audio;
  c->health_filter = health;
  c->device_identity = identity;
  c->device_valid = device_valid;
  c->session_started_ns = os_gettime_ns();
  pthread_mutex_unlock(&c->mutex);
  /* A newer queued stop will dispose of these sessions if settings changed
   * during backend creation. Queue order prevents overlapping opens. */
  blog(LOG_INFO, "[capture-card] Backend creation completed in %.1f ms",
       elapsed_ms(started));
  free_request(r);
}

static void destroy_barrier(void *unused) { (void)unused; }

static void stop_task(void *param) {
  struct request *r = param;
  if (!current_request(r)) {
    free_request(r);
    return;
  }
  blog(LOG_INFO, "[capture-card] Lifecycle generation=%llu reason=%s "
                 "queued_monotonic_ns=%llu waited %.1f ms",
       (unsigned long long)r->generation, r->reason,
       (unsigned long long)r->queued_ns, elapsed_ms(r->queued_ns));
  stop_capture(r->capture);
  blog(LOG_INFO, "[capture-card] Capture sessions released");
  /* Wait for backend destruction before reopening the device. Never join
   * this worker from a destructor; queued requests retain the parent. */
  uint64_t destroy_started = os_gettime_ns();
  obs_queue_task(OBS_TASK_DESTROY, destroy_barrier, NULL, true);
  blog(LOG_INFO, "[capture-card] OBS destruction barrier completed in %.1f ms",
       elapsed_ms(destroy_started));
  start_task(r);
}

enum request_kind { INITIALIZE, KEEP_ACTIVE, TOGGLE_ACTIVE, RECOVER };

static bool request_update(struct capture *c, obs_data_t *settings,
                           enum request_kind kind,
                           uint64_t expected_generation) {
  /* Version 0.1 used an editable combo, which OBS serializes as display text.
   * Recover that exact legacy label format before opening or saving it. */
  const char *device = obs_data_get_string(settings, "video_device");
  const char *legacy_path = strstr(device, " — /dev/");
  if (device[0] != '/' && legacy_path) {
    char *path = bstrdup(legacy_path + strlen(" — "));
    obs_data_set_string(settings, "video_device", path);
    bfree(path);
  }
  struct request *r = bzalloc(sizeof(*r));
  r->capture = c;
  r->queued_ns = os_gettime_ns();
  r->recovery = kind == RECOVER;
  r->reason = kind == INITIALIZE ? "initialize"
              : kind == TOGGLE_ACTIVE ? "manual toggle"
              : kind == RECOVER ? "automatic recovery"
                                : "settings update/manual restart";
  r->owner = obs_source_get_ref(c->source);
  r->settings = obs_data_create();
  /* Freeze values, including defaults, before handing them to another thread.
   */
  obs_data_t *defaults = obs_data_get_defaults(settings);
  obs_data_apply(r->settings, defaults);
  obs_data_apply(r->settings, settings);
  obs_data_release(defaults);
  pthread_mutex_lock(&c->mutex);
  if (kind == RECOVER && (c->generation != expected_generation || !c->enabled ||
                          c->generation != c->completed_generation)) {
    pthread_mutex_unlock(&c->mutex);
    obs_data_release(r->settings);
    obs_source_release(r->owner);
    bfree(r);
    return false;
  }
  bool active = kind == INITIALIZE ? obs_data_get_bool(r->settings, "active")
                : kind == TOGGLE_ACTIVE ? !c->enabled
                                        : c->enabled;
  obs_data_set_bool(r->settings, "active", active);
  obs_data_set_bool(settings, "active", active);
  c->enabled = active;
  c->retry_after_ns = os_gettime_ns() + CAPTURE_RETRY_NS;
  r->generation = ++c->generation;
  pthread_mutex_unlock(&c->mutex);
  os_task_queue_queue_task(lifecycle_queue, stop_task, r);
  return active;
}

static void health_task(void *param) {
  struct request *r = param;
  struct capture *c = r->capture;
  struct stat identity = {0};
  const char *device = obs_data_get_string(r->settings, "video_device");
  bool present = *device && stat(device, &identity) == 0;
  uint64_t now = os_gettime_ns();
  pthread_mutex_lock(&c->mutex);
  bool current = c->enabled && r->generation == c->generation &&
                 c->generation == c->completed_generation;
  bool replaced = present && (!c->device_valid ||
                              identity.st_ino != c->device_identity.st_ino ||
                              identity.st_dev != c->device_identity.st_dev ||
                              identity.st_rdev != c->device_identity.st_rdev);
  uint64_t received = last_frame_ns(c);
  bool stalled = capture_is_stalled(now, c->session_started_ns, received);
  uint64_t started = c->session_started_ns;
  struct stat previous_identity = c->device_identity;
  bool recover = current && *device &&
                 ((!present && c->video) || replaced ||
                  (present && stalled && now >= c->retry_after_ns));
  pthread_mutex_unlock(&c->mutex);
  if (recover) {
    blog(LOG_WARNING, "[capture-card] Recovery: %s (%s)", device,
         !present   ? "device disappeared"
         : replaced ? "device returned/replaced"
                    : "video frames stalled");
    blog(LOG_WARNING, "[capture-card] Recovery evidence: monotonic_ns=%llu "
                      "session_ms=%.1f frame_age_ms=%.1f audio_blocks=%llu "
                      "present=%d replaced=%d inode=%llu->%llu "
                      "rdev=%llu->%llu",
         (unsigned long long)now, (now - started) / 1000000.0,
         received && now >= received ? (now - received) / 1000000.0 : -1.0,
         (unsigned long long)atomic_load(&c->audio_blocks), present, replaced,
         (unsigned long long)previous_identity.st_ino,
         (unsigned long long)identity.st_ino,
         (unsigned long long)previous_identity.st_rdev,
         (unsigned long long)identity.st_rdev);
    request_update(c, r->settings, RECOVER, r->generation);
  }
  pthread_mutex_lock(&c->mutex);
  c->health_pending = false;
  pthread_mutex_unlock(&c->mutex);
  obs_data_release(r->settings);
  obs_source_release(r->owner);
  bfree(r);
}

static void video_tick(void *data, float seconds) {
  (void)seconds;
  struct capture *c = data;
  if (obs_source_removed(c->source))
    return;
  uint64_t now = os_gettime_ns();
  pthread_mutex_lock(&c->mutex);
  bool check = c->enabled && !c->health_pending &&
               c->generation == c->completed_generation &&
               now >= c->next_health_ns;
  if (check) {
    c->health_pending = true;
    c->next_health_ns = now + UINT64_C(1000000000);
  }
  uint64_t generation = c->generation;
  pthread_mutex_unlock(&c->mutex);
  if (!check)
    return;
  struct request *r = bzalloc(sizeof(*r));
  r->capture = c;
  r->owner = obs_source_get_ref(c->source);
  obs_data_t *settings = obs_source_get_settings(c->source);
  r->settings = obs_data_create();
  obs_data_t *defaults = obs_data_get_defaults(settings);
  obs_data_apply(r->settings, defaults);
  obs_data_apply(r->settings, settings);
  obs_data_release(defaults);
  obs_data_release(settings);
  r->generation = generation;
  os_task_queue_queue_task(lifecycle_queue, health_task, r);
}

static void check_capture_health(void *data, calldata_t *call) {
  (void)call;
  struct capture *c = data;
  pthread_mutex_lock(&c->mutex);
  c->next_health_ns = 0;
  pthread_mutex_unlock(&c->mutex);
  video_tick(c, 0);
}

static void update(void *data, obs_data_t *settings) {
  struct capture *c = data;
  /* The properties dialog owns a settings snapshot. Applying another field
   * must not undo an Activate/Deactivate button pressed since it opened. */
  request_update(c, settings, KEEP_ACTIVE, 0);
}

static void defaults(obs_data_t *settings) {
  obs_data_set_default_bool(settings, "active", true);
  obs_data_set_default_string(settings, "audio_device", "@auto");
  obs_data_set_default_int(settings, "pixel_format", V4L2_PIX_FMT_YUYV);
  obs_data_set_default_int(settings, "width", 1920);
  obs_data_set_default_int(settings, "height", 1080);
  obs_data_set_default_int(settings, "fps", 60);
  obs_data_set_default_bool(settings, "buffering", false);
  obs_data_set_default_bool(settings, "auto_reset", false);
  /* Select only a unique UGREEN capture node; don't default to a webcam. */
  glob_t paths = {0};
  if (glob("/dev/v4l/by-id/*UGREEN*-video-index0", 0, NULL, &paths) == 0 &&
      paths.gl_pathc == 1)
    obs_data_set_default_string(settings, "video_device", paths.gl_pathv[0]);
  globfree(&paths);
}

static const char *get_name(void *unused) {
  (void)unused;
  return "Linux Capture Card (Audio + Video)";
}

/* Read-only diagnostics, also usable through OBS scripting/procedure calls. */
static void get_state(void *data, calldata_t *call) {
  struct capture *c = data;
  pthread_mutex_lock(&c->mutex);
  calldata_set_bool(call, "checking_health", c->health_pending);
  calldata_set_bool(call, "active", c->enabled);
  calldata_set_bool(call, "pending", c->generation != c->completed_generation);
  calldata_set_bool(call, "video", c->video != NULL);
  calldata_set_bool(call, "audio", c->audio != NULL);
  calldata_set_bool(call, "video_frames",
                    frame_is_fresh(os_gettime_ns(), last_frame_ns(c)));
  calldata_set_int(call, "audio_blocks",
                   (int64_t)atomic_load(&c->audio_blocks));
  pthread_mutex_unlock(&c->mutex);
}

static void toggle_capture(void *data, calldata_t *call) {
  (void)call;
  struct capture *c = data;
  obs_data_t *settings = obs_source_get_settings(c->source);
  request_update(c, settings, TOGGLE_ACTIVE, 0);
  obs_data_release(settings);
  obs_source_update_properties(c->source);
}

static void *create(obs_data_t *settings, obs_source_t *source) {
  struct capture *c = bzalloc(sizeof(*c));
  c->source = source;
  c->width = 1920;
  c->height = 1080;
  atomic_init(&c->audio_blocks, 0);
  pthread_mutex_init(&c->mutex, NULL);
  /* Persist the initially selected stable ID even if the card is absent
   * next time OBS starts and hardware-dependent defaults cannot find it. */
  char *device = bstrdup(obs_data_get_string(settings, "video_device"));
  obs_data_set_string(settings, "video_device", device);
  bfree(device);
  proc_handler_add(
      obs_source_get_proc_handler(source),
      "void get_capture_state(out bool active, out bool pending, out bool "
      "video, out bool audio, out bool video_frames, out int audio_blocks, "
      "out bool checking_health)",
      get_state, c);
  proc_handler_add(obs_source_get_proc_handler(source), "void toggle_capture()",
                   toggle_capture, c);
  proc_handler_add(obs_source_get_proc_handler(source),
                   "void check_capture_health()", check_capture_health, c);
  request_update(c, settings, INITIALIZE, 0);
  return c;
}

static void destroy(void *data) {
  struct capture *c = data;
  stop_capture(c);
  pthread_mutex_destroy(&c->mutex);
  bfree(c);
}

static void render(void *data, gs_effect_t *effect) {
  (void)effect;
  struct capture *c = data;
  pthread_mutex_lock(&c->mutex);
  if (c->video && c->enabled)
    obs_source_video_render(c->video);
  pthread_mutex_unlock(&c->mutex);
}

static uint32_t get_width(void *data) {
  struct capture *c = data;
  pthread_mutex_lock(&c->mutex);
  uint32_t value = c->video ? obs_source_get_width(c->video) : 0;
  if (value)
    c->width = value;
  value = c->width;
  pthread_mutex_unlock(&c->mutex);
  return value;
}

static uint32_t get_height(void *data) {
  struct capture *c = data;
  pthread_mutex_lock(&c->mutex);
  uint32_t value = c->video ? obs_source_get_height(c->video) : 0;
  if (value)
    c->height = value;
  value = c->height;
  pthread_mutex_unlock(&c->mutex);
  return value;
}

static bool toggle_clicked(obs_properties_t *props, obs_property_t *prop,
                           void *data) {
  (void)props;
  struct capture *c = data;
  obs_data_t *settings = obs_source_get_settings(c->source);
  bool active = request_update(c, settings, TOGGLE_ACTIVE, 0);
  obs_data_release(settings);
  obs_property_set_description(prop, active ? "Deactivate" : "Activate");
  return true;
}

static bool restart_clicked(obs_properties_t *props, obs_property_t *prop,
                            void *data) {
  (void)props;
  (void)prop;
  struct capture *c = data;
  obs_data_t *settings = obs_source_get_settings(c->source);
  request_update(c, settings, KEEP_ACTIVE,
                 0); /* A manually deactivated source stays off. */
  obs_data_release(settings);
  return true;
}

static bool refresh_clicked(obs_properties_t *props, obs_property_t *prop,
                            void *data) {
  (void)props;
  (void)prop;
  obs_source_update_properties(((struct capture *)data)->source);
  return true;
}

static obs_properties_t *properties(void *data) {
  struct capture *c = data;
  obs_properties_t *props = obs_properties_create();
  obs_property_t *video = obs_properties_add_list(
      props, "video_device", "Video device (stable path)", OBS_COMBO_TYPE_LIST,
      OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(video, "Select a capture device", "");
  glob_t paths = {0};
  glob("/dev/v4l/by-id/*", 0, NULL, &paths);
  for (size_t i = 0; i < paths.gl_pathc; i++) {
    int fd = open(paths.gl_pathv[i], O_RDONLY | O_NONBLOCK);
    struct v4l2_capability cap = {0};
    if (fd >= 0 && ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
      uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
                          ? cap.device_caps
                          : cap.capabilities;
      if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        char label[768];
        snprintf(label, sizeof(label), "%s — %s", cap.card, paths.gl_pathv[i]);
        obs_property_list_add_string(video, label, paths.gl_pathv[i]);
      }
    }
    if (fd >= 0)
      close(fd);
  }
  globfree(&paths);
  /* Keep an unplugged saved device selectable instead of switching cameras. */
  if (c) {
    obs_data_t *settings = obs_source_get_settings(c->source);
    const char *saved = obs_data_get_string(settings, "video_device");
    bool found = false;
    for (size_t i = 0; i < obs_property_list_item_count(video); i++)
      found |= strcmp(saved, obs_property_list_item_string(video, i)) == 0;
    if (*saved && !found)
      obs_property_list_add_string(video, saved, saved);
    obs_data_release(settings);
  }
  obs_property_t *audio =
      obs_properties_add_list(props, "audio_device", "Audio device",
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(audio, "Automatic — match USB identity",
                               "@auto");
  obs_property_list_add_string(audio, "No audio", "");
  obs_properties_t *native = audio_devices();
  obs_property_t *list =
      native ? obs_properties_get(native, "device_id") : NULL;
  for (size_t i = 0; list && i < obs_property_list_item_count(list); i++)
    obs_property_list_add_string(audio, obs_property_list_item_name(list, i),
                                 obs_property_list_item_string(list, i));
  obs_properties_destroy(native);
  obs_property_t *format =
      obs_properties_add_list(props, "pixel_format", "Pixel format",
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
  obs_property_list_add_int(format, "Device default", -1);
  obs_property_list_add_int(format, "YUYV (uncompressed)", V4L2_PIX_FMT_YUYV);
  obs_property_list_add_int(format, "MJPEG", V4L2_PIX_FMT_MJPEG);
  obs_property_list_add_int(format, "NV12", V4L2_PIX_FMT_NV12);
  obs_property_list_add_int(format, "UYVY (uncompressed)", V4L2_PIX_FMT_UYVY);
  obs_properties_add_int(props, "width", "Width (0 = device default)", 0, 7680,
                         1);
  obs_properties_add_int(props, "height", "Height (0 = device default)", 0,
                         4320, 1);
  obs_properties_add_int(props, "fps", "Frames per second (0 = device default)",
                         0, 240, 1);
  obs_properties_add_bool(props, "buffering", "Buffer video");
  obs_properties_add_bool(props, "auto_reset",
                          "V4L2 autoreset on frame timeout");
  if (c) {
    pthread_mutex_lock(&c->mutex);
    bool enabled = c->enabled;
    bool frames = frame_is_fresh(os_gettime_ns(), last_frame_ns(c));
    bool audio_open = c->audio != NULL;
    pthread_mutex_unlock(&c->mutex);
    char status[256];
    snprintf(status, sizeof(status), "%s | Video: %s | Audio: %s",
             enabled ? "Activated" : "Deactivated",
             frames ? "receiving frames" : "no recent frames",
             atomic_load(&c->audio_blocks) ? "samples received"
             : audio_open                  ? "waiting for samples"
                                           : "not opened");
    obs_properties_add_text(props, "status", status, OBS_TEXT_INFO);
    obs_properties_add_button2(props, "toggle",
                               enabled ? "Deactivate" : "Activate",
                               toggle_clicked, c);
    obs_properties_add_button2(props, "restart", "Restart capture",
                               restart_clicked, c);
    obs_properties_add_button2(props, "refresh", "Refresh devices and status",
                               refresh_clicked, c);
  }
  return props;
}

bool obs_module_load(void) {
  lifecycle_queue = os_task_queue_create();
  if (!lifecycle_queue)
    return false;
  struct obs_source_info info = {
      .id = "linux_capture_card",
      .type = OBS_SOURCE_TYPE_INPUT,
      .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO |
                      OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE,
      .get_name = get_name,
      .create = create,
      .destroy = destroy,
      .update = update,
      .get_defaults = defaults,
      .get_properties = properties,
      .get_width = get_width,
      .get_height = get_height,
      .video_render = render,
      .video_tick = video_tick,
      .icon_type = OBS_ICON_TYPE_CAMERA,
  };
  capture_frame_health_register();
  obs_register_source(&info);
  blog(LOG_INFO, "[capture-card] Loaded version 0.3.0");
  return true;
}

void obs_module_post_load(void) { capture_toolbar_load(); }

void obs_module_unload(void) {
  capture_toolbar_unload();
  os_task_queue_destroy(lifecycle_queue);
}
