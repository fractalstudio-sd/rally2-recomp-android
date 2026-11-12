/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2003-2026 The Supermodel Team
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/

 /*
  * Audio.cpp
  *
  * SDL audio playback. Implements the OSD audio interface.
  *
  * Buffer sizes and read/write positions must be sample-aligned. A sample is
  * defined to encompass both channels so for, e.g., 16-bit audio as used here,
  * a sample is 4 bytes. Static assertions are employed to ensure that the
  * initial set up of the buffer is correct.
  *
  * Model 3 Audio is always 4 channels. SCSP1 is usually for each front
  * channels (on CN8 connector) and SCSP2 for rear channels (on CN7).
  * The downmix to 2 channels will be performed here in case supermodel audio
  * subsystem does not allow such playback.
  * The default DSB board is supposed to be plug and mixed with the front output
  * channel. The rear channel is usually plugged to the gullbow speakers that
  * are present in most model 3 racing cabinets, while front speakers are only
  * present in Daytona2, Scud Race, Sega Rally 2.
  * As each cabinet as its own wiring, with different mixing, the games.xml
  * database will provide which can of mixing should be applied for each game.
  *
  * From twin uk cabinet diagrams, at least:
  * - lemans24: only stereo on rear speakers (gullbox) on front channels SCSP1/CN8.
  * - scud race: DSB mixed with SCSP2/CN7 for rear (gullbox) speakers, front
  *   connected to SCSP1/CN8.
  * - daytona2: front on SCSP1/CN8, rear on SCSP2/CN7
  * - srally2: SCSP2/CN7 gives left/right, and SCSP1/CN8 is split in 2 channels:
  *   mono front, mono rear. These two channels are them mixed with L/R to give
  *   the quad output.
  * - oceanhun: SCSP1/CN8 and SCSP2/CN7 are mixed together for stereo output.
  * Others are unknown so far, but it is expected that most Step 2.x games should
  * have quad output.
  */

#include "Audio.h"

#include "Supermodel.h"
#include "SDLIncludes.h"

#include <cmath>
#include <algorithm>

  // Model3 audio output is 44.1KHz 4-channel sound and frame rate is 60fps
#define SAMPLE_RATE_M3     (44100)
#define SUPERMODEL_FPS     (60.0f)
#define MODEL3_FPS         (57.53f)

#define MAX_SND_FREQ       (75)
#define MIN_SND_FREQ       (45)
#define MAX_LATENCY        (100)

#define NUM_CHANNELS_M3 (4)

Game::AudioTypes AudioType;
int nbHostAudioChannels = NUM_CHANNELS_M3;   // Number of channels on host

#define SAMPLES_PER_FRAME_M3  (INT32)(SAMPLE_RATE_M3 / MODEL3_FPS)

#define BYTES_PER_SAMPLE_M3   (NUM_CHANNELS_M3 * sizeof(INT16))
#define BYTES_PER_FRAME_M3   (SAMPLES_PER_FRAME_M3 * BYTES_PER_SAMPLE_M3)


static int samples_per_frame_host = SAMPLES_PER_FRAME_M3;
static int bytes_per_sample_host = BYTES_PER_SAMPLE_M3;
static int bytes_per_frame_host = BYTES_PER_FRAME_M3;

// Balance percents for mixer
float BalanceLeftRight = 0; // 0 mid balance, 100: left only,  -100:right only 
float BalanceFrontRear = 0; // 0 mid balance, 100: front only, -100:right only 
// Mixer factor (depends on values above)
float balanceFactorFrontLeft  = 1.0f;
float balanceFactorFrontRight = 1.0f;
float balanceFactorRearLeft   = 1.0f;
float balanceFactorRearRight  = 1.0f;

static bool enabled = true;         // True if sound output is enabled
static constexpr unsigned latency = 20;       // Audio latency to use (ie size of audio buffer) as percentage of max buffer size
static constexpr bool underRunLoop = false;   // True if should loop back to beginning of buffer on under-run, otherwise sound is just skipped

static constexpr unsigned playSamples = 512;  // Size (in samples) of callback play buffer

static UINT32 audioBufferSize = 0;  // Size (in bytes) of audio buffer
static INT8* audioBuffer = NULL;    // Audio buffer

