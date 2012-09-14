/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "Camera2Client::ZslProcessor"
#define ATRACE_TAG ATRACE_TAG_CAMERA
//#define LOG_NDEBUG 0
//#define LOG_NNDEBUG 0

#ifdef LOG_NNDEBUG
#define ALOGVV(...) ALOGV(__VA_ARGS__)
#else
#define ALOGVV(...) ((void)0)
#endif

#include <utils/Log.h>
#include <utils/Trace.h>

#include "ZslProcessor.h"
#include <gui/SurfaceTextureClient.h>
#include "../Camera2Device.h"
#include "../Camera2Client.h"


namespace android {
namespace camera2 {

ZslProcessor::ZslProcessor(
    wp<Camera2Client> client,
    wp<CaptureSequencer> sequencer):
        Thread(false),
        mState(RUNNING),
        mClient(client),
        mSequencer(sequencer),
        mZslBufferAvailable(false),
        mZslStreamId(NO_STREAM),
        mZslReprocessStreamId(NO_STREAM),
        mFrameListHead(0),
        mZslQueueHead(0),
        mZslQueueTail(0) {
    mZslQueue.insertAt(0, kZslBufferDepth);
    mFrameList.insertAt(0, kFrameListDepth);
    sp<CaptureSequencer> captureSequencer = mSequencer.promote();
    if (captureSequencer != 0) captureSequencer->setZslProcessor(this);
}

ZslProcessor::~ZslProcessor() {
    ALOGV("%s: Exit", __FUNCTION__);
    deleteStream();
}

void ZslProcessor::onFrameAvailable() {
    Mutex::Autolock l(mInputMutex);
    if (!mZslBufferAvailable) {
        mZslBufferAvailable = true;
        mZslBufferAvailableSignal.signal();
    }
}

void ZslProcessor::onFrameAvailable(int32_t frameId, CameraMetadata &frame) {
    Mutex::Autolock l(mInputMutex);
    camera_metadata_entry_t entry;
    entry = frame.find(ANDROID_SENSOR_TIMESTAMP);
    nsecs_t timestamp = entry.data.i64[0];
    ALOGVV("Got preview frame for timestamp %lld", timestamp);

    if (mState != RUNNING) return;

    mFrameList.editItemAt(mFrameListHead).acquire(frame);
    mFrameListHead = (mFrameListHead + 1) % kFrameListDepth;

    findMatchesLocked();
}

void ZslProcessor::onBufferReleased(buffer_handle_t *handle) {
    Mutex::Autolock l(mInputMutex);

    buffer_handle_t *expectedHandle =
            &(mZslQueue[mZslQueueTail].buffer.mGraphicBuffer->handle);

    if (handle != expectedHandle) {
        ALOGE("%s: Expected buffer %p, got buffer %p",
                __FUNCTION__, expectedHandle, handle);
    }

    mState = RUNNING;
}

status_t ZslProcessor::updateStream(const Parameters &params) {
    ATRACE_CALL();
    ALOGV("%s: Configuring ZSL streams", __FUNCTION__);
    status_t res;

    Mutex::Autolock l(mInputMutex);

    sp<Camera2Client> client = mClient.promote();
    if (client == 0) return OK;
    sp<Camera2Device> device = client->getCameraDevice();

    if (mZslConsumer == 0) {
        // Create CPU buffer queue endpoint
        mZslConsumer = new BufferItemConsumer(
            GRALLOC_USAGE_HW_CAMERA_ZSL,
            kZslBufferDepth,
            true);
        mZslConsumer->setFrameAvailableListener(this);
        mZslConsumer->setName(String8("Camera2Client::ZslConsumer"));
        mZslWindow = new SurfaceTextureClient(
            mZslConsumer->getProducerInterface());
    }

    if (mZslStreamId != NO_STREAM) {
        // Check if stream parameters have to change
        uint32_t currentWidth, currentHeight;
        res = device->getStreamInfo(mZslStreamId,
                &currentWidth, &currentHeight, 0);
        if (res != OK) {
            ALOGE("%s: Camera %d: Error querying capture output stream info: "
                    "%s (%d)", __FUNCTION__,
                    client->getCameraId(), strerror(-res), res);
            return res;
        }
        if (currentWidth != (uint32_t)params.fastInfo.arrayWidth ||
                currentHeight != (uint32_t)params.fastInfo.arrayHeight) {
            res = device->deleteReprocessStream(mZslReprocessStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old reprocess stream "
                        "for ZSL: %s (%d)", __FUNCTION__,
                        client->getCameraId(), strerror(-res), res);
                return res;
            }
            res = device->deleteStream(mZslStreamId);
            if (res != OK) {
                ALOGE("%s: Camera %d: Unable to delete old output stream "
                        "for ZSL: %s (%d)", __FUNCTION__,
                        client->getCameraId(), strerror(-res), res);
                return res;
            }
            mZslStreamId = NO_STREAM;
        }
    }

