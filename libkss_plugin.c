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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define OUTPUT_SAMPLE_RATE 48000

// PSG/SNG 4 + SCC 5 + OPLL 14 + OPL 9 is the widest set a KSS file can present.
#define LIBKSS_MAX_SCOPE_CHANNELS 32
// Per-channel scope history, as a ring. Power of two: the index wraps with a mask.
#define LIBKSS_SCOPE_WINDOW 2048
// Window the VU peak is taken over.
#define LIBKSS_VU_WINDOW 512
// Default song length when duration is unknown (3 minutes)
#define DEFAULT_LENGTH_MS (3 * 60 * 1000)
// Fade out duration in ms
#define FADE_OUT_MS 3000

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_LOG_API();
RV_PLUGIN_USE_METADATA_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// One scope channel: where the chip emulation leaves this channel's most recent
// sample, plus the name the host shows. The pointers are into the chip structs
// owned by the KSSPLAY's VM, so they stay valid until the song is closed.
typedef struct LibkssScopeChannel {
    const int16_t* source;
    char name[24];
} LibkssScopeChannel;

typedef struct LibkssReplayerData {
    KSS* kss;
    KSSPLAY* kssplay;
    int current_track;
    int elapsed_frames;
    int max_frames;
    LibkssScopeChannel scope_channels[LIBKSS_MAX_SCOPE_CHANNELS];
    uint32_t scope_count;
    bool scope_enabled;
    float scope_ring[LIBKSS_MAX_SCOPE_CHANNELS * LIBKSS_SCOPE_WINDOW];
    uint32_t scope_pos;
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
// Build the scope channel map for the song that was just reset.
//
// Which chips a KSS file drives is decided by its header, and KSSPLAY only runs
// the ones it names -- so the map mirrors the same conditions the mixer uses.
// Each entry points straight at the slot the chip emulation writes its latest
// channel sample into; the mixer updates those as a side effect of rendering,
// so no second pass over the audio is needed.

static void libkss_add_scope_channel(LibkssReplayerData* data, const int16_t* source, const char* name) {
    if (data->scope_count >= LIBKSS_MAX_SCOPE_CHANNELS || source == nullptr) {
        return;
    }
    LibkssScopeChannel* channel = &data->scope_channels[data->scope_count++];
    channel->source = source;
    snprintf(channel->name, sizeof(channel->name), "%s", name);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void libkss_build_scope_channels(LibkssReplayerData* data) {
    data->scope_count = 0;
    data->scope_pos = 0;
    memset(data->scope_ring, 0, sizeof(data->scope_ring));

    VM* vm = data->kssplay != nullptr ? data->kssplay->vm : nullptr;
    if (vm == nullptr) {
        return;
    }

    char name[24];

    if (data->kss->sn76489) {
        if (vm->sng != nullptr) {
            for (int i = 0; i < 3; i++) {
                snprintf(name, sizeof(name), "SN %d", i + 1);
                libkss_add_scope_channel(data, &vm->sng->ch_out[i], name);
            }
            libkss_add_scope_channel(data, &vm->sng->ch_out[3], "SN Noise");
        }
    } else if (vm->psg != nullptr) {
        static const char* s_psg[3] = { "PSG A", "PSG B", "PSG C" };
        for (int i = 0; i < 3; i++) {
            libkss_add_scope_channel(data, &vm->psg->ch_out[i], s_psg[i]);
        }
    }

    if (vm->scc != nullptr) {
        for (int i = 0; i < 5; i++) {
            snprintf(name, sizeof(name), "SCC %d", i + 1);
            libkss_add_scope_channel(data, &vm->scc->ch_out[i], name);
        }
    }

    if (data->kss->fmpac && vm->opll != nullptr) {
        for (int i = 0; i < 9; i++) {
            snprintf(name, sizeof(name), "FM %d", i + 1);
            libkss_add_scope_channel(data, &vm->opll->ch_out[i], name);
        }
        // The five rhythm voices share FM channels 7-9 when rhythm mode is on.
        static const char* s_rhythm[5] = { "Bass Drum", "Hi-Hat", "Snare", "Tom", "Cymbal" };
        for (int i = 0; i < 5; i++) {
            libkss_add_scope_channel(data, &vm->opll->ch_out[9 + i], s_rhythm[i]);
        }
    }

    if (data->kss->msx_audio && vm->opl != nullptr) {
        for (int i = 0; i < 9; i++) {
            snprintf(name, sizeof(name), "OPL %d", i + 1);
            libkss_add_scope_channel(data, &vm->opl->ch_out[i], name);
        }
    }
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

    libkss_build_scope_channels(data);

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
    uint32_t capacity_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);
    uint32_t max_frames = dest.info.frame_count < capacity_frames ? dest.info.frame_count : capacity_frames;

    if (data->scope_enabled && data->scope_count > 0) {
        // KSSPLAY_calc is a per-sample loop whose only per-call work is reading
        // the device volumes, so rendering a sample at a time produces the same
        // audio and lets the chips' per-channel outputs be sampled in step.
        int16_t* output = (int16_t*)dest.channels_output;
        uint32_t pos = data->scope_pos;
        for (uint32_t frame = 0; frame < max_frames; frame++) {
            KSSPLAY_calc(data->kssplay, output + (size_t)frame * 2, 1);
            for (uint32_t c = 0; c < data->scope_count; c++) {
                data->scope_ring[c * LIBKSS_SCOPE_WINDOW + pos]
                    = (float)*data->scope_channels[c].source * (1.0f / 32768.0f);
            }
            pos = (pos + 1) & (LIBKSS_SCOPE_WINDOW - 1);
        }
        data->scope_pos = pos;
    } else {
        // Generate stereo S16 directly to output buffer
        KSSPLAY_calc(data->kssplay, (int16_t*)dest.channels_output, max_frames);
    }

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
// Visualization.
//
// The scope is the chip emulation's own per-channel output, sampled in step with
// the mix -- see libkss_build_scope_channels. Capture costs a per-sample render
// loop, so it only runs while the host has the scope switched on.

static bool libkss_plugin_get_structure(void* user_data, RVVizInfo* out) {
    LibkssReplayerData* data = (LibkssReplayerData*)user_data;
    if (data == nullptr || out == nullptr || data->scope_count == 0) {
        return false;
    }

    out->caps = RVVizCaps_Scope | RVVizCaps_Vu;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = 0;
    out->scope_channel_count = data->scope_count;
    out->column_count = 0;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t libkss_plugin_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    LibkssReplayerData* data = (LibkssReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return 0;
    }

    uint32_t count = data->scope_count < cap ? data->scope_count : cap;
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "%s", data->scope_channels[i].name);
        out[i].scope_width = 1;
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void libkss_plugin_set_scope_enabled(void* user_data, bool on) {
    LibkssReplayerData* data = (LibkssReplayerData*)user_data;
    if (data == nullptr) {
        return;
    }

    if (on && !data->scope_enabled) {
        memset(data->scope_ring, 0, sizeof(data->scope_ring));
    }
    data->scope_enabled = on;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t libkss_plugin_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    LibkssReplayerData* data = (LibkssReplayerData*)user_data;
    if (data == nullptr || out == nullptr || !data->scope_enabled) {
        return 0;
    }
    if (channel < 0 || (uint32_t)channel >= data->scope_count) {
        return 0;
    }

    uint32_t count = cap < LIBKSS_SCOPE_WINDOW ? cap : LIBKSS_SCOPE_WINDOW;
    const float* ring = &data->scope_ring[(uint32_t)channel * LIBKSS_SCOPE_WINDOW];
    uint32_t start = (data->scope_pos + LIBKSS_SCOPE_WINDOW - count) & (LIBKSS_SCOPE_WINDOW - 1);
    for (uint32_t i = 0; i < count; i++) {
        out[i] = ring[(start + i) & (LIBKSS_SCOPE_WINDOW - 1)];
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t libkss_plugin_get_vu(void* user_data, float* out, uint32_t cap) {
    LibkssReplayerData* data = (LibkssReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return 0;
    }

    // The host checks this against the declared channel count on every captured
    // frame, so the count is reported even with the capture switched off.
    uint32_t count = data->scope_count < cap ? data->scope_count : cap;
    for (uint32_t c = 0; c < count; c++) {
        if (!data->scope_enabled) {
            out[c] = 0.0f;
            continue;
        }
        const float* ring = &data->scope_ring[c * LIBKSS_SCOPE_WINDOW];
        uint32_t start = (data->scope_pos + LIBKSS_SCOPE_WINDOW - LIBKSS_VU_WINDOW) & (LIBKSS_SCOPE_WINDOW - 1);
        float peak = 0.0f;
        for (uint32_t i = 0; i < LIBKSS_VU_WINDOW; i++) {
            float v = ring[(start + i) & (LIBKSS_SCOPE_WINDOW - 1)];
            if (v < 0.0f) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
        }
        out[c] = peak;
    }
    return count;
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
    nullptr, // static_destroy

    // Visualization: per-chip-channel scope and VU straight from the emulation.
    libkss_plugin_get_structure,
    nullptr, // get_columns
    nullptr, // get_pattern_channels
    libkss_plugin_get_scope_channels,
    nullptr, // get_position
    nullptr, // get_channel_rows
    nullptr, // get_cells
    libkss_plugin_set_scope_enabled,
    libkss_plugin_get_scope_samples,
    libkss_plugin_get_vu,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_libkss_plugin;
}