static UINT32 writePos = 0;         // Current position at which writing into buffer
static UINT32 playPos = 0;          // Current position at which playing data in buffer via callback

static bool writeWrapped = false;   // True if write position has wrapped around at end of buffer but play position has not done so yet

static unsigned underRuns = 0;      // Number of buffer under-runs that have occured
static unsigned overRuns = 0;       // Number of buffer over-runs that have occured

static AudioCallbackFPtr callback = NULL; // Pointer to audio callback that is called when audio buffer is less than half empty
static void* callbackData = NULL;         // Pointer to data to be passed to audio callback when it is called

static const Util::Config::Node* s_config = 0;

#include <mutex>
#include <atomic>
static std::recursive_mutex s_audMtx;              // guards audioBuffer lifetime against re-open (OutputAudio runs on the sound-board thread)
static std::atomic<unsigned> s_cbCount{0};         // number of times the device pulled data (proves the device is really running)
unsigned AudioCallbacks() { return s_cbCount.load(); }


void SetAudioCallback(AudioCallbackFPtr newCallback, void* newData)
{
    // Lock audio whilst changing callback pointers
    SDL_LockAudio();

    callback = newCallback;
    callbackData = newData;

    SDL_UnlockAudio();
}

int AudioUnderRuns() { return (int)underRuns; }

void SetAudioEnabled(bool newEnabled)
{
    enabled = newEnabled;
}

/// <summary>
/// Set game audio mixing type
/// </summary>
/// <param name="type"></param>
void SetAudioType(Game::AudioTypes type)
{
    AudioType = type;
}

static INT16 MixINT16(float x, float y)
{
    INT32 sum = (INT32)((x + y)*0.5f); //!! dither
    if (sum > INT16_MAX) {
        sum = INT16_MAX;
    }
    if (sum < INT16_MIN) {
        sum = INT16_MIN;
    }
    return (INT16)sum;
}

static float MixFloat(float x, float y)
{
    return (x + y)*0.5f;
}

static INT16 ClampINT16(float x)
{
    INT32 xi = (INT32)x; //!! dither
    if (xi > INT16_MAX) {
        xi = INT16_MAX;
    }
    if (xi < INT16_MIN) {
        xi = INT16_MIN;
    }
    return (INT16)xi;
}

/* Audio FIFO. The original Supermodel ring (play/write positions + a "wrapped" flag) could fall into a state where the write
   position wrapped without the flag being set after an under-run: from then on every callback saw an under-run and the output
   stayed silent for good (seen on phones as a mute game that only came back after a restart). Now: read index + byte count,
   both only touched with the audio lock held. */
static UINT32 fifoRd = 0, fifoCnt = 0;

static void PlayCallback(void* data, Uint8* stream, int len)
{
    s_cbCount++;
    if (!audioBuffer) { memset(stream, 0, len); return; }
    UINT32 n = (UINT32)len < fifoCnt ? (UINT32)len : fifoCnt;
    UINT32 first = n < audioBufferSize - fifoRd ? n : audioBufferSize - fifoRd;
    if (enabled) {
        memcpy(stream, audioBuffer + fifoRd, first);
        if (n > first) memcpy(stream + first, audioBuffer, n - first);
    } else memset(stream, 0, n);
    if (n < (UINT32)len) { memset(stream + n, 0, len - n); underRuns++; }
    fifoRd = (fifoRd + n) % audioBufferSize;
    fifoCnt -= n;
    bool bufferFull = fifoCnt + 2 * bytes_per_frame_host > audioBufferSize;
    if (callback && !bufferFull)
        callback(callbackData);
}

// Stereo output level: the board mix peaks around -25 dBFS, very quiet on a phone speaker. Make-up gain with a soft knee
// (no hard clipping on the loudest crashes), and the peak is kept for the log.
static const float kOutGain = 3.5f;
static float s_outPeak = 0;
static inline INT16 OutSample(float x)
{
    x *= kOutGain;
    float a = fabsf(x);
    if (a > s_outPeak) s_outPeak = a;
    if (a > 22000.f) { a = 22000.f + (a - 22000.f) * 0.3f; if (a > 32767.f) a = 32767.f; x = x < 0 ? -a : a; }
    return (INT16)x;
}
int AudioOutPeak() { int p = (int)s_outPeak; s_outPeak = 0; return p; }
unsigned AudioCallbacks();

