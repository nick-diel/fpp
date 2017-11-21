/*
 *   mpg123 player driver for Falcon Pi Player (FPP)
 *
 *   Copyright (C) 2013 the Falcon Pi Player Developers
 *      Initial development by:
 *      - David Pitts (dpitts)
 *      - Tony Mace (MyKroFt)
 *      - Mathew Mrosko (Materdaddy)
 *      - Chris Pinkham (CaptainMurdoch)
 *      For additional credits and developers, see credits.php.
 *
 *   The Falcon Pi Player (FPP) is free software; you can redistribute it
 *   and/or modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <cmath>

#include <list>
#include <mutex>


extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
    
#include <SDL2/SDL.h>
}


#include "channeloutputthread.h"
#include "common.h"
#include "controlsend.h"
#include "log.h"
#include "SDLOut.h"
#include "Sequence.h"
#include "settings.h"


class AudioBuffer {
public:
    AudioBuffer() {
        size = 44100 * 2 * 2; //per second
        data = (uint8_t*)malloc(size);
        pos = 0;
        next = nullptr;
    }
    ~AudioBuffer() {
        free(data);
    }
    
    uint8_t *data;
    int size;
    int pos;
    AudioBuffer *next;
};



class SDLInternalData {
public:
    SDLInternalData() : curPos(0) {
        formatContext = nullptr;
        codecContext = nullptr;
        audio_stream_idx = -1;
        firstBuffer = lastBuffer = fillBuffer = nullptr;
        curBuffer = nullptr;
        bufferCount = 0;
        doneRead = false;
        frame = av_frame_alloc();
        au_convert_ctx = nullptr;
    }
    ~SDLInternalData() {
        if (frame != nullptr) {
            av_free(frame);
        }
        if (formatContext != nullptr) {
            avformat_close_input(&formatContext);
        }
        if (au_convert_ctx != nullptr) {
            swr_free(&au_convert_ctx);
        }

        while (firstBuffer) {
            AudioBuffer *tmp = firstBuffer;
            firstBuffer = tmp->next;
            delete tmp;
        }
    }
    pthread_t fillThread;
    volatile int stopped;
    AVFormatContext* formatContext;
    AVCodecContext *codecContext;
    int audio_stream_idx = -1;
    AVPacket readingPacket;
    AVFrame* frame;
    AVStream* audioStream;
    SwrContext *au_convert_ctx;

    AudioBuffer *firstBuffer;
    AudioBuffer * volatile curBuffer;
    AudioBuffer *lastBuffer;
    AudioBuffer *fillBuffer;
    int bufferCount;
    bool doneRead;
    std::atomic_uint curPos;
    unsigned int totalDataLen;
    float totalLen;
    
    void addBuffer(AudioBuffer *b) {
        b->size = b->pos;
        b->pos = 0;
        
        if (lastBuffer == nullptr) {
            firstBuffer = b;
            curBuffer = b;
            lastBuffer = b;
        } else {
            lastBuffer->next = b;
            lastBuffer = b;
        }
        bufferCount++;
    }
    bool maybeFillBuffer(bool first) {
        while (firstBuffer != curBuffer) {
            AudioBuffer *tmp = firstBuffer;
            --bufferCount;
            firstBuffer = tmp->next;
            delete tmp;
        }
        if (doneRead || bufferCount > 10) {
            //have ~10 seconds of audio already buffered, don't so anything
            return doneRead;
        }
        
        if (fillBuffer == nullptr) {
            fillBuffer = new AudioBuffer();
        }
        bool newBuf = false;
        while (av_read_frame(formatContext, &readingPacket) == 0) {
            if (readingPacket.stream_index == audioStream->index) {
                while (avcodec_send_packet(codecContext, &readingPacket)) {
                    while (!avcodec_receive_frame(codecContext, frame)) {
                        int sz = frame->nb_samples * 2 * 2;
                        if ((sz + fillBuffer->pos) >= fillBuffer->size) {
                            addBuffer(fillBuffer);
                            fillBuffer = new AudioBuffer();
                            newBuf = true;
                        }
                        uint8_t* out_buffer = &fillBuffer->data[fillBuffer->pos];
                        int outSamples = swr_convert(au_convert_ctx,
                                                     &out_buffer,
                                                     (fillBuffer->size - fillBuffer->pos) / 4,
                                                     (const uint8_t **)frame->data, frame->nb_samples);
                        
                        fillBuffer->pos += (outSamples * 2 * 2);
                        av_frame_unref(frame);
                    }
                }
            }
            av_packet_unref(&readingPacket);
            if (newBuf && bufferCount > (first ? 10 : 5)) {
                return doneRead;
            }
        }
        while (!avcodec_receive_frame(codecContext, frame)) {
            int sz = frame->nb_samples * 2 * 2;
            if ((sz + fillBuffer->pos) >= fillBuffer->size) {
                addBuffer(fillBuffer);
                fillBuffer = new AudioBuffer();
            }
            
            uint8_t* out_buffer = &fillBuffer->data[fillBuffer->pos];
            int outSamples = swr_convert(au_convert_ctx,
                                         &out_buffer,
                                         (fillBuffer->size - fillBuffer->pos) / 4,
                                         (const uint8_t **)frame->data, frame->nb_samples);
            
            fillBuffer->pos += (outSamples * 2 * 2);
        }
        addBuffer(fillBuffer);
        fillBuffer = nullptr;
        doneRead = true;
        return doneRead;
    }
};

static int open_codec_context(int *stream_idx,
                              AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx,
                              enum AVMediaType type,
                              const std::string &src_filename)
{
    int ret, stream_index;
    AVStream *st;
    AVCodec *dec = NULL;
    AVDictionary *opts = NULL;
    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename.c_str());
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];
        /* find decoder for the stream */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }
        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }
        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }
        /* Init the decoders, with or without reference counting */
        av_dict_set(&opts, "refcounted_frames", "0", 0);
        if ((ret = avcodec_open2(*dec_ctx, dec, &opts)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }
    return 0;
}



