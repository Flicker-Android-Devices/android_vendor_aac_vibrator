/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 *
 * Not a contribution.
 */

/*
 * Copyright 2018 The Android Open Source Project
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

/*
 * Changes from Qualcomm Innovation Center, Inc. are provided under the following license:
 * Copyright (c) 2022-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "vendor.rtp.vibratorOL"

#include <cutils/properties.h>
#include <dirent.h>
#include <inttypes.h>
#include <linux/input.h>
#include <log/log.h>
#include <string.h>
#include <unistd.h>
#include <bits/epoll_event.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <thread>

#include "Vibrator.h"
#ifdef USE_EFFECT_STREAM
#include "effect.h"
#endif

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

#define STRONG_MAGNITUDE        0x7fff
#define MEDIUM_MAGNITUDE        0x5fff
#define LIGHT_MAGNITUDE         0x3fff
#define INVALID_VALUE           -1
#define CUSTOM_DATA_LEN         3
#define NAME_BUF_SIZE           32
#define PRIMITIVE_ID_MASK       0x8000
#define MAX_PATTERN_ID          32767

#define MSM_CPU_LAHAINA         415
#define APQ_CPU_LAHAINA         439
#define MSM_CPU_SHIMA           450
#define MSM_CPU_SM8325          501
#define APQ_CPU_SM8325P         502
#define MSM_CPU_YUPIK           475
#define MSM_CPU_CAPE            530
#define APQ_CPU_CAPE            531
#define MSM_CPU_TARO            457
#define MSM_CPU_TARO_LTE        552
#define MSM_CPU_KALAMA          519
#define QCS_CPU_KALAMAP         603
#define MSM_CPU_PINEAPPLE       557
#define MSM_CPU_SUN             618
#define MSM_CPU_CANOE           660

#define test_bit(bit, array)    ((array)[(bit)/8] & (1<<((bit)%8)))

static const char LED_DEVICE[] = "/sys/class/leds/vibrator";
static const char HAPTICS_SYSFS[] = "/sys/class/qcom-haptics";

static constexpr int32_t ComposeDelayMaxMs = 1000;
static constexpr int32_t ComposeSizeMax = 256;

enum composeEvent {
    STOP_COMPOSE = 0,
};

InputFFDevice::InputFFDevice()
{
    DIR *dp;
    FILE *fp = NULL;
    struct dirent *dir;
    uint8_t ffBitmask[FF_CNT / 8];
    char devicename[PATH_MAX];
    const char *INPUT_DIR = "/dev/input/";
    char name[NAME_BUF_SIZE];
    int fd, ret;
    int soc = property_get_int32("ro.vendor.qti.soc_id", -1);

    mVibraFd = INVALID_VALUE;
    mSupportGain = false;
    mSupportEffects = false;
    mSupportExternalControl = false;
    mCurrAppId = INVALID_VALUE;
    mCurrMagnitude = 0x7fff;
    mInExternalControl = false;

    dp = opendir(INPUT_DIR);
    if (!dp) {
        ALOGE("open %s failed, errno = %d", INPUT_DIR, errno);
        return;
    }

    memset(ffBitmask, 0, sizeof(ffBitmask));
    while ((dir = readdir(dp)) != NULL){
        if (dir->d_name[0] == '.' &&
            (dir->d_name[1] == '\0' ||
             (dir->d_name[1] == '.' && dir->d_name[2] == '\0')))
            continue;

        snprintf(devicename, PATH_MAX, "%s%s", INPUT_DIR, dir->d_name);
        fd = TEMP_FAILURE_RETRY(open(devicename, O_RDWR));
        if (fd < 0) {
            ALOGE("open %s failed, errno = %d", devicename, errno);
            continue;
        }

        ret = TEMP_FAILURE_RETRY(ioctl(fd, EVIOCGNAME(sizeof(name)), name));
        if (ret == -1) {
            ALOGE("get input device name %s failed, errno = %d\n", devicename, errno);
            close(fd);
            continue;
        }

        if (strcmp(name, "qcom-hv-haptics") && strcmp(name, "qti-haptics")
                && strcmp(name, "aw-haptic-hv")
                && strcmp(name, "aw8624_haptic")
                && strcmp(name, "aw8695_haptic")
                && strcmp(name, "aw8697_haptic")
                && strcmp(name, "awinic_haptic")
                && strcmp(name, "drv260x:haptics")
                && strcmp(name, "drv2624:haptics")
                && strcmp(name, "haptic_rt")
                && strcmp(name, "si_haptic")) {
            ALOGD("not a supported haptics device\n");
            close(fd);
            continue;
        }

        ALOGI("%s is detected at %s\n", name, devicename);
        ret = TEMP_FAILURE_RETRY(ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffBitmask)), ffBitmask));
        if (ret == -1) {
            ALOGE("ioctl failed, errno = %d", errno);
            close(fd);
            continue;
        }

        if (test_bit(FF_CONSTANT, ffBitmask) ||
                test_bit(FF_PERIODIC, ffBitmask)) {
            mVibraFd = fd;
            if (test_bit(FF_CUSTOM, ffBitmask))
                mSupportEffects = true;
            if (test_bit(FF_GAIN, ffBitmask))
                mSupportGain = true;

            if (soc <= 0 && (fp = fopen("/sys/devices/soc0/soc_id", "r")) != NULL) {
                fscanf(fp, "%u", &soc);
                fclose(fp);
            }
            switch (soc) {
            case MSM_CPU_LAHAINA:
            case APQ_CPU_LAHAINA:
            case MSM_CPU_SHIMA:
            case MSM_CPU_SM8325:
            case APQ_CPU_SM8325P:
            case MSM_CPU_TARO:
            case MSM_CPU_TARO_LTE:
            case MSM_CPU_YUPIK:
            case MSM_CPU_CAPE:
            case APQ_CPU_CAPE:
            case MSM_CPU_KALAMA:
            case QCS_CPU_KALAMAP:
            case MSM_CPU_PINEAPPLE:
            case MSM_CPU_SUN:
            case MSM_CPU_CANOE:
                mSupportExternalControl = true;
                break;
            default:
                mSupportExternalControl = false;
                break;
            }
            break;
        }

        close(fd);
    }

    closedir(dp);
}

bool InputFFDevice::isPresent() {
    return (mVibraFd != INVALID_VALUE);
}

// Xiaomi AW8697 RTP effect IDs (Kernel aw8697_rtp_name index + 10)
enum XiaomiRtpEffect : int {
    RTP_GESTURE_UPSLIDE       = 72,  // Gesture_UpSlide_RTP.bin
    RTP_CHARGE_WIRE           = 74,  // Charge_Wire_RTP.bin
    RTP_CHARGE_WIRELESS       = 75,  // Charge_Wireless_RTP.bin
    RTP_UNLOCK_FAILED         = 76,  // Unlock_Failed_RTP.bin
    RTP_SCREENSHOT            = 85,  // screenshot_rtp.bin
    RTP_FOD_MOTION_RIPPLE     = 159, // FOD_Motion_Ripple_RTP.bin
    RTP_GESTURE_BACK_PULL     = 162, // Gesture_Back_Pull_RTP.bin
    RTP_GESTURE_BACK_RELEASE  = 163, // Gesture_Back_Release_RTP.bin
    RTP_ALERT                 = 164, // alert_rtp.bin
    RTP_FEEDBACK_NEGATIVE_LT  = 165, // feedback_negative_light_rtp.bin
    RTP_FEEDBACK_NEUTRAL      = 166, // feedback_neutral_rtp.bin
    RTP_FEEDBACK_POSITIVE     = 167, // feedback_positive_rtp.bin
    RTP_FINGERPRINT_RECORD    = 168, // fingerprint_record_rtp.bin
    RTP_LOCKDOWN              = 169, // lockdown_rtp.bin
    RTP_SLIDING_DAMPING       = 170, // sliding_damping_rtp.bin
    RTP_TODO_ALLDONE          = 171, // todo_alldone_rtp.bin
    RTP_SIGNAL_BUTTON         = 175, // signal_button_rtp.bin
    RTP_SIGNAL_CLOCK_HIGH     = 176, // signal_clock_high_rtp.bin
    RTP_SIGNAL_CLOCK_UNIT     = 178, // signal_clock_unit_rtp.bin
    RTP_SIGNAL_KEY_HIGH       = 180, // signal_key_high_rtp.bin
    RTP_SIGNAL_KEY_UNIT       = 181, // signal_key_unit_rtp.bin
    RTP_SIGNAL_LIST           = 183, // signal_list_rtp.bin
    RTP_SIGNAL_POPUP          = 185, // signal_popup_rtp.bin
    RTP_SIGNAL_SEEKBAR        = 186, // signal_seekbar_rtp.bin
    RTP_SIGNAL_SWITCH         = 187, // signal_switch_rtp.bin
    RTP_SIGNAL_TAB            = 188, // signal_tab_rtp.bin
    RTP_SIGNAL_TEXT           = 189, // signal_text_rtp.bin
};

static long getRtpDurationMs(int rtpId) {
    switch (rtpId) {
    case RTP_GESTURE_UPSLIDE:       // 72: Gesture_UpSlide_RTP.bin (4985 bytes @ 24kHz)
        return 208;
    case RTP_CHARGE_WIRE:           // 74: Charge_Wire_RTP.bin (16296 bytes @ 24kHz)
        return 679;
    case RTP_CHARGE_WIRELESS:       // 75: Charge_Wireless_RTP.bin (20380 bytes @ 24kHz)
        return 849;
    case RTP_UNLOCK_FAILED:         // 76: Unlock_Failed_RTP.bin (6422 bytes @ 24kHz)
        return 268;
    case RTP_SCREENSHOT:            // 85: screenshot_rtp.bin (2016 bytes @ 24kHz)
        return 84;
    case RTP_FOD_MOTION_RIPPLE:     // 159: FOD_Motion_Ripple_RTP.bin (14004 bytes @ 24kHz)
        return 584;
    case RTP_GESTURE_BACK_PULL:     // 162: Gesture_Back_Pull_RTP.bin (735 bytes @ 24kHz)
        return 31;
    case RTP_GESTURE_BACK_RELEASE:  // 163: Gesture_Back_Release_RTP.bin (559 bytes @ 24kHz)
        return 23;
    case RTP_ALERT:                 // 164: alert_rtp.bin (4089 bytes @ 24kHz)
        return 170;
    case RTP_FEEDBACK_NEGATIVE_LT:  // 165: feedback_negative_light_rtp.bin (2812 bytes @ 24kHz)
        return 117;
    case RTP_FEEDBACK_NEUTRAL:      // 166: feedback_neutral_rtp.bin (344 bytes @ 24kHz)
        return 14;
    case RTP_FEEDBACK_POSITIVE:     // 167: feedback_positive_rtp.bin (3298 bytes @ 24kHz)
        return 137;
    case RTP_FINGERPRINT_RECORD:    // 168: fingerprint_record_rtp.bin (735 bytes @ 24kHz)
        return 31;
    case RTP_LOCKDOWN:              // 169: lockdown_rtp.bin (1543 bytes @ 24kHz)
        return 64;
    case RTP_SLIDING_DAMPING:       // 170: sliding_damping_rtp.bin (4714 bytes @ 24kHz)
        return 196;
    case RTP_TODO_ALLDONE:          // 171: todo_alldone_rtp.bin (10664 bytes @ 24kHz)
        return 444;
    case RTP_SIGNAL_BUTTON:         // 175: signal_button_rtp.bin (3530 bytes @ 24kHz)
        return 147;
    case RTP_SIGNAL_CLOCK_HIGH:     // 176: signal_clock_high_rtp.bin (6487 bytes @ 24kHz)
        return 270;
    case RTP_SIGNAL_CLOCK_UNIT:     // 178: signal_clock_unit_rtp.bin (293 bytes @ 24kHz)
        return 12;
    case RTP_SIGNAL_KEY_HIGH:       // 180: signal_key_high_rtp.bin (3530 bytes @ 24kHz)
        return 147;
    case RTP_SIGNAL_KEY_UNIT:       // 181: signal_key_unit_rtp.bin (312 bytes @ 24kHz)
        return 13;
    case RTP_SIGNAL_LIST:           // 183: signal_list_rtp.bin (288 bytes @ 24kHz)
        return 12;
    case RTP_SIGNAL_POPUP:          // 185: signal_popup_rtp.bin (4022 bytes @ 24kHz)
        return 168;
    case RTP_SIGNAL_SEEKBAR:        // 186: signal_seekbar_rtp.bin (5424 bytes @ 24kHz)
        return 226;
    case RTP_SIGNAL_SWITCH:         // 187: signal_switch_rtp.bin (2079 bytes @ 24kHz)
        return 87;
    case RTP_SIGNAL_TAB:            // 188: signal_tab_rtp.bin (602 bytes @ 24kHz)
        return 25;
    case RTP_SIGNAL_TEXT:           // 189: signal_text_rtp.bin (2290 bytes @ 24kHz)
        return 95;
    default:
        return 0;
    }
}

static int getRtpIdForPrimitive(CompositePrimitive primitive) {
    switch (primitive) {
    case CompositePrimitive::CLICK:
        return RTP_FEEDBACK_NEUTRAL;     // 166: feedback_neutral_rtp.bin (Crisp click)
    case CompositePrimitive::THUD:
        return RTP_SLIDING_DAMPING;      // 170: sliding_damping_rtp.bin (Scroll limit)
    case CompositePrimitive::SPIN:
        return RTP_GESTURE_UPSLIDE;      // 72: Gesture_UpSlide_RTP.bin
    case CompositePrimitive::QUICK_RISE:
        return RTP_GESTURE_BACK_PULL;    // 162: Gesture_Back_Pull_RTP.bin
    case CompositePrimitive::SLOW_RISE:
        return RTP_GESTURE_UPSLIDE;      // 72: Gesture_UpSlide_RTP.bin
    case CompositePrimitive::QUICK_FALL:
        return RTP_GESTURE_BACK_RELEASE; // 163: Gesture_Back_Release_RTP.bin
    case CompositePrimitive::LIGHT_TICK:
        return RTP_FEEDBACK_NEUTRAL;     // 166: feedback_neutral_rtp.bin (Crisp click for keyboard typing)
    case CompositePrimitive::LOW_TICK:
        return RTP_GESTURE_BACK_PULL;    // 162: Gesture_Back_Pull_RTP.bin
    default:
        return RTP_FEEDBACK_NEUTRAL;     // 166
    }
}

static int getEffectId(Effect effect) {
    switch (effect) {
    // Standard AOSP effects mapped to Xiaomi factory RTP waveforms (Global touch experience)
    case Effect::CLICK:
        return RTP_FEEDBACK_NEUTRAL;     // 166: feedback_neutral_rtp.bin (Crisp click, 14ms)
    case Effect::DOUBLE_CLICK:
        return RTP_LOCKDOWN;             // 169: lockdown_rtp.bin (True double click, 64ms)
    case Effect::TICK:
        return RTP_GESTURE_BACK_PULL;    // 162: Gesture_Back_Pull_RTP.bin (Gesture pull elastic thump, 31ms)
    case Effect::THUD:
        return RTP_SLIDING_DAMPING;      // 170: sliding_damping_rtp.bin (Scroll limit, 196ms)
    case Effect::POP:
        return RTP_FEEDBACK_NEGATIVE_LT; // 165: feedback_negative_light_rtp.bin (Negative light pop, 117ms)
    case Effect::HEAVY_CLICK:
        return RTP_FEEDBACK_POSITIVE;    // 167: feedback_positive_rtp.bin (Heavy click, 137ms)
    case Effect::TEXTURE_TICK:
        return RTP_SIGNAL_CLOCK_UNIT;    // 178: signal_clock_unit_rtp.bin (Clock texture unit, 12ms)

    // Dedicated RAM hardware waveforms (AW8697 SRAM loaded waveforms)
    case Effect::RINGTONE_1:
        return 0;                        // RAM ID 0: wf_0 (CLICK, 20ms)
    case Effect::RINGTONE_2:
        return 1;                        // RAM ID 1: wf_1 (DOUBLE CLICK, 20ms)
    case Effect::RINGTONE_3:
        return 2;                        // RAM ID 2: wf_2 (TICK, 20ms)
    case Effect::RINGTONE_4:
        return 4;                        // RAM ID 4: wf_4 (POP, 28ms)
    case Effect::RINGTONE_5:
        return 5;                        // RAM ID 5: wf_5 (HEAVY CLICK, 20ms)

    // Reallocated factory RTP waveforms for rich XML customization (no duplicates of basic effects)
    case Effect::RINGTONE_6:
        return RTP_SIGNAL_SWITCH;        // 187: signal_switch_rtp.bin (Toggle switch, 87ms)
    case Effect::RINGTONE_7:
        return RTP_SIGNAL_SEEKBAR;       // 186: signal_seekbar_rtp.bin (Seekbar notch/tick, 226ms)
    case Effect::RINGTONE_8:
        return RTP_SIGNAL_POPUP;         // 185: signal_popup_rtp.bin (Popup menu/dialog, 168ms)
    case Effect::RINGTONE_9:
        return RTP_SIGNAL_TAB;           // 188: signal_tab_rtp.bin (Tab page switch, 25ms)
    case Effect::RINGTONE_10:
        return RTP_SIGNAL_TEXT;          // 189: signal_text_rtp.bin (Text selection / cursor, 95ms)
    case Effect::RINGTONE_11:
        return RTP_SIGNAL_BUTTON;        // 175: signal_button_rtp.bin (Main action button, 147ms)
    case Effect::RINGTONE_12:
        return RTP_TODO_ALLDONE;         // 171: todo_alldone_rtp.bin (Operation all done fanfare, 444ms)
    case Effect::RINGTONE_13:
        return RTP_ALERT;                // 164: alert_rtp.bin (Alert notice, 170ms)
    case Effect::RINGTONE_14:
        return RTP_FINGERPRINT_RECORD;   // 168: fingerprint_record_rtp.bin (Fingerprint confirm, 31ms)
    case Effect::RINGTONE_15:
        return RTP_UNLOCK_FAILED;        // 76: Unlock_Failed_RTP.bin (Unlock/biometric failed, 268ms)
    default:
        return RTP_FEEDBACK_NEUTRAL;     // 166
    }
}

/** Play vibration
 *
 *  @param effectId:  ID of the predefined effect will be played. If effectId is valid
 *                    (non-negative value), the timeoutMs value will be ignored, and the
 *                    real playing length will be set in param@playLengtMs and returned
 *                    to VibratorService. If effectId is invalid, value in param@timeoutMs
 *                    will be used as the play length for playing a constant effect.
 *  @param timeoutMs: playing length, non-zero means playing, zero means stop playing.
 *  @param playLengthMs: the playing length in ms unit which will be returned to
 *                    VibratorService if the request is playing a predefined effect.
 *                    The custom_data in periodic is reused for returning the playLengthMs
 *                    from kernel space to userspace if the pattern is defined in kernel
 *                    driver. It's been defined with following format:
 *                       <effect-ID, play-time-in-seconds, play-time-in-milliseconds>.
 *                    The effect-ID is used for passing down the predefined effect to
 *                    kernel driver, and the rest two parameters are used for returning
 *                    back the real playing length from kernel driver.
 */