    if (mZslStreamId == NO_STREAM) {
        // Create stream for HAL production
        // TODO: Sort out better way to select resolution for ZSL
        res = device->createStream(mZslWindow,
                params.fastInfo.arrayWidth, params.fastInfo.arrayHeight,
                CAMERA2_HAL_PIXEL_FORMAT_ZSL, 0,
                &mZslStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create output stream for ZSL: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(),
                    strerror(-res), res);
            return res;
        }
        res = device->createReprocessStreamFromStream(mZslStreamId,
                &mZslReprocessStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Can't create reprocess stream for ZSL: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(),
                    strerror(-res), res);
            return res;
        }
    }
    client->registerFrameListener(Camera2Client::kPreviewRequestId, this);

    return OK;
}

status_t ZslProcessor::deleteStream() {
    ATRACE_CALL();
    status_t res;

    Mutex::Autolock l(mInputMutex);

    if (mZslStreamId != NO_STREAM) {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return OK;
        sp<Camera2Device> device = client->getCameraDevice();

        res = device->deleteReprocessStream(mZslReprocessStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Cannot delete ZSL reprocessing stream %d: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(),
                    mZslReprocessStreamId, strerror(-res), res);
            return res;
        }

        mZslReprocessStreamId = NO_STREAM;
        res = device->deleteStream(mZslStreamId);
        if (res != OK) {
            ALOGE("%s: Camera %d: Cannot delete ZSL output stream %d: "
                    "%s (%d)", __FUNCTION__, client->getCameraId(),
                    mZslStreamId, strerror(-res), res);
            return res;
        }

        mZslWindow.clear();
        mZslConsumer.clear();

        mZslStreamId = NO_STREAM;
    }
    return OK;
}

int ZslProcessor::getStreamId() const {
    Mutex::Autolock l(mInputMutex);
    return mZslStreamId;
}

int ZslProcessor::getReprocessStreamId() const {
    Mutex::Autolock l(mInputMutex);
    return mZslReprocessStreamId;
}

