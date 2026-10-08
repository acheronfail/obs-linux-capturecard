/* Opt-in smoke test: opens the selected physical capture card for a few
 * seconds. Uses a separate libobs instance, never modifies the user's OBS
 * configuration. */
#include <X11/Xlib.h>
#include <obs-nix-platform.h>
#include <obs.h>
#include <stdio.h>
#include <stdlib.h>
#include <util/platform.h>

static bool load(const char *path, const char *data) {
  obs_module_t *module = NULL;
  return obs_open_module(&module, path, data) == MODULE_SUCCESS &&
         obs_init_module(module);
}

static bool wait_state(obs_source_t *source, bool active) {
  for (int i = 0; i < 200; i++) {
    calldata_t call;
    calldata_init(&call);
    bool ok = proc_handler_call(obs_source_get_proc_handler(source),
                                "get_capture_state", &call);
    bool ready = ok && !calldata_bool(&call, "pending") &&
                 calldata_bool(&call, "active") == active;
    if (active)
      ready = ready && calldata_bool(&call, "video_frames") &&
              calldata_int(&call, "audio_blocks") > 0;
    else
      ready = ready && !calldata_bool(&call, "video") &&
              !calldata_bool(&call, "audio");
    calldata_free(&call);
    if (ready)
      return true;
    os_sleep_ms(50);
  }
  return false;
}

static void click(obs_source_t *source, const char *button) {
  obs_properties_t *props = obs_source_properties(source);
  obs_property_button_clicked(obs_properties_get(props, button), source);
  obs_properties_destroy(props);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s PLUGIN.so /dev/v4l/by-id/...-video-index0\n",
            argv[0]);
    return 2;
  }
  XInitThreads();
  Display *display = XOpenDisplay(NULL);
  if (!display) {
    fputs("An X11/XWayland display is required for the render smoke test.\n",
          stderr);
    return 2;
  }
  obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
  obs_set_nix_platform_display(display);
  if (!obs_startup("en-US", NULL, NULL))
    return 2;
  struct obs_audio_info ai = {.samples_per_sec = 48000,
                              .speakers = SPEAKERS_STEREO};
  struct obs_video_info vi = {
      .graphics_module = "libobs-opengl.so",
      .fps_num = 60,
      .fps_den = 1,
      .base_width = 1920,
      .base_height = 1080,
      .output_width = 1280,
      .output_height = 720,
      .output_format = VIDEO_FORMAT_NV12,
      .colorspace = VIDEO_CS_709,
      .range = VIDEO_RANGE_PARTIAL,
      .gpu_conversion = true,
      .scale_type = OBS_SCALE_BILINEAR,
  };
  if (!obs_reset_audio(&ai) || obs_reset_video(&vi) != OBS_VIDEO_SUCCESS) {
    obs_shutdown();
    XCloseDisplay(display);
    return 2;
  }
  if (!load("/app/lib/obs-plugins/linux-v4l2.so",
            "/app/share/obs/obs-plugins/linux-v4l2") ||
      !load("/app/lib/obs-plugins/linux-pulseaudio.so",
            "/app/share/obs/obs-plugins/linux-pulseaudio") ||
      !load(argv[1], ".")) {
    obs_shutdown();
    XCloseDisplay(display);
    return 2;
  }
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, "video_device", argv[2]);
  obs_source_t *source = obs_source_create(
      "linux_capture_card", "Hardware smoke test", settings, NULL);
  obs_data_release(settings);
  obs_set_output_source(0, source);
  bool ok = wait_state(source, true);
  if (ok) {
    printf("PASS: video frames (%ux%u) and audio samples received\n",
           obs_source_get_width(source), obs_source_get_height(source));
    /* Cross both watchdog thresholds before exercising manual restarts. */
    os_sleep_ms(12000);
    ok = wait_state(source, true);
    if (ok)
      puts("PASS: continuous fresh-frame health across watchdog thresholds");
    for (int i = 0; i < 3 && ok; i++) {
      click(source, "toggle");
      ok = wait_state(source, false);
      if (ok) {
        click(source, "toggle");
        ok = wait_state(source, true);
      }
    }
    if (ok) {
      click(source, "restart");
      ok = wait_state(source, true);
    }
  }
  /* Finish pending lifecycle work before removing the source and shutting down.
   */
  if (source) {
    obs_data_t *current = obs_source_get_settings(source);
    if (obs_data_get_bool(current, "active"))
      click(source, "toggle");
    obs_data_release(current);
    wait_state(source, false);
  }
  obs_set_output_source(0, NULL);
  obs_source_release(source);
  obs_wait_for_destroy_queue();
  obs_shutdown();
  XCloseDisplay(display);
  puts(ok ? "PASS: hardware activate/deactivate/restart; HDMI power-cycle "
            "recovery still requires manual testing"
          : "FAIL: hardware capture did not become ready; inspect the OBS "
            "backend messages above");
  return ok ? 0 : 1;
}