static void MixChannels(unsigned numSamples, const float* leftFrontBuffer, const float* rightFrontBuffer, const float* leftRearBuffer, const float* rightRearBuffer, void* dest, bool flipStereo)
{
    INT16* p = (INT16*)dest;

    if (nbHostAudioChannels == 1) {
        for (unsigned i = 0; i < numSamples; i++) {
            INT16 monovalue = MixINT16(
                MixFloat(leftFrontBuffer[i] * balanceFactorFrontLeft,rightFrontBuffer[i] * balanceFactorFrontRight),
                MixFloat(leftRearBuffer[i]  * balanceFactorRearLeft, rightRearBuffer[i]  * balanceFactorRearRight));
            *p++ = monovalue;
        }
    } else {
        // Flip again left/right if configured in audio
        switch (AudioType) {
        case Game::STEREO_RL:
        case Game::QUAD_1_FRL_2_RRL:
        case Game::QUAD_1_RRL_2_FRL:
            flipStereo = !flipStereo;
            break;
        default: // all other types are left-before-right ordered and need no flip
            break;
        }

        // Now order channels according to audio type
        if (nbHostAudioChannels == 2) {
            for (unsigned i = 0; i < numSamples; i++) {
                INT16 leftvalue = OutSample((leftFrontBuffer[i] * balanceFactorFrontLeft + leftRearBuffer[i] * balanceFactorRearLeft) * 0.5f);
                INT16 rightvalue = OutSample((rightFrontBuffer[i]*balanceFactorFrontRight + rightRearBuffer[i]*balanceFactorRearRight) * 0.5f);
                if (flipStereo) // swap left and right channels
                {
                    *p++ = rightvalue;
                    *p++ = leftvalue;
                } else {
                    *p++ = leftvalue;
                    *p++ = rightvalue;
                }
            }
        } else if (nbHostAudioChannels == 4) {
            for (unsigned i = 0; i < numSamples; i++) {
                float frontLeftValue = leftFrontBuffer[i]*balanceFactorFrontLeft;
                float frontRightValue = rightFrontBuffer[i]*balanceFactorFrontRight;
                float rearLeftValue = leftRearBuffer[i]*balanceFactorRearLeft;
                float rearRightValue = rightRearBuffer[i]*balanceFactorRearRight;

                // Check game audio type
                switch (AudioType) {
                case Game::MONO: {
                    INT16 monovalue = MixINT16(MixFloat(frontLeftValue, frontRightValue), MixFloat(rearLeftValue, rearRightValue));
                    *p++ = monovalue;
                    *p++ = monovalue;
                    *p++ = monovalue;
                    *p++ = monovalue;
                } break;

                case Game::STEREO_LR:
                case Game::STEREO_RL: {
                    INT16 leftvalue =  MixINT16(frontLeftValue, frontRightValue);
                    INT16 rightvalue = MixINT16(rearLeftValue,  rearRightValue);
                    if (flipStereo) // swap left and right channels
                    {
                        *p++ = rightvalue;
                        *p++ = leftvalue;
                        *p++ = rightvalue;
                        *p++ = leftvalue;
                    } else {
                        *p++ = leftvalue;
                        *p++ = rightvalue;
                        *p++ = leftvalue;
                        *p++ = rightvalue;
                    }
                } break;

                case Game::QUAD_1_FLR_2_RLR:
                case Game::QUAD_1_FRL_2_RRL: {
                    // Normal channels Front Left/Right then Rear Left/Right
                    if (flipStereo) // swap left and right channels
                    {
                        *p++ = ClampINT16(frontRightValue);
                        *p++ = ClampINT16(frontLeftValue);
                        *p++ = ClampINT16(rearRightValue);
                        *p++ = ClampINT16(rearLeftValue);
                    } else {
                        *p++ = ClampINT16(frontLeftValue);
                        *p++ = ClampINT16(frontRightValue);
                        *p++ = ClampINT16(rearLeftValue);
                        *p++ = ClampINT16(rearRightValue);
                    }
                } break;

                case Game::QUAD_1_RLR_2_FLR:
                case Game::QUAD_1_RRL_2_FRL:
                    // Reversed channels Front/Rear Left then Front/Rear Right
                    if (flipStereo) // swap left and right channels
                    {
                        *p++ = ClampINT16(rearRightValue);
                        *p++ = ClampINT16(rearLeftValue);
                        *p++ = ClampINT16(frontRightValue);
                        *p++ = ClampINT16(frontLeftValue);
                    } else {
                        *p++ = ClampINT16(rearLeftValue);
                        *p++ = ClampINT16(rearRightValue);
                        *p++ = ClampINT16(frontLeftValue);
                        *p++ = ClampINT16(frontRightValue);
                    }
                    break;

                case Game::QUAD_1_LR_2_FR_MIX:
                    // Split mix: one goes to left/right, other front/rear (mono)
                    // =>Remix all!
                    INT16 newfrontLeftValue = MixINT16(frontLeftValue, rearLeftValue);
                    INT16 newfrontRightValue = MixINT16(frontLeftValue, rearRightValue);
                    INT16 newrearLeftValue = MixINT16(frontRightValue, rearLeftValue);
                    INT16 newrearRightValue = MixINT16(frontRightValue, rearRightValue);

                    if (flipStereo) // swap left and right channels
                    {
                        *p++ = newfrontRightValue;
                        *p++ = newfrontLeftValue;
                        *p++ = newrearRightValue;
                        *p++ = newrearLeftValue;
                    } else {
                        *p++ = newfrontLeftValue;
                        *p++ = newfrontRightValue;
                        *p++ = newrearLeftValue;
                        *p++ = newrearRightValue;
                    }
                    break;
                }
            }
        }
    }
}