typedef enum SDLSTATE {
    SDLUNINITIALISED,
    SDLINITIALISED,
    SDLOPENED,
    SDLPLAYING,
    SDLNOTPLAYING
} SDLSTATE;


class SDL
{
    SDLSTATE _state;
    SDL_AudioSpec _wanted_spec;
    int _initialisedRate;

public:
    SDL();
    virtual ~SDL();
    
    void Start(SDLInternalData *d) {
        data = d;
        SDL_PauseAudio(0);
        _state = SDLSTATE::SDLPLAYING;
    }
    void Stop() {
        SDL_PauseAudio(1);
        data = nullptr;
        _state = SDLSTATE::SDLNOTPLAYING;
    };
    
    SDLInternalData *data;
};

static SDL sdlManager;
#define DEFAULT_NUM_SAMPLES 1024
#define DEFAULT_RATE 44100

void fill_audio(void *udata, Uint8 *stream, int len) {
    if (sdlManager.data != nullptr) {
        AudioBuffer *buf = sdlManager.data->curBuffer;
        if (buf == nullptr) {
            SDL_memset(stream, 0, len);
            return;
        }
        int sp = 0;
        while (len > 0) {
            int nl = len;
            if (nl > (buf->size - buf->pos)) {
                nl = (buf->size - buf->pos);
            }
            memcpy(&stream[sp], &buf->data[buf->pos], nl);
            buf->pos += nl;
            sp += nl;
            len -= nl;
            sdlManager.data->curPos += nl;
            if (buf->pos == buf->size) {
                buf = buf->next;
                sdlManager.data->curBuffer = buf;
                SDL_memset(&stream[sp], 0, len);
                if (buf == nullptr) {
                    return;
                }
            }
        }
    } else {
        SDL_memset(stream, 0, len);
    }
}


