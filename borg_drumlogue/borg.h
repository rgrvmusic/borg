// Unit state and switch interface.

#ifndef BORG_DRUMLOGUE_BORG_H_
#define BORG_DRUMLOGUE_BORG_H_

#include <cstdint>

namespace borg {

// Read the shim marker; do not perform device I/O here.
void Init(uint32_t samplerate);

// Called from the render thread.
void Render(float* out, uint32_t frames);

// Request a switch from the render thread.
void Request();

const char* StateTag();

}  // namespace borg

#endif  // BORG_DRUMLOGUE_BORG_H_
