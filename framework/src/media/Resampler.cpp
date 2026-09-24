#include "Resampler.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>

#include <resample/speex_resampler.h>

#include "utils/remix.h"

namespace media {
namespace stream {

namespace {

constexpr unsigned int DEFAULT_RESAMPLING_QUALITY = 5;
constexpr unsigned int MAX_RESAMPLING_QUALITY = 10;

} // namespace

Resampler::Resampler() :
	mSpeexResampler(nullptr),
	mRechannelBuffer(nullptr),
	mResampleBuffer(nullptr),
	mInputChannels(0),
	mInputSampleRate(0),
	mInputFormat(0),
	mOutputChannels(0),
	mOutputSampleRate(0),
	mOutputFormat(0),
	mOutputFramesPerProcess(0),
	mInputFramesPerProcess(0),
	mRechannelBufferSize(0),
	mResampleBufferSize(0),
	mInputBytes(0),
	mOutputFrames(0),
	mState(State::NONE),
	mNeedsRechannel(false),
	mNeedsResampling(false)
{
}

Resampler::~Resampler()
{
	release();
}

void Resampler::release()
{
	if (mSpeexResampler) {
		speex_resampler_destroy(mSpeexResampler);
		mSpeexResampler = nullptr;
	}

	mRechannelBuffer.reset();
	mResampleBuffer.reset();

	mInputChannels = 0;
	mInputSampleRate = 0;
	mInputFormat = 0;
	mOutputChannels = 0;
	mOutputSampleRate = 0;
	mOutputFormat = 0;
	mOutputFramesPerProcess = 0;
	mInputFramesPerProcess = 0;
	mRechannelBufferSize = 0;
	mResampleBufferSize = 0;
	mInputBytes = 0;
	mOutputFrames = 0;
	mState = State::NONE;
	mNeedsRechannel = false;
	mNeedsResampling = false;
}

bool Resampler::configure(unsigned int inputChannels, unsigned int inputSampleRate, unsigned int inputFormat,
							unsigned int outputChannels, unsigned int outputSampleRate, unsigned int outputFormat,
							size_t outputPeriodBytes)
{
	release();

	meddbg("ipch = %d, ipsr = %d, ipf = %d, opch = %d, opsr = %d, opf = %d\n", inputChannels, inputSampleRate, inputFormat, outputChannels, outputSampleRate, outputFormat);

	if (inputChannels == 0 || inputSampleRate == 0 || inputFormat == 0 ||
		outputChannels == 0 || outputSampleRate == 0 || outputFormat == 0 ||
		outputPeriodBytes == 0) {
		meddbg("Invalid resampler configuration\n");
		return false;
	}
	/*
	 * rechannel() currently supports only mono or stereo output.
	 */
	if (outputChannels > 2) {
		meddbg("Unsupported output channel count: %u\n", outputChannels);
		return false;
	}

	/*
	 * Currently both input and output formats support only 2 bytes per sample (signed 16-bit PCM).
	 */
	if (inputFormat != 2 || outputFormat != 2) {
		meddbg("Unsupported PCM formats: input = %u output = %u\n", inputFormat, outputFormat);
		return false;
	}

	if (outputPeriodBytes % (outputChannels * outputFormat) != 0) {
		meddbg("Output period is not frame aligned: %lu\n", static_cast<unsigned long>(outputPeriodBytes));
		return false;
	}

	mInputChannels = inputChannels;
	mInputSampleRate = inputSampleRate;
	mInputFormat = inputFormat;
	mOutputChannels = outputChannels;
	mOutputSampleRate = outputSampleRate;
	mOutputFormat = outputFormat;

	mOutputFramesPerProcess = outputPeriodBytes / (outputChannels * outputFormat);
	mInputFramesPerProcess = static_cast<size_t>((static_cast<uint64_t>(mOutputFramesPerProcess) * mInputSampleRate) / mOutputSampleRate);
	if (mInputFramesPerProcess == 0) {
		meddbg("Output period is too small for the sample-rate ratio\n");
		release();
		return false;
	}

	mNeedsRechannel = inputChannels != outputChannels;
	mNeedsResampling = inputSampleRate != outputSampleRate;

	size_t inputBytesPerProcess = mInputFramesPerProcess * mInputChannels * mInputFormat;
	size_t rechannelBytesPerProcess = mInputFramesPerProcess * mOutputChannels * mOutputFormat;
	mRechannelBufferSize = std::max(inputBytesPerProcess, rechannelBytesPerProcess);
	meddbg("mRechannelBuffer size = %d\n", mRechannelBufferSize);
	mRechannelBuffer = std::make_unique<unsigned char[]>(mRechannelBufferSize);
	if (!mRechannelBuffer) {
		meddbg("Rechannel buffer allocation failed: %lu bytes\n", static_cast<unsigned long>(mRechannelBufferSize));
		release();
		return false;
	}

	if (mNeedsResampling) {
		int errCode = RESAMPLER_ERR_SUCCESS;
		unsigned int resamplingQuality = DEFAULT_RESAMPLING_QUALITY;

		if ((outputSampleRate >= inputSampleRate && outputSampleRate % inputSampleRate == 0) ||
			(inputSampleRate > outputSampleRate && inputSampleRate % outputSampleRate == 0)) {
			resamplingQuality = MAX_RESAMPLING_QUALITY;
		}

		mSpeexResampler = speex_resampler_init(outputChannels, inputSampleRate, outputSampleRate, resamplingQuality, &errCode);
		if (!mSpeexResampler) {
			meddbg("speex_resampler_init failed. errno: %d\n", errCode);
			release();
			return false;
		}

		mResampleBufferSize = outputPeriodBytes;
		meddbg("mResampleBufferSize size = %d\n", mResampleBufferSize);
		mResampleBuffer = std::make_unique<unsigned char[]>(mResampleBufferSize);
		if (!mResampleBuffer) {
			meddbg("Resample buffer allocation failed: %lu bytes\n", static_cast<unsigned long>(mResampleBufferSize));
			release();
			return false;
		}
	}

	mState = State::IDLE;
	reset();

	meddbg("Resampler configured: %u/%u -> %u/%u, input frames = %lu, output frames = %lu frames\n",
			mInputChannels, mInputSampleRate, mOutputChannels, mOutputSampleRate, static_cast<unsigned long>(mInputFramesPerProcess), static_cast<unsigned long>(mOutputFramesPerProcess));

	return true;
}

void Resampler::reset()
{
	if (mState == State::NONE) {
		return;
	}

	if (mSpeexResampler) {
		const int error = speex_resampler_reset_mem(mSpeexResampler);
		if (error != RESAMPLER_ERR_SUCCESS) {
			meddbg("speex_resampler_reset_mem failed: %d\n",
				   error);
		}
	}

	std::memset(mRechannelBuffer.get(), 0,
				mRechannelBufferSize);
	if (mResampleBuffer) {
		std::memset(mResampleBuffer.get(), 0,
					mResampleBufferSize);
	}

	mInputBytes = 0;
	mOutputFrames = 0;
	mState = State::IDLE;
}

size_t Resampler::getPendingInputBytes() const
{
	if (mState != State::IDLE && mState != State::BUFFERING) {
		meddbg("Invalid resampler state. state: %d\n", static_cast<int>(mState));
		return 0;
	}

	const size_t inputBytesPerProcess = mInputFramesPerProcess * mInputChannels * mInputFormat;
	const size_t pendingInputBytes = inputBytesPerProcess - mInputBytes;

	return pendingInputBytes;
}

size_t Resampler::pushData(const unsigned char *data, size_t size)
{
	if (!data || size == 0) {
		meddbg("Invalid input parameter\n");
		return 0;
	}

	if (mState != State::IDLE && mState != State::BUFFERING) {
		meddbg("Invalid resampler state. state: %d\n", static_cast<int>(mState));
		return 0;
	}

	size_t pendingInputBytes = getPendingInputBytes();
	const size_t bytesToCopy = std::min(size, pendingInputBytes);

	std::memcpy(mRechannelBuffer.get() + mInputBytes, data, bytesToCopy);
	mInputBytes += bytesToCopy;
	pendingInputBytes -= bytesToCopy;

	mState = pendingInputBytes == 0 ? State::BUFFERED : State::BUFFERING;

	return bytesToCopy;
}

bool Resampler::rechannelData(size_t inputFrames)
{
	if (!mNeedsRechannel) {
		medvdbg("No need for rechanneling\n");
		return true;
	}

	const int32_t rechanneledFrames = rechannel(ch2layout(mInputChannels), ch2layout(mOutputChannels),
												reinterpret_cast<const int16_t *>(mRechannelBuffer.get()), static_cast<uint32_t>(inputFrames),
												reinterpret_cast<int16_t *>(mRechannelBuffer.get()), static_cast<uint32_t>(mRechannelBufferSize / (mOutputChannels * mOutputFormat)));
	if (rechanneledFrames < 0 || static_cast<size_t>(rechanneledFrames) != inputFrames) {
		meddbg("Fail to rechannel each frame, %d/%lu\n", rechanneledFrames, static_cast<unsigned long>(inputFrames));
		return false;
	}

	return true;
}

bool Resampler::resample(size_t frames)
{
	if (!mNeedsResampling) {
		medvdbg("No need for resampling\n");
		mOutputFrames = frames;
		return true;
	}

	size_t usedFrames = 0;
	size_t resampledFrames = 0;
	spx_int16_t *dataIn;
	spx_uint32_t inputFrames;
	spx_int16_t *dataOut;
	spx_uint32_t outputFrames;
	int ret;

	while (frames > usedFrames) {
		dataIn = reinterpret_cast<int16_t *>(mRechannelBuffer.get() + (usedFrames * mOutputChannels * mOutputFormat));
		inputFrames = static_cast<spx_uint32_t>(frames - usedFrames);
		dataOut = reinterpret_cast<int16_t *>(mResampleBuffer.get() + (resampledFrames * mOutputChannels * mOutputFormat));
		outputFrames = mOutputFramesPerProcess - resampledFrames;
		medvdbg("dataIn %p, inputFrames %u\n",
				 static_cast<void *>(dataIn), inputFrames);
		medvdbg("dataOut %p, outputFrames resample buffer can hold %u\n",
				 static_cast<void *>(dataOut), outputFrames);

		ret = speex_resampler_process_interleaved_int(mSpeexResampler, dataIn, &inputFrames, dataOut, &outputFrames);
		if (ret != RESAMPLER_ERR_SUCCESS) {
			meddbg("Fail to resample out:%lu/%lu, error %d\n",
				   static_cast<unsigned long>(usedFrames),
				   static_cast<unsigned long>(frames), ret);
			return false;
		}

		usedFrames += inputFrames;
		if (outputFrames > 0) {
			resampledFrames += outputFrames;
		} else if (frames != usedFrames) {
			meddbg("Error: output buffer is full, used input frames %lu/%lu\n",
				   static_cast<unsigned long>(usedFrames),
				   static_cast<unsigned long>(frames));
			return false;
		}
		medvdbg("%lu frames generated from %lu/%lu\n",
				 static_cast<unsigned long>(resampledFrames),
				 static_cast<unsigned long>(usedFrames),
				 static_cast<unsigned long>(frames));
	}

	medvdbg("resampled frames count: %lu\n",
			 static_cast<unsigned long>(resampledFrames));
	mOutputFrames = resampledFrames;
	return true;
}

Resampler::Result Resampler::processInput()
{
	const size_t bytesPerFrame =
		mInputChannels * mInputFormat;
	const size_t trailingBytes =
		mInputBytes % bytesPerFrame;
	if (trailingBytes != 0) {
		meddbg("Discarding %lu incomplete PCM bytes at EOS\n",
			   static_cast<unsigned long>(trailingBytes));
		mInputBytes -= trailingBytes;
	}

	if (mInputBytes == 0) {
		mState = State::IDLE;
		return Result::NEED_INPUT;
	}

	const size_t inputFrames = mInputBytes / bytesPerFrame;

	if (!rechannelData(inputFrames)) {
		meddbg("Rechanneling failed\n");
		return Result::ERROR;
	}

	if (!resample(inputFrames)) {
		meddbg("Resampling failed\n");
		return Result::ERROR;
	}

	if (mNeedsResampling) {
		mInputBytes = 0;
	}

	if (mOutputFrames == 0) {
		mInputBytes = 0;
		mState = State::IDLE;
		return Result::NEED_INPUT;
	}

	mState = State::PROCESSED;
	return Result::OUTPUT_READY;
}

Resampler::Result Resampler::process()
{
	if (mState != State::BUFFERED) {
		meddbg("Invalid resampler state. state: %d\n",
			   static_cast<int>(mState));
		return Result::ERROR;
	}

	return processInput();
}

Resampler::Result Resampler::drain()
{
	if (mState == State::IDLE) {
		return Result::DRAINED;
	}

	if (mState == State::PROCESSED) {
		return Result::OUTPUT_READY;
	}

	if (mState != State::BUFFERING &&
		mState != State::BUFFERED) {
		return Result::ERROR;
	}

	return processInput();
}

Resampler::ConstBufferView Resampler::output() const
{
	ConstBufferView result = { nullptr, 0 };

	if (mState != State::PROCESSED) {
		return result;
	}

	result.data = mNeedsResampling ?
		mResampleBuffer.get() :
		mRechannelBuffer.get();
	result.size =
		mOutputFrames * mOutputChannels * mOutputFormat;

	return result;
}

bool Resampler::consumeOutput()
{
	if (mState != State::PROCESSED) {
		return false;
	}

	mInputBytes = 0;
	mOutputFrames = 0;
	mState = State::IDLE;

	return true;
}

} // namespace stream
} // namespace media



