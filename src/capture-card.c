/* SPDX-License-Identifier: AGPL-3.0-only */
#include <fcntl.h>
#include <glob.h>
#include <linux/videodev2.h>
#include <obs-module.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <util/bmem.h>
#include <util/task.h>

OBS_DECLARE_MODULE()
static os_task_queue_t *lifecycle_queue;
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

static void stop_capture(struct capture *c) {
  pthread_mutex_lock(&c->mutex);
  obs_source_t *video = c->video;
  obs_source_t *audio = c->audio;
  c->video = NULL;
  c->audio = NULL;
  pthread_mutex_unlock(&c->mutex);
  if (audio) {
    /* Removal waits for an in-flight callback before the instance can be freed.
     */
    obs_source_remove_audio_capture_callback(audio, forward_audio, c);
    obs_source_release(audio);
  }
  if (video) {
    obs_source_dec_showing(video);
    obs_source_release(video);
  }
  atomic_store(&c->audio_blocks, 0);
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
  if (video)
    obs_source_inc_showing(video);

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
  pthread_mutex_unlock(&c->mutex);
  /* A newer queued stop will dispose of these sessions if settings changed
   * during backend creation. Queue order prevents overlapping opens. */
  free_request(r);
}

static void destroy_barrier(void *unused) { (void)unused; }

static void stop_task(void *param) {
  struct request *r = param;
  if (!current_request(r)) {
    free_request(r);
    return;
  }
  stop_capture(r->capture);
  blog(LOG_INFO, "[capture-card] Capture sessions released");
  /* Our worker is separate from OBS's video and destruction threads.
   * Release queues child destruction; this FIFO barrier ensures V4L2 and
   * PulseAudio have closed before reopening. Never join this worker from
   * a source destructor: queued requests hold their own parent reference. */
  obs_queue_task(OBS_TASK_DESTROY, destroy_barrier, NULL, true);
  start_task(r);
}

enum request_kind { INITIALIZE, KEEP_ACTIVE, TOGGLE_ACTIVE };

static bool request_update(struct capture *c, obs_data_t *settings,
                           enum request_kind kind) {
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
  r->owner = obs_source_get_ref(c->source);
  r->settings = obs_data_create();
  /* Freeze values, including defaults, before handing them to another thread.
   */
  obs_data_t *defaults = obs_data_get_defaults(settings);
  obs_data_apply(r->settings, defaults);
  obs_data_apply(r->settings, settings);
  obs_data_release(defaults);
  pthread_mutex_lock(&c->mutex);
  bool active = kind == INITIALIZE ? obs_data_get_bool(r->settings, "active")
                : kind == TOGGLE_ACTIVE ? !c->enabled
                                        : c->enabled;
  obs_data_set_bool(r->settings, "active", active);
  obs_data_set_bool(settings, "active", active);
  c->enabled = active;
  r->generation = ++c->generation;
  pthread_mutex_unlock(&c->mutex);
  os_task_queue_queue_task(lifecycle_queue, stop_task, r);
  return active;
}

static void update(void *data, obs_data_t *settings) {
  struct capture *c = data;
  /* The properties dialog owns a settings snapshot. Applying another field
   * must not undo an Activate/Deactivate button pressed since it opened. */
  request_update(c, settings, KEEP_ACTIVE);
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
  calldata_set_bool(call, "active", c->enabled);
  calldata_set_bool(call, "pending", c->generation != c->completed_generation);
  calldata_set_bool(call, "video", c->video != NULL);
  calldata_set_bool(call, "audio", c->audio != NULL);
  calldata_set_bool(call, "video_frames",
                    c->video && obs_source_get_width(c->video) > 0);
  calldata_set_int(call, "audio_blocks",
                   (int64_t)atomic_load(&c->audio_blocks));
  pthread_mutex_unlock(&c->mutex);
}

static void toggle_capture(void *data, calldata_t *call) {
  (void)call;
  struct capture *c = data;
  obs_data_t *settings = obs_source_get_settings(c->source);
  request_update(c, settings, TOGGLE_ACTIVE);
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
      "video, out bool audio, out bool video_frames, out int audio_blocks)",
      get_state, c);
  proc_handler_add(obs_source_get_proc_handler(source), "void toggle_capture()",
                   toggle_capture, c);
  request_update(c, settings, INITIALIZE);
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
  bool active = request_update(c, settings, TOGGLE_ACTIVE);
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
  request_update(c, settings,
                 KEEP_ACTIVE); /* A manually deactivated source stays off. */
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
    bool frames = c->video && obs_source_get_width(c->video) > 0;
    bool audio_open = c->audio != NULL;
    pthread_mutex_unlock(&c->mutex);
    char status[256];
    snprintf(status, sizeof(status), "%s | Video: %s | Audio: %s",
             enabled ? "Activated" : "Deactivated",
             frames ? "frames received" : "no frames yet",
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
      .icon_type = OBS_ICON_TYPE_CAMERA,
  };
  obs_register_source(&info);
  blog(LOG_INFO, "[capture-card] Loaded version 0.2.0");
  return true;
}

void obs_module_post_load(void) { capture_toolbar_load(); }

void obs_module_unload(void) {
  capture_toolbar_unload();
  os_task_queue_destroy(lifecycle_queue);
}
