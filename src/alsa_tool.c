#define _GNU_SOURCE
#include <time.h>
#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

// #region agent log
static void append_debug_ndjson(const char *hypothesis_id, const char *location, const char *message,
                                int rc, int err, const char *device, const char *phase) {
  char line[768];
  unsigned long long ts = (unsigned long long)time(NULL) * 1000ULL;
  snprintf(line, sizeof(line),
           "{\"sessionId\":\"7d5fdb\",\"runId\":\"pcm-open\",\"hypothesisId\":\"%s\","
           "\"timestamp\":%llu,\"location\":\"%s\",\"message\":\"%s\","
           "\"data\":{\"rc\":%d,\"errno\":%d,\"device\":\"%s\",\"phase\":\"%s\"}}",
           hypothesis_id, ts, location, message, rc, err, device ? device : "", phase ? phase : "");
  const char *paths[] = {
      "/Users/ocean/Downloads/alsalib_play_with_cursor/.cursor/debug-7d5fdb.log",
      ".cursor/debug-7d5fdb.log",
      NULL,
  };
  for (int i = 0; paths[i]; i++) {
    FILE *fp = fopen(paths[i], "a");
    if (fp) {
      fputs(line, fp);
      fputc('\n', fp);
      fclose(fp);
      return;
    }
  }
  fprintf(stderr, "[debug-ndjson] %s\n", line);
}
// #endregion

typedef struct {
  uint16_t audio_format;
  uint16_t channels;
  uint32_t sample_rate;
  uint16_t bits_per_sample;
  uint32_t data_size;
  long data_offset;
} WavInfo;

static void print_usage(const char *prog) {
  fprintf(stderr,
          "Usage:\n"
          "  %s play [-c card] [-p subdev] <wav_file> [device]\n"
          "  %s record [-c card] [-p subdev] <out_pcm_file> <seconds> <sample_rate> "
          "<channels> <bits> [device]\n"
          "  %s record-wav [-c card] [-p subdev] <out_wav_file> <seconds> <sample_rate> "
          "<channels> <bits> [device]\n\n"
          "  -c, --card N   use plughw:N,<subdev> (see aplay -l for card index)\n"
          "  -p, --pcm M    PCM subdevice index (default 0; use with -c)\n"
          "  [device]       full ALSA PCM name (e.g. plughw:1,0); not with -c\n\n"
          "Supported sample_rate: 44100, 48000\n"
          "Supported channels   : 2, 4, 8\n"
          "Supported bits       : 8, 16, 32\n",
          prog, prog, prog);
}

static int resolve_device_from_card_opt(int card, int pcm_sub, const char *explicit_dev,
                                        char *buf, size_t buf_sz, const char **out_dev) {
  if (explicit_dev && card >= 0) {
    fprintf(stderr, "Error: use either -c/--card or [device], not both.\n");
    return -1;
  }
  if (explicit_dev) {
    *out_dev = explicit_dev;
    return 0;
  }
  if (card >= 0) {
    snprintf(buf, buf_sz, "plughw:%d,%d", card, pcm_sub);
    *out_dev = buf;
    return 0;
  }
  *out_dev = NULL;
  return 0;
}

static int is_supported_rate(unsigned int rate) { return rate == 44100 || rate == 48000; }

static int is_supported_channels(unsigned int ch) { return ch == 2 || ch == 4 || ch == 8; }

static int is_supported_bits(unsigned int bits) { return bits == 8 || bits == 16 || bits == 32; }

static snd_pcm_format_t bits_to_format(unsigned int bits) {
  switch (bits) {
  case 8:
    return SND_PCM_FORMAT_U8;
  case 16:
    return SND_PCM_FORMAT_S16_LE;
  case 32:
    return SND_PCM_FORMAT_S32_LE;
  default:
    return SND_PCM_FORMAT_UNKNOWN;
  }
}

