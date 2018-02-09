/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "oboe/Oboe.h"

#include "opensles/AudioStreamBuffered.h"
#include "common/AudioClock.h"

namespace oboe {

/*
 * AudioStream with a FifoBuffer
 */
AudioStreamBuffered::AudioStreamBuffered(const AudioStreamBuilder &builder)
        : AudioStream(builder) {
}
AudioStreamBuffered::~AudioStreamBuffered() {
    LOGD("~AudioStreamBuffered() called");
}

void AudioStreamBuffered::allocateFifo() {
    // If the caller does not provide a callback use our own internal
    // callback that reads data from the FIFO.
    if (usingFIFO()) {
        // FIFO is configured with the same format and channels as the stream.
        int32_t capacity = getBufferCapacityInFrames();
        if (capacity == oboe::kUnspecified) {
            capacity = getFramesPerBurst() * 16; // arbitrary
            mBufferCapacityInFrames = capacity;
        }
        LOGD("AudioStreamBuffered(): new FifoBuffer(bytesPerFrame=%d, capacity=%d)",
             getBytesPerFrame(), capacity);
        mFifoBuffer.reset(new FifoBuffer(getBytesPerFrame(), capacity));
        LOGD("AudioStreamBuffered(): mStreamCallback = %p", mStreamCallback);
    }
}

int64_t AudioStreamBuffered::getFramesWritten() const {
    if (usingFIFO()) {
        return (int64_t) mFifoBuffer->getWriteCounter();
    } else {
        return AudioStream::getFramesWritten();
    }
}

int64_t AudioStreamBuffered::getFramesRead() const {
    if (usingFIFO()) {
        return (int64_t) mFifoBuffer->getReadCounter();
    } else {
        return AudioStream::getFramesRead();
    }
}


