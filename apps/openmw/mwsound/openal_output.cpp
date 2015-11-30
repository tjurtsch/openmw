#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <vector>

#include <stdint.h>

#include <components/vfs/manager.hpp>

#include <boost/thread.hpp>

#include "openal_output.hpp"
#include "sound_decoder.hpp"
#include "sound.hpp"
#include "soundmanagerimp.hpp"
#include "loudness.hpp"

#ifndef ALC_ALL_DEVICES_SPECIFIER
#define ALC_ALL_DEVICES_SPECIFIER 0x1013
#endif


#define MAKE_PTRID(id) ((void*)(uintptr_t)id)
#define GET_PTRID(ptr) ((ALuint)(uintptr_t)ptr)

namespace
{

const int sLoudnessFPS = 20; // loudness values per second of audio

}

namespace MWSound
{

static void fail(const std::string &msg)
{ throw std::runtime_error("OpenAL exception: " + msg); }

static void throwALCerror(ALCdevice *device)
{
    ALCenum err = alcGetError(device);
    if(err != ALC_NO_ERROR)
    {
        const ALCchar *errstring = alcGetString(device, err);
        fail(errstring ? errstring : "");
    }
}

static void throwALerror()
{
    ALenum err = alGetError();
    if(err != AL_NO_ERROR)
    {
        const ALchar *errstring = alGetString(err);
        fail(errstring ? errstring : "");
    }
}


static ALenum getALFormat(ChannelConfig chans, SampleType type)
{
    static const struct {
        ALenum format;
        ChannelConfig chans;
        SampleType type;
    } fmtlist[] = {
        { AL_FORMAT_MONO16,   ChannelConfig_Mono,   SampleType_Int16 },
        { AL_FORMAT_MONO8,    ChannelConfig_Mono,   SampleType_UInt8 },
        { AL_FORMAT_STEREO16, ChannelConfig_Stereo, SampleType_Int16 },
        { AL_FORMAT_STEREO8,  ChannelConfig_Stereo, SampleType_UInt8 },
    };
    static const size_t fmtlistsize = sizeof(fmtlist)/sizeof(fmtlist[0]);

    for(size_t i = 0;i < fmtlistsize;i++)
    {
        if(fmtlist[i].chans == chans && fmtlist[i].type == type)
            return fmtlist[i].format;
    }

    if(alIsExtensionPresent("AL_EXT_MCFORMATS"))
    {
        static const struct {
            char name[32];
            ChannelConfig chans;
            SampleType type;
        } mcfmtlist[] = {
            { "AL_FORMAT_QUAD16",   ChannelConfig_Quad,    SampleType_Int16 },
            { "AL_FORMAT_QUAD8",    ChannelConfig_Quad,    SampleType_UInt8 },
            { "AL_FORMAT_51CHN16",  ChannelConfig_5point1, SampleType_Int16 },
            { "AL_FORMAT_51CHN8",   ChannelConfig_5point1, SampleType_UInt8 },
            { "AL_FORMAT_71CHN16",  ChannelConfig_7point1, SampleType_Int16 },
            { "AL_FORMAT_71CHN8",   ChannelConfig_7point1, SampleType_UInt8 },
        };
        static const size_t mcfmtlistsize = sizeof(mcfmtlist)/sizeof(mcfmtlist[0]);

        for(size_t i = 0;i < mcfmtlistsize;i++)
        {
            if(mcfmtlist[i].chans == chans && mcfmtlist[i].type == type)
            {
                ALenum format = alGetEnumValue(mcfmtlist[i].name);
                if(format != 0 && format != -1)
                    return format;
            }
        }
    }
    if(alIsExtensionPresent("AL_EXT_FLOAT32"))
    {
        static const struct {
            char name[32];
            ChannelConfig chans;
            SampleType type;
        } fltfmtlist[] = {
            { "AL_FORMAT_MONO_FLOAT32",   ChannelConfig_Mono,   SampleType_Float32 },
            { "AL_FORMAT_STEREO_FLOAT32", ChannelConfig_Stereo, SampleType_Float32 },
        };
        static const size_t fltfmtlistsize = sizeof(fltfmtlist)/sizeof(fltfmtlist[0]);

        for(size_t i = 0;i < fltfmtlistsize;i++)
        {
            if(fltfmtlist[i].chans == chans && fltfmtlist[i].type == type)
            {
                ALenum format = alGetEnumValue(fltfmtlist[i].name);
                if(format != 0 && format != -1)
                    return format;
            }
        }
        if(alIsExtensionPresent("AL_EXT_MCFORMATS"))
        {
            static const struct {
                char name[32];
                ChannelConfig chans;
                SampleType type;
            } fltmcfmtlist[] = {
                { "AL_FORMAT_QUAD32",  ChannelConfig_Quad,    SampleType_Float32 },
                { "AL_FORMAT_51CHN32", ChannelConfig_5point1, SampleType_Float32 },
                { "AL_FORMAT_71CHN32", ChannelConfig_7point1, SampleType_Float32 },
            };
            static const size_t fltmcfmtlistsize = sizeof(fltmcfmtlist)/sizeof(fltmcfmtlist[0]);

            for(size_t i = 0;i < fltmcfmtlistsize;i++)
            {
                if(fltmcfmtlist[i].chans == chans && fltmcfmtlist[i].type == type)
                {
                    ALenum format = alGetEnumValue(fltmcfmtlist[i].name);
                    if(format != 0 && format != -1)
                        return format;
                }
            }
        }
    }

    fail(std::string("Unsupported sound format (")+getChannelConfigName(chans)+", "+getSampleTypeName(type)+")");
    return AL_NONE;
}


//
// A streaming OpenAL sound.
//
class OpenAL_SoundStream
{
    static const ALuint sNumBuffers = 6;
    static const ALfloat sBufferLength;

private:
    ALuint mSource;