static const char *find_first_pcm_device(snd_pcm_stream_t stream) {
  int card = -1;
  if (snd_card_next(&card) < 0 || card < 0) {
    return NULL;
  }

  while (card >= 0) {
    snd_ctl_t *ctl = NULL;

    char hw_name[32];
    snprintf(hw_name, sizeof(hw_name), "hw:%d", card);
    if (snd_ctl_open(&ctl, hw_name, 0) >= 0) {
      int dev = -1;
      while (1) {
        if (snd_ctl_pcm_next_device(ctl, &dev) < 0 || dev < 0) {
          break;
        }
        snd_pcm_info_t *pcminfo = NULL;
        snd_pcm_info_alloca(&pcminfo);
        snd_pcm_info_set_device(pcminfo, dev);
        snd_pcm_info_set_subdevice(pcminfo, 0);
        snd_pcm_info_set_stream(pcminfo, stream);
        if (snd_ctl_pcm_info(ctl, pcminfo) >= 0) {
          static char device[40];
          /* plughw avoids Pi ALSA "Unknown error 524" on bare hw:N,M for many drivers. */
          snprintf(device, sizeof(device), "plughw:%d,%d", card, dev);
          snd_ctl_close(ctl);
          return device;
        }
      }
      snd_ctl_close(ctl);
    }
    if (snd_card_next(&card) < 0) {
      break;
    }
  }
  return NULL;
}

static int snd_pcm_open_with_fallback(snd_pcm_t **pcm, const char *requested,
                                      snd_pcm_stream_t stream, char *used_out, size_t used_sz) {
  int rc = snd_pcm_open(pcm, requested, stream, 0);
  int saved_errno = errno;
  append_debug_ndjson("H1_primary_open", "alsa_tool.c:snd_pcm_open_with_fallback", "snd_pcm_open primary",
                      rc, saved_errno, requested, "primary");
  if (rc >= 0) {
    snprintf(used_out, used_sz, "%s", requested);
    append_debug_ndjson("H0_primary_ok", "alsa_tool.c:snd_pcm_open_with_fallback", "snd_pcm_open success",
                        0, 0, requested, "primary_ok");
    return 0;
  }

  int card = -1;
  int dev = -1;
  if (sscanf(requested, "hw:%d,%d", &card, &dev) == 2) {
    char alt[64];
    snprintf(alt, sizeof(alt), "plughw:%d,%d", card, dev);
    errno = 0;
    rc = snd_pcm_open(pcm, alt, stream, 0);
    saved_errno = errno;
    append_debug_ndjson("H2_plughw_fallback", "alsa_tool.c:snd_pcm_open_with_fallback",
                        "snd_pcm_open plughw", rc, saved_errno, alt, "plughw");
    if (rc >= 0) {
      snprintf(used_out, used_sz, "%s", alt);
      append_debug_ndjson("H0_plughw_ok", "alsa_tool.c:snd_pcm_open_with_fallback", "snd_pcm_open success",
                          0, 0, alt, "plughw_ok");
      return 0;
    }
  }

  if (strcmp(requested, "default") != 0) {
    errno = 0;
    rc = snd_pcm_open(pcm, "default", stream, 0);
    saved_errno = errno;
    append_debug_ndjson("H3_default_fallback", "alsa_tool.c:snd_pcm_open_with_fallback",
                        "snd_pcm_open default", rc, saved_errno, "default", "default");
    if (rc >= 0) {
      snprintf(used_out, used_sz, "default");
      append_debug_ndjson("H0_default_ok", "alsa_tool.c:snd_pcm_open_with_fallback", "snd_pcm_open success",
                          0, 0, "default", "default_ok");
      return 0;
    }
  }

  snprintf(used_out, used_sz, "%s", requested);
  return rc;
}

static int parse_wav_header(FILE *fp, WavInfo *info) {
  unsigned char header[44];
  if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
    return -1;
  }

  if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    return -1;
  }

  info->audio_format = (uint16_t)(header[20] | (header[21] << 8));
  info->channels = (uint16_t)(header[22] | (header[23] << 8));
  info->sample_rate =
      (uint32_t)(header[24] | (header[25] << 8) | (header[26] << 16) | (header[27] << 24));
  info->bits_per_sample = (uint16_t)(header[34] | (header[35] << 8));
  info->data_size =
      (uint32_t)(header[40] | (header[41] << 8) | (header[42] << 16) | (header[43] << 24));
  info->data_offset = 44;

  if (info->audio_format != 1) {
    fprintf(stderr, "Only PCM WAV is supported.\n");
    return -1;
  }
  if (!is_supported_rate(info->sample_rate) || !is_supported_channels(info->channels) ||
      !is_supported_bits(info->bits_per_sample)) {
    fprintf(stderr, "WAV format not supported by this tool.\n");
    return -1;
  }
  return 0;
}

