///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// libkss Playback Plugin
//
// Implements RVPlaybackPlugin interface for MSX music formats using the libkss library.
// Supported formats: KSS, MGS, BGM, OPX, MPK, MBM
// Sound chips: AY-3-8910 (PSG), SN76489, YM2413 (OPLL), Y8950 (MSX-AUDIO), Konami SCC
// libkss uses per-instance state so multiple files can be decoded concurrently.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// C11 nullptr compatibility
#ifndef nullptr
#define nullptr ((void*)0)
#endif

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include "kss.h"
#include "kssplay.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define OUTPUT_SAMPLE_RATE 48000
// Default song length when duration is unknown (3 minutes)
#define DEFAULT_LENGTH_MS (3 * 60 * 1000)
// Fade out duration in ms
#define FADE_OUT_MS 3000

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_LOG_API();
RV_PLUGIN_USE_METADATA_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct LibkssReplayerData {
    KSS* kss;
    KSSPLAY* kssplay;
    int current_track;
    int elapsed_frames;
    int max_frames;
} LibkssReplayerData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* libkss_plugin_supported_extensions(void) {
    return "kss,mgs,bgm,opx,mpk,mbm";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* libkss_plugin_create(const RVService* service_api) {
    LibkssReplayerData* data = malloc(sizeof(LibkssReplayerData));
    if (data == nullptr) {
        return nullptr;
    }
    memset(data, 0, sizeof(LibkssReplayerData));

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int libkss_plugin_destroy(void* user_data) {
    LibkssReplayerData* data = (LibkssReplayerData*)user_data;

    if (data->kssplay) {
        KSSPLAY_delete(data->kssplay);
    }
    if (data->kss) {
        KSS_delete(data->kss);
    }

    free(data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int libkss_plugin_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)service_api;

    LibkssReplayerData* data = (LibkssReplayerData*)user_data;

    // Clean up previous playback state
    if (data->kssplay) {
        KSSPLAY_delete(data->kssplay);
        data->kssplay = nullptr;
    }
    if (data->kss) {
        KSS_delete(data->kss);
        data->kss = nullptr;
    }

    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr) {
        rv_error("libkss: Failed to load %s to memory", url);
        return -1;
    }

    // Extract filename from URL for format detection
    const char* filename = strrchr(url, '/');
    if (filename) {
        filename++;
    } else {
        filename = url;
    }

    // KSS_bin2kss auto-detects format and converts to KSS container
    data->kss = KSS_bin2kss(read_res.data, (uint32_t)read_res.data_size, filename);
    rv_io_free_url_to_memory(read_res.data);

    if (data->kss == nullptr) {
        rv_error("libkss: Failed to parse %s", url);
        return -1;
    }

    // Create player: 48kHz, stereo, 16-bit
    data->kssplay = KSSPLAY_new(OUTPUT_SAMPLE_RATE, 2, 16);
    if (data->kssplay == nullptr) {
        rv_error("libkss: Failed to create KSSPLAY instance");
        KSS_delete(data->kss);
        data->kss = nullptr;
        return -1;
    }

    KSSPLAY_set_data(data->kssplay, data->kss);

    // Clamp subsong to valid range
    int track = (int)subsong;
    if (track < data->kss->trk_min) {
        track = data->kss->trk_min;
    }
    if (track > data->kss->trk_max) {
        track = data->kss->trk_min;
    }
    data->current_track = track;

    KSSPLAY_reset(data->kssplay, (uint32_t)track, 0);

    // Set up silence detection (5 seconds)
    KSSPLAY_set_silent_limit(data->kssplay, 5000);

    // Calculate max frames for default song length
    data->max_frames = ((int64_t)DEFAULT_LENGTH_MS * OUTPUT_SAMPLE_RATE) / 1000;
    data->elapsed_frames = 0;

    // Start fade near the end
    int fade_start_ms = DEFAULT_LENGTH_MS - FADE_OUT_MS;
    if (fade_start_ms < 0) {
        fade_start_ms = 0;
    }
    KSSPLAY_fade_start(data->kssplay, (uint32_t)fade_start_ms);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void libkss_plugin_close(void* user_data) {
    LibkssReplayerData* data = (LibkssReplayerData*)user_data;

    if (data->kssplay) {
        KSSPLAY_delete(data->kssplay);
        data->kssplay = nullptr;
    }
    if (data->kss) {
        KSS_delete(data->kss);
        data->kss = nullptr;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult libkss_plugin_probe_can_play(uint8_t* probe_data, uint64_t data_size, const char* url,
                                                  uint64_t total_size) {
    (void)total_size;

    if (data_size < 4) {
        return RVProbeResult_Unsupported;
    }

    // KSS native format: "KSCC" or "KSSX"
    if (probe_data[0] == 'K' && probe_data[1] == 'S' && probe_data[2] == 'S'
        && (probe_data[3] == 'C' || probe_data[3] == 'X')) {
        return RVProbeResult_Supported;
    }

    // MGS format: "MGS" header
    if (data_size >= 32 && probe_data[0] == 'M' && probe_data[1] == 'G' && probe_data[2] == 'S') {
        return RVProbeResult_Supported;
    }

    // MPK format: "MPK" header
    if (probe_data[0] == 'M' && probe_data[1] == 'P' && probe_data[2] == 'K') {
        return RVProbeResult_Supported;
    }

    // OPX format: check byte at 0x7D == 0x1A (requires > 160 bytes)
    if (data_size > 160 && probe_data[0x7D] == 0x1A) {
        return RVProbeResult_Supported;
    }

    // BGM format: starts with 0xFE
    if (probe_data[0] == 0xFE && data_size >= 0x60) {
        // Additional check: "BTO" at offset 0x50
        if (probe_data[0x50] == 'B' && probe_data[0x51] == 'T' && probe_data[0x52] == 'O') {
            return RVProbeResult_Supported;
        }
    }

    // MBM: no magic bytes, use extension check
    if (url != nullptr) {
        const char* dot = strrchr(url, '.');
        if (dot != nullptr && strcasecmp(dot, ".mbm") == 0) {
            return RVProbeResult_Unsure;
        }
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo libkss_plugin_read_data(void* user_data, RVReadData dest) {
    LibkssReplayerData* data = (LibkssReplayerData*)user_data;
    RVAudioFormat format = { RVAudioStreamFormat_S16, 2, OUTPUT_SAMPLE_RATE };

    if (data->kssplay == nullptr) {
        return (RVReadInfo) { format, 0, RVReadStatus_Error};
    }

    // Check if song ended (fade finished, silence detected, or max length reached)
    if (KSSPLAY_get_fade_flag(data->kssplay) == 2 || KSSPLAY_get_stop_flag(data->kssplay)
        || data->elapsed_frames >= data->max_frames) {
        return (RVReadInfo) { format, 0, RVReadStatus_Finished};
    }

    // Calculate how many S16 stereo frames fit in the output buffer
    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);

    // Generate stereo S16 directly to output buffer
    KSSPLAY_calc(data->kssplay, (int16_t*)dest.channels_output, max_frames);

    data->elapsed_frames += (int)max_frames;

    // Check end conditions after rendering
    RVReadStatus status = RVReadStatus_Ok;
    if (KSSPLAY_get_fade_flag(data->kssplay) == 2 || KSSPLAY_get_stop_flag(data->kssplay)
        || data->elapsed_frames >= data->max_frames) {
        status = RVReadStatus_Finished;
    }

    return (RVReadInfo) { format, (uint32_t)max_frames, status};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t libkss_plugin_seek(void* user_data, int64_t ms) {
    (void)user_data;
    (void)ms;
    // libkss has no native seek support
    return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int libkss_plugin_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr) {
        return -1;
    }

    const char* filename = strrchr(url, '/');
    if (filename) {
        filename++;
    } else {
        filename = url;
    }

    KSS* kss = KSS_bin2kss(read_res.data, (uint32_t)read_res.data_size, filename);
    rv_io_free_url_to_memory(read_res.data);

    if (kss == nullptr) {
        return -1;
    }

    RVMetadataId index = rv_metadata_create_url(url);

    // Extract title if available
    const char* title = KSS_get_title(kss);
    if (title != nullptr && title[0] != '\0') {
        rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, title);
    }

    // Set song type based on KSS type field
    const char* song_type = "KSS";
    switch (kss->type) {
        case 1:
            song_type = "MGS";
            break;
        case 2:
            song_type = "MBM";
            break;
        case 3:
            song_type = "MPK";
            break; // MPK106
        case 4:
            song_type = "MPK";
            break; // MPK103
        case 5:
            song_type = "BGM";
            break;
        case 6:
            song_type = "OPX";
            break;
        default:
            break;
    }
    rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, song_type);
    rv_metadata_set_tag(index, RV_METADATA_AUTHORINGTOOL_TAG, "MSX");

    // Set default duration
    rv_metadata_set_tag_f64(index, RV_METADATA_LENGTH_TAG, DEFAULT_LENGTH_MS / 1000.0);

    // Add subsongs if the track range spans more than one
    int track_count = kss->trk_max - kss->trk_min + 1;
    if (track_count > 1) {
        for (int i = 0; i < track_count; i++) {
            rv_metadata_add_subsong(index, (uint32_t)i, "", (float)(DEFAULT_LENGTH_MS / 1000.0));
        }
    }

    KSS_delete(kss);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void libkss_plugin_event(void* user_data, uint8_t* event_data, uint64_t len) {
    (void)user_data;
    (void)event_data;
    (void)len;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void libkss_plugin_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_libkss_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "libkss",
    "0.0.1",
    "libkss 1.2.1",
    libkss_plugin_probe_can_play,
    libkss_plugin_supported_extensions,
    libkss_plugin_create,
    libkss_plugin_destroy,
    libkss_plugin_event,
    libkss_plugin_open,
    libkss_plugin_close,
    libkss_plugin_read_data,
    libkss_plugin_seek,
    libkss_plugin_metadata,
    libkss_plugin_static_init,
    nullptr, // settings_updated
    nullptr, // get_tracker_info
    nullptr, // get_pattern_cell
    nullptr, // get_pattern_num_rows
    nullptr, // get_scope_data
    nullptr, // static_destroy
    nullptr, // get_scope_channel_names
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_libkss_plugin;
}