int InputFFDevice::play(int effectId, uint32_t timeoutMs, long *playLengthMs) {
    struct ff_effect effect;
    struct input_event play;
    int16_t data[CUSTOM_DATA_LEN] = {0, 0, 0};
    int ret;
#ifdef USE_EFFECT_STREAM
    const struct effect_stream *stream;
#endif

    mtx.lock();
    /* For QMAA compliance, return OK even if vibrator device doesn't exist */
    if (!isPresent()) {
        if (playLengthMs != NULL)
            *playLengthMs = 0;
        mtx.unlock();
        return 0;
    }

    if (timeoutMs != 0) {
        if (mCurrAppId != INVALID_VALUE) {
            ret = TEMP_FAILURE_RETRY(ioctl(mVibraFd, EVIOCRMFF, mCurrAppId));
            if (ret == -1) {
                ALOGE("ioctl EVIOCRMFF failed, errno = %d", -errno);
                goto errout;
            }
            mCurrAppId = INVALID_VALUE;
        }

        memset(&effect, 0, sizeof(effect));
        if (effectId != INVALID_VALUE) {
            data[0] = effectId;
            effect.type = FF_PERIODIC;
            effect.u.periodic.waveform = FF_CUSTOM;
            effect.u.periodic.magnitude = mCurrMagnitude;
            effect.u.periodic.custom_data = data;
            effect.u.periodic.custom_len = sizeof(int16_t) * CUSTOM_DATA_LEN;
#ifdef USE_EFFECT_STREAM
            stream = get_effect_stream(effectId);
            if (stream != NULL) {
                effect.u.periodic.custom_data = (int16_t *)stream;
                effect.u.periodic.custom_len = sizeof(*stream);
            }
#endif
        } else {
            effect.type = FF_CONSTANT;
            effect.u.constant.level = mCurrMagnitude;
            effect.replay.length = timeoutMs;
        }

        effect.id = mCurrAppId;
        effect.replay.delay = 0;

        ret = TEMP_FAILURE_RETRY(ioctl(mVibraFd, EVIOCSFF, &effect));
        if (ret == -1) {
            ALOGE("ioctl EVIOCSFF failed, errno = %d", -errno);
            goto errout;
        }

        mCurrAppId = effect.id;
        if (effectId != INVALID_VALUE && playLengthMs != NULL) {
            long rtpDur = getRtpDurationMs(effectId);
            if (rtpDur > 0) {
                *playLengthMs = rtpDur;
            } else if (data[1] > 0 && data[2] > 0 && data[2] < 5000) {
                *playLengthMs = data[2];
            } else {
                *playLengthMs = data[1] * 1000 + data[2];
            }
            if (*playLengthMs <= 0 || *playLengthMs >= 5000) {
                *playLengthMs = 25;
            }
#ifdef USE_EFFECT_STREAM
            if (stream != NULL && stream->play_rate_hz != 0)
                *playLengthMs = ((stream->length * 1000) / stream->play_rate_hz) + 1;
#endif
        }

        play.value = 1;
        play.type = EV_FF;
        play.code = mCurrAppId;
        play.time.tv_sec = 0;
        play.time.tv_usec = 0;
        ret = TEMP_FAILURE_RETRY(write(mVibraFd, (const void*)&play, sizeof(play)));
        if (ret == -1) {
            ALOGE("write failed, errno = %d\n", -errno);
            ret = TEMP_FAILURE_RETRY(ioctl(mVibraFd, EVIOCRMFF, mCurrAppId));
            if (ret == -1)
                ALOGE("ioctl EVIOCRMFF failed, errno = %d", -errno);
            goto errout;
        }
        ALOGD("InputFFDevice::play: effectId=%d, playLength=%ldms",
              effectId, playLengthMs ? *playLengthMs : -1);
    } else if (mCurrAppId != INVALID_VALUE) {
        ret = TEMP_FAILURE_RETRY(ioctl(mVibraFd, EVIOCRMFF, mCurrAppId));
        if (ret == -1) {
            ALOGE("ioctl EVIOCRMFF failed, errno = %d", -errno);
            goto errout;
        }
        mCurrAppId = INVALID_VALUE;
    }
    mtx.unlock();
    return 0;

errout:
    mCurrAppId = INVALID_VALUE;
    mtx.unlock();
    return ret;
}