static int set_hw_params(snd_pcm_t *pcm, unsigned int rate, unsigned int channels,
                         snd_pcm_format_t format, snd_pcm_uframes_t *frames) {
  snd_pcm_hw_params_t *params = NULL;
  snd_pcm_hw_params_alloca(&params);

  int rc = snd_pcm_hw_params_any(pcm, params);
  if (rc < 0)
    return rc;
  rc = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0)
    return rc;
  rc = snd_pcm_hw_params_set_format(pcm, params, format);
  if (rc < 0)
    return rc;
  rc = snd_pcm_hw_params_set_channels(pcm, params, channels);
  if (rc < 0)
    return rc;
  rc = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0);
  if (rc < 0)
    return rc;

  *frames = 1024;
  rc = snd_pcm_hw_params_set_period_size_near(pcm, params, frames, 0);
  if (rc < 0)
    return rc;

  return snd_pcm_hw_params(pcm, params);
}

static int do_play(const char *wav_file, const char *device_opt) {
  const char *device =
      device_opt ? device_opt : find_first_pcm_device(SND_PCM_STREAM_PLAYBACK);
  if (!device) {
    fprintf(stderr, "No usable ALSA playback device found.\n");
    return 1;
  }

  FILE *fp = fopen(wav_file, "rb");
  if (!fp) {
    fprintf(stderr, "Cannot open WAV file: %s\n", strerror(errno));
    return 1;
  }

  WavInfo wi;
  if (parse_wav_header(fp, &wi) < 0) {
    fclose(fp);
    fprintf(stderr, "Invalid/unsupported WAV file.\n");
    return 1;
  }

  snd_pcm_t *pcm = NULL;
  char used_dev[64];
  int rc = snd_pcm_open_with_fallback(&pcm, device, SND_PCM_STREAM_PLAYBACK, used_dev, sizeof(used_dev));
  if (rc < 0) {
    fclose(fp);
    fprintf(stderr, "snd_pcm_open(playback) failed: %s (rc=%d)\n", snd_strerror(rc), rc);
    if (rc < 0 && -rc < 1500) {
      fprintf(stderr, "  strerror(-rc): %s\n", strerror(-rc));
    }
    fprintf(stderr,
            "Hint: on Raspberry Pi, try `plughw:0,0` or `default`, or set /etc/asound.conf "
            "(defaults.pcm.card / defaults.ctl.card) to match your audio output.\n");
    return 1;
  }
  printf("Playback device: %s\n", used_dev);

  snd_pcm_uframes_t frames = 0;
  snd_pcm_format_t format = bits_to_format(wi.bits_per_sample);
  rc = set_hw_params(pcm, wi.sample_rate, wi.channels, format, &frames);
  if (rc < 0) {
    snd_pcm_close(pcm);
    fclose(fp);
    fprintf(stderr, "set_hw_params(playback) failed: %s\n", snd_strerror(rc));
    return 1;
  }

  size_t frame_bytes = (wi.bits_per_sample / 8) * wi.channels;
  size_t chunk_bytes = frames * frame_bytes;
  uint8_t *buffer = (uint8_t *)malloc(chunk_bytes);
  if (!buffer) {
    snd_pcm_close(pcm);
    fclose(fp);
    fprintf(stderr, "Out of memory.\n");
    return 1;
  }

  while (!feof(fp)) {
    size_t n = fread(buffer, 1, chunk_bytes, fp);
    if (n == 0)
      break;
    snd_pcm_sframes_t written = snd_pcm_writei(pcm, buffer, n / frame_bytes);
    if (written == -EPIPE) {
      snd_pcm_prepare(pcm);
      continue;
    } else if (written < 0) {
      fprintf(stderr, "snd_pcm_writei failed: %s\n", snd_strerror((int)written));
      break;
    }
  }

  snd_pcm_drain(pcm);
  snd_pcm_close(pcm);
  free(buffer);
  fclose(fp);
  return 0;
}