SDL::SDL() : data(nullptr) {
    _state = SDLSTATE::SDLUNINITIALISED;
    
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        return;
    }
    
    _state = SDLSTATE::SDLINITIALISED;
    _initialisedRate = DEFAULT_RATE;
    
    _wanted_spec.freq = _initialisedRate;
    _wanted_spec.format = AUDIO_S16SYS;
    _wanted_spec.channels = 2;
    _wanted_spec.silence = 0;
    _wanted_spec.samples = DEFAULT_NUM_SAMPLES;
    _wanted_spec.callback = fill_audio;
    _wanted_spec.userdata = nullptr;
    
    if (SDL_OpenAudio(&_wanted_spec, nullptr) < 0)
    {
        return;
    }
    
    _state = SDLSTATE::SDLOPENED;
}
SDL::~SDL() {
    if (_state != SDLSTATE::SDLOPENED && _state != SDLSTATE::SDLINITIALISED && _state != SDLSTATE::SDLUNINITIALISED)
    {
        Stop();
    }
    if (_state != SDLSTATE::SDLINITIALISED && _state != SDLSTATE::SDLUNINITIALISED)
    {
        SDL_CloseAudio();
    }
    if (_state != SDLSTATE::SDLUNINITIALISED)
    {
        SDL_Quit();
    }
}


void *BufferFillThread(void *d) {
    SDLInternalData *data = (SDLInternalData *)d;
    while (!data->stopped && !data->maybeFillBuffer(false)) {
        usleep(100);
    }
    data->stopped++;
}
/*
 *
 */
SDLOutput::SDLOutput(const std::string &mediaFilename, MediaOutputStatus *status)
{
	LogDebug(VB_MEDIAOUT, "SDLOutput::SDLOutput(%s)\n",
		mediaFilename.c_str());

    
    std::string fullAudioPath = mediaFilename;
    if (!FileExists(mediaFilename.c_str())) {
        fullAudioPath = getMusicDirectory();
        fullAudioPath += "/";
        fullAudioPath += mediaFilename;
    }

    if (!FileExists(fullAudioPath.c_str()))
    {
        LogErr(VB_MEDIAOUT, "%s does not exist!\n", fullAudioPath.c_str());
        return;
    }
    
	m_mediaFilename = fullAudioPath;
	m_mediaOutputStatus = status;
    
    data = new SDLInternalData();

    
    // Initialize FFmpeg codecs
    av_register_all();
    int res = avformat_open_input(&data->formatContext, m_mediaFilename.c_str(), nullptr, nullptr);
    if (avformat_find_stream_info(data->formatContext, nullptr) < 0)
    {
        LogErr(VB_MEDIAOUT, "Could not find suitable input stream!\n");
        avformat_close_input(&data->formatContext);
        data->formatContext = nullptr;
        return;
    }

    if (open_codec_context(&data->audio_stream_idx, &data->codecContext, data->formatContext, AVMEDIA_TYPE_AUDIO, m_mediaFilename) >= 0) {
        data->audioStream = data->formatContext->streams[data->audio_stream_idx];
    }
    /* dump input information to stderr */
    av_dump_format(data->formatContext, 0, m_mediaFilename.c_str(), 0);
    
    int64_t duration = data->formatContext->duration + (data->formatContext->duration <= INT64_MAX - 5000 ? 5000 : 0);
    int secs  = duration / AV_TIME_BASE;
    int us    = duration % AV_TIME_BASE;
    int mins  = secs / 60;
    secs %= 60;
    
    m_mediaOutputStatus->secondsTotal = secs;
    m_mediaOutputStatus->minutesTotal = mins;
    
    int64_t in_channel_layout = av_get_default_channel_layout(data->codecContext->channels);
    
    uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
    int out_sample_rate = DEFAULT_RATE;
    
    data->au_convert_ctx = swr_alloc_set_opts(nullptr,
                                        out_channel_layout, out_sample_fmt, out_sample_rate,
                                        in_channel_layout, data->codecContext->sample_fmt, data->codecContext->sample_rate, 0, nullptr);
    swr_init(data->au_convert_ctx);
    
    //get an estimate of the total length
    float d = duration / AV_TIME_BASE;
    float usf = (100 * us);
    usf /= (float)AV_TIME_BASE;
    usf /= 100.0f;
    d += usf;
    data->totalLen = d;
    data->totalDataLen = d * DEFAULT_RATE * 2 * 2;
    data->maybeFillBuffer(true);
    data->stopped = 0;
    
    
    pthread_attr_t tattr;
    sched_param param;
    
    pthread_attr_init (&tattr);
    pthread_attr_getschedparam (&tattr, &param);
    param.sched_priority = 10;
    pthread_attr_setschedparam (&tattr, &param);
    
    pthread_create(&data->fillThread, NULL, BufferFillThread, (void *)data);
}

