/*
 * Movies: played by the host.
 *
 * The game plays its logo and opening movies (and a few later ones) through
 * DirectShow, which is Windows' own: MoviePlay (0x004886D0) builds a filter
 * graph for the file and the game then draws what the graph decodes, frame by
 * frame, until the movie ends or a button is pressed. None of that exists
 * here. MoviePlay is wrapped (WRAPS in run_lift.py), and on this host the
 * wrap plays the whole file itself, with ffmpeg, before it returns: the
 * picture through the Direct3D layer's back buffer, the sound through the
 * mixer. The game is never told a movie started, so it goes straight on.
 *
 * Without ffmpeg (FC_WITH_FFMPEG) there is no player and a movie is skipped.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "win32hle.h"

int fc_movie_log;

#ifdef FC_WITH_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/* A wait with the machine given up, so the game's other threads (its music
 * timer) keep their turns. */
static void nap(unsigned ms) {
    hle_block_begin();
    usleep(ms * 1000u);
    hle_block_end();
}

/* Plays `path` to its end, or until skip() says to stop. 1 if it played. */
int fc_movie_play(const char* path, int (*skip)(void)) {
    AVFormatContext* fmt = NULL;
    AVCodecContext *vdec = NULL, *adec = NULL;
    struct SwsContext* sws = NULL;
    SwrContext* swr = NULL;
    AVFrame *frame = av_frame_alloc(), *aframe = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    uint8_t* rgba = NULL;
    int16_t* pcm = NULL;
    int vs, as, stream = -1, shown = 0, dropped = 0, ok = 0, stop = 0;
    int ow = 0, oh = 0;
    uint64_t pushed = 0;
    uint32_t t0 = 0;

    if (avformat_open_input(&fmt, path, NULL, NULL) != 0 || avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "[movie] %s could not be opened\n", path);
        goto done;
    }
    vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vs, NULL, 0);
    if (vs < 0) { fprintf(stderr, "[movie] %s has no picture\n", path); goto done; }
    {
        const AVCodec* c = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
        vdec = c ? avcodec_alloc_context3(c) : NULL;
        if (!vdec || avcodec_parameters_to_context(vdec, fmt->streams[vs]->codecpar) < 0) goto done;
        vdec->thread_count = 3;                       /* the console has four cores, and one is the game's */
        vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(vdec, c, NULL) < 0) { fprintf(stderr, "[movie] no decoder for the picture of %s\n", path); goto done; }
    }
    if (as >= 0) {
        const AVCodec* c = avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id);
        adec = c ? avcodec_alloc_context3(c) : NULL;
        if (adec && (avcodec_parameters_to_context(adec, fmt->streams[as]->codecpar) < 0 || avcodec_open2(adec, c, NULL) < 0))
            avcodec_free_context(&adec);
        if (adec) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, adec->sample_rate, &adec->ch_layout, adec->sample_fmt,
                                    adec->sample_rate, 0, NULL) < 0 || swr_init(swr) < 0)
                avcodec_free_context(&adec);
        }
        if (adec) stream = hle_audio_stream_open(adec->sample_rate, 2);
        if (adec) pcm = malloc(4 * 16384);
    }
    /* The picture is shown no larger than 1280 by 720: what is sent to the
     * graphics chip each frame is a quarter of a 1080p picture's bytes. */
    ow = vdec->width, oh = vdec->height;
    if (ow > 1280) oh = oh * 1280 / ow, ow = 1280;
    if (oh > 720) ow = ow * 720 / oh, oh = 720;
    ow &= ~1, oh &= ~1;
    rgba = malloc((size_t)ow * (size_t)oh * 4);
    if (!rgba || ow <= 0 || oh <= 0) goto done;
    fprintf(stderr, "[movie] %s: %dx%d %s shown at %dx%d, %s\n", path, vdec->width, vdec->height, vdec->codec->name, ow, oh,
            adec ? adec->codec->name : "no sound");
    ok = 1;
    t0 = hle_ticks_ms();

    while (!stop && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(vdec, pkt) == 0) {
            while (!stop && avcodec_receive_frame(vdec, frame) == 0) {
                double at = frame->best_effort_timestamp == AV_NOPTS_VALUE ? 0.0
                          : (double)frame->best_effort_timestamp * av_q2d(fmt->streams[vs]->time_base);
                for (;;) {                            /* until this frame is due */
                    /* The clock is the wall's. The sound is handed to the mixer as
                     * it is read, a little ahead of the picture beside it in the
                     * file, and plays from the first sample on at its own rate:
                     * the two start together and stay so. (What the mixer has
                     * played cannot be the clock: a file whose picture starts
                     * after its sound would wait for sound that is only read
                     * once the wait is over.) */
                    double now = (double)(hle_ticks_ms() - t0) / 1000.0;
                    if (skip && skip()) { stop = 1; break; }
                    if (fc_movie_log) {
                        static uint32_t said;
                        if (hle_ticks_ms() - said >= 1000) {
                            said = hle_ticks_ms();
                            fprintf(stderr, "[movie] frame due at %.3f, clock %.3f, %d shown, sound: %llu pushed, %d waiting\n", at, now, shown,
                                    (unsigned long long)pushed, stream >= 0 ? hle_audio_stream_queued(stream) : -1);
                        }
                    }
                    if (at - now < 0.002) {
                        if (now - at > 0.12) { dropped++; break; }     /* too late to be worth showing */
                        sws = sws_getCachedContext(sws, frame->width, frame->height, (enum AVPixelFormat)frame->format, ow, oh,
                                                   AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
                        if (sws) {
                            uint8_t* planes[4] = { rgba, NULL, NULL, NULL };
                            int strides[4] = { ow * 4, 0, 0, 0 };
                            sws_scale(sws, (const uint8_t* const*)frame->data, frame->linesize, 0, frame->height, planes, strides);
                            hle_d3d9_show_picture(rgba, ow, oh);
                            shown++;
                        }
                        break;
                    }
                    nap(at - now > 0.02 ? 8 : 1);
                }
                av_frame_unref(frame);
            }
        } else if (adec && pkt->stream_index == as && avcodec_send_packet(adec, pkt) == 0) {
            while (avcodec_receive_frame(adec, aframe) == 0) {
                int n = swr_convert(swr, (uint8_t**)&pcm, 16384, (const uint8_t**)aframe->extended_data, aframe->nb_samples);
                if (n > 0 && stream >= 0) hle_audio_stream_push(stream, pcm, n), pushed += (uint64_t)n;
                av_frame_unref(aframe);
            }
        }
        av_packet_unref(pkt);
    }
    /* what was pushed and not yet heard, unless the player left */
    while (!stop && stream >= 0 && hle_audio_stream_queued(stream) > 0 && !(skip && skip())) nap(10);
    fprintf(stderr, "[movie] %s: %d frames shown, %d dropped, %.1f s%s\n", path, shown, dropped, (double)(hle_ticks_ms() - t0) / 1000.0,
            stop ? " (stopped by the player)" : "");
done:
    if (stream >= 0) hle_audio_stream_close(stream);
    free(pcm), free(rgba);
    if (sws) sws_freeContext(sws);
    if (swr) swr_free(&swr);
    if (vdec) avcodec_free_context(&vdec);
    if (adec) avcodec_free_context(&adec);
    if (fmt) avformat_close_input(&fmt);
    av_frame_free(&frame), av_frame_free(&aframe);
    av_packet_free(&pkt);
    return ok;
}
#else
int fc_movie_play(const char* path, int (*skip)(void)) {
    (void)skip;
    fprintf(stderr, "[movie] %s is skipped: this build has no movie player (it needs ffmpeg)\n", path);
    return 0;
}
#endif
