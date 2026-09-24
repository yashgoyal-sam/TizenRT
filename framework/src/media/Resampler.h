#ifndef __MEDIA_RESAMPLER_H
#define __MEDIA_RESAMPLER_H

#include <cstddef>
#include <memory>

#include <resample/speex_resampler.h>

namespace media {
namespace stream {

class Resampler
{
public:
	struct ConstBufferView {
		const unsigned char *data;
		size_t size;
	};

	enum class Result {
		ERROR,
		NEED_INPUT,
		OUTPUT_READY,
		DRAINED,
	};

	Resampler();
	~Resampler();

	bool configure(unsigned int inputChannels,
				   unsigned int inputSampleRate,
				   unsigned int inputFormat,
				   unsigned int outputChannels,
				   unsigned int outputSampleRate,
				   unsigned int outputFormat,
				   size_t outputPeriodBytes);
	void reset();

	size_t getPendingInputBytes() const;
	size_t pushData(const unsigned char *data, size_t size);

	Result process();
	Result drain();

	ConstBufferView output() const;
	bool consumeOutput();

private:
	/* Normal batch lifecycle: NONE -> IDLE -> BUFFERING -> BUFFERED -> PROCESSED -> IDLE. */
	enum class State {
		NONE,
		IDLE,
		BUFFERING,
		BUFFERED,
		PROCESSED,
	};

	void release();
	bool rechannelData(size_t inputFrames);
	bool resample(size_t inputFrames);
	Result processInput();

	SpeexResamplerState *mSpeexResampler;
	std::unique_ptr<unsigned char[]> mRechannelBuffer;
	std::unique_ptr<unsigned char[]> mResampleBuffer;

	unsigned int mInputChannels;
	unsigned int mInputSampleRate;
	unsigned int mInputFormat;
	unsigned int mOutputChannels;
	unsigned int mOutputSampleRate;
	unsigned int mOutputFormat;

	size_t mOutputFramesPerProcess;
	size_t mInputFramesPerProcess;
	size_t mRechannelBufferSize;
	size_t mResampleBufferSize;
	size_t mInputBytes;
	size_t mOutputFrames;
	State mState;

	bool mNeedsRechannel;
	bool mNeedsResampling;
};

} // namespace stream
} // namespace media

#endif



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

// #ifndef __MEDIA_RESAMPLER_H
// #define __MEDIA_RESAMPLER_H

// #include <tinyara/config.h>
// #include <memory>
// #include <cstdio>
// #include <media/MediaTypes.h>

// #ifndef CONFIG_AUDIO_RESAMPLER_BUFSIZE
// #define CONFIG_AUDIO_RESAMPLER_BUFSIZE 4096
// #endif

// #define RESAMPLER_QUALITY_DEFAULT 5
// #define RESAMPLER_QUALITY_MAX 10

// #ifdef CONFIG_AUDIO_RESAMPLER_BUFSIZE
// #include "resample/speex_resampler.h"
// #endif

// namespace media {

// /**
//  * @brief Audio format parameters for user (source/decoded) side or output (hardware) side.
//  */
// struct audio_format_s {
// 	unsigned int channels;
// 	unsigned int sampleRate;
// 	int format;
// };

// typedef struct audio_format_s audio_format_t;

// /**
//  * @brief Resampler class that performs rechanneling and resampling of PCM data.
//  *
//  * Flow:
//  * 1. Decoded PCM data is pushed via pushData(), which rechannels and appends to RechannelBuffer.
//  * 2. When RechannelBuffer is full or EOS, processResample() resamples into ResampleBuffer.
//  * 3. Resampled data is pulled via pullData() into the stream buffer.
//  *
//  * Only two buffers are used: RechannelBuffer and ResampleBuffer.
//  */
// class Resampler
// {
// public:
//  Resampler(unsigned int userSampleRate, unsigned int userChannels, int userFormat,
//      unsigned int outputSampleRate, unsigned int outputChannels, int outputFormat);
//  ~Resampler();