/*
static void LogAudioInfo(SDL_AudioSpec *fmt)
{
    InfoLog("Audio device information:");
    InfoLog("    Frequency: %d", fmt->freq);
    InfoLog("     Channels: %d", fmt->channels);
    InfoLog("Sample Format: %d", fmt->format);
    InfoLog("");
}
*/

/// <summary>
/// Prepare audio subsystem on host.
/// The requested channels is deduced, and SDL will make sure it is compatible with this.
/// </summary>
/// <param name="config"></param>
/// <returns></returns>
Result OpenAudio(const Util::Config::Node& config)
{
    s_config = &config;
    // Initialize SDL audio sub-system
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return ErrorLog("Unable to initialize SDL audio sub-system: %s\n", SDL_GetError());

    // Number of channels requested in config (default is 4)
    nbHostAudioChannels = (int)s_config->Get("NbSoundChannels").ValueAs<int>();

    // If game is only stereo or mono, enforce host to reduce number of channels
    switch (AudioType) {
    case Game::MONO:
        nbHostAudioChannels = std::min(nbHostAudioChannels, 1);
        break;
    case Game::STEREO_LR:
    case Game::STEREO_RL:
        nbHostAudioChannels = std::min(nbHostAudioChannels, 2);
        break;
    default: // quad types need up to 4 channels; config value already defaults to 4
        break;
    }
    // Mixer Balance
    float balancelr = std::max(-100.f, std::min(100.f, s_config->Get("BalanceLeftRight").ValueAs<float>()));
    balancelr *= 0.01f;
    BalanceLeftRight = balancelr;

    float balancefr = std::max(-100.f, std::min(100.f, s_config->Get("BalanceFrontRear").ValueAs<float>()));
    balancefr *= 0.01f;
    BalanceFrontRear = balancefr;

    balanceFactorFrontLeft  = (BalanceLeftRight < 0.f ? 1.f + BalanceLeftRight : 1.f) * (BalanceFrontRear < 0 ? 1.f + BalanceFrontRear : 1.f);
    balanceFactorFrontRight = (BalanceLeftRight > 0.f ? 1.f - BalanceLeftRight : 1.f) * (BalanceFrontRear < 0 ? 1.f + BalanceFrontRear : 1.f);
    balanceFactorRearLeft   = (BalanceLeftRight < 0.f ? 1.f + BalanceLeftRight : 1.f) * (BalanceFrontRear > 0 ? 1.f - BalanceFrontRear : 1.f);
    balanceFactorRearRight  = (BalanceLeftRight > 0.f ? 1.f - BalanceLeftRight : 1.f) * (BalanceFrontRear > 0 ? 1.f - BalanceFrontRear : 1.f);

    // Set up audio specification
    SDL_AudioSpec desired{};
    desired.freq = SAMPLE_RATE_M3;
    // Number of host channels to use (choice limited to 1,2,4)
    desired.channels = nbHostAudioChannels;
    desired.format = AUDIO_S16SYS;
    desired.samples = playSamples;
    desired.callback = PlayCallback;

    // Now force SDL to use the format we requested (nullptr); it will convert if necessary
    if (SDL_OpenAudio(&desired, nullptr) < 0) {
        if (desired.channels==2) {
            return ErrorLog("Unable to open 44.1KHz 2-channel audio with SDL: %s\n", SDL_GetError());
        } else if (desired.channels==4) {
            return ErrorLog("Unable to open 44.1KHz 4-channel audio with SDL: %s\n", SDL_GetError());
        } else {
            return ErrorLog("Unable to open 44.1KHz channel audio with SDL: %s\n", SDL_GetError());
        }
    }

    float soundFreq_Hz = (float)s_config->Get("SoundFreq").ValueAs<float>();
    if (soundFreq_Hz>MAX_SND_FREQ)
        soundFreq_Hz = MAX_SND_FREQ;
    if (soundFreq_Hz<MIN_SND_FREQ)
        soundFreq_Hz = MIN_SND_FREQ;
    samples_per_frame_host = (INT32)(SAMPLE_RATE_M3 / soundFreq_Hz);
    bytes_per_sample_host = (nbHostAudioChannels * sizeof(INT16));
    bytes_per_frame_host =  (samples_per_frame_host * bytes_per_sample_host);


    // Create audio buffer
    uint32_t bufferSize = ((SAMPLE_RATE_M3 * latency) / MAX_LATENCY) * bytes_per_sample_host;
    if (!(bufferSize % bytes_per_sample_host == 0)) {
        return ErrorLog("must be an integer multiple of the sample size\n");
    }
    audioBufferSize = bufferSize;

    int minBufferSize = 3 * bytes_per_frame_host;
    audioBufferSize = std::max<int>(minBufferSize, audioBufferSize);
    audioBuffer = new(std::nothrow) INT8[audioBufferSize];
    if (audioBuffer == NULL) {
        float audioBufMB = (float)audioBufferSize / (float)0x100000;
        return ErrorLog("Insufficient memory for audio latency buffer (need %1.1f MB).", audioBufMB);
    }
    memset(audioBuffer, 0, sizeof(INT8) * audioBufferSize);

    // Start with half the buffer of silence queued (the latency the pacing aims for builds from there)
    fifoRd = 0;
    fifoCnt = (audioBufferSize / 2) / bytes_per_sample_host * bytes_per_sample_host;

    // Reset counters
    underRuns = 0;
    overRuns = 0;

    // Start audio playing
    SDL_PauseAudio(0);
    return Result::OKAY;
}

