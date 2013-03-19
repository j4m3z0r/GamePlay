/**
 * Stubs for the handful of OpenAL methods used so that we can build correctly.
 */

#include "al.h"
#include "alc.h"

AL_API ALenum AL_APIENTRY alGetError( void )
{
    return 0;
}

ALC_API ALCboolean      ALC_APIENTRY alcCloseDevice( ALCdevice *device )
{
    return 1;
}

ALC_API ALCboolean      ALC_APIENTRY alcMakeContextCurrent( ALCcontext *context )
{
    return 1;
}

ALC_API void            ALC_APIENTRY alcDestroyContext( ALCcontext *context ) { }

AL_API void AL_APIENTRY alSourcei( ALuint sid, ALenum param, ALint value ) { }

AL_API void AL_APIENTRY alSourcef( ALuint sid, ALenum param, ALfloat value ) { }

AL_API void AL_APIENTRY alSourcefv( ALuint sid, ALenum param, const ALfloat* values ) { }

AL_API void AL_APIENTRY alGenSources( ALsizei n, ALuint* sources ) { }

AL_API void AL_APIENTRY alDeleteSources( ALsizei n, const ALuint* sources ) { }