int InputFFDevice::on(int32_t timeoutMs) {
    ALOGD("InputFFDevice::on: timeoutMs=%d", timeoutMs);
    return play(INVALID_VALUE, timeoutMs, NULL);
}

int InputFFDevice::off() {
    ALOGD("InputFFDevice::off");
    return play(INVALID_VALUE, 0, NULL);
}

int InputFFDevice::setAmplitude(uint8_t amplitude) {
    int32_t tmp;
    int ret;
    struct input_event ie;

    /* For QMAA compliance, return OK even if vibrator device doesn't exist */
    if (!isPresent())
        return 0;

    ALOGD("InputFFDevice::setAmplitude: amplitude=%u", amplitude);

    tmp = amplitude * STRONG_MAGNITUDE / 255;
    ie.type = EV_FF;
    ie.code = FF_GAIN;
    ie.value = tmp;

    ret = TEMP_FAILURE_RETRY(write(mVibraFd, &ie, sizeof(ie)));
    if (ret == -1) {
        ALOGE("write FF_GAIN failed, errno = %d", -errno);
        return ret;
    }

    mCurrMagnitude = tmp;
    return 0;
}

int InputFFDevice::playEffect(int effectId, EffectStrength es, long *playLengthMs) {
    if (effectId > MAX_PATTERN_ID) {
        ALOGE("effect id %d exceeds %d", effectId, MAX_PATTERN_ID);
        return -1;
    }

    switch (es) {
    case EffectStrength::LIGHT:
        mCurrMagnitude = LIGHT_MAGNITUDE;
        break;
    case EffectStrength::MEDIUM:
        mCurrMagnitude = MEDIUM_MAGNITUDE;
        break;
    case EffectStrength::STRONG:
        mCurrMagnitude = STRONG_MAGNITUDE;
        break;
    default:
        return -1;
    }

    int ret = play(effectId, INVALID_VALUE, playLengthMs);
    ALOGD("playEffect: effectId=%d, strength=%d, playLength=%ldms, ret=%d",
          effectId, static_cast<int>(es), playLengthMs ? *playLengthMs : -1, ret);
    return ret;
}

