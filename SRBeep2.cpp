/***********************************
Orig: A Docile Sloth adocilesloth@gmail.com
Now: EBK21 chkd13303@gmail.com
Modified by: redsigma (2026-04)
Changes: audio queue/mute logic, input-capture enforcement, audio fading logic
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
#include <cmath>
#include <filesystem>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define unused(x) (void)(x)

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <SDL3_mixer/SDL_mixer.h>


struct RecordStartMuteConfig {
        uint32_t enabled_mode = 0;
        bool skip_already_muted_sources = true;
        bool enable_input_capture_enforcement = false;
        uint32_t max_duration_limit_ms = 0;
        uint32_t audio_fade_on_stop_duration_ms = 0;
        std::string input_capture_target;
        std::vector<std::string> input_capture_device_names;
        std::string record_toggle_hotkey_fallback;
        std::string pause_toggle_hotkey_fallback;
        std::vector<std::string> source_names;
};

struct SourceMuteState {
        std::string name;
        obs_source_t *source = NULL;
        bool was_muted = false;
};

static constexpr uint32_t max_duration_poll_step_ms = 20;
static constexpr uint32_t shutdown_wait_timeout_ms = 2000;

struct SoundJob {
        enum class DeferredAction {
                None,
                StartRecording,
                UnpauseRecording,
        };

        std::string file_name;
        bool apply_record_start_mute = false;
        bool clear_interrupt_before_play = false;
        DeferredAction deferred_action = DeferredAction::None;
        uint64_t deferred_start_pending_action_id = 0;
        uint64_t deferred_unpause_pending_action_id = 0;
};

std::mutex audioMutex;
std::atomic_int queue = 0;

std::thread audioWorkerThread;
std::mutex audioJobMutex;
std::condition_variable audioJobCv;
bool audioWorkerRunning = false;
std::deque<SoundJob> audioJobs;

std::atomic_bool interrupt_current_track = false;
std::atomic_uint32_t audio_fade_on_stop_duration_ms = 0;
std::atomic_bool shutdown_requested = false;
std::atomic_uint64_t deferred_start_pending_action_id_counter = 0;
std::atomic_uint64_t deferred_start_active_pending_action_id = 0;
std::atomic_uint64_t deferred_unpause_pending_action_id_counter = 0;
std::atomic_uint64_t deferred_unpause_active_pending_action_id = 0;
std::atomic_bool deferred_start_commit_in_progress = false;

std::mutex playbackStateMutex;
std::condition_variable playbackStateCv;
bool activeTrackStopped = false;

MIX_Mixer* sdlmixer = NULL;
thread_local MIX_Audio* audio = NULL;
thread_local MIX_Track* track = NULL;

obs_hotkey_id record_toggle_hotkey = OBS_INVALID_HOTKEY_ID;
obs_hotkey_id pause_toggle_hotkey = OBS_INVALID_HOTKEY_ID;

OBS_DECLARE_MODULE()

static bool mode_uses_deferred_hotkeys();

static void SDLCALL on_track_stopped(void *userdata, MIX_Track *stopped_track) {
        unused(userdata);
        unused(stopped_track);
        {
                std::lock_guard<std::mutex> lock(playbackStateMutex);
                activeTrackStopped = true;
        }
        playbackStateCv.notify_all();
}

static std::u8string to_u8string(const std::string &value) {
        return std::u8string(value.begin(), value.end());
}

static std::string module_data_file_path(const std::string &relative_path) {
        const char *module_data_path = obs_get_module_data_path(obs_current_module());
        if (!module_data_path) {
                return relative_path;
        }

        std::string cleaned_relative = relative_path;
        while (!cleaned_relative.empty() && (cleaned_relative[0] == '/' || cleaned_relative[0] == '\\')) {
                cleaned_relative.erase(0, 1);
        }

        std::filesystem::path base_path(to_u8string(module_data_path));
        std::filesystem::path relative(to_u8string(cleaned_relative));
        std::filesystem::path full_path = (base_path / relative).lexically_normal();
        std::u8string utf8 = full_path.u8string();
        return std::string(utf8.begin(), utf8.end());
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

static bool contains_case_insensitive(const std::string &haystack, const std::string &needle) {
        if (needle.empty()) {
                return false;
        }

        std::string lowered_haystack = lowercase_copy(haystack);
        std::string lowered_needle = lowercase_copy(needle);
        return lowered_haystack.find(lowered_needle) != std::string::npos;
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

static uint32_t parse_enabled_mode(const std::string &value, uint32_t fallback_mode) {
        std::string trimmed = trim_copy(value);
        if (trimmed.empty()) {
                return fallback_mode;
        }

        std::string lowered = lowercase_copy(trimmed);
        if (lowered == "true" || lowered == "yes" || lowered == "on") {
                return 1;
        }
        if (lowered == "false" || lowered == "no" || lowered == "off") {
                return 0;
        }

        uint32_t parsed = parse_nonnegative_u32(trimmed, fallback_mode, 2);
        if (parsed > 2) {
                return fallback_mode;
        }
        return parsed;
}

static uint32_t parse_seconds_to_ms(const std::string &value, uint32_t fallback_ms) {
        std::string trimmed = trim_copy(value);
        if (trimmed.empty()) {
                return fallback_ms;
        }

        try {
                double seconds = std::stod(trimmed);
                if (seconds < 0.0) {
                        return fallback_ms;
                }

                double milliseconds = seconds * 1000.0;

                return (uint32_t)std::llround(milliseconds);
        } catch (...) {
                return fallback_ms;
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

static std::vector<std::string> split_plus_tokens(const std::string &value) {
        std::vector<std::string> tokens;
        std::stringstream parser(value);
        std::string token;

        while (std::getline(parser, token, '+')) {
                std::string cleaned = trim_copy(token);
                if (!cleaned.empty()) {
                        tokens.push_back(cleaned);
                }
        }

        return tokens;
}

static obs_key_t parse_hotkey_main_key_token(const std::string &token) {
        if (token.empty()) {
                return OBS_KEY_NONE;
        }

        std::string upper = token;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return (char)std::toupper(c); });

        if (upper == "ESC") {
                upper = "ESCAPE";
        } else if (upper == "ENTER") {
                upper = "RETURN";
        } else if (upper == "DEL") {
                upper = "DELETE";
        } else if (upper == "INS") {
                upper = "INSERT";
        } else if (upper == "PGUP") {
                upper = "PAGEUP";
        } else if (upper == "PGDN") {
                upper = "PAGEDOWN";
        }

        if (upper.rfind("OBS_KEY_", 0) == 0) {
                return obs_key_from_name(upper.c_str());
        }

        std::string prefixed = "OBS_KEY_" + upper;
        return obs_key_from_name(prefixed.c_str());
}

static bool parse_hotkey_combination(const std::string &spec, obs_key_combination_t &combo_out) {
        combo_out.modifiers = 0;
        combo_out.key = OBS_KEY_NONE;

        std::vector<std::string> tokens = split_plus_tokens(spec);
        if (tokens.empty()) {
                return false;
        }

        bool found_main_key = false;
        for (const std::string &token : tokens) {
                std::string lowered = lowercase_copy(token);
                if (lowered == "ctrl" || lowered == "control") {
                        combo_out.modifiers |= INTERACT_CONTROL_KEY;
                        continue;
                }
                if (lowered == "alt") {
                        combo_out.modifiers |= INTERACT_ALT_KEY;
                        continue;
                }
                if (lowered == "shift") {
                        combo_out.modifiers |= INTERACT_SHIFT_KEY;
                        continue;
                }
                if (lowered == "win" || lowered == "windows" || lowered == "meta" || lowered == "command" || lowered == "cmd") {
                        combo_out.modifiers |= INTERACT_COMMAND_KEY;
                        continue;
                }

                if (found_main_key) {
                        return false;
                }

                combo_out.key = parse_hotkey_main_key_token(token);
                if (combo_out.key == OBS_KEY_NONE) {
                        return false;
                }
                found_main_key = true;
        }

        return found_main_key;
}

static RecordStartMuteConfig load_record_config() {
        RecordStartMuteConfig config;

        std::string true_path = module_data_file_path("record_config.ini");

        std::ifstream file(true_path);
        if (!file.is_open()) {
                blog(LOG_INFO, "SRBeep2: record_config.ini not found. Mode 0 (all beeps off) is active.");
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

                if (key == "record_beep_mode") {
                        config.enabled_mode = parse_enabled_mode(value, config.enabled_mode);
                } else if (key == "skip_already_muted_sources") {
                        config.skip_already_muted_sources = parse_bool_value(value);
                } else if (key == "enable_input_capture_enforcement") {
                        config.enable_input_capture_enforcement = parse_bool_value(value);
                } else if (key == "max_duration_limit") {
                        uint32_t seconds = parse_nonnegative_u32(value, config.max_duration_limit_ms / 1000, 3600);
                        config.max_duration_limit_ms = seconds * 1000;
                } else if (key == "audio_fade_on_stop_duration") {
                        config.audio_fade_on_stop_duration_ms = parse_seconds_to_ms(value, config.audio_fade_on_stop_duration_ms);
                } else if (key == "input_capture_target") {
                        config.input_capture_target = value;
                } else if (key == "input_capture_device") {
                        config.input_capture_device_names = parse_sources_csv(value);
                } else if (key == "record_toggle_hotkey_fallback") {
                        config.record_toggle_hotkey_fallback = value;
                } else if (key == "pause_toggle_hotkey_fallback") {
                        config.pause_toggle_hotkey_fallback = value;
                } else if (key == "sources") {
                        config.source_names = parse_sources_csv(value);
                }
        }

        return config;
}

static void enforce_input_capture_device(const RecordStartMuteConfig &config) {
        if (!config.enable_input_capture_enforcement || config.input_capture_target.empty() || config.input_capture_device_names.empty()) {
                return;
        }

        obs_source_t *target_source = obs_get_source_by_name(config.input_capture_target.c_str());
        if (!target_source) {
                blog(LOG_WARNING, "SRBeep2: Input capture target '%s' not found.", config.input_capture_target.c_str());
                return;
        }

        obs_properties_t *properties = obs_source_properties(target_source);
        if (!properties) {
                blog(LOG_WARNING, "SRBeep2: Could not read properties for source '%s'.", config.input_capture_target.c_str());
                obs_source_release(target_source);
                return;
        }

        obs_property_t *device_property = obs_properties_get(properties, "device_id");
        if (!device_property) {
                blog(LOG_WARNING, "SRBeep2: Source '%s' does not expose a 'device_id' property.", config.input_capture_target.c_str());
                obs_properties_destroy(properties);
                obs_source_release(target_source);
                return;
        }

        const size_t item_count = obs_property_list_item_count(device_property);
        std::string matched_device_id;
        std::string matched_device_name;

        for (const std::string &candidate_name : config.input_capture_device_names) {
                for (size_t i = 0; i < item_count; ++i) {
                        const char *list_name = obs_property_list_item_name(device_property, i);
                        const char *list_id = obs_property_list_item_string(device_property, i);
                        if (!list_name || !list_id) {
                                continue;
                        }

                        if (!contains_case_insensitive(list_name, candidate_name)) {
                                continue;
                        }

                        matched_device_id = list_id;
                        matched_device_name = list_name;
                        break;
                }

                if (!matched_device_id.empty()) {
                        break;
                }
        }

        obs_properties_destroy(properties);

        if (matched_device_id.empty()) {
                blog(LOG_INFO, "SRBeep2: No matching input capture device found for source '%s'.", config.input_capture_target.c_str());
                obs_source_release(target_source);
                return;
        }

        obs_data_t *settings = obs_source_get_settings(target_source);
        const char *current_device_id = obs_data_get_string(settings, "device_id");
        if (!current_device_id) {
                current_device_id = "";
        }

        if (matched_device_id != current_device_id) {
                obs_data_set_string(settings, "device_id", matched_device_id.c_str());
                obs_source_update(target_source, settings);
                blog(LOG_INFO, "SRBeep2: Source '%s' device set to '%s'.", config.input_capture_target.c_str(), matched_device_name.c_str());
        }

        obs_data_release(settings);
        obs_source_release(target_source);
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
        audio = NULL;
        track = NULL;

        ++queue;
        auto cleanup = [&]() {
                if (likely(NULL != track)) {
                        MIX_DestroyTrack(track);
                        track = NULL;
                }
                if (likely(NULL != audio)) {
                        MIX_DestroyAudio(audio);
                        audio = NULL;
                }
                {
                        std::lock_guard<std::mutex> lock(audioMutex);
                        --queue;
                        if (queue == 0 && NULL != sdlmixer) {
                                MIX_DestroyMixer(sdlmixer);
                                sdlmixer = NULL;
                        }
                }
        };
        auto fail = [&](int line) {
                blog(LOG_ERROR, "Failed to play audio! At %s:%d", __FILE__, line);
                blog(LOG_ERROR, "%s", SDL_GetError());
                cleanup();
        };

        {
                std::lock_guard<std::mutex> lock(audioMutex);
                if (likely(queue == 1)) {
                        sdlmixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
                        if (unlikely(NULL == sdlmixer)) {
                                fail(__LINE__);
                                return;
                        }
                }
        }

        audio = MIX_LoadAudio(NULL, filepath, false);
        if (unlikely(NULL == audio)) {
                fail(__LINE__);
                return;
        }

        track = MIX_CreateTrack(sdlmixer);
        if (unlikely(!track)) {
                fail(__LINE__);
                return;
        }

        if (unlikely(!MIX_SetTrackStoppedCallback(track, on_track_stopped, NULL))) {
                fail(__LINE__);
                return;
        }

        if (unlikely(!MIX_SetTrackAudio(track, audio))) {
                fail(__LINE__);
                return;
        }

        {
                std::lock_guard<std::mutex> lock(playbackStateMutex);
                activeTrackStopped = false;
        }

        sound = MIX_PlayTrack(track, 0);

        if (unlikely(!sound)) {
                fail(__LINE__);
                return;
        }

        bool stop_requested_for_track = false;
        bool done_waiting_for_track = false;
        while (!done_waiting_for_track) {
                bool track_stopped = false;
                {
                        std::unique_lock<std::mutex> lock(playbackStateMutex);
                        playbackStateCv.wait_for(lock, std::chrono::milliseconds(shutdown_wait_timeout_ms), [] {
                                return activeTrackStopped || interrupt_current_track.load() || shutdown_requested.load();
                        });
                        track_stopped = activeTrackStopped;
                }

                if (track_stopped) {
                        done_waiting_for_track = true;
                        continue;
                }

                if (shutdown_requested.load()) {
                        if (!stop_requested_for_track) {
                                MIX_StopTrack(track, 0);
                        }
                        done_waiting_for_track = true;
                        continue;
                }

                if (stop_requested_for_track) {
                        continue;
                }

                if (!interrupt_current_track.exchange(false)) {
                        continue;
                }

                const uint32_t fade_ms = audio_fade_on_stop_duration_ms.load();
                Sint64 fade_out_frames = (fade_ms > 0) ? MIX_TrackMSToFrames(track, (Sint64)fade_ms) : 0;
                if (fade_out_frames < 0) {
                        fade_out_frames = 0;
                }

                MIX_StopTrack(track, fade_out_frames);
                stop_requested_for_track = true;
        }
        cleanup();
}

void play_sound(std::string file_name) {
        std::string true_path = module_data_file_path(file_name);
        play_clip(true_path.c_str());
}

static void play_record_start_sound_with_optional_source_mute() {
        RecordStartMuteConfig config = load_record_config();
        audio_fade_on_stop_duration_ms = config.audio_fade_on_stop_duration_ms;
        enforce_input_capture_device(config);

        if (config.enabled_mode != 1 || config.source_names.empty()) {
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
                                SDL_Delay(max_duration_poll_step_ms);
                                waited_ms += max_duration_poll_step_ms;
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

static void queue_deferred_sound_job(const char *file_name, bool apply_record_start_mute, SoundJob::DeferredAction deferred_action) {
        {
                std::lock_guard<std::mutex> lock(audioJobMutex);
                SoundJob job;
                if (file_name != NULL) {
                        job.file_name = file_name;
                }
                job.apply_record_start_mute = apply_record_start_mute;
                job.clear_interrupt_before_play = true;
                job.deferred_action = deferred_action;
                if (deferred_action == SoundJob::DeferredAction::StartRecording) {
                        const uint64_t token = deferred_start_pending_action_id_counter.fetch_add(1) + 1;
                        job.deferred_start_pending_action_id = token;
                        deferred_start_active_pending_action_id = token;
                } else if (deferred_action == SoundJob::DeferredAction::UnpauseRecording) {
                        const uint64_t token = deferred_unpause_pending_action_id_counter.fetch_add(1) + 1;
                        job.deferred_unpause_pending_action_id = token;
                        deferred_unpause_active_pending_action_id = token;
                }
                audioJobs.push_back(job);
        }
        audioJobCv.notify_one();
}

static void queue_stop_sound_job(const char *file_name) {
        {
                std::lock_guard<std::mutex> lock(audioJobMutex);
                interrupt_current_track = true;
                deferred_start_active_pending_action_id = 0;
                deferred_unpause_active_pending_action_id = 0;
                deferred_start_commit_in_progress = false;
                audioJobs.clear();

                SoundJob job;
                if (file_name != NULL) {
                        job.file_name = file_name;
                }
                job.apply_record_start_mute = false;
                job.clear_interrupt_before_play = true;
                audioJobs.push_back(job);
        }
        playbackStateCv.notify_all();
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

                if (job.clear_interrupt_before_play) {
                        interrupt_current_track = false;
                }

                if (job.apply_record_start_mute) {
                        play_record_start_sound_with_optional_source_mute();
                } else if (!job.file_name.empty()) {
                        play_sound(job.file_name);
                }

                if (shutdown_requested.load()) {
                        continue;
                }

                if (job.deferred_action == SoundJob::DeferredAction::StartRecording) {
                        deferred_start_commit_in_progress = true;

                        if (!mode_uses_deferred_hotkeys()) {
                                uint64_t expected = job.deferred_start_pending_action_id;
                                if (expected != 0) {
                                        deferred_start_active_pending_action_id.compare_exchange_strong(expected, 0);
                                }
                                deferred_start_commit_in_progress = false;
                                continue;
                        }

                        uint64_t expected = job.deferred_start_pending_action_id;
                        if (expected == 0 || !deferred_start_active_pending_action_id.compare_exchange_strong(expected, 0)) {
                                deferred_start_commit_in_progress = false;
                                continue;
                        }

                        if (!obs_frontend_recording_active()) {
                                obs_frontend_recording_start();
                        }
                        deferred_start_commit_in_progress = false;
                } else if (job.deferred_action == SoundJob::DeferredAction::UnpauseRecording) {
                        if (!mode_uses_deferred_hotkeys()) {
                                uint64_t expected = job.deferred_unpause_pending_action_id;
                                if (expected != 0) {
                                        deferred_unpause_active_pending_action_id.compare_exchange_strong(expected, 0);
                                }
                                continue;
                        }

                        uint64_t expected = job.deferred_unpause_pending_action_id;
                        if (expected == 0 || !deferred_unpause_active_pending_action_id.compare_exchange_strong(expected, 0)) {
                                continue;
                        }

                        if (obs_frontend_recording_active() && obs_frontend_recording_paused()) {
                                obs_frontend_recording_pause(false);
                        }
                }
        }
}

static bool mode_uses_deferred_hotkeys() {
        RecordStartMuteConfig config = load_record_config();
        return config.enabled_mode == 2;
}

static bool hotkey_has_bindings(obs_hotkey_id hotkey_id) {
        if (hotkey_id == OBS_INVALID_HOTKEY_ID) {
                return false;
        }

        obs_data_array_t *saved = obs_hotkey_save(hotkey_id);
        size_t count = saved ? obs_data_array_count(saved) : 0;
        if (saved) {
                obs_data_array_release(saved);
        }

        return count > 0;
}

static void apply_hotkey_fallback_if_unbound(obs_hotkey_id hotkey_id, const std::string &fallback_spec, const char *hotkey_name) {
        if (hotkey_id == OBS_INVALID_HOTKEY_ID || fallback_spec.empty()) {
                return;
        }

        if (hotkey_has_bindings(hotkey_id)) {
                blog(LOG_INFO, "SRBeep2: '%s' already has UI bindings; .ini fallback ignored.", hotkey_name);
                return;
        }

        obs_key_combination_t combo = {};
        if (!parse_hotkey_combination(fallback_spec, combo)) {
                blog(LOG_WARNING, "SRBeep2: Invalid %s fallback hotkey '%s'.", hotkey_name, fallback_spec.c_str());
                return;
        }

        obs_hotkey_load_bindings(hotkey_id, &combo, 1);
        blog(LOG_INFO, "SRBeep2: Applied .ini fallback hotkey for '%s': %s", hotkey_name, fallback_spec.c_str());
}

static void on_record_toggle_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
        unused(data);
        unused(id);
        unused(hotkey);

        if (!pressed || !mode_uses_deferred_hotkeys()) {
                return;
        }

        if (!obs_frontend_recording_active()) {
                if (deferred_start_active_pending_action_id.load() != 0) {
                        {
                                std::lock_guard<std::mutex> lock(audioJobMutex);
                                deferred_start_active_pending_action_id = 0;
                                interrupt_current_track = true;
                                audioJobs.clear();
                        }
                        playbackStateCv.notify_all();
                        audioJobCv.notify_all();
                        return;
                }
                if (deferred_start_commit_in_progress.load()) {
                        return;
                }

                queue_deferred_sound_job("/record_start_sound.mp3", true, SoundJob::DeferredAction::StartRecording);
                return;
        }

        queue_stop_sound_job("/record_stop_sound.mp3");
}

static void on_pause_toggle_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
        unused(data);
        unused(id);
        unused(hotkey);

        if (!pressed || !mode_uses_deferred_hotkeys()) {
                return;
        }

        if (!obs_frontend_recording_active()) {
                return;
        }

        if (obs_frontend_recording_paused()) {
                if (deferred_unpause_active_pending_action_id.load() != 0) {
                        {
                                std::lock_guard<std::mutex> lock(audioJobMutex);
                                deferred_unpause_active_pending_action_id = 0;
                                interrupt_current_track = true;
                                audioJobs.clear();
                        }
                        playbackStateCv.notify_all();
                        audioJobCv.notify_all();
                        return;
                }
                queue_deferred_sound_job("/pause_stop_sound.mp3", true, SoundJob::DeferredAction::UnpauseRecording);
                return;
        }

        obs_frontend_recording_pause(true);
}

void obsstudio_srbeep_frontend_event_callback(enum obs_frontend_event event, void * private_data) {
        unused(private_data);
        RecordStartMuteConfig config = load_record_config();
        const bool beeps_disabled = config.enabled_mode == 0;
        const bool deferred_mode = config.enabled_mode == 2;

        if (beeps_disabled) {
                if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED || event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED) {
                        enforce_input_capture_device(config);
                }
                return;
        }

        if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
                queue_sound_job("/stream_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_STARTED) {
                if (!deferred_mode) {
                        queue_sound_job("/record_start_sound.mp3", true);
                }
        } else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED) {
                queue_sound_job("/buffer_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_PAUSED) {
                queue_sound_job("/pause_start_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
                queue_stop_sound_job("/stream_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
                queue_stop_sound_job("/record_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED) {
                queue_stop_sound_job("/buffer_stop_sound.mp3");
        } else if (event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED) {
                if (!deferred_mode) {
                        queue_sound_job("/pause_stop_sound.mp3", true);
                }
        }
}

extern "C" void obs_module_unload(void) {
        obs_frontend_remove_event_callback(obsstudio_srbeep_frontend_event_callback, 0);
        if (record_toggle_hotkey != OBS_INVALID_HOTKEY_ID) {
                obs_hotkey_unregister(record_toggle_hotkey);
                record_toggle_hotkey = OBS_INVALID_HOTKEY_ID;
        }
        if (pause_toggle_hotkey != OBS_INVALID_HOTKEY_ID) {
                obs_hotkey_unregister(pause_toggle_hotkey);
                pause_toggle_hotkey = OBS_INVALID_HOTKEY_ID;
        }
        shutdown_requested = true;
        deferred_start_active_pending_action_id = 0;
        deferred_unpause_active_pending_action_id = 0;
        deferred_start_commit_in_progress = false;
        {
                std::lock_guard<std::mutex> lock(audioJobMutex);
                interrupt_current_track = true;
                audioWorkerRunning = false;
                audioJobs.clear();
        }
        playbackStateCv.notify_all();
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
        RecordStartMuteConfig config;

        shutdown_requested = false;
        if (!SDL_Init(0)) {
                blog(LOG_ERROR, "SRBeep2: SDL_Init failed: %s", SDL_GetError());
                return false;
        }
        if (!MIX_Init()) {
                blog(LOG_ERROR, "SRBeep2: MIX_Init failed: %s", SDL_GetError());
                SDL_Quit();
                return false;
        }
        {
                config = load_record_config();
                audio_fade_on_stop_duration_ms = config.audio_fade_on_stop_duration_ms;
        }
        {
                std::lock_guard<std::mutex> lock(audioJobMutex);
                audioWorkerRunning = true;
        }
        audioWorkerThread = std::thread(audio_worker_loop);
        record_toggle_hotkey = obs_hotkey_register_frontend(
                "srbeep2.toggle_recording_deferred",
                "SRBeep2: Toggle Recording",
                on_record_toggle_hotkey,
                NULL
        );
        pause_toggle_hotkey = obs_hotkey_register_frontend(
                "srbeep2.toggle_pause_deferred",
                "SRBeep2: Toggle Pause",
                on_pause_toggle_hotkey,
                NULL
        );

        apply_hotkey_fallback_if_unbound(record_toggle_hotkey, config.record_toggle_hotkey_fallback, "Toggle Recording");
        apply_hotkey_fallback_if_unbound(pause_toggle_hotkey, config.pause_toggle_hotkey_fallback, "Toggle Pause");

        obs_frontend_add_event_callback(obsstudio_srbeep_frontend_event_callback, 0);
        return true;
}


