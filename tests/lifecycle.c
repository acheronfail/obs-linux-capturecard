/* Integration test using real libobs and deterministic fake capture backends.
 * No camera, microphone, display server, or HDMI source is needed. */
#include <assert.h>
#include <obs.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <util/platform.h>

static atomic_int video_live, audio_live, video_opens, audio_opens, forwarded;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static obs_source_t *audio_child;

static const char *name(void *unused) {
  (void)unused;
  return "Test backend";
}

static void *video_create(obs_data_t *settings, obs_source_t *source) {
  /* A restart must never open a second capture before the first closes. */
  assert(atomic_fetch_add(&video_live, 1) == 0);
  assert(
      strcmp(
          obs_data_get_string(settings, "device_id"),
          "/dev/v4l/by-id/usb-MACROSILICON_UGREEN_25773_TEST-video-index0") ==
      0);
  assert(obs_data_get_int(settings, "resolution") ==
         ((INT64_C(1920) << 32) | 1080));
  assert(obs_data_get_int(settings, "framerate") == ((INT64_C(1) << 32) | 60));
  atomic_fetch_add(&video_opens, 1);
  return source;
}

static void video_destroy(void *data) {
  (void)data;
  os_sleep_ms(5); /* Make premature reopen reproducible. */
  assert(atomic_fetch_sub(&video_live, 1) == 1);
}

static uint32_t width(void *data) {
  (void)data;
  return 1280;
}
static uint32_t height(void *data) {
  (void)data;
  return 720;
}
static void render(void *data, gs_effect_t *effect) {
  (void)data;
  (void)effect;
}
static void video_defaults(obs_data_t *settings) {
  obs_data_set_default_int(settings, "pixelformat", -1);
}

static void *audio_create(obs_data_t *settings, obs_source_t *source) {
  assert(atomic_fetch_add(&audio_live, 1) == 0);
  assert(
      strcmp(
          obs_data_get_string(settings, "device_id"),
          "alsa_input.usb-MACROSILICON_UGREEN_25773_TEST-02.analog-stereo") ==
      0);
  pthread_mutex_lock(&audio_mutex);
  audio_child = source;
  pthread_mutex_unlock(&audio_mutex);
  atomic_fetch_add(&audio_opens, 1);
  return source;
}

static void audio_destroy(void *data) {
  (void)data;
  pthread_mutex_lock(&audio_mutex);
  audio_child = NULL;
  pthread_mutex_unlock(&audio_mutex);
  assert(atomic_fetch_sub(&audio_live, 1) == 1);
}