int InputFFDevice::playPrimitive(int primitiveId, float amplitude, long *playLengthMs) {
    int32_t tmp;
    int ret = 0;

    if (primitiveId > MAX_PATTERN_ID) {
        ALOGE("primitive id %d exceeds %d", primitiveId, MAX_PATTERN_ID);
        return -1;
    }

    if (amplitude <= 0.0f) {
        if (playLengthMs != NULL)
            *playLengthMs = 0;
        return 0;
    }

    // Map amplitude across [LIGHT_MAGNITUDE, STRONG_MAGNITUDE] (16383 ~ 32767).
    tmp = LIGHT_MAGNITUDE + (int32_t)(amplitude * (STRONG_MAGNITUDE - LIGHT_MAGNITUDE));
    if (tmp > STRONG_MAGNITUDE)
        tmp = STRONG_MAGNITUDE;
    mCurrMagnitude = tmp;

    int rtpId = getRtpIdForPrimitive(static_cast<CompositePrimitive>(primitiveId));
    ret = play(rtpId, INVALID_VALUE, playLengthMs);
    ALOGD("playPrimitive: primitive=%d, rtpId=%d, scale=%.2f, playLength=%ldms, ret=%d",
          primitiveId, rtpId, amplitude, playLengthMs ? *playLengthMs : -1, ret);
    if (ret != 0)
        ALOGE("Failed to play primitive %d (rtpId %d)", primitiveId, rtpId);

    return ret;
}