// /* ****************************************************************
//  *
//  * Copyright 2024 Samsung Electronics All Rights Reserved.
//  *
//  * Licensed under the Apache License, Version 2.0 (the "License");
//  * you may not use this file except in compliance with the License.
//  * You may obtain a copy of the License at
//  *
//  * http://www.apache.org/licenses/LICENSE-2.0
//  *
//  * Unless required by applicable law or agreed to in writing, software
//  * distributed under the License is distributed on an "AS IS" BASIS,
//  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  * See the License for the specific language governing permissions and
//  * limitations under the License.
//  *
//  ******************************************************************/

// #include <tinyara/config.h>
// #include <string.h>
// #include <debug.h>

// #include "Resampler.h"
// #include "utils/remix.h"

// namespace media {

// Resampler::Resampler(unsigned int userSampleRate, unsigned int userChannels, int userFormat,
//       unsigned int outputSampleRate, unsigned int outputChannels, int outputFormat) :
//  mRechannelBufferSize(0),
//  mRechannelBufferPos(0),
//  mResampleBufferSize(0),
//  mResampleBufferDataSize(0),
//  mResampleBufferReadPos(0),
//  mNeedRechannel(false),
//  mNeedResample(false),
// #ifdef CONFIG_AUDIO_RESAMPLER_BUFSIZE
//  mSpeexResampler(nullptr)
// #endif
// {
// }