//  /**
//   * @brief Factory method to create and initialize a Resampler.
//   * @return shared_ptr to Resampler on success, nullptr on failure.
//   */
//  static std::shared_ptr<Resampler> create(
//   unsigned int userSampleRate, unsigned int userChannels, int userFormat,
//   unsigned int outputSampleRate, unsigned int outputChannels, int outputFormat);

//  /**
//   * @brief Initialize buffers and speex resampler.
//   * @return true on success, false on failure.
//   */
//  bool init();

//  /**
//   * @brief Get remaining space in the RechannelBuffer (in bytes).
//   */
//  size_t getRechannelBufferAvailSpace(void);

//  /**
//   * @brief Push decoded PCM data into the Resampler.
//   *
//   * The data is rechanneled from user channels to output channels and appended
//   * to the RechannelBuffer. Only whole frames are processed.
//   *
//   * @param buf Pointer to decoded PCM data (in user format/channels).
//   * @param size Number of bytes to push.
//   * @return Number of bytes consumed from buf.
//   */
//  size_t pushData(const unsigned char *buf, size_t size);

//  /**
//   * @brief Perform resampling on the data accumulated in RechannelBuffer.
//   *
//   * Should be called when RechannelBuffer is full or on EOS.
//   * Resampled output is stored in ResampleBuffer.
//   *
//   * @param eos True if this is the final data (end-of-stream).
//   * @return true on success, false on failure.
//   */
//  bool processResample(bool eos);

//  /**
//   * @brief Pull resampled data from ResampleBuffer.
//   * @param buf Output buffer to copy data into.
//   * @param size Maximum bytes to pull.
//   * @return Number of bytes actually copied.
//   */
//  size_t pullData(unsigned char *buf, size_t size);

//  /**
//   * @brief Check if there is resampled data available to pull.
//   */
//  bool hasResampledData(void);

//  /**
//   * @brief Check if rechanneling or resampling is needed at all.
//   * @return true if conversion is required.
//   */
//  bool isNecessary(void);

//  /**
//   * @brief Reset internal buffer positions (for seek / restart).
//   */
//  void reset(void);

//  /**
//   * @brief Get the number of bytes per frame on the user (input) side.
//   */
//  size_t getUserBytesPerFrame(void);

//  /**
//   * @brief Get the number of bytes per frame on the output side.
//   */
//  size_t getOutputBytesPerFrame(void);

//  size_t getInputBytesForOutput(size_t outBytes);

// private:
//  /**
//   * @brief Get bytes per sample for a given audio format type.
//   */
//  static unsigned int getBytesPerSample(int format);

//  /* User-side (decoded PCM) audio parameters */
//  audio_format_t mUser;

//  /* Output-side (hardware) audio parameters */
//  audio_format_t mOutput;

//  /* RechannelBuffer: holds rechanneled data (output channels, user sample rate) */
//  std::unique_ptr<unsigned char[]> mRechannelBuffer;
//  size_t mRechannelBufferSize; /* Total size of RechannelBuffer in bytes */
//  size_t mRechannelBufferPos; /* Current write position (bytes written) */

//  /* ResampleBuffer: holds resampled data (output channels, output sample rate) */
//  std::unique_ptr<unsigned char[]> mResampleBuffer;
//  size_t mResampleBufferSize; /* Total size of ResampleBuffer in bytes */
//  size_t mResampleBufferDataSize; /* Amount of valid resampled data in bytes */
//  size_t mResampleBufferReadPos; /* Current read position in bytes */

//  /* Flags */
//  bool mNeedRechannel; /* True if channel counts differ */
//  bool mNeedResample; /* True if sample rates differ */

// #ifdef CONFIG_AUDIO_RESAMPLER_BUFSIZE
//  SpeexResamplerState *mSpeexResampler;
// #endif
// };

// } // namespace media

// #endif