status_t ZslProcessor::pushToReprocess(int32_t requestId) {
    ALOGV("%s: Send in reprocess request with id %d",
            __FUNCTION__, requestId);
    Mutex::Autolock l(mInputMutex);
    status_t res;
    sp<Camera2Client> client = mClient.promote();

    if (client == 0) return false;

    if (mZslQueueTail != mZslQueueHead) {
        CameraMetadata request;
        size_t index = mZslQueueTail;
        while (request.isEmpty() && index != mZslQueueHead) {
            request = mZslQueue[index].frame;
            index = (index + 1) % kZslBufferDepth;
        }
        if (request.isEmpty()) {
            ALOGE("No request in ZSL queue to send!");
            return BAD_VALUE;
        }
        buffer_handle_t *handle =
            &(mZslQueue[index].buffer.mGraphicBuffer->handle);

        uint8_t requestType = ANDROID_REQUEST_TYPE_REPROCESS;
        res = request.update(ANDROID_REQUEST_TYPE,
                &requestType, 1);
        uint8_t inputStreams[1] = { mZslReprocessStreamId };
        if (res == OK) request.update(ANDROID_REQUEST_INPUT_STREAMS,
                inputStreams, 1);
        uint8_t outputStreams[1] = { client->getCaptureStreamId() };
        if (res == OK) request.update(ANDROID_REQUEST_OUTPUT_STREAMS,
                outputStreams, 1);
        res = request.update(ANDROID_REQUEST_ID,
                &requestId, 1);

        if (res != OK ) {
            ALOGE("%s: Unable to update frame to a reprocess request", __FUNCTION__);
            return INVALID_OPERATION;
        }

        res = client->getCameraDevice()->pushReprocessBuffer(mZslReprocessStreamId,
                handle, this);
        if (res != OK) {
            ALOGE("%s: Unable to push buffer for reprocessing: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }

        res = client->getCameraDevice()->capture(request);
        if (res != OK ) {
            ALOGE("%s: Unable to send ZSL reprocess request to capture: %s (%d)",
                    __FUNCTION__, strerror(-res), res);
            return res;
        }

        mState = LOCKED;
    } else {
        ALOGE("%s: Nothing to push", __FUNCTION__);
        return BAD_VALUE;
    }
    return OK;
}

void ZslProcessor::dump(int fd, const Vector<String16>& args) const {
}

bool ZslProcessor::threadLoop() {
    status_t res;

    {
        Mutex::Autolock l(mInputMutex);
        while (!mZslBufferAvailable) {
            res = mZslBufferAvailableSignal.waitRelative(mInputMutex,
                    kWaitDuration);
            if (res == TIMED_OUT) return true;
        }
        mZslBufferAvailable = false;
    }

    do {
        sp<Camera2Client> client = mClient.promote();
        if (client == 0) return false;
        res = processNewZslBuffer(client);
    } while (res == OK);

    return true;
}

status_t ZslProcessor::processNewZslBuffer(sp<Camera2Client> &client) {
    ATRACE_CALL();
    status_t res;

    ALOGVV("Trying to get next buffer");
    BufferItemConsumer::BufferItem item;
    res = mZslConsumer->acquireBuffer(&item);
    if (res != OK) {
        if (res != BufferItemConsumer::NO_BUFFER_AVAILABLE) {
            ALOGE("%s: Camera %d: Error receiving ZSL image buffer: "
                    "%s (%d)", __FUNCTION__,
                    client->getCameraId(), strerror(-res), res);
        } else {
            ALOGVV("  No buffer");
        }
        return res;
    }

    Mutex::Autolock l(mInputMutex);

    if (mState == LOCKED) {
        ALOGVV("In capture, discarding new ZSL buffers");
        mZslConsumer->releaseBuffer(item);
        return OK;
    }

    ALOGVV("Got ZSL buffer: head: %d, tail: %d", mZslQueueHead, mZslQueueTail);

    if ( (mZslQueueHead + 1) % kZslBufferDepth == mZslQueueTail) {
        ALOGVV("Releasing oldest buffer");
        mZslConsumer->releaseBuffer(mZslQueue[mZslQueueTail].buffer);
        mZslQueue.replaceAt(mZslQueueTail);
        mZslQueueTail = (mZslQueueTail + 1) % kZslBufferDepth;
    }

    ZslPair &queueHead = mZslQueue.editItemAt(mZslQueueHead);

    queueHead.buffer = item;
    queueHead.frame.release();

    mZslQueueHead = (mZslQueueHead + 1) % kZslBufferDepth;

    ALOGVV("  Acquired buffer, timestamp %lld", queueHead.buffer.mTimestamp);

    findMatchesLocked();

    return OK;
}

void ZslProcessor::findMatchesLocked() {
    ALOGVV("Scanning");
    for (size_t i = 0; i < mZslQueue.size(); i++) {
        ZslPair &queueEntry = mZslQueue.editItemAt(i);
        nsecs_t bufferTimestamp = queueEntry.buffer.mTimestamp;
        IF_ALOGV() {
            camera_metadata_entry_t entry;
            nsecs_t frameTimestamp = 0;
            if (!queueEntry.frame.isEmpty()) {
                entry = queueEntry.frame.find(ANDROID_SENSOR_TIMESTAMP);
                frameTimestamp = entry.data.i64[0];
            }
            ALOGVV("   %d: b: %lld\tf: %lld", i,
                    bufferTimestamp, frameTimestamp );
        }
        if (queueEntry.frame.isEmpty() && bufferTimestamp != 0) {
            // Have buffer, no matching frame. Look for one
            for (size_t j = 0; j < mFrameList.size(); j++) {
                bool match = false;
                CameraMetadata &frame = mFrameList.editItemAt(j);
                if (!frame.isEmpty()) {
                    camera_metadata_entry_t entry;
                    entry = frame.find(ANDROID_SENSOR_TIMESTAMP);
                    if (entry.count == 0) {
                        ALOGE("%s: Can't find timestamp in frame!",
                                __FUNCTION__);
                        continue;
                    }
                    nsecs_t frameTimestamp = entry.data.i64[0];
                    if (bufferTimestamp == frameTimestamp) {
                        ALOGVV("%s: Found match %lld", __FUNCTION__,
                                frameTimestamp);
                        match = true;
                    } else {
                        int64_t delta = abs(bufferTimestamp - frameTimestamp);
                        if ( delta < 1000000) {
                            ALOGVV("%s: Found close match %lld (delta %lld)",
                                    __FUNCTION__, bufferTimestamp, delta);
                            match = true;
                        }
                    }
                }
                if (match) {
                    queueEntry.frame.acquire(frame);
                    break;
                }
            }
        }
    }
}

}; // namespace camera2
}; // namespace android