LedVibratorDevice::LedVibratorDevice() {
    char devicename[PATH_MAX];
    int fd;

    mDetected = false;
    mIsLdo = false;

    snprintf(devicename, sizeof(devicename), "%s/%s", LED_DEVICE, "activate");
    fd = TEMP_FAILURE_RETRY(open(devicename, O_RDWR));
    if (fd < 0) {
        ALOGE("open %s failed, errno = %d", devicename, errno);
        return;
    }

    mDetected = true;

    snprintf(devicename, sizeof(devicename), "%s/%s", LED_DEVICE, "device/driver");
    mIsLdo = realpath(devicename, devicename) && strstr(devicename, "/qcom,qpnp-vibrator-ldo");
}

int LedVibratorDevice::write_value(const char *file, const char *value) {
    int fd;
    int ret;

    fd = TEMP_FAILURE_RETRY(open(file, O_WRONLY));
    if (fd < 0) {
        ALOGE("open %s failed, errno = %d", file, errno);
        return -errno;
    }

    ret = TEMP_FAILURE_RETRY(write(fd, value, strlen(value) + 1));
    if (ret == -1) {
        ret = -errno;
    } else if (ret != strlen(value) + 1) {
        /* even though EAGAIN is an errno value that could be set
           by write() in some cases, none of them apply here.  So, this return
           value can be clearly identified when debugging and suggests the
           caller that it may try to call vibrator_on() again */
        ret = -EAGAIN;
    } else {
        ret = 0;
    }

    errno = 0;
    close(fd);

    return ret;
}