    ALuint mBuffers[sNumBuffers];
    ALint mCurrentBufIdx;

    ALenum mFormat;
    ALsizei mSampleRate;
    ALuint mBufferSize;
    ALuint mFrameSize;
    ALint mSilence;

    DecoderPtr mDecoder;

    volatile bool mIsFinished;

    void updateAll(bool local);

    OpenAL_SoundStream(const OpenAL_SoundStream &rhs);
    OpenAL_SoundStream& operator=(const OpenAL_SoundStream &rhs);

    friend class OpenAL_Output;

public:
    OpenAL_SoundStream(ALuint src, DecoderPtr decoder);
    ~OpenAL_SoundStream();

    bool isPlaying();
    double getStreamDelay() const;
    double getStreamOffset() const;

    bool process();
    ALint refillQueue();
};
const ALfloat OpenAL_SoundStream::sBufferLength = 0.125f;


//
// A background streaming thread (keeps active streams processed)
//
struct OpenAL_Output::StreamThread {
    typedef std::vector<OpenAL_SoundStream*> StreamVec;
    StreamVec mStreams;

    typedef std::vector<std::pair<DecoderPtr,Sound_Loudness*> > DecoderLoudnessVec;
    DecoderLoudnessVec mDecoderLoudness;

    volatile bool mQuitNow;
    boost::mutex mMutex;
    boost::condition_variable mCondVar;
    boost::thread mThread;

    StreamThread()
      : mQuitNow(false), mThread(boost::ref(*this))
    {
    }
    ~StreamThread()
    {
        mQuitNow = true;
        mMutex.lock(); mMutex.unlock();
        mCondVar.notify_all();
        mThread.join();
    }

    // boost::thread entry point
    void operator()()
    {
        boost::unique_lock<boost::mutex> lock(mMutex);
        while(!mQuitNow)
        {
            StreamVec::iterator iter = mStreams.begin();
            while(iter != mStreams.end())
            {
                if((*iter)->process() == false)
                    iter = mStreams.erase(iter);
                else
                    ++iter;
            }

            // Only do one loudness decode at a time, in case it takes particularly long we don't
            // want to block up anything.
            DecoderLoudnessVec::iterator dliter = mDecoderLoudness.begin();
            if(dliter != mDecoderLoudness.end())
            {
                DecoderPtr decoder = dliter->first;
                Sound_Loudness *loudness = dliter->second;
                mDecoderLoudness.erase(dliter);
                lock.unlock();

                std::vector<char> data;
                ChannelConfig chans = ChannelConfig_Mono;
                SampleType type = SampleType_Int16;
                int srate = 48000;
                try {
                    decoder->getInfo(&srate, &chans, &type);
                    decoder->readAll(data);
                }
                catch(std::exception &e) {
                    std::cerr<< "Failed to decode audio: "<<e.what() <<std::endl;
                }

                loudness->analyzeLoudness(data, srate, chans, type, static_cast<float>(sLoudnessFPS));
                lock.lock();
                continue;
            }
            mCondVar.timed_wait(lock, boost::posix_time::milliseconds(50));
        }
    }