// Resampler::~Resampler()
// {
// #ifdef CONFIG_AUDIO_RESAMPLER_BUFSIZE
//  if (mSpeexResampler) {
//   speex_resampler_destroy(mSpeexResampler);
//   mSpeexResampler = nullptr;
//  }
// #endif
// }

// std::shared_ptr<Resampler> Resampler::create(
//  unsigned int userSampleRate, unsigned int userChannels, int userFormat,
//  unsigned int outputSampleRate, unsigned int outputChannels, int outputFormat)
// {
//  if (userChannels == 0 || outputChannels == 0 || userSampleRate == 0 || outputSampleRate == 0) {
//   meddbg("Resampler::create invalid params: userCh=%u outCh=%u userSR=%u outSR=%u\n",
//       userChannels, outputChannels, userSampleRate, outputSampleRate);
//   return nullptr;
//  }

//  auto instance = std::make_shared<Resampler>(
//   userSampleRate, userChannels, userFormat,
//   outputSampleRate, outputChannels, outputFormat);

//  instance->mUser.sampleRate = userSampleRate;
//  instance->mUser.channels = userChannels;
//  instance->mUser.format = userFormat;

//  instance->mOutput.sampleRate = outputSampleRate;
//  instance->mOutput.channels = outputChannels;
//  instance->mOutput.format = outputFormat;