// Audio-clocked pacing: bytes queued ahead of the play cursor, or -1 if there is no audio device.
int AudioBufferedBytes()
{
    std::lock_guard<std::recursive_mutex> lk(s_audMtx);
    if (!audioBuffer) return -1;
    SDL_LockAudio();
    int ahead = (int)fifoCnt;
    SDL_UnlockAudio();
    return ahead;
}
int AudioBytesPerFrame() { return bytes_per_frame_host; }

bool OutputAudio(unsigned numSamples, const float* leftFrontBuffer, const float* rightFrontBuffer, const float* leftRearBuffer, const float* rightRearBuffer, bool flipStereo)
{
    //printf("OutputAudio(%u) [writePos = %u, writeWrapped = %s, playPos = %u, audioBufferSize = %u]\n",
    //	numSamples, writePos, (writeWrapped ? "true" : "false"), playPos, audioBufferSize);

    std::lock_guard<std::recursive_mutex> lk(s_audMtx);
    if (!audioBuffer) return false;   // no device (yet): drop samples instead of crashing

    UINT32 bytesRemaining;
    UINT32 bytesToCopy;
    INT16* src;

    // Number of samples should never be more than max number of samples per frame
    if (numSamples > (unsigned)samples_per_frame_host)
        numSamples = samples_per_frame_host;

    // Mix together left and right channels into single chunk of data
    INT16 mixBuffer[NUM_CHANNELS_M3 * (SAMPLE_RATE_M3 / MIN_SND_FREQ)];
    MixChannels(numSamples, leftFrontBuffer, rightFrontBuffer, leftRearBuffer, rightRearBuffer, mixBuffer, flipStereo);

    SDL_LockAudio();
    UINT32 numBytes = numSamples * bytes_per_sample_host;
    if (fifoCnt < (UINT32)bytes_per_frame_host) {   // ran dry (stall, slow frame): queue a few frames of silence so playback restarts smoothly
        UINT32 pad = (UINT32)(3 * bytes_per_frame_host);
        if (fifoCnt + pad + numBytes > audioBufferSize) pad = 0;
        for (UINT32 i = 0; i < pad; i++) audioBuffer[(fifoRd + fifoCnt + i) % audioBufferSize] = 0;
        fifoCnt += pad;
    }
    bool written = false;
    if (fifoCnt + numBytes <= audioBufferSize) {   // otherwise over-run: this chunk is dropped
        UINT32 wr = (fifoRd + fifoCnt) % audioBufferSize;
        UINT32 first = numBytes < audioBufferSize - wr ? numBytes : audioBufferSize - wr;
        memcpy(audioBuffer + wr, mixBuffer, first);
        if (numBytes > first) memcpy(audioBuffer, (UINT8 *)mixBuffer + first, numBytes - first);
        fifoCnt += numBytes;
        written = true;
    } else overRuns++;
    bool bufferFull = !written || fifoCnt + 2 * bytes_per_frame_host > audioBufferSize;
    SDL_UnlockAudio();
    return bufferFull;
}