    void add(OpenAL_SoundStream *stream)
    {
        boost::unique_lock<boost::mutex> lock(mMutex);
        if(std::find(mStreams.begin(), mStreams.end(), stream) == mStreams.end())
        {
            mStreams.push_back(stream);
            lock.unlock();
            mCondVar.notify_all();
        }
    }

    void remove(OpenAL_SoundStream *stream)
    {
        boost::lock_guard<boost::mutex> lock(mMutex);
        StreamVec::iterator iter = std::find(mStreams.begin(), mStreams.end(), stream);
        if(iter != mStreams.end()) mStreams.erase(iter);
    }

    void removeAll()
    {
        boost::lock_guard<boost::mutex> lock(mMutex);
        mStreams.clear();
        mDecoderLoudness.clear();
    }

    void add(DecoderPtr decoder, Sound_Loudness *loudness)
    {
        boost::unique_lock<boost::mutex> lock(mMutex);
        mDecoderLoudness.push_back(std::make_pair(decoder, loudness));
        lock.unlock();
        mCondVar.notify_all();
    }

private:
    StreamThread(const StreamThread &rhs);
    StreamThread& operator=(const StreamThread &rhs);
};


OpenAL_SoundStream::OpenAL_SoundStream(ALuint src, DecoderPtr decoder)
  : mSource(src), mCurrentBufIdx(0), mFrameSize(0), mSilence(0), mDecoder(decoder), mIsFinished(false)
{
    alGenBuffers(sNumBuffers, mBuffers);
    throwALerror();
    try
    {
        int srate;
        ChannelConfig chans;
        SampleType type;

        mDecoder->getInfo(&srate, &chans, &type);
        mFormat = getALFormat(chans, type);
        mSampleRate = srate;

        switch(type)
        {
            case SampleType_UInt8: mSilence = 0x80;
            case SampleType_Int16: mSilence = 0x00;
            case SampleType_Float32: mSilence = 0x00;
        }

        mFrameSize = framesToBytes(1, chans, type);
        mBufferSize = static_cast<ALuint>(sBufferLength*srate);
        mBufferSize *= mFrameSize;
    }
    catch(std::exception&)
    {
        alDeleteBuffers(sNumBuffers, mBuffers);
        alGetError();
        throw;
    }
    mIsFinished = false;
}
OpenAL_SoundStream::~OpenAL_SoundStream()
{
    alDeleteBuffers(sNumBuffers, mBuffers);
    alGetError();

    mDecoder->close();
}

bool OpenAL_SoundStream::isPlaying()
{
    ALint state;

    alGetSourcei(mSource, AL_SOURCE_STATE, &state);
    throwALerror();

    if(state == AL_PLAYING || state == AL_PAUSED)
        return true;
    return !mIsFinished;
}

double OpenAL_SoundStream::getStreamDelay() const
{
    ALint state = AL_STOPPED;
    double d = 0.0;
    ALint offset;

    alGetSourcei(mSource, AL_SAMPLE_OFFSET, &offset);
    alGetSourcei(mSource, AL_SOURCE_STATE, &state);
    if(state == AL_PLAYING || state == AL_PAUSED)
    {
        ALint queued;
        alGetSourcei(mSource, AL_BUFFERS_QUEUED, &queued);
        ALint inqueue = mBufferSize/mFrameSize*queued - offset;
        d = (double)inqueue / (double)mSampleRate;
    }

    throwALerror();
    return d;
}

double OpenAL_SoundStream::getStreamOffset() const
{
    ALint state = AL_STOPPED;
    ALint offset;
    double t;

    alGetSourcei(mSource, AL_SAMPLE_OFFSET, &offset);
    alGetSourcei(mSource, AL_SOURCE_STATE, &state);
    if(state == AL_PLAYING || state == AL_PAUSED)
    {
        ALint queued;
        alGetSourcei(mSource, AL_BUFFERS_QUEUED, &queued);
        ALint inqueue = mBufferSize/mFrameSize*queued - offset;
        t = (double)(mDecoder->getSampleOffset() - inqueue) / (double)mSampleRate;
    }
    else
    {
        /* Underrun, or not started yet. The decoder offset is where we'll play
         * next. */
        t = (double)mDecoder->getSampleOffset() / (double)mSampleRate;
    }

    throwALerror();
    return t;
}

bool OpenAL_SoundStream::process()
{
    try {
        if(refillQueue() > 0)
        {
            ALint state;
            alGetSourcei(mSource, AL_SOURCE_STATE, &state);
            if(state != AL_PLAYING && state != AL_PAUSED)
            {
                refillQueue();
                alSourcePlay(mSource);
            }
        }
    }
    catch(std::exception&) {
        std::cout<< "Error updating stream \""<<mDecoder->getName()<<"\"" <<std::endl;
        mIsFinished = true;
    }
    return !mIsFinished;
}

ALint OpenAL_SoundStream::refillQueue()
{
    ALint processed;
    alGetSourcei(mSource, AL_BUFFERS_PROCESSED, &processed);
    while(processed > 0)
    {
        ALuint buf;
        alSourceUnqueueBuffers(mSource, 1, &buf);
        --processed;
    }

    ALint queued;
    alGetSourcei(mSource, AL_BUFFERS_QUEUED, &queued);
    if(!mIsFinished && (ALuint)queued < sNumBuffers)
    {
        std::vector<char> data(mBufferSize);
        for(;!mIsFinished && (ALuint)queued < sNumBuffers;++queued)
        {
            size_t got = mDecoder->read(&data[0], data.size());
            if(got < data.size())
            {
                mIsFinished = true;
                memset(&data[got], mSilence, data.size()-got);
            }
            if(got > 0)
            {
                ALuint bufid = mBuffers[mCurrentBufIdx];
                alBufferData(bufid, mFormat, &data[0], data.size(), mSampleRate);
                alSourceQueueBuffers(mSource, 1, &bufid);
                mCurrentBufIdx = (mCurrentBufIdx+1) % sNumBuffers;
            }
        }
    }

    return queued;
}


//
// An OpenAL output device
//
std::vector<std::string> OpenAL_Output::enumerate()
{
    std::vector<std::string> devlist;
    const ALCchar *devnames;

    if(alcIsExtensionPresent(NULL, "ALC_ENUMERATE_ALL_EXT"))
        devnames = alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER);
    else
        devnames = alcGetString(NULL, ALC_DEVICE_SPECIFIER);
    while(devnames && *devnames)
    {
        devlist.push_back(devnames);
        devnames += strlen(devnames)+1;
    }
    return devlist;
}