//  if (instance->init()) {
//   return instance;
//  }

//  meddbg("Resampler::create - init failed\n");
//  return nullptr;
// }

// unsigned int Resampler::getBytesPerSample(int format)
// {
//  switch (format) {
//  case AUDIO_FORMAT_TYPE_S8:
//   return 1;
//  case AUDIO_FORMAT_TYPE_S16_LE:
//   return 2;
//  case AUDIO_FORMAT_TYPE_S32_LE:
//   return 4;
//  default:
//   return 2; /* Default to 16-bit */
//  }
// }

// size_t Resampler::getUserBytesPerFrame(void)
// {
//  return mUser.channels * getBytesPerSample(mUser.format);
// }

// size_t Resampler::getOutputBytesPerFrame(void)
// {
//  return mOutput.channels * getBytesPerSample(mOutput.format);
// }

// size_t Resampler::getInputBytesForOutput(size_t outputBytes)
// {
// 	size_t outputFrameBytes = getOutputBytesPerFrame();
// 	size_t sourceFrameBytes = getUserBytesPerFrame();
// 	if (outputFrameBytes == 0 || sourceFrameBytes == 0) {
// 		return 0;
// 	}

// 	uint64_t outputFrames = outputBytes / outputFrameBytes;
// 	if (outputFrames > UINT64_MAX / mUser.sampleRate) {
// 		return 0;
// 	}
// 	uint64_t sourceFrames = outputFrames * mUser.sampleRate / mOutput.sampleRate;
// 	if (sourceFrames == 0 && outputFrames > 0) {
// 		sourceFrames = 1;
// 	}
// 	if (sourceFrames > SIZE_MAX / sourceFrameBytes) {
// 		return 0;
// 	}
// 	return (size_t)sourceFrames * sourceFrameBytes;
// }