int LedVibratorDevice::on(int32_t timeoutMs) {
    char file[PATH_MAX];
    char value[32];
    int ret;

    snprintf(file, sizeof(file), "%s/%s", LED_DEVICE, "state");
    ret = write_value(file, "1");
    if (ret < 0)
       goto error;

    snprintf(file, sizeof(file), "%s/%s", LED_DEVICE, "duration");
    snprintf(value, sizeof(value), "%u\n", timeoutMs);
    ret = write_value(file, value);
    if (ret < 0)
       goto error;

    snprintf(file, sizeof(file), "%s/%s", LED_DEVICE, "activate");
    ret = write_value(file, "1");
    if (ret < 0)
       goto error;

    return 0;

error:
    ALOGE("Failed to turn on vibrator ret: %d\n", ret);
    return ret;
}

int LedVibratorDevice::off()
{
    char file[PATH_MAX];
    int ret;

    snprintf(file, sizeof(file), "%s/%s", LED_DEVICE, "activate");
    ret = write_value(file, "0");
    return ret;
}

static bool is_VI_sense_supported() {
    char visense_sysfs[50];
    char visense[3];
    int fd, ret;

    ret = snprintf(visense_sysfs, sizeof(visense_sysfs), "%s%s", HAPTICS_SYSFS, "/visense_enabled");
    if (ret < 0) {
        ALOGE("Failed to generate visense_enabled path name, ret = %d\n", ret);
        return false;
    }

    fd = TEMP_FAILURE_RETRY(open(visense_sysfs, O_RDONLY));
    if (fd < 0) {
        ALOGE("Open %s failed, fd = %d\n", visense_sysfs, fd);
        return false;
    }

    ret = TEMP_FAILURE_RETRY(read(fd, visense, sizeof(visense)));
    close(fd);
    if (ret < 0) {
        ALOGE("Failed to read %s, errno = %d\n", visense_sysfs, errno);
        return false;
    }

    return atoi(visense);
}

VibratorOL::VibratorOL() {
    struct epoll_event ev;

    mSupportVISense = is_VI_sense_supported();

    epollfd = INVALID_VALUE;
    pipefd[0] = INVALID_VALUE;
    pipefd[1] = INVALID_VALUE;
    inComposition = false;

    if (!ff.mSupportEffects)
        return;

    if (pipe(pipefd)) {
        ALOGE("Failed to get pipefd error=%d", errno);
        return;
    }

    epollfd = epoll_create1(0);
    if (epollfd < 0) {
        ALOGE("Failed to create epoll fd error=%d", errno);
        goto pipefd_close;
    }

    ev.events = EPOLLIN;
    ev.data.fd = pipefd[0];

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd[0], &ev) == -1) {
        ALOGE("Failed to add pipefd to epoll ctl error=%d", errno);
        goto epollfd_close;
    }

    return;

epollfd_close:
    close(epollfd);
    epollfd = INVALID_VALUE;
pipefd_close:
    close(pipefd[0]);
    close(pipefd[1]);
    pipefd[0] = INVALID_VALUE;
    pipefd[1] = INVALID_VALUE;
}

VibratorOL::~VibratorOL() {
    if (epollfd != INVALID_VALUE)
        close(epollfd);
    if (pipefd[0] != INVALID_VALUE)
        close(pipefd[0]);
    if (pipefd[1] != INVALID_VALUE)
        close(pipefd[1]);
}

