#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace tlr {

// Sound categories and events shared by Thunderhead and Lightning Rod, and the
// playback engine both use. See docs/PROTOCOL.md under Audio.

// The 38 categories are the files of assets\sounds, each named
// "ThunderheadLightningRod <Category>.wav"; together they are the default
// scheme. A custom scheme assigns another file to a category.
const int kSoundCategoryCount = 38;
extern const wchar_t* const kSoundCategories[kSoundCategoryCount];
const wchar_t* const kSoundFilePrefix = L"ThunderheadLightningRod ";
const wchar_t* const kSoundFolder = L"sounds";      // beside the executable, in both applications
std::wstring soundDefaultFile(const wchar_t* category);    // "ThunderheadLightningRod Startup.wav"
int soundCategoryIndex(const std::wstring& category);      // -1 when the name is not a category

// The events that play a sound, each tied to one category. key names the
// event in the SOUND scheme message Thunderhead sends to an agent.
enum SoundEvent {
    SoundThOpen, SoundThHello, SoundThClose, SoundThClientOffline,
    SoundLrOpen, SoundLrHello, SoundLrClose, SoundLrReport, SoundLrBadIp,
    kSoundEventCount
};
struct SoundEventDef { SoundEvent event; const wchar_t* app; const wchar_t* label; const wchar_t* category; const char* key; };
extern const SoundEventDef kSoundEvents[kSoundEventCount];
const SoundEventDef* soundEventOfCategory(const wchar_t* category);   // NULL when no event uses the category

// ---- playback -------------------------------------------------------------
// .wav files go through PlaySound from a copy held in memory after the first
// play (SND_MEMORY), so that an event sound starts without a disk read; every
// other file (.mp3) goes through the multimedia command interface (MCI, the
// MPEGVideo device), which decodes without any player process. Playback is
// asynchronous: soundPlay returns at once and a watcher closes the MCI device
// the moment the sound has ended, so that nothing stays open or buzzing.
// A new sound replaces one still playing. The functions may be called from any
// thread. Paths are full paths.
void soundSetMuted(bool muted);          // while muted every play is a silent success
bool soundMuted();
bool soundFileSupported(const std::wstring& path);                // .wav or .mp3 by extension
std::vector<std::wstring> soundFilesIn(const std::wstring& dir);  // names of the supported files, sorted
bool soundPlay(const std::wstring& path, std::string& error);
// Plays and returns when the sound has ended, or after maxMs, whichever is
// first; for the sound an application plays as it closes.
bool soundPlayAndWait(const std::wstring& path, DWORD maxMs, std::string& error);
void soundStop();        // stops whatever plays and closes the devices
void soundShutdown();    // soundStop and frees the held copies; last call before the process ends

} // namespace tlr