// bool Resampler::init()
// {
//  mNeedRechannel = (mUser.channels != mOutput.channels);
//  mNeedResample = (mUser.sampleRate != mOutput.sampleRate);

//  if (!mNeedRechannel && !mNeedResample) {
//   medvdbg("Resampler::init - no conversion needed (passthrough)\n");
//   /* Even in passthrough mode, we still allocate buffers so the pipeline works uniformly */
//  }

//  /*
//   * RechannelBuffer holds data in output-channel layout at user sample rate.
//   * Size is determined by CONFIG_AUDIO_RESAMPLER_BUFSIZE.
//   * This buffer accumulates rechanneled frames before resampling.
//   */
//  mRechannelBufferSize = CONFIG_AUDIO_RESAMPLER_BUFSIZE;
//  mRechannelBuffer = std::unique_ptr<unsigned char[]>(new unsigned char[mRechannelBufferSize]);
//  if (!mRechannelBuffer) {
//   meddbg("Resampler::init - failed to allocate RechannelBuffer (%u bytes)\n", mRechannelBufferSize);
//   return false;
//  }
//  mRechannelBufferPos = 0;

//  /*
//   * ResampleBuffer holds data in output-channel layout at output sample rate.
//   * When upsampling, output can be larger than input, so we scale the buffer.
//   * Size = CONFIG_AUDIO_RESAMPLER_BUFSIZE * (outputSR / userSR) + extra margin.
//   */
//  if (mNeedResample && mUser.sampleRate > 0) {
//   /* Calculate ratio and add 1 frame margin to avoid truncation */
//   float ratio = (float)mOutput.sampleRate / (float)mUser.sampleRate;
//   size_t scaledSize = (size_t)((float)CONFIG_AUDIO_RESAMPLER_BUFSIZE * ratio) + getOutputBytesPerFrame();
//   /* Ensure at least CONFIG_AUDIO_RESAMPLER_BUFSIZE */
//   mResampleBufferSize = (scaledSize > CONFIG_AUDIO_RESAMPLER_BUFSIZE) ? scaledSize : CONFIG_AUDIO_RESAMPLER_BUFSIZE;
//  } else {
//   /* No resampling: ResampleBuffer same size as RechannelBuffer */
//   mResampleBufferSize = mRechannelBufferSize;
//  }