static int do_record(const char *out_pcm, unsigned int seconds, unsigned int sample_rate,
                     unsigned int channels, unsigned int bits, const char *device_opt) {
  if (!is_supported_rate(sample_rate) || !is_supported_channels(channels) || !is_supported_bits(bits)) {
    fprintf(stderr, "Unsupported record parameters.\n");
    return 1;
  }

  const char *device =
      device_opt ? device_opt : find_first_pcm_device(SND_PCM_STREAM_CAPTURE);
  if (!device) {
    fprintf(stderr, "No usable ALSA capture/playback device found.\n");
    return 1;
  }

  snd_pcm_t *pcm = NULL;
  char used_dev[64];
  int rc = snd_pcm_open_with_fallback(&pcm, device, SND_PCM_STREAM_CAPTURE, used_dev, sizeof(used_dev));
  if (rc < 0) {
    fprintf(stderr, "snd_pcm_open(capture) failed: %s (rc=%d)\n", snd_strerror(rc), rc);
    return 1;
  }
  printf("Record device: %s\n", used_dev);

  snd_pcm_uframes_t frames = 0;
  snd_pcm_format_t format = bits_to_format(bits);
  rc = set_hw_params(pcm, sample_rate, channels, format, &frames);
  if (rc < 0) {
    snd_pcm_close(pcm);
    fprintf(stderr, "set_hw_params(capture) failed: %s\n", snd_strerror(rc));
    return 1;
  }

  FILE *fp = fopen(out_pcm, "wb");
  if (!fp) {
    snd_pcm_close(pcm);
    fprintf(stderr, "Cannot open output file: %s\n", strerror(errno));
    return 1;
  }

  size_t frame_bytes = (bits / 8) * channels;
  size_t chunk_bytes = frames * frame_bytes;
  uint8_t *buffer = (uint8_t *)malloc(chunk_bytes);
  if (!buffer) {
    snd_pcm_close(pcm);
    fclose(fp);
    fprintf(stderr, "Out of memory.\n");
    return 1;
  }

  uint64_t total_frames = (uint64_t)seconds * sample_rate;
  uint64_t done_frames = 0;
  while (done_frames < total_frames) {
    snd_pcm_sframes_t to_read = (snd_pcm_sframes_t)frames;
    if ((uint64_t)to_read > total_frames - done_frames) {
      to_read = (snd_pcm_sframes_t)(total_frames - done_frames);
    }
    snd_pcm_sframes_t nread = snd_pcm_readi(pcm, buffer, to_read);
    if (nread == -EPIPE) {
      snd_pcm_prepare(pcm);
      continue;
    } else if (nread < 0) {
      fprintf(stderr, "snd_pcm_readi failed: %s\n", snd_strerror((int)nread));
      break;
    }
    size_t written = fwrite(buffer, frame_bytes, (size_t)nread, fp);
    if (written != (size_t)nread) {
      fprintf(stderr, "File write failed.\n");
      break;
    }
    done_frames += (uint64_t)nread;
  }

  snd_pcm_close(pcm);
  fclose(fp);
  free(buffer);
  return 0;
}

static void write_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void write_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static int write_wav_header(FILE *fp, unsigned int sample_rate, unsigned int channels,
                            unsigned int bits, uint32_t data_size) {
  uint8_t h[44];
  uint16_t block_align = (uint16_t)((channels * bits) / 8);
  uint32_t byte_rate = sample_rate * block_align;
  uint32_t riff_size = 36 + data_size;

  memcpy(h + 0, "RIFF", 4);
  write_le32(h + 4, riff_size);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  write_le32(h + 16, 16);
  write_le16(h + 20, 1);
  write_le16(h + 22, (uint16_t)channels);
  write_le32(h + 24, sample_rate);
  write_le32(h + 28, byte_rate);
  write_le16(h + 32, block_align);
  write_le16(h + 34, (uint16_t)bits);
  memcpy(h + 36, "data", 4);
  write_le32(h + 40, data_size);

  return fwrite(h, 1, sizeof(h), fp) == sizeof(h) ? 0 : -1;
}

