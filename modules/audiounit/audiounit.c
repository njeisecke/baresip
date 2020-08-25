/**
 * @file audiounit.c  AudioUnit sound driver
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include "audiounit.h"


/**
 * @defgroup audiounit audiounit
 *
 * Audio driver module for OSX/iOS AudioUnit
 */


#define MAX_NB_FRAMES 4096


struct conv_buf {
	void *mem[2];
	uint8_t mem_idx;
	uint32_t nb_frames;
};


AudioComponent audiounit_io = NULL;
AudioComponent audiounit_conv = NULL;

static struct auplay *auplay;
static struct ausrc *ausrc;


static void conv_buf_destructor(void *arg)
{
	struct conv_buf *buf = (struct conv_buf *)arg;

	mem_deref(buf->mem[0]);
	mem_deref(buf->mem[1]);
}


int conv_buf_alloc(struct conv_buf **bufp, size_t framesz)
{
	struct conv_buf *buf;

	if (!bufp)
		return EINVAL;

	buf = mem_zalloc(sizeof(*buf), conv_buf_destructor);
	if (!buf)
		return ENOMEM;

	buf->mem_idx = 0;
	buf->nb_frames = 0;
	buf->mem[0] = mem_alloc(MAX_NB_FRAMES * framesz, NULL);
	buf->mem[1] = mem_alloc(MAX_NB_FRAMES * framesz, NULL);

	*bufp = buf;

	return 0;
}


int  get_nb_frames(struct conv_buf *buf, uint32_t *nb_frames)
{
	if (!buf)
		return EINVAL;

	*nb_frames = buf->nb_frames;

	return 0;
}


OSStatus init_data_write(struct conv_buf *buf, void **data,
			 size_t framesz, uint32_t nb_frames)
{
	uint32_t mem_idx = buf->mem_idx;

	if (buf->nb_frames + nb_frames > MAX_NB_FRAMES) {
		return kAudioUnitErr_TooManyFramesToProcess;
	}

	*data = (uint8_t*)buf->mem[mem_idx] +
		buf->nb_frames * framesz;

	buf->nb_frames = buf->nb_frames + nb_frames;

	return noErr;
}


OSStatus init_data_read(struct conv_buf *buf, void **data,
			size_t framesz, uint32_t nb_frames)
{
	uint8_t *src;
	uint32_t delta = 0;
	uint32_t mem_idx = buf->mem_idx;

	if (buf->nb_frames < nb_frames) {
		return kAudioUnitErr_TooManyFramesToProcess;
	}

	*data = buf->mem[mem_idx];

	delta = buf->nb_frames - nb_frames;

	src = (uint8_t *)buf->mem[mem_idx] + nb_frames * framesz;

	memcpy(buf->mem[(mem_idx+1)%2],
	       (void *)src, delta * framesz);

	buf->mem_idx = (mem_idx + 1)%2;
	buf->nb_frames = delta;

	return noErr;
}


uint32_t audiounit_aufmt_to_formatflags(enum aufmt fmt)
{
	switch (fmt) {

	case AUFMT_S16LE:  return kLinearPCMFormatFlagIsSignedInteger;
	case AUFMT_S24_3LE:return kLinearPCMFormatFlagIsSignedInteger;
	case AUFMT_FLOAT:  return kLinearPCMFormatFlagIsFloat;
	default: return 0;
	}
}

