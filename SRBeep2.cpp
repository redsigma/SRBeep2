/***********************************
Orig: A Docile Sloth adocilesloth@gmail.com
Now: EBK21 chkd13303@gmail.com
************************************/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <thread>
#include <atomic>
#include <sstream>
#include <mutex>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cctype>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <SDL3_mixer/SDL_mixer.h>


#define EXIT_WITH_ERROR(x) do { \
        blog(LOG_ERROR, "Failed to play audio! At %s:%d", __FILE__, (int)(x)); \
        blog(LOG_ERROR, SDL_GetError()); \
        goto exit; \
} while(0)

struct RecordStartMuteConfig {
        bool enabled = false;
        uint32_t duration_ms = 1200;
        std::vector<std::string> source_names;
};

struct SourceMuteState {
        std::string name;
        bool was_muted = false;
        obs_source_t *source = NULL;
};

std::mutex audioMutex;
std::thread st_stt_Thread, st_sto_Thread, rc_stt_Thread, rc_sto_Thread, bf_stt_Thread, bf_sto_Thread, ps_stt_Thread, ps_sto_Thread;
std::atomic_int queue = 0;

MIX_Mixer* sdlmixer = NULL;
thread_local MIX_Audio* audio = NULL;
thread_local MIX_Track* track = NULL;

OBS_DECLARE_MODULE()

std::string clean_path(std::string audio_path) {
        std::string cleaned_path;
        if (audio_path.find("..") != std::string::npos) {
                size_t pos = audio_path.find("..");
                cleaned_path = audio_path.substr(pos);
        }
        else {
                #ifdef _WIN32
                while (audio_path.length() > 0 && islower((unsigned char)audio_path[0])) {
                        audio_path = audio_path.substr(1);
                }
                #else
                while (audio_path.length() > 0 && audio_path.substr(0, 1) != "/") {
                        audio_path = audio_path.substr(1);
                }
                #endif
                cleaned_path = audio_path;
        }
        return cleaned_path;
}

static std::string trim_copy(const std::string &value) {
        size_t first = 0;
        while (first < value.size() && std::isspace((unsigned char)value[first])) {
                ++first;
        }

        if (first == value.size()) {
                return std::string();
        }

        size_t last = value.size();
        while (last > first && std::isspace((unsigned char)value[last - 1])) {
                --last;
        }

        return value.substr(first, last - first);
}

static std::string lowercase_copy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return value;
}

static bool parse_bool_value(const std::string &value) {
        std::string lowered = lowercase_copy(trim_copy(value));
        return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

static uint32_t parse_duration_ms(const std::string &value, uint32_t fallback) {
        std::string trimmed = trim_copy(value);
        if (trimmed.empty()) {
                return fallback;
        }

        try {
                long long parsed = std::stoll(trimmed);
                if (parsed < 0) {
                        return fallback;
                }
                if (parsed > 60000) {
                        return 60000;
                }
                return (uint32_t)parsed;
        } catch (...) {
                return fallback;
        }
}

static std::vector<std::string> parse_sources_csv(const std::string &value) {
        std::vector<std::string> source_names;
        std::stringstream parser(value);
        std::string token;

        while (std::getline(parser, token, ',')) {
                std::string cleaned = trim_copy(token);
                if (!cleaned.empty()) {
                        source_names.push_back(cleaned);
                }
        }

        return source_names;
}

static RecordStartMuteConfig load_record_start_mute_config() {
        RecordStartMuteConfig config;

        const char *obs_data_path = obs_get_module_data_path(obs_current_module());
        std::stringstream config_path;
        config_path << obs_data_path;
        config_path << "/record_start_mute_config.ini";
        std::string true_path = clean_path(config_path.str());

        std::ifstream file(true_path);
        if (!file.is_open()) {
                blog(LOG_INFO, "SRBeep2: record_start_mute_config.ini not found. Source muting is disabled.");
                return config;
        }

        std::string line;
        while (std::getline(file, line)) {
                std::string cleaned = trim_copy(line);
                if (cleaned.empty() || cleaned[0] == '#' || cleaned[0] == ';') {
                        continue;
                }

                size_t separator = cleaned.find('=');
                if (separator == std::string::npos) {
                        continue;
                }

                std::string key = lowercase_copy(trim_copy(cleaned.substr(0, separator)));
                std::string value = trim_copy(cleaned.substr(separator + 1));

                if (key == "enabled") {
                        config.enabled = parse_bool_value(value);
                } else if (key == "duration_ms") {
                        config.duration_ms = parse_duration_ms(value, config.duration_ms);
                } else if (key == "sources") {
                        config.source_names = parse_sources_csv(value);
                }
        }

        return config;
}

static std::vector<SourceMuteState> mute_configured_sources(const std::vector<std::string> &source_names) {
        std::vector<SourceMuteState> muted_sources;

        for (const std::string &name : source_names) {
                obs_source_t *source = obs_get_source_by_name(name.c_str());
                if (!source) {
                        blog(LOG_WARNING, "SRBeep2: Source '%s' was not found for temporary muting.", name.c_str());
                        continue;
                }

                bool was_muted = obs_source_muted(source);
                if (!was_muted) {
                        obs_source_set_muted(source, true);
                }

                SourceMuteState state;
                state.name = name;
                state.was_muted = was_muted;
                state.source = source;
                muted_sources.push_back(state);
        }

        return muted_sources;
}

static void restore_muted_sources(std::vector<SourceMuteState> &muted_sources) {
        for (SourceMuteState &state : muted_sources) {
                if (!state.source) {
                        continue;
                }

                obs_source_set_muted(state.source, state.was_muted);
                obs_source_release(state.source);
                state.source = NULL;
        }

        muted_sources.clear();
}

void play_clip(const char * filepath) {
        bool sound = false;
        ++queue;
        {
                std::lock_guard<std::mutex> lock(audioMutex);
                if (likely(queue == 1)) {
                        sdlmixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
                        if (unlikely(NULL == sdlmixer))
                                EXIT_WITH_ERROR(__LINE__);
                }
        }

        audio = MIX_LoadAudio(NULL, filepath, false);
        if (unlikely(NULL == audio))
                EXIT_WITH_ERROR(__LINE__);

        track = MIX_CreateTrack(sdlmixer);
        if (unlikely(!track))
                EXIT_WITH_ERROR(__LINE__);

        if (unlikely(!MIX_SetTrackAudio(track, audio)))
                EXIT_WITH_ERROR(__LINE__);

        sound = MIX_PlayTrack(track, 0);

        if (unlikely(!sound))
                EXIT_WITH_ERROR(__LINE__);

        while (MIX_TrackPlaying(track) && sound) {
                SDL_Delay(60 + 30 * (queue - 1));
        }

exit:
        if(likely(NULL != track)) MIX_DestroyTrack(track);
        if(likely(NULL != audio)) MIX_DestroyAudio(audio);
        {
                std::lock_guard<std::mutex> lock(audioMutex);
                --queue;
                if (queue == 0 && NULL != sdlmixer) {
                        MIX_DestroyMixer(sdlmixer);
                        sdlmixer = NULL;
                }
        }
        return;
}

void play_sound(std::string file_name) {
        const char * obs_data_path = obs_get_module_data_path(obs_current_module());
        std::stringstream audio_path;
        std::string true_path;

        audio_path << obs_data_path;
        audio_path << file_name;
        true_path = clean_path(audio_path.str());
        play_clip(true_path.c_str());
        audio_path.str("");

        return;
}

static void play_record_start_sound_with_optional_source_mute() {
        RecordStartMuteConfig config = load_record_start_mute_config();

        if (!config.enabled || config.source_names.empty()) {
                play_sound("/record_start_sound.mp3");
                return;
        }

        std::vector<SourceMuteState> muted_sources = mute_configured_sources(config.source_names);

        if (muted_sources.empty()) {
                play_sound("/record_start_sound.mp3");
                return;
        }

        auto mute_start = std::chrono::steady_clock::now();
        play_sound("/record_start_sound.mp3");

        uint32_t elapsed_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - mute_start).count();

        if (config.duration_ms > elapsed_ms) {
                SDL_Delay(config.duration_ms - elapsed_ms);
        }

        restore_muted_sources(muted_sources);
}