static obs_properties_t *audio_properties(void *data) {
  (void)data;
  obs_properties_t *props = obs_properties_create();
  obs_property_t *list =
      obs_properties_add_list(props, "device_id", "Device", OBS_COMBO_TYPE_LIST,
                              OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(list, "Built-in microphone",
                               "alsa_input.internal");
  obs_property_list_add_string(
      list, "UGREEN",
      "alsa_input.usb-MACROSILICON_UGREEN_25773_TEST-02.analog-stereo");
  return props;
}

static void received(void *param, obs_source_t *source,
                     const struct audio_data *audio, bool muted) {
  (void)param;
  (void)source;
  assert(!muted);
  assert(audio->frames == 480);
  assert(((float *)audio->data[0])[0] == 0.25f);
  assert(((float *)audio->data[1])[0] == -0.25f);
  atomic_fetch_add(&forwarded, 1);
}

static void wait_ready(obs_source_t *source, bool enabled) {
  for (int i = 0; i < 1000; i++) {
    calldata_t call;
    calldata_init(&call);
    assert(proc_handler_call(obs_source_get_proc_handler(source),
                             "get_capture_state", &call));
    bool pending = calldata_bool(&call, "pending");
    if (!pending) {
      assert(calldata_bool(&call, "active") == enabled);
      assert(calldata_bool(&call, "video") == enabled);
      assert(calldata_bool(&call, "audio") == enabled);
    }
    calldata_free(&call);
    if (!pending)
      return;
    os_sleep_ms(5);
  }
  assert(!"Capture lifecycle timed out");
}

static void click(obs_source_t *source, const char *button) {
  obs_properties_t *props = obs_source_properties(source);
  assert(props);
  assert(
      obs_property_button_clicked(obs_properties_get(props, button), source));
  obs_properties_destroy(props);
}

static void barrier(void *unused) { (void)unused; }

int main(int argc, char **argv) {
  assert(argc == 2);
  assert(obs_startup("en-US", NULL, NULL));
  struct obs_audio_info ai = {.samples_per_sec = 48000,
                              .speakers = SPEAKERS_STEREO};
  assert(obs_reset_audio(&ai));
  struct obs_source_info video = {
      .id = "v4l2_input",
      .type = OBS_SOURCE_TYPE_INPUT,
      .output_flags = OBS_SOURCE_VIDEO,
      .get_name = name,
      .create = video_create,
      .destroy = video_destroy,
      .get_defaults = video_defaults,
      .get_width = width,
      .get_height = height,
      .video_render = render,
  };
  struct obs_source_info audio = {
      .id = "pulse_input_capture",
      .type = OBS_SOURCE_TYPE_INPUT,
      .output_flags = OBS_SOURCE_AUDIO,
      .get_name = name,
      .create = audio_create,
      .destroy = audio_destroy,
      .get_properties = audio_properties,
  };
  obs_register_source(&video);
  obs_register_source(&audio);
  obs_module_t *module;
  assert(obs_open_module(&module, argv[1], ".") == MODULE_SUCCESS);
  assert(obs_init_module(module));
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(
      settings, "video_device",
      "UGREEN 25773 — "
      "/dev/v4l/by-id/usb-MACROSILICON_UGREEN_25773_TEST-video-index0");
  obs_data_set_int(settings, "width", 1920);
  obs_data_set_int(settings, "height", 1080);
  obs_data_set_int(settings, "fps", 60);
  obs_source_t *source =
      obs_source_create("linux_capture_card", "Test capture", settings, NULL);
  assert(source);
  wait_ready(source, true);
  obs_data_t *repaired = obs_source_get_settings(source);
  assert(obs_data_get_string(repaired, "video_device")[0] == '/');
  obs_data_release(repaired);
  obs_properties_t *device_props = obs_source_properties(source);
  assert(obs_property_list_type(obs_properties_get(
             device_props, "video_device")) == OBS_COMBO_TYPE_LIST);
  obs_properties_destroy(device_props);
  assert(obs_source_get_width(source) == 1280);
  assert(obs_source_get_height(source) == 720);
  obs_source_add_audio_capture_callback(source, received, NULL);
  float left[480], right[480];
  for (int i = 0; i < 480; i++) {
    left[i] = 0.25f;
    right[i] = -0.25f;
  }
  struct obs_source_audio packet = {
      .data = {(const uint8_t *)left, (const uint8_t *)right},
      .frames = 480,
      .speakers = SPEAKERS_STEREO,
      .format = AUDIO_FORMAT_FLOAT_PLANAR,
      .samples_per_sec = 48000,
      .timestamp = os_gettime_ns(),
  };
  pthread_mutex_lock(&audio_mutex);
  assert(obs_source_get_audio_mixers(audio_child) == 0);
  obs_source_output_audio(audio_child, &packet);
  pthread_mutex_unlock(&audio_mutex);
  assert(atomic_load(&forwarded) == 1);
  click(source, "toggle");
  wait_ready(source, false);
  assert(video_live == 0 && audio_live == 0);
  assert(obs_source_get_width(source) ==
         1280); /* Don't collapse scene transforms. */
  int opened = video_opens;
  click(source, "restart");
  wait_ready(source, false);
  assert(video_opens == opened); /* Restart respects manual deactivation. */
  click(source, "toggle");
  wait_ready(source, true);
  for (int i = 0; i < 10; i++) {
    click(source, "restart");
    wait_ready(source, true);
  }
  for (int i = 0; i < 30; i++)
    click(source, "restart");
  wait_ready(source, true); /* Superseded requests must not overlap capture. */
  assert(video_opens == audio_opens);
  obs_source_remove_audio_capture_callback(source, received, NULL);
  obs_source_release(source);
  /* Parent destruction queues child destruction; two barriers drain both. */
  obs_queue_task(OBS_TASK_DESTROY, barrier, NULL, true);
  obs_queue_task(OBS_TASK_DESTROY, barrier, NULL, true);
  assert(video_live == 0 && audio_live == 0);

  /* Saved deactivation must survive source recreation. */
  obs_data_set_bool(settings, "active", false);
  source =
      obs_source_create("linux_capture_card", "Initially off", settings, NULL);
  wait_ready(source, false);
  assert(video_live == 0 && audio_live == 0);
  obs_source_release(source);
  obs_queue_task(OBS_TASK_DESTROY, barrier, NULL, true);

  /* Removing a source while lifecycle work is queued must not free callback
   * state early or leak the child device handles. */
  obs_data_set_bool(settings, "active", true);
  source = obs_source_create("linux_capture_card", "Removed during restart",
                             settings, NULL);
  obs_weak_source_t *weak = obs_source_get_weak_source(source);
  for (int i = 0; i < 20; i++)
    click(source, "restart");
  obs_source_release(source);
  bool gone = false;
  for (int i = 0; i < 1000; i++) {
    obs_source_t *ref = obs_weak_source_get_source(weak);
    if (!ref) {
      gone = true;
      break;
    }
    obs_source_release(ref);
    os_sleep_ms(5);
  }
  assert(gone);
  obs_weak_source_release(weak);
  obs_queue_task(OBS_TASK_DESTROY, barrier, NULL, true);
  obs_queue_task(OBS_TASK_DESTROY, barrier, NULL, true);
  assert(video_live == 0 && audio_live == 0);
  obs_data_release(settings);
  obs_shutdown();
  puts("PASS: module load, USB audio matching, sample forwarding, "
       "deactivate/activate, restart ordering, burst requests, cleanup");
  return 0;
}
