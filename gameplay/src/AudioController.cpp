// Note, this class will just be stubbed out if NOAUDIO is #defined.

#include "Base.h"
#include "AudioController.h"
#include "AudioListener.h"
#include "AudioBuffer.h"
#include "AudioSource.h"

namespace gameplay
{

AudioController::AudioController() 
    : _alcDevice(NULL), _alcContext(NULL), _pausingSource(NULL)
{
}

AudioController::~AudioController()
{
}

void AudioController::initialize()
{
#ifndef NOAUDIO
    _alcDevice = alcOpenDevice(NULL);
    if (!_alcDevice)
    {
        GP_ERROR("Unable to open OpenAL device.\n");
        return;
    }
    
    _alcContext = alcCreateContext(_alcDevice, NULL);
    ALCenum alcErr = alcGetError(_alcDevice);
# ifdef EMSCRIPTEN
    // Emscripten's OpenAL implementation requires a current context to be set
    // in order for alcGetError to work. Since at this point we haven't yet set
    // it to be current, we get an error here. Just rely on the context being
    // created as our signal that it succeeded.
    if (!_alcContext)
# else
    if (!_alcContext || alcErr != ALC_NO_ERROR)
# endif // EMSCRIPTEN
    {
        alcCloseDevice(_alcDevice);
        GP_ERROR("Unable to create OpenAL context. Error: %d\n", alcErr);
        return;
    }
    
    alcMakeContextCurrent(_alcContext);
    alcErr = alcGetError(_alcDevice);
    if (alcErr != ALC_NO_ERROR)
    {
        GP_ERROR("Unable to make OpenAL context current. Error: %d\n", alcErr);
    }
#endif // NOAUDIO
}

void AudioController::finalize()
{
#ifndef NOAUDIO
    alcMakeContextCurrent(NULL);
    if (_alcContext)
    {
        alcDestroyContext(_alcContext);
        _alcContext = NULL;
    }
    if (_alcDevice)
    {
        alcCloseDevice(_alcDevice);
        _alcDevice = NULL;
    }
#endif // NOAUDIO
}

void AudioController::pause()
{
#ifndef NOAUDIO
    std::set<AudioSource*>::iterator itr = _playingSources.begin();

    // For each source that is playing, pause it.
    AudioSource* source = NULL;
    while (itr != _playingSources.end())
    {
        GP_ASSERT(*itr);
        source = *itr;
        _pausingSource = source;
        source->pause();
        _pausingSource = NULL;
        itr++;
    }
#endif // NOAUDIO
}

void AudioController::resume()
{   
#ifndef NOAUDIO
    alcMakeContextCurrent(_alcContext);

    std::set<AudioSource*>::iterator itr = _playingSources.begin();

    // For each source that is playing, resume it.
    AudioSource* source = NULL;
    while (itr != _playingSources.end())
    {
        GP_ASSERT(*itr);
        source = *itr;
        source->resume();
        itr++;
    }
#endif // NOAUDIO
}

void AudioController::update(float elapsedTime)
{
#ifndef NOAUDIO
    AudioListener* listener = AudioListener::getInstance();
    if (listener)
    {
#ifndef EMSCRIPTEN
#warning Emscripten OpenAL does not implement alListenerf yet
        AL_CHECK( alListenerf(AL_GAIN, listener->getGain()) );
        AL_CHECK( alListenerfv(AL_ORIENTATION, (ALfloat*)listener->getOrientation()) );
        AL_CHECK( alListenerfv(AL_VELOCITY, (ALfloat*)&listener->getVelocity()) );
        AL_CHECK( alListenerfv(AL_POSITION, (ALfloat*)&listener->getPosition()) );
#endif // EMSCRIPTEN
    }
#endif // NOAUDIO
}

}
