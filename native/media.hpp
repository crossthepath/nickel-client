#pragma once
#include <string>
#include <vector>
// Media playback on the PC (Spotify Manager) through the Windows
// GlobalSystemMediaTransportControlsSessionManager API; nothing is read from or written to the game.
namespace media{
struct State{bool error=false,available=false,playing=false,canToggle=false,canPrevious=false,canNext=false,canSeek=false;std::string title,artist,album;double position=0,duration=0;};
void Start();                               // starts the polling thread (once, on first request)
State Get();                                // last known state, position extrapolated while playing
void Command(int which,double fraction=0);
// Current track cover (RGBA); `serial` changes with each new image, 0 = no cover.
unsigned Cover(std::vector<unsigned char>* rgba,int* width,int* height);
// Decodes an image (PNG, JPEG...) to RGBA with WIC; initialises COM for the call if needed.
bool DecodeImage(const unsigned char* bytes,size_t size,std::vector<unsigned char>* rgba,int* width,int* height);  // 0 play / pause, 1 previous, 2 next, 3 seek to "fraction" of the duration
}