//  mResampleBuffer = std::unique_ptr<unsigned char[]>(new unsigned char[mResampleBufferSize]);
//  if (!mResampleBuffer) {
//   meddbg("Resampler::init - failed to allocate ResampleBuffer (%u bytes)\n", mResampleBufferSize);
//   mRechannelBuffer.reset();
//   return false;
//  }
//  mResampleBufferDataSize = 0;
//  mResampleBufferReadPos = 0;

// #ifdef CONFIG_AUDIO_RESAMPLER_BUFSIZE
//  /* Initialize Speex resampler if sample rate conversion is needed */
//  if (mNeedResample) {
//   int err_code;
//   int quality = RESAMPLER_QUALITY_DEFAULT;

//   /* Use highest quality for integral multiples */
//   if ((mOutput.sampleRate >= mUser.sampleRate && mOutput.sampleRate % mUser.sampleRate == 0) ||
//    (mUser.sampleRate >= mOutput.sampleRate && mUser.sampleRate % mOutput.sampleRate == 0)) {
//    quality = RESAMPLER_QUALITY_MAX;
//   }

//   /*
//    * speex_resampler_init(nb_channels, input_rate, output_rate, quality, &err)
//    * Here we resample from user sample rate to output sample rate.
//    * The data entering the resampler has output channels (already rechanneled).
//    */
//   mSpeexResampler = speex_resampler_init(
//    mOutput.channels,
//    mUser.sampleRate,
//    mOutput.sampleRate,
//    quality,
//    &err_code);

//   if (!mSpeexResampler) {
//    meddbg("Resampler::init - speex_resampler_init failed, err=%d\n", err_code);
//    mRechannelBuffer.reset();
//    mResampleBuffer.reset();
//    return false;
//   }
//   medvdbg("Resampler::init - Speex resampler created: %uHz -> %uHz, %uch, quality=%d\n",
//     mUser.sampleRate, mOutput.sampleRate, mOutput.channels, quality);
//  }
// #endif

//  medvdbg("Resampler::init - userSR=%u userCh=%u -> outSR=%u outCh=%u, rechannel=%d resample=%d\n",
//    mUser.sampleRate, mUser.channels, mOutput.sampleRate, mOutput.channels,
//    mNeedRechannel, mNeedResample);
//  medvdbg("Resampler::init - RechannelBuf=%u bytes, ResampleBuf=%u bytes\n",
//    mRechannelBufferSize, mResampleBufferSize);

//  return true;
// }

// bool Resampler::isNecessary(void)
// {
//  return mNeedRechannel || mNeedResample;
// }

