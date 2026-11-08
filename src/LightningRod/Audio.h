#pragma once

#include <windows.h>
#include <string>

namespace tlr {

// Sets the master mute of the node's default playback device. On Vista and
// later this goes through Core Audio (IAudioEndpointVolume on the default
// render endpoint); on XP, where Core Audio does not exist, through the
// multimedia mixer API on the first mixer's speaker line. mutedNow receives
// the state read back afterwards, method the API that did it. Returns false
// with error set when no API could set the state or the read-back state is
// not what was asked for. The calling thread must have initialised COM.
bool setAudioMute(bool on, bool& mutedNow, std::string& method, std::string& error);

} // namespace tlr