void OpenAL_Output::init(const std::string &devname)
{
    deinit();

    mDevice = alcOpenDevice(devname.c_str());
    if(!mDevice)
    {
        if(devname.empty())
            fail("Failed to open default device");
        else
            fail("Failed to open \""+devname+"\"");
    }
    else
    {
        const ALCchar *name = NULL;
        if(alcIsExtensionPresent(mDevice, "ALC_ENUMERATE_ALL_EXT"))
            name = alcGetString(mDevice, ALC_ALL_DEVICES_SPECIFIER);
        if(alcGetError(mDevice) != AL_NO_ERROR || !name)
            name = alcGetString(mDevice, ALC_DEVICE_SPECIFIER);
        std::cout << "Opened \""<<name<<"\"" << std::endl;
    }

    mContext = alcCreateContext(mDevice, NULL);
    if(!mContext || alcMakeContextCurrent(mContext) == ALC_FALSE)
    {
        if(mContext)
            alcDestroyContext(mContext);
        mContext = 0;
        fail(std::string("Failed to setup context: ")+alcGetString(mDevice, alcGetError(mDevice)));
    }

    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    throwALerror();

    ALCint maxmono=0, maxstereo=0;
    alcGetIntegerv(mDevice, ALC_MONO_SOURCES, 1, &maxmono);
    alcGetIntegerv(mDevice, ALC_STEREO_SOURCES, 1, &maxstereo);
    throwALCerror(mDevice);

    try
    {
        ALCuint maxtotal = std::min<ALCuint>(maxmono+maxstereo, 256);
        if (maxtotal == 0) // workaround for broken implementations
            maxtotal = 256;
        for(size_t i = 0;i < maxtotal;i++)
        {
            ALuint src = 0;
            alGenSources(1, &src);
            throwALerror();
            mFreeSources.push_back(src);
        }
    }
    catch(std::exception &e)
    {
        std::cout <<"Error: "<<e.what()<<", trying to continue"<< std::endl;
    }
    if(mFreeSources.empty())
        fail("Could not allocate any sources");

    mInitialized = true;
}

