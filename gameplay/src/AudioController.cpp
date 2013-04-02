// Note, this class is just stubbed out for Emscripten as it does not yet
// provide an OpenAL implementation.

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
    _alcDevice = alcOpenDevice(NULL);
    if (!_alcDevice)
    {
        GP_ERROR("Unable to open OpenAL device.\n");
        return;
    }
    
    _alcContext = alcCreateContext(_alcDevice, NULL);
    ALCenum alcErr = alcGetError(_alcDevice);
#ifdef EMSCRIPTEN
    // Emscripten's OpenAL implementation requires a current context to be set
    // in order for alcGetError to work. Since at this point we haven't yet set
    // it to be current, we get an error here. Just rely on the context being
    // created as our signal that it succeeded.
    if (!_alcContext)
#else
    if (!_alcContext || alcErr != ALC_NO_ERROR)
#endif // EMSCRIPTEN
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
}

void AudioController::finalize()
{
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
}

void AudioController::pause()
{
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
}

void AudioController::resume()
{   
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
}

void AudioController::update(float elapsedTime)
{
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
}

}
