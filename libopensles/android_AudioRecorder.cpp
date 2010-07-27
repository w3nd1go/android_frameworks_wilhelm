/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include "sles_allinclusive.h"

// use this flag to dump all recorded audio into a file
//#define MONITOR_RECORDING
#ifdef MONITOR_RECORDING
#define MONITOR_TARGET "/sdcard/monitor.raw"
#include <stdio.h>
static FILE* gMonitorFp = NULL;
#endif

#define ERROR_SINK_MUST_BE_BUFFERQUEUE \
        "Cannot create AudioRecorder: data sink must be SL_DATALOCATOR_BUFFERQUEUE"
#define ERROR_SINK_FORMAT_MUST_BE_PCM \
        "Cannot create AudioRecorder: data sink must be in PCM format"
#define ERROR_SOURCE_MUST_BE_IODEVICE \
        "Cannot create AudioRecorder: data source must be SL_DATALOCATOR_IODEVICE"
#define ERROR_IODEVICE_MUST_BE_AUDIOINPUT \
        "Cannot create AudioRecorder: data source device type must be SL_IODEVICE_AUDIOINPUT"
#define ERROR_INPUT_ID_MUST_BE_DEFAULT \
        "Cannot create AudioRecorder: data source device ID must be SL_DEFAULTDEVICEID_AUDIOINPUT"

//-----------------------------------------------------------------------------
SLresult android_audioRecorder_checkSourceSinkSupport(CAudioRecorder* ar) {

    const SLDataSource *pAudioSrc = &ar->mDataSource.u.mSource;
    const SLDataSink   *pAudioSnk = &ar->mDataSink.u.mSink;

    // Sink check:
    // only buffer queue sinks are supported, regardless of the data source
    if (SL_DATALOCATOR_BUFFERQUEUE != *(SLuint32 *)pAudioSnk->pLocator) {
        SL_LOGE(ERROR_SINK_MUST_BE_BUFFERQUEUE);
        return SL_RESULT_PARAMETER_INVALID;
    } else {
        // only PCM buffer queues are supported
        SLuint32 formatType = *(SLuint32 *)pAudioSnk->pFormat;
        if (SL_DATAFORMAT_PCM == formatType) {
            SLDataFormat_PCM *df_pcm = (SLDataFormat_PCM *)ar->mDataSink.u.mSink.pFormat;
            ar->mSampleRateMilliHz = df_pcm->samplesPerSec;
            ar->mNumChannels = df_pcm->numChannels;
            SL_LOGV("AudioRecorder requested sample rate = %lumHz, %u channel(s)",
                    ar->mSampleRateMilliHz, ar->mNumChannels);
        }
        else {
            SL_LOGE(ERROR_SINK_FORMAT_MUST_BE_PCM);
            return SL_RESULT_PARAMETER_INVALID;
        }
    }

    // Source check:
    // only input device sources are supported
    // check it's an IO device
    if (SL_DATALOCATOR_IODEVICE != *(SLuint32 *)pAudioSrc->pLocator) {
        SL_LOGE(ERROR_SOURCE_MUST_BE_IODEVICE);
        return SL_RESULT_PARAMETER_INVALID;
    } else {

        // check it's an input device
        SLDataLocator_IODevice *dl_iod =  (SLDataLocator_IODevice *) pAudioSrc->pLocator;
        if (SL_IODEVICE_AUDIOINPUT != dl_iod->deviceType) {
            SL_LOGE(ERROR_IODEVICE_MUST_BE_AUDIOINPUT);
            return SL_RESULT_PARAMETER_INVALID;
        }

        // check it's the default input device, others aren't supported here
        if (SL_DEFAULTDEVICEID_AUDIOINPUT != dl_iod->deviceID) {
            SL_LOGE(ERROR_INPUT_ID_MUST_BE_DEFAULT);
            return SL_RESULT_PARAMETER_INVALID;
        }
    }

    return SL_RESULT_SUCCESS;
}
//-----------------------------------------------------------------------------
static void audioRecorder_callback(int event, void* user, void *info) {
    //SL_LOGV("audioRecorder_callback(%d, %p, %p) entering", event, user, info);

    CAudioRecorder *ar = (CAudioRecorder *)user;
    void * callbackPContext = NULL;

    switch(event) {
    case android::AudioRecord::EVENT_MORE_DATA: {
        slBufferQueueCallback callback = NULL;
        android::AudioRecord::Buffer* pBuff = (android::AudioRecord::Buffer*)info;

        // push data to the buffer queue
        interface_lock_exclusive(&ar->mBufferQueue);

        if (ar->mBufferQueue.mState.count != 0) {
            assert(ar->mBufferQueue.mFront != ar->mBufferQueue.mRear);

            BufferHeader *oldFront = ar->mBufferQueue.mFront;
            BufferHeader *newFront = &oldFront[1];

            // FIXME handle 8bit based on buffer format
            short *pDest = (short*)((char *)oldFront->mBuffer + ar->mBufferQueue.mSizeConsumed);
            if (ar->mBufferQueue.mSizeConsumed + pBuff->size < oldFront->mSize) {
                // can't consume the whole or rest of the buffer in one shot
                ar->mBufferQueue.mSizeConsumed += pBuff->size;
                // leave pBuff->size untouched
                // consume data
                // FIXME can we avoid holding the lock during the copy?
                memcpy (pDest, pBuff->i16, pBuff->size);
#ifdef MONITOR_RECORDING
                if (NULL != gMonitorFp) { fwrite(pBuff->i16, pBuff->size, 1, gMonitorFp); }
#endif
            } else {
                // finish pushing the buffer or push the buffer in one shot
                pBuff->size = oldFront->mSize - ar->mBufferQueue.mSizeConsumed;
                ar->mBufferQueue.mSizeConsumed = 0;
                if (newFront ==  &ar->mBufferQueue.mArray[ar->mBufferQueue.mNumBuffers + 1]) {
                    newFront = ar->mBufferQueue.mArray;
                }
                ar->mBufferQueue.mFront = newFront;

                ar->mBufferQueue.mState.count--;
                ar->mBufferQueue.mState.playIndex++;
                // consume data
                // FIXME can we avoid holding the lock during the copy?
                memcpy (pDest, pBuff->i16, pBuff->size);
#ifdef MONITOR_RECORDING
                if (NULL != gMonitorFp) { fwrite(pBuff->i16, pBuff->size, 1, gMonitorFp); }
#endif
                // data has been copied to the buffer, and the buffer queue state has been updated
                // we will notify the client if applicable
                callback = ar->mBufferQueue.mCallback;
                // save callback data
                callbackPContext = ar->mBufferQueue.mContext;
            }
        } else {
            // no destination to push the data
            pBuff->size = 0;
        }

        interface_unlock_exclusive(&ar->mBufferQueue);
        // notify client
        if (NULL != callback) {
            (*callback)(&ar->mBufferQueue.mItf, callbackPContext);
        }
        }
        break;

    case android::AudioRecord::EVENT_MARKER:
        // FIXME implement
        SL_LOGE("FIXME audioRecorder_callback(EVENT_MARKER, %p, %p) not supported", user, info);
        break;

    case android::AudioRecord::EVENT_NEW_POS:
        // FIXME implement
        SL_LOGE("FIXME audioRecorder_callback(EVENT_NEW_POS, %p, %p) not supported", user, info);
        break;

    }
}