void OpenAL_Output::deinit()
{
    mStreamThread->removeAll();

    for(size_t i = 0;i < mFreeSources.size();i++)
        alDeleteSources(1, &mFreeSources[i]);
    mFreeSources.clear();

    alcMakeContextCurrent(0);
    if(mContext)
        alcDestroyContext(mContext);
    mContext = 0;
    if(mDevice)
        alcCloseDevice(mDevice);
    mDevice = 0;

    mInitialized = false;
}


Sound_Handle OpenAL_Output::loadSound(const std::string &fname)
{
    throwALerror();

    DecoderPtr decoder = mManager.getDecoder();
    // Workaround: Bethesda at some point converted some of the files to mp3, but the references were kept as .wav.
    if(decoder->mResourceMgr->exists(fname))
        decoder->open(fname);
    else
    {
        std::string file = fname;
        std::string::size_type pos = file.rfind('.');
        if(pos != std::string::npos)
            file = file.substr(0, pos)+".mp3";
        decoder->open(file);
    }

    std::vector<char> data;
    ChannelConfig chans;
    SampleType type;
    ALenum format;
    int srate;

    decoder->getInfo(&srate, &chans, &type);
    format = getALFormat(chans, type);

    decoder->readAll(data);
    decoder->close();

    ALuint buf = 0;
    try {
        alGenBuffers(1, &buf);
        alBufferData(buf, format, &data[0], data.size(), srate);
        throwALerror();
    }
    catch(...) {
        if(buf && alIsBuffer(buf))
            alDeleteBuffers(1, &buf);
        throw;
    }
    return MAKE_PTRID(buf);
}

void OpenAL_Output::unloadSound(Sound_Handle data)
{
    ALuint buffer = GET_PTRID(data);
    // Make sure no sources are playing this buffer before unloading it.
    SoundVec::const_iterator iter = mActiveSounds.begin();
    for(;iter != mActiveSounds.end();++iter)
    {
        if(!(*iter)->mHandle)
            continue;

        ALuint source = GET_PTRID((*iter)->mHandle);
        ALint srcbuf;
        alGetSourcei(source, AL_BUFFER, &srcbuf);
        if((ALuint)srcbuf == buffer)
        {
            alSourceStop(source);
            alSourcei(source, AL_BUFFER, 0);
        }
    }
    alDeleteBuffers(1, &buffer);
}

size_t OpenAL_Output::getSoundDataSize(Sound_Handle data) const
{
    ALuint buffer = GET_PTRID(data);
    ALint size = 0;

    alGetBufferi(buffer, AL_SIZE, &size);
    throwALerror();

    return (ALuint)size;
}