ndk::ScopedAStatus VibratorOL::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK;

    if (ledVib.mDetected) {
        ALOGD("QTI Vibrator reporting capabilities: %d", *_aidl_return);
        return ndk::ScopedAStatus::ok();
    }

    if (ff.mSupportGain)
        *_aidl_return |= IVibrator::CAP_AMPLITUDE_CONTROL;
    if (ff.mSupportEffects) {
        *_aidl_return |= IVibrator::CAP_PERFORM_CALLBACK;
        std::vector<CompositePrimitive> supportedPrimitives;
        getSupportedPrimitives(&supportedPrimitives);
        if (!supportedPrimitives.empty())
            *_aidl_return |= IVibrator::CAP_COMPOSE_EFFECTS;
    }
    if (ff.mSupportExternalControl)
        *_aidl_return |= IVibrator::CAP_EXTERNAL_CONTROL;

    ALOGD("QTI Vibrator reporting capabilities: %d", *_aidl_return);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::off() {
    int ret;
    int composeEven = STOP_COMPOSE;

    ALOGD("QTI Vibrator off");
    if (ledVib.mDetected)
        ret = ledVib.off();
    else
        ret = ff.off();
    if (ret != 0)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

    if (inComposition) {
        ret = write(pipefd[1], &composeEven, sizeof(composeEven));
        if (ret < 0) {
            ALOGE("Failed to send STOP_COMPOSE event");
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));
        }
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    int ret;

    if (ledVib.mIsLdo) {
        // See QPNP_VIB_MIN_PLAY_MS, QPNP_VIB_MAX_PLAY_MS in leds-qpnp-vibrator-ldo.c
        timeoutMs = std::clamp(timeoutMs, 50, 15000);
    }

    ALOGD("Vibrator on for timeoutMs: %d", timeoutMs);
    if (ledVib.mDetected)
        ret = ledVib.on(timeoutMs);
    else
        ret = ff.on(timeoutMs);

    if (ret != 0)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

    if (callback != nullptr) {
        std::thread([=] {
            ALOGD("Starting on on another thread");
            usleep(timeoutMs * 1000);
            ALOGD("Notifying on complete");
            if (!callback->onComplete().isOk()) {
                ALOGE("Failed to call onComplete");
            }
        }).detach();
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::perform(Effect effect, EffectStrength es, const std::shared_ptr<IVibratorCallback>& callback, int32_t* _aidl_return) {
    long playLengthMs;
    int ret;

    if (ledVib.mDetected)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    if (effect < Effect::CLICK || effect > Effect::TEXTURE_TICK)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    if (es != EffectStrength::LIGHT && es != EffectStrength::MEDIUM && es != EffectStrength::STRONG)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    int effectId = getEffectId(effect);
    if (effectId < 10) {
        ALOGD("Vibrator perform ram effect=%d -> ramId=%d, strength=%d",
              static_cast<int>(effect), effectId, static_cast<int>(es));
    } else {
        ALOGD("Vibrator perform rtp effect=%d -> rtpId=%d, strength=%d",
              static_cast<int>(effect), effectId, static_cast<int>(es));
    }

    ret = ff.playEffect(effectId, es, &playLengthMs);
    if (ret != 0)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

    if (callback != nullptr) {
        std::thread([=] {
            ALOGD("Starting perform on another thread");
            usleep(playLengthMs * 1000);
            ALOGD("Notifying perform complete");
            callback->onComplete();
        }).detach();
    }

    *_aidl_return = playLengthMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    if (ledVib.mDetected)
        return ndk::ScopedAStatus::ok();

        if (Offload.mEnabled == 1) {
            *_aidl_return = {Effect::CLICK, Effect::DOUBLE_CLICK, Effect::TICK, Effect::THUD,
                             Effect::POP, Effect::HEAVY_CLICK, Effect::RINGTONE_12,
                             Effect::RINGTONE_13, Effect::RINGTONE_14, Effect::RINGTONE_15};
            return ndk::ScopedAStatus::ok();
        }
#ifndef USE_EFFECT_STREAM
        *_aidl_return = {Effect::CLICK, Effect::DOUBLE_CLICK, Effect::TICK, Effect::THUD,
                         Effect::POP, Effect::HEAVY_CLICK, Effect::TEXTURE_TICK,
                         Effect::RINGTONE_1, Effect::RINGTONE_2, Effect::RINGTONE_3,
                         Effect::RINGTONE_4, Effect::RINGTONE_5, Effect::RINGTONE_6,
                         Effect::RINGTONE_7, Effect::RINGTONE_8, Effect::RINGTONE_9,
                         Effect::RINGTONE_10, Effect::RINGTONE_11, Effect::RINGTONE_12,
                         Effect::RINGTONE_13, Effect::RINGTONE_14, Effect::RINGTONE_15};
#else
    for (int32_t effectId = static_cast<int32_t>(Effect::CLICK);
         effectId <= static_cast<int32_t>(Effect::TEXTURE_TICK);
         effectId++) {
        const struct effect_stream *stream;

        stream = get_effect_stream(effectId);
        if (stream)
            _aidl_return->push_back(static_cast<Effect>(effectId));
    }
#endif

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::setAmplitude(float amplitude) {
    uint8_t tmp;
    int ret;

    if (ledVib.mDetected)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    if (!ff.mSupportGain)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    ALOGD("Vibrator set amplitude: %f", amplitude);

    if (amplitude <= 0.0f || amplitude > 1.0f)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));

    if (ff.mInExternalControl)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    tmp = (uint8_t)(amplitude * 0xff);
    ret = ff.setAmplitude(tmp);
    if (ret != 0)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_SERVICE_SPECIFIC));

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::setExternalControl(bool enabled) {
    if (ledVib.mDetected)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    ALOGD("Vibrator set external control: %d", enabled);
    if (!ff.mSupportExternalControl)
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));

    ff.mInExternalControl = enabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getCompositionDelayMax(int32_t* maxDelayMs) {
    *maxDelayMs = ComposeDelayMaxMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getCompositionSizeMax(int32_t* maxSize) {
    *maxSize = ComposeSizeMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getSupportedPrimitives(std::vector<CompositePrimitive>* supported) {
#ifndef USE_EFFECT_STREAM
    *supported =  {
        CompositePrimitive::NOOP,   CompositePrimitive::CLICK,
        CompositePrimitive::THUD,   CompositePrimitive::SPIN,
        CompositePrimitive::QUICK_RISE, CompositePrimitive::SLOW_RISE,
        CompositePrimitive::QUICK_FALL, CompositePrimitive::LIGHT_TICK,
        CompositePrimitive::LOW_TICK,
    };
#else
    for (int32_t primitiveId = static_cast<int32_t>(CompositePrimitive::NOOP);
         primitiveId <= static_cast<int32_t>(CompositePrimitive::LOW_TICK);
         primitiveId++) {
        const struct effect_stream *stream;
        int32_t effectId  = primitiveId | PRIMITIVE_ID_MASK;

        stream = get_effect_stream(effectId);
        if (stream)
            supported->push_back(static_cast<CompositePrimitive>(primitiveId));
    }
#endif
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* durationMs) {
#ifdef USE_EFFECT_STREAM
    uint32_t primitive_id = static_cast<uint32_t>(primitive);
    primitive_id |= PRIMITIVE_ID_MASK;
    const struct effect_stream *stream;
    stream = get_effect_stream(primitive_id);
    if (stream != NULL && stream->play_rate_hz != 0)
        *durationMs = ((stream->length * 1000) / stream->play_rate_hz) + 1;

    ALOGD("primitive-%d duration is %dms", static_cast<int>(primitive), *durationMs);
    return ndk::ScopedAStatus::ok();
#endif

    /* For QMAA compliance */
    if (!ff.isPresent()) {
        *durationMs = 0; /* fake a constant duration for all primitives */
        return ndk::ScopedAStatus::ok();
    }

    if (primitive == CompositePrimitive::NOOP) {
        *durationMs = 0;
        return ndk::ScopedAStatus::ok();
    }

    int rtpId = getRtpIdForPrimitive(primitive);
    *durationMs = getRtpDurationMs(rtpId);
    if (*durationMs <= 0) {
        *durationMs = 15;
    }

    ALOGD("getPrimitiveDuration: primitive=%d -> rtpId=%d, duration=%dms",
          static_cast<int>(primitive), rtpId, *durationMs);
    return ndk::ScopedAStatus::ok();
}

void VibratorOL::composePlayThread(VibratorOL *vibrator,
                            const std::vector<CompositeEffect>& composite,
                            const std::shared_ptr<IVibratorCallback>& callback){
    struct epoll_event events;
    long playLengthMs = 0;
    int nfd = 0;
    int status = 0;
    int ret = 0;

    ALOGD("start a new thread for composeEffect");
    for (auto& e : composite) {
        if (e.delayMs) {
            nfd = epoll_wait(vibrator->epollfd, &events, 1, e.delayMs);
            if ((nfd == -1) && (errno != EINTR)) {
                ALOGE("Failed to wait delayMs, error=%d", errno);
                break;
            }

            if (nfd > 0) {
                /* It's supposed that STOP_COMPOSE command is received so quit the composition */
                ret = read(vibrator->pipefd[0], &status, sizeof(int));
                if (ret < 0) {
                    ALOGE("Failed to read stop status from pipe(delayMs), status = %d", status);
                    break;
                }
                if (status == STOP_COMPOSE)
                    break;
            }
        }

        vibrator->ff.playPrimitive((static_cast<int>(e.primitive)), e.scale, &playLengthMs);
        nfd = epoll_wait(vibrator->epollfd, &events, 1, playLengthMs);
        if (nfd == -1 && (errno != EINTR)) {
            ALOGE("Failed to wait sleep playLengthMs, error=%d", errno);
            break;
        }

        if (nfd > 0) {
            /* It's supposed that STOP_COMPOSE command is received so quit the composition */
            ret = read(vibrator->pipefd[0], &status, sizeof(int));
            if (ret < 0) {
                ALOGE("Failed to read stop status from pipe(playLengthMs), status = %d", status);
                break;
            }
            if (status == STOP_COMPOSE) {

                /*
                 * There is a corner case that the off() command could be executed in
                 * main thread before the primitive play is triggered in the child thread,
                 * such as, when playing a very short primitive effect while the system is
                 * pretty busy (one example is enabling all kernel console log after executed
                 * "echo Y > /sys/module/printk/parameters/ignore_loglevel"), the child thread
                 * may not be able to schedule out for running before the main thread times out
                 * on the primitive duration and sent the off() command, there won't be any
                 * off() command coming again to stop the primitive effect after it's triggered.
                 *
                 * However, the primitive could be played out and stopped automatically but the
                 * haptics driver does expect an explicit off() command to restore HW/SW logic
                 * after that, so call it here. It would result a redundant off() command in
                 * normal case but it won't do any harm because it would be ignored and not sent
                 * to haptics driver because of an invalid mCurrAppId. It would also result in the
                 * primitive effect to stop immediately right after it's triggered in such
                 * corner case. But considering the main thread has stopped it before off() is
                 * called here, take this as a limitation and it is expected not playing the
                 * vibration out.
                 */

                vibrator->ff.off();
                break;
            }
        }
    }

    ALOGD("Notifying composite complete, playlength= %ld", playLengthMs);
    if (callback)
        callback->onComplete();

    vibrator->inComposition = false;
}

ndk::ScopedAStatus VibratorOL::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    int status, nfd = 0, durationMs = 0, timeoutMs = 0;
    struct epoll_event events;

    if (composite.size() > ComposeSizeMax) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::vector<CompositePrimitive> supported;
    getSupportedPrimitives(&supported);

    ALOGD("compose: count=%zu", composite.size());
    for (size_t i = 0; i < composite.size(); i++) {
        const auto& e = composite[i];
        if (e.delayMs > ComposeDelayMaxMs) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (e.scale < 0.0f || e.scale > 1.0f) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (std::find(supported.begin(), supported.end(), e.primitive) == supported.end()) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }

        getPrimitiveDuration(e.primitive, &durationMs);
        timeoutMs += durationMs + e.delayMs;

        int rtpId = getRtpIdForPrimitive(e.primitive);
        ALOGD("  compose[%zu]: primitive=%d -> rtpId=%d, scale=%.2f, delay=%dms",
              i, static_cast<int>(e.primitive), rtpId, e.scale, e.delayMs);
    }

    /*
     * wait for 2 times of the play length timeout to make sure last play has been
     * terminated successfully.
     */
    timeoutMs = (timeoutMs + 10) * 2;
    /* Stop previous composition if it has not yet been completed */
    if (inComposition) {
        ALOGD("Last composePlayThread has not done yet, stop it manually");
        off();

        int waitTimeout = 100;
        while (inComposition && waitTimeout--)
            usleep(1000);

        if (waitTimeout <= 0 && inComposition) {
            ALOGE("wait for last composePlayThread done timeout");
            return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
        }
    }

    /* Read the pipe again to remove any stale data before triggering a new play */
    nfd = epoll_wait(epollfd, &events, 1, 0);
    if (nfd == -1 && (errno != EINTR)) {
        ALOGE("Failed to wait sleep playLengthMs, error=%d", errno);
        return ndk::ScopedAStatus::fromExceptionCode(EX_SERVICE_SPECIFIC);
    }
    if (nfd > 0) {
        ALOGD("A stale event is cached in the pipe, remove it");
        read(pipefd[0], &status, sizeof(int));
    }

    inComposition = true;
    composeThread = std::thread(composePlayThread, this, composite, callback);
    composeThread.detach();

    ALOGD("trigger composition successfully");
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorOL::getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::alwaysOnEnable(int32_t id __unused, Effect effect __unused,
                                            EffectStrength strength __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::alwaysOnDisable(int32_t id __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getResonantFrequency(float *resonantFreqHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getQFactor(float *qFactor __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getFrequencyResolution(float *freqResolutionHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getFrequencyMinimum(float *freqMinimumHz __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getBandwidthAmplitudeMap(std::vector<float> *_aidl_return __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getPwlePrimitiveDurationMax(int32_t *durationMs __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getPwleCompositionSizeMax(int32_t *maxSize __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::getSupportedBraking(std::vector<Braking> *supported __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus VibratorOL::composePwle(const std::vector<PrimitivePwle> &composite __unused,
                           const std::shared_ptr<IVibratorCallback> &callback __unused) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