#if ! TARGET_OS_IPHONE
int audiounit_enum_devices(const char *name, struct list *dev_list,
			   AudioDeviceID *matching_device_id, Boolean *match_found, Boolean is_input)
{
	AudioObjectPropertyAddress propertyAddress = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMaster
	};

	AudioDeviceID *audioDevices = NULL;
	UInt32 dataSize = 0;
	UInt32 deviceCount;
	OSStatus status;

	int err = 0;

	if (!dev_list && !matching_device_id)
		return EINVAL;

	if (matching_device_id && NULL == match_found)
		return EINVAL;

	if (NULL == matching_device_id && match_found)
		return EINVAL;

	if (match_found)
		*match_found = false;

	if (matching_device_id || match_found) {
		if (!str_isset(name))
			return 0;
	}

	status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
						&propertyAddress,
						0,
						NULL,
						&dataSize);
	if (kAudioHardwareNoError != status) {
		warning("AudioObjectGetPropertyDataSize"
			" (kAudioHardwarePropertyDevices) failed: %i\n",
			status);
		err = ENODEV;
		goto out;
	}

	deviceCount = dataSize / sizeof(AudioDeviceID);

	audioDevices = mem_zalloc(dataSize, NULL);
	if (NULL == audioDevices) {
		err = ENOMEM;
		goto out;
	}

	status = AudioObjectGetPropertyData(kAudioObjectSystemObject,
					    &propertyAddress,
					    0,
					    NULL,
					    &dataSize,
					    audioDevices);
	if (kAudioHardwareNoError != status) {
		warning("AudioObjectGetPropertyData"
			" (kAudioHardwarePropertyDevices) failed: %i\n",
			status);
		err = ENODEV;
		goto out;
	}

	if (is_input)
		propertyAddress.mScope = kAudioDevicePropertyScopeInput;
	else
		propertyAddress.mScope = kAudioDevicePropertyScopeOutput;

	for (UInt32 i = 0; i < deviceCount; ++i) {

		CFStringRef deviceName = NULL;
		const char *name_str;
		/* fallback if CFStringGetCStringPtr fails */
		char name_buf[64];

		propertyAddress.mSelector   = kAudioDevicePropertyStreams;
		status = AudioObjectGetPropertyDataSize(audioDevices[i],
							&propertyAddress,
							0,
							NULL,
							&dataSize);
		if (dataSize == 0)
			continue;

		dataSize = sizeof(deviceName);
		propertyAddress.mSelector =
			kAudioDevicePropertyDeviceNameCFString;

		status = AudioObjectGetPropertyData(audioDevices[i],
						    &propertyAddress,
						    0,
						    NULL,
						    &dataSize,
						    &deviceName);
		if (kAudioHardwareNoError != status) {
			warning("AudioObjectGetPropertyData"
				" (kAudioDevicePropertyDeviceNameCFString)"
				" failed: %i\n", status);
			continue;
		}

		name_str = CFStringGetCStringPtr(deviceName,
						 kCFStringEncodingUTF8);

		/* CFStringGetCStringPtr can and does fail
		 * (documented behavior) */
		if (0 == name_str) {
			if (!CFStringGetCString(deviceName,
						name_buf,
						sizeof(name_buf),
						kCFStringEncodingUTF8)) {
				warning("CFStringGetCString "
					" failed: %i\n", status);
				continue;
			}
			name_str = name_buf;
		}

		if (matching_device_id) {
			if (0 == str_casecmp(name, name_str)) {
				*matching_device_id = audioDevices[i];
				*match_found = true;
				break;
			}
		}
		else {
			err = mediadev_add(dev_list, name_str);
			if (err)
				break;
		}
	}

 out:
	mem_deref(audioDevices);

	return err;
}
#endif

static int module_init(void)
{
	AudioComponentDescription desc;
	CFStringRef name = NULL;
	int err;

	desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
	desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
#else
	desc.componentSubType = kAudioUnitSubType_HALOutput;
#endif
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	audiounit_comp_io = AudioComponentFindNext(NULL, &desc);
	if (!audiounit_comp_io) {
#if TARGET_OS_IPHONE
		warning("audiounit: Voice Processing I/O not found\n");
#else
		warning("audiounit: AUHAL not found\n");
#endif
		return ENOENT;
	}

	if (0 == AudioComponentCopyName(audiounit_comp_io, &name)) {
		debug("audiounit: using component '%s'\n",
		      CFStringGetCStringPtr(name, kCFStringEncodingUTF8));
	}

	desc.componentType = kAudioUnitType_FormatConverter;
	desc.componentSubType = kAudioUnitSubType_AUConverter;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	audiounit_comp_conv = AudioComponentFindNext(NULL, &desc);
	if (!audiounit_comp_conv) {
		warning("audiounit: AU Converter not found\n");
		return ENOENT;
	}

	if (0 == AudioComponentCopyName(audiounit_comp_conv, &name)) {
		debug("audiounit: using component '%s'\n",
		      CFStringGetCStringPtr(name, kCFStringEncodingUTF8));
	}

	err  = auplay_register(&auplay, baresip_auplayl(),
			       "audiounit", audiounit_player_alloc);
	err |= ausrc_register(&ausrc, baresip_ausrcl(),
			      "audiounit", audiounit_recorder_alloc);


	err  = audiounit_player_init(auplay);
	err |= audiounit_recorder_init(ausrc);

	return err;
}


static int module_close(void)
{
	ausrc  = mem_deref(ausrc);
	auplay = mem_deref(auplay);

	return 0;
}


EXPORT_SYM const struct mod_export DECL_EXPORTS(audiounit) = {
	"audiounit",
	"audio",
	module_init,
	module_close,
};