// size_t Resampler::getRechannelBufferAvailSpace(void)
// {
//  if (mRechannelBufferPos >= mRechannelBufferSize) {
//   return 0;
//  }
//  return mRechannelBufferSize - mRechannelBufferPos;
// }

// size_t Resampler::pushData(const unsigned char *buf, size_t size)
// {
//  if (!buf || size == 0) {
//   return 0;
//  }

//  size_t userBytesPerFrame = getUserBytesPerFrame();
//  size_t outputBytesPerFrame = getOutputBytesPerFrame();

//  if (userBytesPerFrame == 0 || outputBytesPerFrame == 0) {
//   meddbg("Resampler::pushData - invalid bytes per frame\n");
//   return 0;
//  }

//  /* Calculate how many frames we can accept based on available space in RechannelBuffer */
//  size_t availSpace = getRechannelBufferAvailSpace();
//  if (availSpace == 0) {
//   return 0;
//  }

//  /* Max frames that fit in remaining RechannelBuffer space (output layout) */
//  size_t maxOutputFrames = availSpace / outputBytesPerFrame;
//  if (maxOutputFrames == 0) {
//   return 0;
//  }

//  /* Max frames available in input (user layout) */
//  size_t inputFrames = size / userBytesPerFrame;
//  if (inputFrames == 0) {
//   return 0;
//  }

//  /* Process the minimum of available input frames and what fits in buffer */
//  size_t framesToProcess = (inputFrames < maxOutputFrames) ? inputFrames : maxOutputFrames;

//  if (mNeedRechannel) {
//   /* Rechannel from user channels to output channels */
//   uint32_t inLayout = ch2layout(mUser.channels);
//   uint32_t outLayout = ch2layout(mOutput.channels);

//   int32_t rechanneled = rechannel(
//    inLayout, outLayout,
//    (const int16_t *)buf, (uint32_t)framesToProcess,
//    (int16_t *)(mRechannelBuffer.get() + mRechannelBufferPos),
//    (uint32_t)maxOutputFrames);

//   if (rechanneled < 0) {
//    meddbg("Resampler::pushData - rechannel failed, ret=%d\n", rechanneled);
//    return 0;
//   }

//   size_t bytesWritten = (size_t)rechanneled * outputBytesPerFrame;
//   mRechannelBufferPos += bytesWritten;

//   /* Return bytes consumed from input */
//   return (size_t)rechanneled * userBytesPerFrame;
//  } else {
//   /* No rechanneling needed - just copy data into RechannelBuffer */
//   size_t bytesToCopy = framesToProcess * userBytesPerFrame;
//   memcpy(mRechannelBuffer.get() + mRechannelBufferPos, buf, bytesToCopy);
//   mRechannelBufferPos += bytesToCopy;
//   return bytesToCopy;
//  }
// }

// bool Resampler::processResample(bool eos)
// {
//  if (mRechannelBufferPos == 0) {
//   /* Nothing to resample */
//   return true;
//  }

//  size_t outputBytesPerFrame = getOutputBytesPerFrame();
//  if (outputBytesPerFrame == 0) {
//   meddbg("Resampler::processResample - invalid output bytes per frame\n");
//   return false;
//  }

//  /* Reset ResampleBuffer for new output */
//  mResampleBufferDataSize = 0;
//  mResampleBufferReadPos = 0;

//  if (!mNeedResample) {
//   /*
//    * No sample rate conversion needed.
//    * Just move RechannelBuffer data to ResampleBuffer directly.
//    */
//   size_t bytesToCopy = mRechannelBufferPos;
//   if (bytesToCopy > mResampleBufferSize) {
//    meddbg("Resampler::processResample - data exceeds ResampleBuffer! %u > %u\n",
//        bytesToCopy, mResampleBufferSize);
//    bytesToCopy = mResampleBufferSize;
//   }
//   memcpy(mResampleBuffer.get(), mRechannelBuffer.get(), bytesToCopy);
//   mResampleBufferDataSize = bytesToCopy;
//   mRechannelBufferPos = 0;
//   return true;
//  }