MWBase::SoundPtr OpenAL_Output::playSound(Sound_Handle data, float vol, float basevol, float pitch, int flags, float offset)
{
    boost::shared_ptr<Sound> sound;
    ALuint source;

    if(mFreeSources.empty())
        fail("No free sources");
    source = mFreeSources.front();
    mFreeSources.pop_front();

    try {
        alSourcef(source, AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(source, AL_MAX_DISTANCE, 1000.0f);
        alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcei(source, AL_LOOPING, (flags&MWBase::SoundManager::Play_Loop) ? AL_TRUE : AL_FALSE);

        ALfloat gain = vol*basevol;
        if(!(flags&MWBase::SoundManager::Play_NoEnv) && mListenerEnv == Env_Underwater)
        {
            gain *= 0.9f;
            pitch *= 0.7f;
        }

        alSourcef(source, AL_GAIN, gain);
        alSourcef(source, AL_PITCH, pitch);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSource3f(source, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
        alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);

        alSourcef(source, AL_SEC_OFFSET, offset/pitch);
        alSourcei(source, AL_BUFFER, GET_PTRID(data));

        alSourcePlay(source);
        throwALerror();

        sound.reset(new Sound(osg::Vec3f(0.f, 0.f, 0.f), vol, basevol, pitch, 1.0f, 1000.0f, flags));
        sound->mHandle = MAKE_PTRID(source);
        mActiveSounds.push_back(sound);
    }
    catch(std::exception&) {
        mFreeSources.push_back(source);
        throw;
    }

    return sound;
}

MWBase::SoundPtr OpenAL_Output::playSound3D(Sound_Handle data, const osg::Vec3f &pos, float vol, float basevol, float pitch,
                                            float mindist, float maxdist, int flags, float offset)
{
    boost::shared_ptr<Sound> sound;
    ALuint source;

    if(mFreeSources.empty())
        fail("No free sources");
    source = mFreeSources.front();
    mFreeSources.pop_front();

    try {
        alSourcef(source, AL_REFERENCE_DISTANCE, mindist);
        alSourcef(source, AL_MAX_DISTANCE, maxdist);
        alSourcef(source, AL_ROLLOFF_FACTOR, 1.0f);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
        alSourcei(source, AL_LOOPING, (flags&MWBase::SoundManager::Play_Loop) ? AL_TRUE : AL_FALSE);

        ALfloat gain = vol*basevol;
        if((pos - mListenerPos).length2() > maxdist*maxdist)
            gain = 0.0f;
        if(!(flags&MWBase::SoundManager::Play_NoEnv) && mListenerEnv == Env_Underwater)
        {
            gain *= 0.9f;
            pitch *= 0.7f;
        }

        alSourcef(source, AL_GAIN, gain);
        alSourcef(source, AL_PITCH, pitch);
        alSourcefv(source, AL_POSITION, pos.ptr());
        alSource3f(source, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
        alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);

        alSourcef(source, AL_SEC_OFFSET, offset/pitch);
        alSourcei(source, AL_BUFFER, GET_PTRID(data));

        alSourcePlay(source);
        throwALerror();

        sound.reset(new Sound(pos, vol, basevol, pitch, mindist, maxdist, flags));
        sound->mHandle = MAKE_PTRID(source);
        mActiveSounds.push_back(sound);
    }
    catch(std::exception&) {
        mFreeSources.push_back(source);
        throw;
    }

    return sound;
}

void OpenAL_Output::stopSound(MWBase::SoundPtr sound)
{
    if(!sound->mHandle)
        return;

    ALuint source = GET_PTRID(sound->mHandle);
    sound->mHandle = 0;

    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);

    mFreeSources.push_back(source);
    mActiveSounds.erase(std::find(mActiveSounds.begin(), mActiveSounds.end(), sound));
}

bool OpenAL_Output::isSoundPlaying(MWBase::SoundPtr sound)
{
    if(!sound->mHandle)
        return false;
    ALuint source = GET_PTRID(sound->mHandle);
    ALint state;

    alGetSourcei(source, AL_SOURCE_STATE, &state);
    throwALerror();

    return state == AL_PLAYING || state == AL_PAUSED;
}