/*
 *
 */
SDLOutput::~SDLOutput()
{
    Stop();
    delete data;
}

/*
 *
 */
int SDLOutput::Start(void)
{
	LogDebug(VB_MEDIAOUT, "mpg123Output::Start()\n");
    sdlManager.Start(data);
	m_mediaOutputStatus->status = MEDIAOUTPUTSTATUS_PLAYING;
	return 1;
}

/*
 *
 */
int SDLOutput::Process(void)
{
    static int lastRemoteSync = 0;

    unsigned int cur = data->curPos;
    float pct = cur;
    pct /= data->totalDataLen;
    
    m_mediaOutputStatus->mediaSeconds = data->totalLen * pct;

    float ss, s;
    ss = std::modf( m_mediaOutputStatus->mediaSeconds, &s);
    m_mediaOutputStatus->secondsElapsed = s;
    ss *= 100;
    m_mediaOutputStatus->subSecondsElapsed = ss;

    float rem = data->totalLen - m_mediaOutputStatus->mediaSeconds;
    ss = std::modf( rem, &s);
    m_mediaOutputStatus->secondsRemaining = s;
    ss *= 100;
    m_mediaOutputStatus->subSecondsRemaining = ss;

    
    if (data->curBuffer == nullptr) {
        m_mediaOutputStatus->status = MEDIAOUTPUTSTATUS_IDLE;
    }
    if (getFPPmode() == MASTER_MODE)
    {
        if ((m_mediaOutputStatus->secondsElapsed > 0) &&
            (lastRemoteSync != m_mediaOutputStatus->secondsElapsed))
        {
            SendMediaSyncPacket(m_mediaFilename.c_str(), 0,
                                m_mediaOutputStatus->mediaSeconds);
            lastRemoteSync = m_mediaOutputStatus->secondsElapsed;
        }
    }
    
    if ((sequence->IsSequenceRunning()) &&
        (m_mediaOutputStatus->secondsElapsed > 0))
    {
        LogExcess(VB_MEDIAOUT,
                  "Elapsed: %.2d.%.2d  Remaining: %.2d Total %.2d:%.2d.\n",
                  m_mediaOutputStatus->secondsElapsed,
                  m_mediaOutputStatus->subSecondsElapsed,
                  m_mediaOutputStatus->secondsRemaining,
                  m_mediaOutputStatus->minutesTotal,
                  m_mediaOutputStatus->secondsTotal);
        
        CalculateNewChannelOutputDelay(m_mediaOutputStatus->mediaSeconds);
    }
	return 1;
}
int SDLOutput::IsPlaying(void) {
    return m_mediaOutputStatus->status == MEDIAOUTPUTSTATUS_PLAYING;
}

int  SDLOutput::Close(void)
{
    Stop();
}

/*
 *
 */
int SDLOutput::Stop(void)
{
	LogDebug(VB_MEDIAOUT, "SDLOutput::Stop()\n");
    data->stopped++;
    sdlManager.Stop();
	m_mediaOutputStatus->status = MEDIAOUTPUTSTATUS_IDLE;
    pthread_join( data->fillThread, NULL);

	return 1;
}