// #ifdef CONFIG_AUDIO_RESAMPLER_BUFSIZE
//  if (!mSpeexResampler) {
//   meddbg("Resampler::processResample - speex resampler is null\n");
//   return false;
//  }

//  /* Calculate frames in RechannelBuffer */
//  spx_uint32_t inputFrames = (spx_uint32_t)(mRechannelBufferPos / outputBytesPerFrame);
//  spx_uint32_t usedFrames = 0;
//  size_t resampledBytes = 0;

//  while (usedFrames < inputFrames) {
//   spx_int16_t *dataIn = (spx_int16_t *)(mRechannelBuffer.get() + usedFrames * outputBytesPerFrame);
//   spx_uint32_t inFrames = inputFrames - usedFrames;

//   spx_int16_t *dataOut = (spx_int16_t *)(mResampleBuffer.get() + resampledBytes);
//   spx_uint32_t outFrames = (spx_uint32_t)((mResampleBufferSize - resampledBytes) / outputBytesPerFrame);

//   if (outFrames == 0) {
//    meddbg("Resampler::processResample - ResampleBuffer is full\n");
//    break;
//   }

//   int ret = speex_resampler_process_interleaved_int(
//    mSpeexResampler, dataIn, &inFrames, dataOut, &outFrames);

//   if (ret != RESAMPLER_ERR_SUCCESS) {
//    meddbg("Resampler::processResample - speex resample failed, err=%d\n", ret);
//    mRechannelBufferPos = 0;
//    return false;
//   }

//   usedFrames += inFrames;
//   resampledBytes += (size_t)outFrames * outputBytesPerFrame;

//   if (outFrames == 0 && inFrames == 0) {
//    /* No progress — avoid infinite loop */
//    meddbg("Resampler::processResample - no progress in resampling\n");
//    break;
//   }
//  }

//  mResampleBufferDataSize = resampledBytes;
//  mRechannelBufferPos = 0;

//  medvdbg("Resampler::processResample - resampled %u input frames -> %u output bytes (eos=%d)\n",
//    inputFrames, mResampleBufferDataSize, eos);

//  return true;
// #else
//  /* No speex available, cannot resample. Copy as-is. */
//  size_t bytesToCopy = mRechannelBufferPos;
//  if (bytesToCopy > mResampleBufferSize) {
//   bytesToCopy = mResampleBufferSize;
//  }
//  memcpy(mResampleBuffer.get(), mRechannelBuffer.get(), bytesToCopy);
//  mResampleBufferDataSize = bytesToCopy;
//  mRechannelBufferPos = 0;
//  return true;
// #endif
// }

// size_t Resampler::pullData(unsigned char *buf, size_t size)
// {
//  if (!buf || size == 0) {
//   return 0;
//  }

//  size_t available = mResampleBufferDataSize - mResampleBufferReadPos;
//  if (available == 0) {
//   return 0;
//  }

//  size_t toCopy = (size < available) ? size : available;
//  memcpy(buf, mResampleBuffer.get() + mResampleBufferReadPos, toCopy);
//  mResampleBufferReadPos += toCopy;

//  return toCopy;
// }

// bool Resampler::hasResampledData(void)
// {
//  return (mResampleBufferReadPos < mResampleBufferDataSize);
// }

// void Resampler::reset(void)
// {
//  mRechannelBufferPos = 0;
//  mResampleBufferDataSize = 0;
//  mResampleBufferReadPos = 0;

// #ifdef CONFIG_AUDIO_RESAMPLER_BUFSIZE
//  if (mSpeexResampler) {
//   speex_resampler_reset_mem(mSpeexResampler);
//  }
// #endif
// }

// } // namespace media