void OpenAL_Output::updateSound(MWBase::SoundPtr sound)
{
    if(!sound->mHandle) return;
    ALuint source = GET_PTRID(sound->mHandle);

    const osg::Vec3f &pos = sound->getPosition();
    ALfloat gain = sound->getRealVolume();
    ALfloat pitch = sound->getPitch();
    if(sound->getIs3D())
    {
        ALfloat maxdist = sound->getMaxDistance();
        if((pos - mListenerPos).length2() > maxdist*maxdist)
            gain = 0.0f;
    }
    if(sound->getUseEnv() && mListenerEnv == Env_Underwater)
    {
        gain *= 0.9f;
        pitch *= 0.7f;
    }

    alSourcef(source, AL_GAIN, gain);
    alSourcef(source, AL_PITCH, pitch);
    alSourcefv(source, AL_POSITION, pos.ptr());
    alSource3f(source, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
    alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
}


MWBase::SoundStreamPtr OpenAL_Output::streamSound(DecoderPtr decoder, float basevol, float pitch, int flags)
{
    MWBase::SoundStreamPtr sound;
    OpenAL_SoundStream *stream = 0;
    ALuint source;

    if(mFreeSources.empty())
        fail("No free sources");
    source = mFreeSources.front();
    mFreeSources.pop_front();

    if((flags&MWBase::SoundManager::Play_Loop))
        std::cout <<"Warning: cannot loop stream \""<<decoder->getName()<<"\""<< std::endl;
    try {
        alSourcef(source, AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(source, AL_MAX_DISTANCE, 1000.0f);
        alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcei(source, AL_LOOPING, AL_FALSE);

        ALfloat gain = basevol;
        if(!(flags&MWBase::SoundManager::Play_NoEnv) && mListenerEnv == Env_Underwater)
        {
            gain *= 0.9f;
            pitch *= 0.7f;
        }

        alSourcef(source, AL_GAIN, gain);
        alSourcef(source, AL_PITCH, pitch);
        alSource3f(source, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSource3f(source, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
        alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
        throwALerror();

        sound.reset(new Stream(osg::Vec3f(0.0f, 0.0f, 0.0f), 1.0f, basevol, pitch, 1.0f, 1000.0f, flags));
        stream = new OpenAL_SoundStream(source, decoder);
        mStreamThread->add(stream);
        sound->mHandle = stream;
        mActiveStreams.push_back(sound);
    }
    catch(std::exception&) {
        mStreamThread->remove(stream);
        delete stream;
        mFreeSources.push_back(source);
        throw;
    }

    return sound;
}

MWBase::SoundStreamPtr OpenAL_Output::streamSound3D(DecoderPtr decoder, const osg::Vec3f &pos, float volume, float basevol, float pitch, float mindist, float maxdist, int flags)
{
    MWBase::SoundStreamPtr sound;
    OpenAL_SoundStream *stream = 0;
    ALuint source;

    if(mFreeSources.empty())
        fail("No free sources");
    source = mFreeSources.front();
    mFreeSources.pop_front();

    if((flags&MWBase::SoundManager::Play_Loop))
        std::cout <<"Warning: cannot loop stream \""<<decoder->getName()<<"\""<< std::endl;
    try {
        alSourcef(source, AL_REFERENCE_DISTANCE, mindist);
        alSourcef(source, AL_MAX_DISTANCE, maxdist);
        alSourcef(source, AL_ROLLOFF_FACTOR, 1.0f);
        alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
        alSourcei(source, AL_LOOPING, AL_FALSE);

        ALfloat gain = volume*basevol;
        if((pos - mListenerPos).length2() > maxdist*maxdist)
            gain = 0.0f;
        if(!(flags&MWBase::SoundManager::Play_NoEnv) && mListenerEnv == Env_Underwater)
        {
            gain *= 0.9f;
            pitch *= 0.7f;
        }

        alSourcef(source, AL_GAIN, gain);
        alSourcef(source, AL_PITCH, pitch);
        alSourcefv(source, AL_POSITION, pos.ptr());
        alSource3f(source, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
        alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
        throwALerror();

        sound.reset(new Stream(pos, volume, basevol, pitch, mindist, maxdist, flags));
        stream = new OpenAL_SoundStream(source, decoder);
        mStreamThread->add(stream);
        sound->mHandle = stream;
        mActiveStreams.push_back(sound);
    }
    catch(std::exception&) {
        mStreamThread->remove(stream);
        delete stream;
        mFreeSources.push_back(source);
        throw;
    }

    return sound;
}

void OpenAL_Output::stopStream(MWBase::SoundStreamPtr sound)
{
    if(!sound->mHandle)
        return;
    OpenAL_SoundStream *stream = reinterpret_cast<OpenAL_SoundStream*>(sound->mHandle);
    ALuint source = stream->mSource;

    sound->mHandle = 0;
    mStreamThread->remove(stream);

    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);

    mFreeSources.push_back(source);
    mActiveStreams.erase(std::find(mActiveStreams.begin(), mActiveStreams.end(), sound));

    delete stream;
}

double OpenAL_Output::getStreamDelay(MWBase::SoundStreamPtr sound)
{
    if(!sound->mHandle)
        return 0.0;
    OpenAL_SoundStream *stream = reinterpret_cast<OpenAL_SoundStream*>(sound->mHandle);
    return stream->getStreamDelay();
}

double OpenAL_Output::getStreamOffset(MWBase::SoundStreamPtr sound)
{
    if(!sound->mHandle)
        return 0.0;
    OpenAL_SoundStream *stream = reinterpret_cast<OpenAL_SoundStream*>(sound->mHandle);
    boost::lock_guard<boost::mutex> lock(mStreamThread->mMutex);
    return stream->getStreamOffset();
}

bool OpenAL_Output::isStreamPlaying(MWBase::SoundStreamPtr sound)
{
    if(!sound->mHandle)
        return false;
    OpenAL_SoundStream *stream = reinterpret_cast<OpenAL_SoundStream*>(sound->mHandle);
    boost::lock_guard<boost::mutex> lock(mStreamThread->mMutex);
    return stream->isPlaying();
}

void OpenAL_Output::updateStream(MWBase::SoundStreamPtr sound)
{
    if(!sound->mHandle) return;
    OpenAL_SoundStream *stream = reinterpret_cast<OpenAL_SoundStream*>(sound->mHandle);
    ALuint source = stream->mSource;

    const osg::Vec3f &pos = sound->getPosition();
    ALfloat gain = sound->getRealVolume();
    ALfloat pitch = sound->getPitch();
    if(sound->getIs3D())
    {
        ALfloat maxdist = sound->getMaxDistance();
        if((pos - mListenerPos).length2() > maxdist*maxdist)
            gain = 0.0f;
    }
    if(sound->getUseEnv() && mListenerEnv == Env_Underwater)
    {
        gain *= 0.9f;
        pitch *= 0.7f;
    }

    alSourcef(source, AL_GAIN, gain);
    alSourcef(source, AL_PITCH, pitch);
    alSourcefv(source, AL_POSITION, pos.ptr());
    alSource3f(source, AL_DIRECTION, 0.0f, 0.0f, 0.0f);
    alSource3f(source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
}


void OpenAL_Output::startUpdate()
{
    alcSuspendContext(alcGetCurrentContext());
}

void OpenAL_Output::finishUpdate()
{
    alcProcessContext(alcGetCurrentContext());
}


void OpenAL_Output::updateListener(const osg::Vec3f &pos, const osg::Vec3f &atdir, const osg::Vec3f &updir, Environment env)
{
    if(mContext)
    {
        ALfloat orient[6] = {
            atdir.x(), atdir.y(), atdir.z(),
            updir.x(), updir.y(), updir.z()
        };
        alListenerfv(AL_POSITION, pos.ptr());
        alListenerfv(AL_ORIENTATION, orient);
        throwALerror();
    }

    mListenerPos = pos;
    mListenerEnv = env;
}


void OpenAL_Output::pauseSounds(int types)
{
    std::vector<ALuint> sources;
    SoundVec::const_iterator sound = mActiveSounds.begin();
    for(;sound != mActiveSounds.end();++sound)
    {
        if(*sound && (*sound)->mHandle && ((*sound)->getPlayType()&types))
            sources.push_back(GET_PTRID((*sound)->mHandle));
    }
    StreamVec::const_iterator stream = mActiveStreams.begin();
    for(;stream != mActiveStreams.end();++stream)
    {
        if(*stream && (*stream)->mHandle && ((*stream)->getPlayType()&types))
        {
            OpenAL_SoundStream *strm = reinterpret_cast<OpenAL_SoundStream*>((*stream)->mHandle);
            sources.push_back(strm->mSource);
        }
    }
    if(!sources.empty())
    {
        alSourcePausev(sources.size(), &sources[0]);
        throwALerror();
    }
}

void OpenAL_Output::resumeSounds(int types)
{
    std::vector<ALuint> sources;
    SoundVec::const_iterator sound = mActiveSounds.begin();
    for(;sound != mActiveSounds.end();++sound)
    {
        if(*sound && (*sound)->mHandle && ((*sound)->getPlayType()&types))
            sources.push_back(GET_PTRID((*sound)->mHandle));
    }
    StreamVec::const_iterator stream = mActiveStreams.begin();
    for(;stream != mActiveStreams.end();++stream)
    {
        if(*stream && (*stream)->mHandle && ((*stream)->getPlayType()&types))
        {
            OpenAL_SoundStream *strm = reinterpret_cast<OpenAL_SoundStream*>((*stream)->mHandle);
            sources.push_back(strm->mSource);
        }
    }
    if(!sources.empty())
    {
        alSourcePlayv(sources.size(), &sources[0]);
        throwALerror();
    }
}


void OpenAL_Output::loadLoudnessAsync(DecoderPtr decoder, Sound_Loudness *loudness)
{
    mStreamThread->add(decoder, loudness);
}


OpenAL_Output::OpenAL_Output(SoundManager &mgr)
  : Sound_Output(mgr), mDevice(0), mContext(0)
  , mListenerPos(0.0f, 0.0f, 0.0f), mListenerEnv(Env_Normal)
  , mStreamThread(new StreamThread)
{
}

OpenAL_Output::~OpenAL_Output()
{
    deinit();
}

}
