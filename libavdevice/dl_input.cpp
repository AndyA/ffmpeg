/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <exception>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "DeckLinkAPI.h"
#include "dl_input.h"

DeckLinkCaptureDelegate::DeckLinkCaptureDelegate(AVFormatContext *ctx) :
  m_ctx(ctx), m_refCount(0) {
  pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkCaptureDelegate::~DeckLinkCaptureDelegate() {
  pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkCaptureDelegate::AddRef(void) {
  pthread_mutex_lock(&m_mutex);
  m_refCount++;
  pthread_mutex_unlock(&m_mutex);

  return (ULONG)m_refCount;
}

ULONG DeckLinkCaptureDelegate::Release(void) {
  pthread_mutex_lock(&m_mutex);
  m_refCount--;
  pthread_mutex_unlock(&m_mutex);

  if (m_refCount == 0) {
    delete this;
    return 0;
  }

  return (ULONG)m_refCount;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived(
  IDeckLinkVideoInputFrame *vf, IDeckLinkAudioInputPacket *af) {
  struct decklink_pipe *self = (struct decklink_pipe *) m_ctx->priv_data;
  int rc;

  pthread_mutex_lock(&self->mutex);

  if (!self->fifo) goto done;

  // Handle Video Frame
  if (vf) {

    if (vf->GetFlags() & bmdFrameHasNoInputSource) {
      av_log(m_ctx, AV_LOG_WARNING, "No input detected\n");
    }
    else {
      AVPacket pkt;
      size_t size = vf->GetRowBytes() * vf->GetHeight();
      void *bytes;
      BMDTimeValue frame_time, frame_duration;

      if (av_fifo_space(self->fifo) < (int) sizeof(pkt)) {
        av_log(m_ctx, AV_LOG_ERROR, "Fifo overrun\n");
        goto done;
      }

      if (rc = av_new_packet(&pkt, size), rc < 0) {
        av_log(m_ctx, AV_LOG_ERROR, "Could not create packet of size %d\n", size);
        goto done;
      }

      vf->GetBytes(&bytes);
      memcpy(pkt.data, bytes, size);

      vf->GetStreamTime(&frame_time, &frame_duration, 1000000);
      pkt.dts = pkt.pts = frame_time;
      pkt.stream_index = 0;
      pkt.flags |= AV_PKT_FLAG_KEY;

//      av_log(m_ctx, AV_LOG_INFO, "video frame: pts=%llu\n", (unsigned long long) pkt.pts);

      av_fifo_generic_write(self->fifo, &pkt, sizeof(pkt), NULL);
    }
  }

  // Handle Audio Frame
  if (af) {
    AVPacket pkt;
    size_t size = af->GetSampleFrameCount() * 2 * 2;
    void *bytes;
    BMDTimeValue packet_time;

    if (av_fifo_space(self->fifo) < (int) sizeof(pkt)) {
      av_log(m_ctx, AV_LOG_ERROR, "Fifo overrun\n");
      goto done;
    }

    if (rc = av_new_packet(&pkt, size), rc < 0) {
      av_log(m_ctx, AV_LOG_ERROR, "Could not create packet of size %d\n", size);
      goto done;
    }

    af->GetBytes(&bytes);
    memcpy(pkt.data, bytes, size);

    af->GetPacketTime(&packet_time, 1000000);
    pkt.pts = packet_time;
    pkt.stream_index = 1;
    pkt.flags |= AV_PKT_FLAG_KEY;

//    av_log(m_ctx, AV_LOG_INFO, "audio frame: pts=%llu\n", (unsigned long long) pkt.pts);

    av_fifo_generic_write(self->fifo, &pkt, sizeof(pkt), NULL);
  }

  pthread_cond_signal(&self->non_empty);

done:
  pthread_mutex_unlock(&self->mutex);

  return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFormatChanged(
  BMDVideoInputFormatChangedEvents events,
  IDeckLinkDisplayMode *mode,
  BMDDetectedVideoInputFormatFlags) {
  return S_OK;
}

static int dl_find(AVFormatContext *ctx, IDeckLink **dl) {
  IDeckLinkIterator *dli = CreateDeckLinkIteratorInstance();
  HRESULT rc;

  if (!dli) {
    av_log(ctx, AV_LOG_ERROR,
           "Can't find a Decklink device. Are the drivers installed?\n");
    return AVERROR_EXTERNAL;
  }

  if (rc = dli->Next(dl), rc != S_OK) {
    av_log(ctx, AV_LOG_ERROR, "No Decklink devices found.\n");
    return AVERROR_EXTERNAL;
  }

  dli->Release();
  return 0;
}

extern "C" int dl_startup(AVFormatContext *ctx) {
  struct decklink_pipe *self = (struct decklink_pipe *) ctx->priv_data;
  struct decklink_work *w = (struct decklink_work *) av_mallocz(sizeof(*w));
  int rc;

  self->w = w;

  if ((rc = dl_find(ctx, &w->dl))) goto bail;

  if (w->dl->QueryInterface(IID_IDeckLinkInput, (void **)&w->dli) != S_OK) {
    av_log(ctx, AV_LOG_ERROR, "No inputs found\n");
    goto bail_err;
  }

  w->delegate = new DeckLinkCaptureDelegate(ctx);
  w->dli->SetCallback(w->delegate);

  if (w->dli->EnableVideoInput(bmdModeHD1080i50, bmdFormat8BitYUV, 0) != S_OK) {
    av_log(ctx, AV_LOG_ERROR, "Failed to enable video input\n");
    goto bail_err;
  }

  if (w->dli->EnableAudioInput(bmdAudioSampleRate48kHz, 16, 2) != S_OK) {
    av_log(ctx, AV_LOG_ERROR, "Failed to enable audio input\n");
    goto bail_err;
  }

  if (w->dli->StartStreams() != S_OK) {
    av_log(ctx, AV_LOG_ERROR, "Failed to enable audio input\n");
    goto bail_err;
  }

  return 0;

bail_err:
  rc = AVERROR_EXTERNAL;
bail:
  dl_shutdown(ctx);
  return rc;
}

extern "C" int dl_shutdown(AVFormatContext *ctx) {
  struct decklink_pipe *self = (struct decklink_pipe *) ctx->priv_data;
  struct decklink_work *w = self->w;

  if (w->dli) w->dli->Release();
  if (w->dl) w->dl->Release();
  if (w->delegate) w->delegate->Release();

  av_free(self->w);
  return 0;
}