    // This is called by the OpenSL ES callback to read or write the back end of the FIFO.
DataCallbackResult AudioStreamBuffered::onDefaultCallback(void *audioData, int numFrames) {
    int32_t framesTransferred  = 0;

    if (getDirection() == oboe::Direction::Output) {
        // Read from the FIFO and write to audioData
        framesTransferred = mFifoBuffer->readNow(audioData, numFrames);
        LOGV("%s() read %d / %d frames from FIFO", __func__, framesTransferred, numFrames);
    } else {
        // Read from audioData and write to the FIFO
        framesTransferred = mFifoBuffer->write(audioData, numFrames); // FIXME writeNow????
        LOGV("%s() wrote %d / %d frames to FIFO", __func__, framesTransferred, numFrames);
    }

    if (framesTransferred < numFrames) {
        // TODO If we do not allow FIFO to wrap then our timestamps will drift when there is an XRun!
        incrementXRunCount();
    }
    markCallbackTime(numFrames); // so foreground knows how long to wait.
    return DataCallbackResult::Continue;
}

void AudioStreamBuffered::markCallbackTime(int numFrames) {
    mLastBackgroundSize = numFrames;
    mBackgroundRanAtNanoseconds = AudioClock::getNanoseconds();
}

int64_t AudioStreamBuffered::predictNextCallbackTime() {
    if (mBackgroundRanAtNanoseconds == 0) {
        return 0;
    }
    int64_t nanosPerBuffer = (kNanosPerSecond * mLastBackgroundSize) / getSampleRate();
    const int64_t margin = 200 * kNanosPerMicrosecond; // arbitrary delay so we wake up just after
    return mBackgroundRanAtNanoseconds + nanosPerBuffer + margin;
}

// TODO: This method should return a tuple of Result,int32_t where the
// 2nd return param is the frames written. Maybe not!

// Common code for read/write.
int32_t AudioStreamBuffered::transfer(void *buffer,
                                      int32_t numFrames,
                                      int64_t timeoutNanoseconds) {
    // Validate arguments.
    if (buffer == nullptr) {
        LOGE("AudioStreamBuffered::%s(): buffer is NULL", __func__);
        return (int32_t) Result ::ErrorNull;
    }
    if (numFrames < 0) {
        LOGE("AudioStreamBuffered::%s(): numFrames is negative", __func__);
        return (int32_t) Result::ErrorOutOfRange;
    } else if (numFrames == 0) {
        return 0;
    }
    if (timeoutNanoseconds < 0) {
        LOGE("AudioStreamBuffered::%s(): timeoutNanoseconds is negative", __func__);
        return (int32_t) Result ::ErrorOutOfRange;
    }

    int32_t result = 0;
    uint8_t *data = (uint8_t *)buffer;
    int32_t framesLeft = numFrames;
    int64_t timeToQuit = 0;
    bool repeat = true;

    // Calculate when to timeout.
    if (timeoutNanoseconds > 0) {
        timeToQuit = AudioClock::getNanoseconds() + timeoutNanoseconds;
    }

    // Loop until we get the data, or we have an error, or we timeout.
    do {
        // read or write
        if (getDirection() == Direction::Input) {
            result = mFifoBuffer->read(data, framesLeft);
        } else {
            result = mFifoBuffer->write(data, framesLeft);
        }
        if (result > 0) {
            data += mFifoBuffer->convertFramesToBytes(result);
            framesLeft -= result;
        }

        // If we need more data then sleep and try again.
        if (framesLeft > 0 && result >= 0 && timeoutNanoseconds > 0) {
            int64_t timeNow = AudioClock::getNanoseconds();
            if (timeNow >= timeToQuit) {
                LOGE("AudioStreamBuffered::%s(): TIMEOUT", __func__);
                repeat = false; // TIMEOUT
            } else {
                // Figure out how long to sleep.
                int64_t sleepForNanos;
                int64_t wakeTimeNanos = predictNextCallbackTime();
                if (wakeTimeNanos <= 0) {
                    // No estimate available. Sleep for one burst.
                    sleepForNanos = (getFramesPerBurst() * kNanosPerSecond) / getSampleRate();
                } else {
                    // Don't sleep past timeout.
                    if (wakeTimeNanos > timeToQuit) {
                        wakeTimeNanos = timeToQuit;
                    }
                    sleepForNanos = wakeTimeNanos - timeNow;
                    // Avoid rapid loop with no sleep.
                    const int64_t minSleepTime = kNanosPerMillisecond; // arbitrary
                    if (sleepForNanos < minSleepTime) {
                        sleepForNanos = minSleepTime;
                    }
                }

                AudioClock::sleepForNanos(sleepForNanos);
            }

        } else {
            repeat = false;
        }
    } while(repeat);

    return result < 0 ? result : (numFrames - framesLeft);
}

// Write to the FIFO so the callback can read from it.
int32_t AudioStreamBuffered::write(const void *buffer,
                                   int32_t numFrames,
                                   int64_t timeoutNanoseconds) {
    if (getDirection() == Direction::Input) {
        return (int32_t) Result::ErrorUnavailable;  // TODO review, better error code?
    }
    return transfer((void *) buffer, numFrames, timeoutNanoseconds);
}

// Read data from the FIFO that was written by the callback.
int32_t AudioStreamBuffered::read(void *buffer,
                                  int32_t numFrames,
                                  int64_t timeoutNanoseconds) {
    if (getDirection() == Direction::Output) {
        return (int32_t) Result::ErrorUnavailable; // TODO review, better error code?
    }
    return transfer(buffer, numFrames, timeoutNanoseconds);
}

// Only supported when we are not using a callback.
Result AudioStreamBuffered::setBufferSizeInFrames(int32_t requestedFrames)
{
    if (mFifoBuffer != nullptr) {
        if (requestedFrames > mFifoBuffer->getBufferCapacityInFrames()) {
            requestedFrames = mFifoBuffer->getBufferCapacityInFrames();
        }
        mFifoBuffer->setThresholdFrames(requestedFrames);
        return Result::OK;
    } else {
        return Result::ErrorUnimplemented;
    }
}

int32_t AudioStreamBuffered::getBufferSizeInFrames() const {
    if (mFifoBuffer != nullptr) {
        return mFifoBuffer->getThresholdFrames();
    } else {
        return AudioStream::getBufferSizeInFrames();
    }
}

int32_t AudioStreamBuffered::getBufferCapacityInFrames() const {
    if (mFifoBuffer != nullptr) {
        return mFifoBuffer->getBufferCapacityInFrames();
    } else {
        return AudioStream::getBufferCapacityInFrames();
    }
}

} // namespace oboe