static int do_record_wav(const char *out_wav, unsigned int seconds, unsigned int sample_rate,
                         unsigned int channels, unsigned int bits, const char *device_opt) {
  if (!is_supported_rate(sample_rate) || !is_supported_channels(channels) || !is_supported_bits(bits)) {
    fprintf(stderr, "Unsupported record-wav parameters.\n");
    return 1;
  }

  const char *device =
      device_opt ? device_opt : find_first_pcm_device(SND_PCM_STREAM_CAPTURE);
  if (!device) {
    fprintf(stderr, "No usable ALSA capture device found.\n");
    return 1;
  }

  snd_pcm_t *pcm = NULL;
  char used_dev[64];
  int rc = snd_pcm_open_with_fallback(&pcm, device, SND_PCM_STREAM_CAPTURE, used_dev, sizeof(used_dev));
  if (rc < 0) {
    fprintf(stderr, "snd_pcm_open(capture) failed: %s (rc=%d)\n", snd_strerror(rc), rc);
    return 1;
  }
  printf("Record WAV device: %s\n", used_dev);

  snd_pcm_uframes_t frames = 0;
  snd_pcm_format_t format = bits_to_format(bits);
  rc = set_hw_params(pcm, sample_rate, channels, format, &frames);
  if (rc < 0) {
    snd_pcm_close(pcm);
    fprintf(stderr, "set_hw_params(capture) failed: %s\n", snd_strerror(rc));
    return 1;
  }

  FILE *fp = fopen(out_wav, "wb+");
  if (!fp) {
    snd_pcm_close(pcm);
    fprintf(stderr, "Cannot open output WAV file: %s\n", strerror(errno));
    return 1;
  }
  if (write_wav_header(fp, sample_rate, channels, bits, 0) < 0) {
    snd_pcm_close(pcm);
    fclose(fp);
    fprintf(stderr, "Failed to write WAV header.\n");
    return 1;
  }

  size_t frame_bytes = (bits / 8) * channels;
  size_t chunk_bytes = frames * frame_bytes;
  uint8_t *buffer = (uint8_t *)malloc(chunk_bytes);
  if (!buffer) {
    snd_pcm_close(pcm);
    fclose(fp);
    fprintf(stderr, "Out of memory.\n");
    return 1;
  }

  uint64_t total_frames = (uint64_t)seconds * sample_rate;
  uint64_t done_frames = 0;
  uint64_t data_bytes = 0;
  while (done_frames < total_frames) {
    snd_pcm_sframes_t to_read = (snd_pcm_sframes_t)frames;
    if ((uint64_t)to_read > total_frames - done_frames) {
      to_read = (snd_pcm_sframes_t)(total_frames - done_frames);
    }
    snd_pcm_sframes_t nread = snd_pcm_readi(pcm, buffer, to_read);
    if (nread == -EPIPE) {
      snd_pcm_prepare(pcm);
      continue;
    } else if (nread < 0) {
      fprintf(stderr, "snd_pcm_readi failed: %s\n", snd_strerror((int)nread));
      break;
    }
    size_t written = fwrite(buffer, frame_bytes, (size_t)nread, fp);
    if (written != (size_t)nread) {
      fprintf(stderr, "File write failed.\n");
      break;
    }
    done_frames += (uint64_t)nread;
    data_bytes += (uint64_t)nread * frame_bytes;
  }

  if (fseek(fp, 0, SEEK_SET) == 0) {
    uint32_t final_data_size = data_bytes > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)data_bytes;
    if (write_wav_header(fp, sample_rate, channels, bits, final_data_size) < 0) {
      fprintf(stderr, "Failed to update WAV header.\n");
    }
  } else {
    fprintf(stderr, "Failed to seek for WAV header update.\n");
  }

  snd_pcm_close(pcm);
  fclose(fp);
  free(buffer);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  if (strcmp(argv[1], "play") == 0) {
    int card = -1;
    int pcm_sub = 0;
    optind = 2;
    opterr = 0;
    static const struct option play_long[] = {
        {"card", required_argument, NULL, 'c'},
        {"pcm", required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0}};
    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:", play_long, NULL)) != -1) {
      switch (opt) {
      case 'c':
        card = (int)strtol(optarg, NULL, 10);
        if (card < 0) {
          fprintf(stderr, "Invalid --card value.\n");
          return 1;
        }
        break;
      case 'p':
        pcm_sub = (int)strtol(optarg, NULL, 10);
        if (pcm_sub < 0) {
          fprintf(stderr, "Invalid --pcm value.\n");
          return 1;
        }
        break;
      default:
        print_usage(argv[0]);
        return 1;
      }
    }
    if (optind >= argc) {
      print_usage(argv[0]);
      return 1;
    }
    const char *wav_file = argv[optind];
    const char *explicit_dev = (optind + 1 < argc) ? argv[optind + 1] : NULL;
    if (optind + 2 < argc) {
      fprintf(stderr, "Too many arguments.\n");
      print_usage(argv[0]);
      return 1;
    }
    static char play_devbuf[64];
    const char *dev = NULL;
    if (resolve_device_from_card_opt(card, pcm_sub, explicit_dev, play_devbuf, sizeof(play_devbuf),
                                     &dev) < 0)
      return 1;
    return do_play(wav_file, dev);
  }

  if (strcmp(argv[1], "record") == 0) {
    int card = -1;
    int pcm_sub = 0;
    optind = 2;
    opterr = 0;
    static const struct option rec_long[] = {
        {"card", required_argument, NULL, 'c'},
        {"pcm", required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0}};
    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:", rec_long, NULL)) != -1) {
      switch (opt) {
      case 'c':
        card = (int)strtol(optarg, NULL, 10);
        if (card < 0) {
          fprintf(stderr, "Invalid --card value.\n");
          return 1;
        }
        break;
      case 'p':
        pcm_sub = (int)strtol(optarg, NULL, 10);
        if (pcm_sub < 0) {
          fprintf(stderr, "Invalid --pcm value.\n");
          return 1;
        }
        break;
      default:
        print_usage(argv[0]);
        return 1;
      }
    }
    int n = argc - optind;
    if (n != 5 && n != 6) {
      print_usage(argv[0]);
      return 1;
    }
    const char *out_pcm = argv[optind];
    unsigned int sec = (unsigned int)strtoul(argv[optind + 1], NULL, 10);
    unsigned int rate = (unsigned int)strtoul(argv[optind + 2], NULL, 10);
    unsigned int ch = (unsigned int)strtoul(argv[optind + 3], NULL, 10);
    unsigned int bits = (unsigned int)strtoul(argv[optind + 4], NULL, 10);
    const char *explicit_dev = (n == 6) ? argv[optind + 5] : NULL;
    static char rec_devbuf[64];
    const char *dev = NULL;
    if (resolve_device_from_card_opt(card, pcm_sub, explicit_dev, rec_devbuf, sizeof(rec_devbuf),
                                     &dev) < 0)
      return 1;
    return do_record(out_pcm, sec, rate, ch, bits, dev);
  }

  if (strcmp(argv[1], "record-wav") == 0) {
    int card = -1;
    int pcm_sub = 0;
    optind = 2;
    opterr = 0;
    static const struct option rwav_long[] = {
        {"card", required_argument, NULL, 'c'},
        {"pcm", required_argument, NULL, 'p'},
        {NULL, 0, NULL, 0}};
    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:", rwav_long, NULL)) != -1) {
      switch (opt) {
      case 'c':
        card = (int)strtol(optarg, NULL, 10);
        if (card < 0) {
          fprintf(stderr, "Invalid --card value.\n");
          return 1;
        }
        break;
      case 'p':
        pcm_sub = (int)strtol(optarg, NULL, 10);
        if (pcm_sub < 0) {
          fprintf(stderr, "Invalid --pcm value.\n");
          return 1;
        }
        break;
      default:
        print_usage(argv[0]);
        return 1;
      }
    }
    int n = argc - optind;
    if (n != 5 && n != 6) {
      print_usage(argv[0]);
      return 1;
    }
    const char *out_wav = argv[optind];
    unsigned int sec = (unsigned int)strtoul(argv[optind + 1], NULL, 10);
    unsigned int rate = (unsigned int)strtoul(argv[optind + 2], NULL, 10);
    unsigned int ch = (unsigned int)strtoul(argv[optind + 3], NULL, 10);
    unsigned int bits = (unsigned int)strtoul(argv[optind + 4], NULL, 10);
    const char *explicit_dev = (n == 6) ? argv[optind + 5] : NULL;
    static char rwav_devbuf[64];
    const char *dev = NULL;
    if (resolve_device_from_card_opt(card, pcm_sub, explicit_dev, rwav_devbuf, sizeof(rwav_devbuf),
                                     &dev) < 0)
      return 1;
    return do_record_wav(out_wav, sec, rate, ch, bits, dev);
  }

  print_usage(argv[0]);
  return 1;
}
