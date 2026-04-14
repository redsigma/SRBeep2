# SRBeep2

SRBeep2 is an OBS plugin that plays audio cues for recording state changes:
- Start / Stop recording
- Pause / Unpause recording

It also supports optional temporary source muting and input capture device enforcement.

## Install

### Windows
Copy plugin files into your OBS install:
- `obs-plugins/64bit/srbeep2.dll`
- `data/obs-plugins/srbeep2/*`

### Linux
Copy plugin files into your OBS user config:
- `~/.config/obs-studio/plugins/srbeep2/bin/64bit/srbeep2.so`
- `~/.config/obs-studio/plugins/srbeep2/data/*`

### Linux (Flatpak OBS)
Copy plugin files into Flatpak OBS config:
- `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/srbeep2/bin/64bit/srbeep2.so`
- `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/srbeep2/data/*`

Note: `record_config.ini` is loaded from the plugin data root (`.../data/record_config.ini`).

## Configuration

Default config file location:
- `<plugin data dir>/record_config.ini`

Example:

```ini
; 0 = all beeps off (input capture enforcement can still run)
; 1 = play beep + immediate OBS action (optional temporary source mute)
; 2 = deferred hotkey mode (beep first, action after beep finishes)
record_beep_mode=1

; Mode 2 fallback hotkeys (used only when UI hotkeys are unbound)
record_toggle_hotkey_fallback=CTRL+ALT+R
pause_toggle_hotkey_fallback=

; Mode 1 source mute behavior
skip_already_muted_sources=true
max_duration_limit=0
sources=Mic/Aux,Desktop Audio

; Stop-interrupt fade in seconds (supports decimals)
audio_fade_on_stop_duration=0.25

; Optional input capture device enforcement
enable_input_capture_enforcement=false
input_capture_target=Audio Input Capture
input_capture_device=Shure,Yeti Stereo,Blue Yeti
```

### Mode behavior summary
- Mode `0`: No beeps. Input capture enforcement can still run if enabled
- Mode `1`: OBS action happens immediately; beep plays at the event moment.
- Mode `2`: Use SRBeep2 hotkeys in OBS. Start/unpause action is deferred until beep playback ends.

## OBS hotkeys (used in Mode 2)
Bind these in **Settings -> Hotkeys**:
- `SRBeep2: Toggle Recording`
- `SRBeep2: Toggle Pause`

## Troubleshooting
- If plugin loads but config is ignored, confirm `record_config.ini` is in the plugin `data` root.
- OBS logs shows when `record_config.ini` was not loaded. If the config is missing, SRBeep2 defaults to `record_beep_mode=0`.
- If audio files do not play, place them near `record_config.ini`.
- Check OBS logs via **Help -> Log Files -> View Current Log** and search for `SRBeep2`.