void CloseAudio()
{
    std::lock_guard<std::recursive_mutex> lk(s_audMtx);
    // Close SDL audio output
    SDL_CloseAudio();

    // Delete audio buffer
    delete[] audioBuffer;
    audioBuffer = nullptr;
}

// Audio watchdog: called once per emulated frame from the main loop. Guarantees sound on every device:
//  - if the device could not be opened at start (or got lost later), keeps retrying every ~1 s;
//  - if the device is open but its callback does not run (stuck/suspended output, route change, first-launch glitch), re-opens it;
//  - one extra re-open shortly after start (some Android audio HALs open "successfully" but stay silent until re-opened);
//  - makes sure the device is not left paused.
void AudioWatchdog(bool paused)
{
    if (!s_config) return;
    static int frame = 0, lastCheck = 0, failStreak = 0; static unsigned lastCb = 0; static bool warmDone = false;
    frame++;
    if (paused) { lastCheck = frame; return; }
    if (frame - lastCheck < 60) return;     // check once a second
    lastCheck = frame;
    bool open;
    { std::lock_guard<std::recursive_mutex> lk(s_audMtx); open = audioBuffer != nullptr; }
    unsigned cb = s_cbCount.load();
    bool stalled = open && frame > 90 && cb == lastCb;
    bool warm = open && !warmDone && frame >= 60;
#ifndef __ANDROID__
    warm = false;
#endif
    lastCb = cb;
    if (open && !stalled && !warm) {
        if (SDL_GetAudioStatus() != SDL_AUDIO_PLAYING) SDL_PauseAudio(0);
        failStreak = 0;
        return;
    }
    if (warm) warmDone = true;
    fprintf(stderr, "[audio] watchdog: %s -> (re)opening device\n", !open ? "no device" : stalled ? "callback stalled" : "warm-up");
    {
        std::lock_guard<std::recursive_mutex> lk(s_audMtx);
        SDL_CloseAudio();
        delete[] audioBuffer; audioBuffer = nullptr;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        if (OpenAudio(*s_config) != Result::OKAY) { failStreak++; fprintf(stderr, "[audio] re-open failed (%d)\n", failStreak); }
        else { failStreak = 0; lastCb = s_cbCount.load(); }
    }
}
