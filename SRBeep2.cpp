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
#include <condition_variable>
#include <deque>
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
        bool skip_already_muted_sources = true;
        uint32_t max_duration_limit_ms = 0;
        std::vector<std::string> source_names;
};

struct SourceMuteState {
        std::string name;
        bool was_muted = false;
        obs_source_t *source = NULL;
};

std::mutex audioMutex;
std::thread audioWorkerThread;
std::mutex audioJobMutex;
std::condition_variable audioJobCv;
bool audioWorkerRunning = false;
std::atomic_int queue = 0;

struct SoundJob {
        std::string file_name;
        bool apply_record_start_mute = false;
};

std::deque<SoundJob> audioJobs;

MIX_Mixer* sdlmixer = NULL;
thread_local MIX_Audio* audio = NULL;
thread_local MIX_Track* track = NULL;

OBS_DECLARE_MODULE()

std::string clean_path(std::string audio_path) {
        std::string cleaned_path;
        if (audio_path.empty()) {
                return cleaned_path;
        }

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

static uint32_t parse_nonnegative_u32(const std::string &value, uint32_t fallback, uint32_t max_value) {
        std::string trimmed = trim_copy(value);
        if (trimmed.empty()) {
                return fallback;
        }

        try {
                long long parsed = std::stoll(trimmed);
                if (parsed < 0) {
                        return fallback;
                }
                if (parsed > max_value) {
                        return max_value;
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

static RecordStartMuteConfig load_record_config() {
        RecordStartMuteConfig config;

        const char *obs_data_path = obs_get_module_data_path(obs_current_module());
        std::stringstream config_path;
        config_path << obs_data_path;
        config_path << "/record_config.ini";
        std::string true_path = clean_path(config_path.str());

        std::ifstream file(true_path);
        if (!file.is_open()) {
                blog(LOG_INFO, "SRBeep2: record_config.ini not found. Source muting is disabled.");
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
                } else if (key == "skip_already_muted_sources") {
                        config.skip_already_muted_sources = parse_bool_value(value);
                } else if (key == "max_duration_limit") {
                        uint32_t seconds = parse_nonnegative_u32(value, config.max_duration_limit_ms / 1000, 3600);
                        config.max_duration_limit_ms = seconds * 1000;
                } else if (key == "sources") {
                        config.source_names = parse_sources_csv(value);
                }
        }

        return config;
}

static std::vector<SourceMuteState> mute_configured_sources(const RecordStartMuteConfig &config) {
        std::vector<SourceMuteState> muted_sources;

        for (const std::string &name : config.source_names) {
                obs_source_t *source = obs_get_source_by_name(name.c_str());
                if (!source) {
                        blog(LOG_WARNING, "SRBeep2: Source '%s' was not found for temporary muting.", name.c_str());
                        continue;
                }

                bool was_muted = obs_source_muted(source);
                if (config.skip_already_muted_sources && was_muted) {
                        obs_source_release(source);
                        continue;
                }
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

static void restore_muted_sources(std::vector<SourceMuteState> &muted_sources, bool skip_already_muted_sources) {
        for (SourceMuteState &state : muted_sources) {
                if (!state.source) {
                        continue;
                }

                if (skip_already_muted_sources) {
                        obs_source_set_muted(state.source, state.was_muted);
                } else {
                        obs_source_set_muted(state.source, false);
                }
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
        RecordStartMuteConfig config = load_record_config();

        if (!config.enabled || config.source_names.empty()) {
                play_sound("/record_start_sound.mp3");
                return;
        }

        std::vector<SourceMuteState> muted_sources = mute_configured_sources(config);

        if (muted_sources.empty()) {
                play_sound("/record_start_sound.mp3");
                return;
        }

        std::atomic_bool playback_finished = false;
        std::mutex restore_mutex;
        bool restored = false;
        auto restore_once = [&]() {
                std::lock_guard<std::mutex> lock(restore_mutex);
                if (restored) {
                        return;
                }
                restore_muted_sources(muted_sources, config.skip_already_muted_sources);
                restored = true;
        };

        std::thread max_duration_thread;
        if (config.max_duration_limit_ms > 0) {
                max_duration_thread = std::thread([&]() {
                        uint32_t waited_ms = 0;
                        while (!playback_finished.load() && waited_ms < config.max_duration_limit_ms) {
                                const uint32_t step_ms = 20;
                                SDL_Delay(step_ms);
                                waited_ms += step_ms;
                        }

                        if (!playback_finished.load()) {
                                restore_once();
                        }
                });
        }

        play_sound("/record_start_sound.mp3");
        playback_finished = true;
        restore_once();

        if (max_duration_thread.joinable()) {
                max_duration_thread.join();
        }
}

static void queue_sound_job(const char *file_name, bool apply_record_start_mute = false) {
        {
                std::lock_guard<std::mutex> lock(audioJobMutex);
                SoundJob job;
                if (file_name != NULL) {
                        job.file_name = file_name;
                }
                job.apply_record_start_mute = apply_record_start_mute;
                audioJobs.push_back(job);
        }
        audioJobCv.notify_one();
}

static void audio_worker_loop() {
        while (true) {
                SoundJob job;
                {
                        std::unique_lock<std::mutex> lock(audioJobMutex);
                        audioJobCv.wait(lock, [] {
                                return !audioWorkerRunning || !audioJobs.empty();
                        });

                        if (!audioWorkerRunning && audioJobs.empty()) {
                                return;
                        }

                        job = audioJobs.front();
                        audioJobs.pop_front();
                }

                if (job.apply_record_start_mute) {
                        play_record_start_sound_with_optional_source_mute();
                } else if (!job.file_name.empty()) {
                        play_sound(job.file_name);
                }
        }
}

void obsstudio_srbeep_frontend_event_callback(enum obs_frontend_event event, void * private_data) {
        if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
                queue_sound_job("/stream_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
                queue_sound_job("/record_start_sound.mp3", true);
        } else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED) {
                queue_sound_job("/buffer_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_PAUSED) {
                queue_sound_job("/pause_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
                queue_sound_job("/stream_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
                queue_sound_job("/record_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED) {
                queue_sound_job("/buffer_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED) {
                queue_sound_job("/pause_stop_sound.mp3");
        }
}

extern "C" void obs_module_unload(void) {
        obs_frontend_remove_event_callback(obsstudio_srbeep_frontend_event_callback, 0);
        {
                std::lock_guard<std::mutex> lock(audioJobMutex);
                audioWorkerRunning = false;
        }
        audioJobCv.notify_all();
        if (audioWorkerThread.joinable()) {
                audioWorkerThread.join();
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
        if (SDL_Init(0) < 0) {
                blog(LOG_ERROR, "SRBeep2: SDL_Init failed: %s", SDL_GetError());
                return false;
        }
        if (!MIX_Init()) {
                blog(LOG_ERROR, "SRBeep2: MIX_Init failed: %s", SDL_GetError());
                SDL_Quit();
                return false;
        }
        {
                std::lock_guard<std::mutex> lock(audioJobMutex);
                audioWorkerRunning = true;
        }
        audioWorkerThread = std::thread(audio_worker_loop);
        obs_frontend_add_event_callback(obsstudio_srbeep_frontend_event_callback, 0);
        return true;
}