//-----------------------------------------------------------------------------
SLresult android_audioRecorder_create(CAudioRecorder* ar) {
    SL_LOGV("android_audioRecorder_create(%p) entering", ar);

    SLresult result = SL_RESULT_SUCCESS;

    ar->mAudioRecord = NULL;

    return result;
}


//-----------------------------------------------------------------------------
SLresult android_audioRecorder_realize(CAudioRecorder* ar, SLboolean async) {
    SL_LOGV("android_audioRecorder_realize(%p) entering", ar);

    SLresult result = SL_RESULT_SUCCESS;

    // initialize platform-independent CAudioRecorder fields
    if (SL_DATALOCATOR_BUFFERQUEUE != ar->mDataSink.mLocator.mLocatorType) {
        SL_LOGE(ERROR_SINK_MUST_BE_BUFFERQUEUE);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }
    //  the following platform-independent field have been initialized in CreateAudioRecorder()
    //    ar->mNumChannels
    //    ar->mSampleRateMilliHz

    SL_LOGV("new AudioRecord %u channels, %lu mHz\n", ar->mNumChannels, ar->mSampleRateMilliHz);

    // initialize platform-specific CAudioRecorder fields
    ar->mAudioRecord = new android::AudioRecord();
    ar->mAudioRecord->set(android::AUDIO_SOURCE_DEFAULT, // source
            sles_to_android_sampleRate(ar->mSampleRateMilliHz), // sample rate in Hertz
            android::AudioSystem::PCM_16_BIT,   //FIXME use format from buffer queue sink
            sles_to_android_channelMask(ar->mNumChannels, 0 /*no channel mask*/), // channel config
            0,                     //frameCount min
            0,                     // flags
            audioRecorder_callback,// callback_t
            (void*)ar,             // user, callback data, here the AudioRecorder
            0,                     // notificationFrames
            false);                // threadCanCallJava, note: this will prevent direct Java
                                   //   callbacks, but we don't want them in the recording loop

    if (android::NO_ERROR != ar->mAudioRecord->initCheck()) {
        SL_LOGE("android_audioRecorder_realize(%p) error creating AudioRecord object", ar);
        result = SL_RESULT_CONTENT_UNSUPPORTED;
    }

#ifdef MONITOR_RECORDING
    gMonitorFp = fopen(MONITOR_TARGET, "w");
    if (NULL == gMonitorFp) { SL_LOGE("error opening %s", MONITOR_TARGET); }
    else { SL_LOGE("recording to %s", MONITOR_TARGET); } // LOGE so it's always displayed
#endif

    return result;
}


//-----------------------------------------------------------------------------
void android_audioRecorder_destroy(CAudioRecorder* ar) {
    SL_LOGV("android_audioRecorder_destroy(%p) entering", ar);

    if (NULL != ar->mAudioRecord) {
        ar->mAudioRecord->stop();
        delete ar->mAudioRecord;
        ar->mAudioRecord = NULL;
    }

#ifdef MONITOR_RECORDING
    if (NULL != gMonitorFp) { fclose(gMonitorFp); }
    gMonitorFp = NULL;
#endif
}


//-----------------------------------------------------------------------------
void android_audioRecorder_setRecordState(CAudioRecorder* ar, SLuint32 state) {
    SL_LOGV("android_audioRecorder_setRecordState(%p, %lu) entering", ar, state);

    if (NULL == ar->mAudioRecord) {
        return;
    }

    switch (state) {
     case SL_RECORDSTATE_STOPPED:
         ar->mAudioRecord->stop();
         break;
     case SL_RECORDSTATE_PAUSED:
         // Note that pausing is treated like stop as this implementation only records to a buffer
         //  queue, so there is no notion of destination being "opened" or "closed" (See description
         //  of SL_RECORDSTATE in specification)
         ar->mAudioRecord->stop();
         break;
     case SL_RECORDSTATE_RECORDING:
         ar->mAudioRecord->start();
         break;
     default:
         break;
     }

}