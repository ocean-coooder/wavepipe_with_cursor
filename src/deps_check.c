#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static uint64_t now_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void append_ndjson(const char *path, const char *json) {
  FILE *fp = fopen(path, "a");
  if (!fp) {
    return;
  }
  fputs(json, fp);
  fputs("\n", fp);
  fclose(fp);
}

int main(void) {
  const char *log_path = "/Users/ocean/Downloads/alsalib_play_with_cursor/.cursor/debug-7d5fdb.log";
  const char *session_id = "7d5fdb";

  const char *compiler = "unknown";
#if defined(__clang__)
  compiler = "clang";
#elif defined(__GNUC__)
  compiler = "gcc";
#endif

  int has_header = 0;
#if defined(__has_include)
#if __has_include(<alsa/asoundlib.h>)
  has_header = 1;
#else
  has_header = 0;
#endif
#else
  /* Compiler does not support __has_include; assume missing. */
  has_header = 0;
#endif

  uint64_t ts = now_ms();
  char json[512];
  snprintf(json, sizeof(json),
           "{\"sessionId\":\"%s\",\"runId\":\"buildcheck\",\"hypothesisId\":\"H1_missing_dev_header\","
           "\"timestamp\":%llu,\"location\":\"deps_check.c:main\",\"message\":\"alsa/asoundlib.h presence\","
           "\"data\":{\"has_header\":%d,\"compiler\":\"%s\"}}",
           session_id, (unsigned long long)ts, has_header, compiler);
  append_ndjson(log_path, json);

  if (has_header) {
    puts("OK: found ALSA header <alsa/asoundlib.h>.");
    return 0;
  }

  puts("FAIL: missing ALSA header <alsa/asoundlib.h>.");
  puts("Install on target: sudo apt install -y libasound2-dev");
  return 2;
}