void obsstudio_srbeep_frontend_event_callback(enum obs_frontend_event event, void * private_data) {
        if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
                if (st_stt_Thread.joinable()) {
                        st_stt_Thread.join();
                }
                st_stt_Thread = std::thread(play_sound, "/stream_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
                if (rc_stt_Thread.joinable()) {
                        rc_stt_Thread.join();
                }
                rc_stt_Thread = std::thread(play_record_start_sound_with_optional_source_mute);
        } else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED) {
                if (bf_stt_Thread.joinable()) {
                        bf_stt_Thread.join();
                }
                bf_stt_Thread = std::thread(play_sound, "/buffer_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_PAUSED) {
                if (ps_stt_Thread.joinable()) {
                        ps_stt_Thread.join();
                }
                ps_stt_Thread = std::thread(play_sound, "/pause_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
                if (st_sto_Thread.joinable()) {
                        st_sto_Thread.join();
                }
                st_sto_Thread = std::thread(play_sound, "/stream_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
                if (rc_sto_Thread.joinable()) {
                        rc_sto_Thread.join();
                }
                rc_sto_Thread = std::thread(play_sound, "/record_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED) {
                if (bf_sto_Thread.joinable()) {
                        bf_sto_Thread.join();
                }
                bf_sto_Thread = std::thread(play_sound, "/buffer_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED) {
                if (ps_stt_Thread.joinable()) {
                        ps_stt_Thread.join();
                }
                ps_stt_Thread = std::thread(play_sound, "/pause_stop_sound.mp3");
        }
}

extern "C" void obs_module_unload(void) {
        if (st_stt_Thread.joinable()) {
                st_stt_Thread.join();
        }
        if (st_sto_Thread.joinable()) {
                st_sto_Thread.join();
        }
        if (rc_stt_Thread.joinable()) {
                rc_stt_Thread.join();
        }
        if (rc_sto_Thread.joinable()) {
                rc_sto_Thread.join();
        }
        if (bf_stt_Thread.joinable()) {
                bf_stt_Thread.join();
        }
        if (bf_sto_Thread.joinable()) {
                bf_sto_Thread.join();
        }
        if (ps_stt_Thread.joinable()) {
                ps_stt_Thread.join();
        }
        if (ps_sto_Thread.joinable()) {
                ps_sto_Thread.join();
        }
        MIX_Quit();
        SDL_Quit();
        return;
}

extern "C" const char * obs_module_author(void) {
        return "EBK21";
}

extern "C" const char * obs_module_name(void) {
        return "Stream/Recording Start/Stop Beeps";
}

extern "C" const char * obs_module_description(void) {
        return "Adds audio sound when streaming/recording/buffer starts/stops or when recording is paused/unpaused.";
}

extern "C" bool obs_module_load(void) {
        SDL_Init(0);
        MIX_Init();
        obs_frontend_add_event_callback(obsstudio_srbeep_frontend_event_callback, 0);
        return true;
}
