/*
 * ReacJack CoreAudio AudioServerPlugIn (milestone 5 of docs/COREAUDIO_PLAN.md).
 *
 * Publishes an input-only virtual device "ReacJack REAC": 40 channels of
 * float32 at 48 kHz. This milestone records silence; milestone 6 connects
 * the IO path to the reacjackd shared memory ring.
 *
 * The plug-in runs inside coreaudiod, so nothing here may block or allocate
 * on the IO path (GetZeroTimeStamp and DoIOOperation).
 */

#include <pthread.h>
#include <string.h>

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>

#include "../shared_audio.h"

enum {
  kObjectID_PlugIn = kAudioObjectPlugInObject,
  kObjectID_Device = 2,
  kObjectID_Stream_Input = 3
};

#define kDevice_Name "ReacJack REAC"
#define kDevice_Manufacturer "ReacJack"
#define kDevice_UID "ReacJackDevice_UID"
#define kDevice_ModelUID "ReacJackDevice_ModelUID"
#define kDevice_RingName "/reacjack-audio"

enum {
  kDevice_ChannelCount = 40,
  kDevice_BytesPerFrame = kDevice_ChannelCount * sizeof(Float32),
  kDevice_RingFrames = 8192 /* zero timestamp period */
};
static const Float64 kDevice_SampleRate = 48000.0;

static pthread_mutex_t gPlugIn_StateMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gPlugIn_RefCount = 0;
static AudioServerPlugInHostRef gPlugIn_Host = NULL;

static pthread_mutex_t gDevice_IOMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt64 gDevice_IOIsRunning = 0;
static Float64 gDevice_HostTicksPerFrame = 0.0;
static UInt64 gDevice_AnchorHostTime = 0;

/* Opened on the first StartIO and closed on the last StopIO, so the IO path
 * itself never maps or unmaps memory. Absent daemon means silence. */
static SharedAudio gDevice_Ring;
static Boolean gDevice_RingIsOpen = false;

static AudioStreamBasicDescription device_stream_format(void)
{
  AudioStreamBasicDescription format;
  memset(&format, 0, sizeof(format));
  format.mSampleRate = kDevice_SampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
  format.mBytesPerPacket = kDevice_BytesPerFrame;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = kDevice_BytesPerFrame;
  format.mChannelsPerFrame = kDevice_ChannelCount;
  format.mBitsPerChannel = 32;
  return format;
}

static Boolean formats_equal(const AudioStreamBasicDescription *a,
                             const AudioStreamBasicDescription *b)
{
  return a->mSampleRate == b->mSampleRate && a->mFormatID == b->mFormatID &&
         a->mFormatFlags == b->mFormatFlags &&
         a->mBytesPerPacket == b->mBytesPerPacket &&
         a->mFramesPerPacket == b->mFramesPerPacket &&
         a->mBytesPerFrame == b->mBytesPerFrame &&
         a->mChannelsPerFrame == b->mChannelsPerFrame &&
         a->mBitsPerChannel == b->mBitsPerChannel;
}

static UInt32 channel_layout_size(void)
{
  return offsetof(AudioChannelLayout, mChannelDescriptions) +
         kDevice_ChannelCount * sizeof(AudioChannelDescription);
}

/* ==== property tables ==================================================== */

static Boolean plugin_has_property(const AudioObjectPropertyAddress *address)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
    case kAudioPlugInPropertyTranslateUIDToDevice:
    case kAudioPlugInPropertyResourceBundle:
      return true;
    default:
      return false;
  }
}

static Boolean device_has_property(const AudioObjectPropertyAddress *address)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioObjectPropertyControlList:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyRelatedDevices:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertyStreams:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyZeroTimeStampPeriod:
      return true;
    case kAudioDevicePropertyPreferredChannelsForStereo:
    case kAudioDevicePropertyPreferredChannelLayout:
      return address->mScope == kAudioObjectPropertyScopeInput ||
             address->mScope == kAudioObjectPropertyScopeGlobal;
    default:
      return false;
  }
}

static Boolean stream_has_property(const AudioObjectPropertyAddress *address)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyLatency:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
      return true;
    default:
      return false;
  }
}

/* ==== COM glue =========================================================== */

static AudioServerPlugInDriverInterface gDriverInterface;
static AudioServerPlugInDriverInterface *gDriverInterfacePtr = &gDriverInterface;
static AudioServerPlugInDriverRef gDriverRef = &gDriverInterfacePtr;

static HRESULT ReacJack_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
  if (inDriver != gDriverRef || outInterface == NULL) {
    return kAudioHardwareIllegalOperationError;
  }

  CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
  if (uuid == NULL) {
    return kAudioHardwareIllegalOperationError;
  }

  Boolean supported = CFEqual(uuid, IUnknownUUID) ||
                      CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID);
  CFRelease(uuid);
  if (!supported) {
    return E_NOINTERFACE;
  }

  pthread_mutex_lock(&gPlugIn_StateMutex);
  gPlugIn_RefCount++;
  pthread_mutex_unlock(&gPlugIn_StateMutex);
  *outInterface = gDriverRef;
  return S_OK;
}

static ULONG ReacJack_AddRef(void *inDriver)
{
  if (inDriver != gDriverRef) {
    return 0;
  }

  pthread_mutex_lock(&gPlugIn_StateMutex);
  UInt32 count = ++gPlugIn_RefCount;
  pthread_mutex_unlock(&gPlugIn_StateMutex);
  return count;
}

static ULONG ReacJack_Release(void *inDriver)
{
  if (inDriver != gDriverRef) {
    return 0;
  }

  pthread_mutex_lock(&gPlugIn_StateMutex);
  UInt32 count = gPlugIn_RefCount > 0 ? --gPlugIn_RefCount : 0;
  pthread_mutex_unlock(&gPlugIn_StateMutex);
  return count;
}

/* ==== plug-in lifecycle ================================================== */

static OSStatus ReacJack_Initialize(AudioServerPlugInDriverRef inDriver,
                                    AudioServerPlugInHostRef inHost)
{
  if (inDriver != gDriverRef) {
    return kAudioHardwareBadObjectError;
  }

  struct mach_timebase_info timebase;
  mach_timebase_info(&timebase);
  Float64 ticks_per_second =
      1000000000.0 * (Float64)timebase.denom / (Float64)timebase.numer;
  gDevice_HostTicksPerFrame = ticks_per_second / kDevice_SampleRate;
  gPlugIn_Host = inHost;
  return 0;
}

static OSStatus ReacJack_CreateDevice(AudioServerPlugInDriverRef inDriver,
                                      CFDictionaryRef inDescription,
                                      const AudioServerPlugInClientInfo *inClientInfo,
                                      AudioObjectID *outDeviceObjectID)
{
  (void)inDriver;
  (void)inDescription;
  (void)inClientInfo;
  (void)outDeviceObjectID;
  return kAudioHardwareUnsupportedOperationError;
}

static OSStatus ReacJack_DestroyDevice(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inDeviceObjectID)
{
  (void)inDriver;
  (void)inDeviceObjectID;
  return kAudioHardwareUnsupportedOperationError;
}

static OSStatus ReacJack_AddDeviceClient(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID inDeviceObjectID,
                                         const AudioServerPlugInClientInfo *inClientInfo)
{
  (void)inClientInfo;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }
  return 0;
}

static OSStatus ReacJack_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver,
                                            AudioObjectID inDeviceObjectID,
                                            const AudioServerPlugInClientInfo *inClientInfo)
{
  (void)inClientInfo;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }
  return 0;
}

static OSStatus ReacJack_PerformDeviceConfigurationChange(
    AudioServerPlugInDriverRef inDriver,
    AudioObjectID inDeviceObjectID,
    UInt64 inChangeAction,
    void *inChangeInfo)
{
  (void)inChangeAction;
  (void)inChangeInfo;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }
  return 0;
}

static OSStatus ReacJack_AbortDeviceConfigurationChange(
    AudioServerPlugInDriverRef inDriver,
    AudioObjectID inDeviceObjectID,
    UInt64 inChangeAction,
    void *inChangeInfo)
{
  (void)inChangeAction;
  (void)inChangeInfo;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }
  return 0;
}

/* ==== properties ========================================================= */

static Boolean ReacJack_HasProperty(AudioServerPlugInDriverRef inDriver,
                                    AudioObjectID inObjectID,
                                    pid_t inClientProcessID,
                                    const AudioObjectPropertyAddress *inAddress)
{
  (void)inClientProcessID;
  if (inDriver != gDriverRef || inAddress == NULL) {
    return false;
  }

  switch (inObjectID) {
    case kObjectID_PlugIn:
      return plugin_has_property(inAddress);
    case kObjectID_Device:
      return device_has_property(inAddress);
    case kObjectID_Stream_Input:
      return stream_has_property(inAddress);
    default:
      return false;
  }
}

static OSStatus ReacJack_IsPropertySettable(AudioServerPlugInDriverRef inDriver,
                                            AudioObjectID inObjectID,
                                            pid_t inClientProcessID,
                                            const AudioObjectPropertyAddress *inAddress,
                                            Boolean *outIsSettable)
{
  if (inDriver != gDriverRef || inAddress == NULL || outIsSettable == NULL) {
    return kAudioHardwareBadObjectError;
  }
  if (!ReacJack_HasProperty(inDriver, inObjectID, inClientProcessID, inAddress)) {
    return kAudioHardwareUnknownPropertyError;
  }

  *outIsSettable = false;
  if (inObjectID == kObjectID_Device &&
      inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
    *outIsSettable = true;
  }
  if (inObjectID == kObjectID_Stream_Input &&
      (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
       inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)) {
    *outIsSettable = true;
  }
  return 0;
}

static OSStatus plugin_property_size(const AudioObjectPropertyAddress *address,
                                     UInt32 *outSize)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
      *outSize = sizeof(AudioClassID);
      return 0;
    case kAudioObjectPropertyOwner:
      *outSize = sizeof(AudioObjectID);
      return 0;
    case kAudioObjectPropertyManufacturer:
    case kAudioPlugInPropertyResourceBundle:
      *outSize = sizeof(CFStringRef);
      return 0;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
      *outSize = sizeof(AudioObjectID);
      return 0;
    case kAudioPlugInPropertyTranslateUIDToDevice:
      *outSize = sizeof(AudioObjectID);
      return 0;
    default:
      return kAudioHardwareUnknownPropertyError;
  }
}

static OSStatus device_property_size(const AudioObjectPropertyAddress *address,
                                     UInt32 *outSize)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
      *outSize = sizeof(AudioClassID);
      return 0;
    case kAudioObjectPropertyOwner:
      *outSize = sizeof(AudioObjectID);
      return 0;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
      *outSize = sizeof(CFStringRef);
      return 0;
    case kAudioObjectPropertyOwnedObjects:
      *outSize = sizeof(AudioObjectID);
      return 0;
    case kAudioObjectPropertyControlList:
      *outSize = 0;
      return 0;
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyZeroTimeStampPeriod:
      *outSize = sizeof(UInt32);
      return 0;
    case kAudioDevicePropertyRelatedDevices:
      *outSize = sizeof(AudioObjectID);
      return 0;
    case kAudioDevicePropertyStreams:
      *outSize = address->mScope == kAudioObjectPropertyScopeOutput
                     ? 0
                     : sizeof(AudioObjectID);
      return 0;
    case kAudioDevicePropertyNominalSampleRate:
      *outSize = sizeof(Float64);
      return 0;
    case kAudioDevicePropertyAvailableNominalSampleRates:
      *outSize = sizeof(AudioValueRange);
      return 0;
    case kAudioDevicePropertyPreferredChannelsForStereo:
      *outSize = 2 * sizeof(UInt32);
      return 0;
    case kAudioDevicePropertyPreferredChannelLayout:
      *outSize = channel_layout_size();
      return 0;
    default:
      return kAudioHardwareUnknownPropertyError;
  }
}

static OSStatus stream_property_size(const AudioObjectPropertyAddress *address,
                                     UInt32 *outSize)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
      *outSize = sizeof(AudioClassID);
      return 0;
    case kAudioObjectPropertyOwner:
      *outSize = sizeof(AudioObjectID);
      return 0;
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyLatency:
      *outSize = sizeof(UInt32);
      return 0;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
      *outSize = sizeof(AudioStreamBasicDescription);
      return 0;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
      *outSize = sizeof(AudioStreamRangedDescription);
      return 0;
    default:
      return kAudioHardwareUnknownPropertyError;
  }
}

static OSStatus ReacJack_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver,
                                             AudioObjectID inObjectID,
                                             pid_t inClientProcessID,
                                             const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize,
                                             const void *inQualifierData,
                                             UInt32 *outDataSize)
{
  (void)inClientProcessID;
  (void)inQualifierDataSize;
  (void)inQualifierData;
  if (inDriver != gDriverRef || inAddress == NULL || outDataSize == NULL) {
    return kAudioHardwareBadObjectError;
  }

  switch (inObjectID) {
    case kObjectID_PlugIn:
      return plugin_property_size(inAddress, outDataSize);
    case kObjectID_Device:
      return device_property_size(inAddress, outDataSize);
    case kObjectID_Stream_Input:
      return stream_property_size(inAddress, outDataSize);
    default:
      return kAudioHardwareBadObjectError;
  }
}

static OSStatus write_uint32(void *outData, UInt32 inDataSize, UInt32 *outDataSize,
                             UInt32 value)
{
  if (inDataSize < sizeof(UInt32)) {
    return kAudioHardwareBadPropertySizeError;
  }
  *(UInt32 *)outData = value;
  *outDataSize = sizeof(UInt32);
  return 0;
}

static OSStatus write_object_id(void *outData, UInt32 inDataSize, UInt32 *outDataSize,
                                AudioObjectID value)
{
  if (inDataSize < sizeof(AudioObjectID)) {
    return kAudioHardwareBadPropertySizeError;
  }
  *(AudioObjectID *)outData = value;
  *outDataSize = sizeof(AudioObjectID);
  return 0;
}

static OSStatus write_string(void *outData, UInt32 inDataSize, UInt32 *outDataSize,
                             CFStringRef value)
{
  if (inDataSize < sizeof(CFStringRef)) {
    return kAudioHardwareBadPropertySizeError;
  }
  *(CFStringRef *)outData = CFStringCreateCopy(NULL, value);
  *outDataSize = sizeof(CFStringRef);
  return 0;
}

static OSStatus plugin_property_data(const AudioObjectPropertyAddress *address,
                                     UInt32 inQualifierDataSize,
                                     const void *inQualifierData,
                                     UInt32 inDataSize,
                                     UInt32 *outDataSize,
                                     void *outData)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
      return write_uint32(outData, inDataSize, outDataSize, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
      return write_uint32(outData, inDataSize, outDataSize, kAudioPlugInClassID);
    case kAudioObjectPropertyOwner:
      return write_object_id(outData, inDataSize, outDataSize, kAudioObjectUnknown);
    case kAudioObjectPropertyManufacturer:
      return write_string(outData, inDataSize, outDataSize, CFSTR(kDevice_Manufacturer));
    case kAudioPlugInPropertyResourceBundle:
      return write_string(outData, inDataSize, outDataSize, CFSTR(""));
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
      if (inDataSize < sizeof(AudioObjectID)) {
        *outDataSize = 0;
        return 0;
      }
      return write_object_id(outData, inDataSize, outDataSize, kObjectID_Device);
    case kAudioPlugInPropertyTranslateUIDToDevice: {
      if (inQualifierDataSize != sizeof(CFStringRef) || inQualifierData == NULL) {
        return kAudioHardwareBadPropertySizeError;
      }
      CFStringRef uid = *(CFStringRef *)inQualifierData;
      AudioObjectID found = kAudioObjectUnknown;
      if (uid != NULL &&
          CFStringCompare(uid, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo) {
        found = kObjectID_Device;
      }
      return write_object_id(outData, inDataSize, outDataSize, found);
    }
    default:
      return kAudioHardwareUnknownPropertyError;
  }
}

static OSStatus device_property_data(const AudioObjectPropertyAddress *address,
                                     UInt32 inDataSize,
                                     UInt32 *outDataSize,
                                     void *outData)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
      return write_uint32(outData, inDataSize, outDataSize, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
      return write_uint32(outData, inDataSize, outDataSize, kAudioDeviceClassID);
    case kAudioObjectPropertyOwner:
      return write_object_id(outData, inDataSize, outDataSize, kObjectID_PlugIn);
    case kAudioObjectPropertyName:
      return write_string(outData, inDataSize, outDataSize, CFSTR(kDevice_Name));
    case kAudioObjectPropertyManufacturer:
      return write_string(outData, inDataSize, outDataSize, CFSTR(kDevice_Manufacturer));
    case kAudioDevicePropertyDeviceUID:
      return write_string(outData, inDataSize, outDataSize, CFSTR(kDevice_UID));
    case kAudioDevicePropertyModelUID:
      return write_string(outData, inDataSize, outDataSize, CFSTR(kDevice_ModelUID));
    case kAudioObjectPropertyOwnedObjects:
      return write_object_id(outData, inDataSize, outDataSize, kObjectID_Stream_Input);
    case kAudioObjectPropertyControlList:
      *outDataSize = 0;
      return 0;
    case kAudioDevicePropertyTransportType:
      return write_uint32(outData, inDataSize, outDataSize,
                          kAudioDeviceTransportTypeVirtual);
    case kAudioDevicePropertyRelatedDevices:
      return write_object_id(outData, inDataSize, outDataSize, kObjectID_Device);
    case kAudioDevicePropertyClockDomain:
      return write_uint32(outData, inDataSize, outDataSize, 0);
    case kAudioDevicePropertyDeviceIsAlive:
      return write_uint32(outData, inDataSize, outDataSize, 1);
    case kAudioDevicePropertyDeviceIsRunning: {
      pthread_mutex_lock(&gDevice_IOMutex);
      UInt32 running = gDevice_IOIsRunning > 0 ? 1 : 0;
      pthread_mutex_unlock(&gDevice_IOMutex);
      return write_uint32(outData, inDataSize, outDataSize, running);
    }
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
      return write_uint32(outData, inDataSize, outDataSize, 1);
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
      return write_uint32(outData, inDataSize, outDataSize, 0);
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyIsHidden:
      return write_uint32(outData, inDataSize, outDataSize, 0);
    case kAudioDevicePropertyZeroTimeStampPeriod:
      return write_uint32(outData, inDataSize, outDataSize, kDevice_RingFrames);
    case kAudioDevicePropertyStreams:
      if (address->mScope == kAudioObjectPropertyScopeOutput ||
          inDataSize < sizeof(AudioObjectID)) {
        *outDataSize = 0;
        return 0;
      }
      return write_object_id(outData, inDataSize, outDataSize, kObjectID_Stream_Input);
    case kAudioDevicePropertyNominalSampleRate:
      if (inDataSize < sizeof(Float64)) {
        return kAudioHardwareBadPropertySizeError;
      }
      *(Float64 *)outData = kDevice_SampleRate;
      *outDataSize = sizeof(Float64);
      return 0;
    case kAudioDevicePropertyAvailableNominalSampleRates: {
      if (inDataSize < sizeof(AudioValueRange)) {
        *outDataSize = 0;
        return 0;
      }
      AudioValueRange *range = (AudioValueRange *)outData;
      range->mMinimum = kDevice_SampleRate;
      range->mMaximum = kDevice_SampleRate;
      *outDataSize = sizeof(AudioValueRange);
      return 0;
    }
    case kAudioDevicePropertyPreferredChannelsForStereo: {
      if (inDataSize < 2 * sizeof(UInt32)) {
        return kAudioHardwareBadPropertySizeError;
      }
      UInt32 *channels = (UInt32 *)outData;
      channels[0] = 1;
      channels[1] = 2;
      *outDataSize = 2 * sizeof(UInt32);
      return 0;
    }
    case kAudioDevicePropertyPreferredChannelLayout: {
      if (inDataSize < channel_layout_size()) {
        return kAudioHardwareBadPropertySizeError;
      }
      AudioChannelLayout *layout = (AudioChannelLayout *)outData;
      layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
      layout->mChannelBitmap = 0;
      layout->mNumberChannelDescriptions = kDevice_ChannelCount;
      for (UInt32 i = 0; i < kDevice_ChannelCount; i++) {
        AudioChannelDescription *description = &layout->mChannelDescriptions[i];
        description->mChannelLabel = kAudioChannelLabel_Discrete_0 + i;
        description->mChannelFlags = 0;
        description->mCoordinates[0] = 0;
        description->mCoordinates[1] = 0;
        description->mCoordinates[2] = 0;
      }
      *outDataSize = channel_layout_size();
      return 0;
    }
    default:
      return kAudioHardwareUnknownPropertyError;
  }
}

static OSStatus stream_property_data(const AudioObjectPropertyAddress *address,
                                     UInt32 inDataSize,
                                     UInt32 *outDataSize,
                                     void *outData)
{
  switch (address->mSelector) {
    case kAudioObjectPropertyBaseClass:
      return write_uint32(outData, inDataSize, outDataSize, kAudioObjectClassID);
    case kAudioObjectPropertyClass:
      return write_uint32(outData, inDataSize, outDataSize, kAudioStreamClassID);
    case kAudioObjectPropertyOwner:
      return write_object_id(outData, inDataSize, outDataSize, kObjectID_Device);
    case kAudioStreamPropertyIsActive:
      return write_uint32(outData, inDataSize, outDataSize, 1);
    case kAudioStreamPropertyDirection:
      return write_uint32(outData, inDataSize, outDataSize, 1); /* input */
    case kAudioStreamPropertyTerminalType:
      return write_uint32(outData, inDataSize, outDataSize,
                          kAudioStreamTerminalTypeLine);
    case kAudioStreamPropertyStartingChannel:
      return write_uint32(outData, inDataSize, outDataSize, 1);
    case kAudioStreamPropertyLatency:
      return write_uint32(outData, inDataSize, outDataSize, 0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
      if (inDataSize < sizeof(AudioStreamBasicDescription)) {
        return kAudioHardwareBadPropertySizeError;
      }
      *(AudioStreamBasicDescription *)outData = device_stream_format();
      *outDataSize = sizeof(AudioStreamBasicDescription);
      return 0;
    }
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
      if (inDataSize < sizeof(AudioStreamRangedDescription)) {
        *outDataSize = 0;
        return 0;
      }
      AudioStreamRangedDescription *description =
          (AudioStreamRangedDescription *)outData;
      description->mFormat = device_stream_format();
      description->mSampleRateRange.mMinimum = kDevice_SampleRate;
      description->mSampleRateRange.mMaximum = kDevice_SampleRate;
      *outDataSize = sizeof(AudioStreamRangedDescription);
      return 0;
    }
    default:
      return kAudioHardwareUnknownPropertyError;
  }
}

static OSStatus ReacJack_GetPropertyData(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID inObjectID,
                                         pid_t inClientProcessID,
                                         const AudioObjectPropertyAddress *inAddress,
                                         UInt32 inQualifierDataSize,
                                         const void *inQualifierData,
                                         UInt32 inDataSize,
                                         UInt32 *outDataSize,
                                         void *outData)
{
  (void)inClientProcessID;
  if (inDriver != gDriverRef || inAddress == NULL || outDataSize == NULL ||
      outData == NULL) {
    return kAudioHardwareBadObjectError;
  }

  switch (inObjectID) {
    case kObjectID_PlugIn:
      return plugin_property_data(inAddress, inQualifierDataSize, inQualifierData,
                                  inDataSize, outDataSize, outData);
    case kObjectID_Device:
      return device_property_data(inAddress, inDataSize, outDataSize, outData);
    case kObjectID_Stream_Input:
      return stream_property_data(inAddress, inDataSize, outDataSize, outData);
    default:
      return kAudioHardwareBadObjectError;
  }
}

static OSStatus ReacJack_SetPropertyData(AudioServerPlugInDriverRef inDriver,
                                         AudioObjectID inObjectID,
                                         pid_t inClientProcessID,
                                         const AudioObjectPropertyAddress *inAddress,
                                         UInt32 inQualifierDataSize,
                                         const void *inQualifierData,
                                         UInt32 inDataSize,
                                         const void *inData)
{
  (void)inClientProcessID;
  (void)inQualifierDataSize;
  (void)inQualifierData;
  if (inDriver != gDriverRef || inAddress == NULL || inData == NULL) {
    return kAudioHardwareBadObjectError;
  }

  if (inObjectID == kObjectID_Device &&
      inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
    if (inDataSize != sizeof(Float64)) {
      return kAudioHardwareBadPropertySizeError;
    }
    /* 48 kHz is the only rate; setting it again is a no-op. */
    return *(const Float64 *)inData == kDevice_SampleRate
               ? 0
               : kAudioDeviceUnsupportedFormatError;
  }

  if (inObjectID == kObjectID_Stream_Input &&
      (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
       inAddress->mSelector == kAudioStreamPropertyPhysicalFormat)) {
    if (inDataSize != sizeof(AudioStreamBasicDescription)) {
      return kAudioHardwareBadPropertySizeError;
    }
    AudioStreamBasicDescription requested =
        *(const AudioStreamBasicDescription *)inData;
    AudioStreamBasicDescription supported = device_stream_format();
    return formats_equal(&requested, &supported) ? 0
                                                 : kAudioDeviceUnsupportedFormatError;
  }

  return kAudioHardwareUnknownPropertyError;
}

/* ==== IO ================================================================= */

static OSStatus ReacJack_StartIO(AudioServerPlugInDriverRef inDriver,
                                 AudioObjectID inDeviceObjectID,
                                 UInt32 inClientID)
{
  (void)inClientID;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }

  pthread_mutex_lock(&gDevice_IOMutex);
  if (gDevice_IOIsRunning == UINT64_MAX) {
    pthread_mutex_unlock(&gDevice_IOMutex);
    return kAudioHardwareIllegalOperationError;
  }
  if (gDevice_IOIsRunning == 0) {
    gDevice_AnchorHostTime = mach_absolute_time();
    gDevice_RingIsOpen = shared_audio_open(&gDevice_Ring, kDevice_RingName) == 0;
  }
  gDevice_IOIsRunning++;
  pthread_mutex_unlock(&gDevice_IOMutex);
  return 0;
}

static OSStatus ReacJack_StopIO(AudioServerPlugInDriverRef inDriver,
                                AudioObjectID inDeviceObjectID,
                                UInt32 inClientID)
{
  (void)inClientID;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }

  pthread_mutex_lock(&gDevice_IOMutex);
  OSStatus result = 0;
  if (gDevice_IOIsRunning == 0) {
    result = kAudioHardwareIllegalOperationError;
  } else {
    gDevice_IOIsRunning--;
    if (gDevice_IOIsRunning == 0 && gDevice_RingIsOpen) {
      shared_audio_close(&gDevice_Ring);
      gDevice_RingIsOpen = false;
    }
  }
  pthread_mutex_unlock(&gDevice_IOMutex);
  return result;
}

static OSStatus ReacJack_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver,
                                          AudioObjectID inDeviceObjectID,
                                          UInt32 inClientID,
                                          Float64 *outSampleTime,
                                          UInt64 *outHostTime,
                                          UInt64 *outSeed)
{
  (void)inClientID;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device ||
      outSampleTime == NULL || outHostTime == NULL || outSeed == NULL) {
    return kAudioHardwareBadObjectError;
  }

  pthread_mutex_lock(&gDevice_IOMutex);
  UInt64 current_host_time = mach_absolute_time();
  Float64 ticks_per_period = gDevice_HostTicksPerFrame * (Float64)kDevice_RingFrames;
  UInt64 elapsed_ticks = current_host_time - gDevice_AnchorHostTime;
  UInt64 periods = (UInt64)((Float64)elapsed_ticks / ticks_per_period);
  *outSampleTime = (Float64)(periods * kDevice_RingFrames);
  *outHostTime = gDevice_AnchorHostTime + (UInt64)((Float64)periods * ticks_per_period);
  *outSeed = 1;
  pthread_mutex_unlock(&gDevice_IOMutex);
  return 0;
}

static OSStatus ReacJack_WillDoIOOperation(AudioServerPlugInDriverRef inDriver,
                                           AudioObjectID inDeviceObjectID,
                                           UInt32 inClientID,
                                           UInt32 inOperationID,
                                           Boolean *outWillDo,
                                           Boolean *outWillDoInPlace)
{
  (void)inClientID;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device ||
      outWillDo == NULL || outWillDoInPlace == NULL) {
    return kAudioHardwareBadObjectError;
  }

  *outWillDo = inOperationID == kAudioServerPlugInIOOperationReadInput;
  *outWillDoInPlace = true;
  return 0;
}

static OSStatus ReacJack_BeginIOOperation(AudioServerPlugInDriverRef inDriver,
                                          AudioObjectID inDeviceObjectID,
                                          UInt32 inClientID,
                                          UInt32 inOperationID,
                                          UInt32 inIOBufferFrameSize,
                                          const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
  (void)inClientID;
  (void)inOperationID;
  (void)inIOBufferFrameSize;
  (void)inIOCycleInfo;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }
  return 0;
}

static OSStatus ReacJack_DoIOOperation(AudioServerPlugInDriverRef inDriver,
                                       AudioObjectID inDeviceObjectID,
                                       AudioObjectID inStreamObjectID,
                                       UInt32 inClientID,
                                       UInt32 inOperationID,
                                       UInt32 inIOBufferFrameSize,
                                       const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                       void *ioMainBuffer,
                                       void *ioSecondaryBuffer)
{
  (void)inStreamObjectID;
  (void)inClientID;
  (void)inIOCycleInfo;
  (void)ioSecondaryBuffer;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }

  if (inOperationID == kAudioServerPlugInIOOperationReadInput &&
      ioMainBuffer != NULL) {
    if (gDevice_RingIsOpen) {
      /* Interleaves the ring's channels into the device buffer; missing
       * frames and channels beyond the ring's become silence. */
      shared_audio_read_interleaved(&gDevice_Ring, (float *)ioMainBuffer,
                                    kDevice_ChannelCount, inIOBufferFrameSize);
    } else {
      memset(ioMainBuffer, 0, (size_t)inIOBufferFrameSize * kDevice_BytesPerFrame);
    }
  }

  return 0;
}

static OSStatus ReacJack_EndIOOperation(AudioServerPlugInDriverRef inDriver,
                                        AudioObjectID inDeviceObjectID,
                                        UInt32 inClientID,
                                        UInt32 inOperationID,
                                        UInt32 inIOBufferFrameSize,
                                        const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
  (void)inClientID;
  (void)inOperationID;
  (void)inIOBufferFrameSize;
  (void)inIOCycleInfo;
  if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device) {
    return kAudioHardwareBadObjectError;
  }
  return 0;
}

/* ==== factory ============================================================ */

static AudioServerPlugInDriverInterface gDriverInterface = {
    NULL,
    ReacJack_QueryInterface,
    ReacJack_AddRef,
    ReacJack_Release,
    ReacJack_Initialize,
    ReacJack_CreateDevice,
    ReacJack_DestroyDevice,
    ReacJack_AddDeviceClient,
    ReacJack_RemoveDeviceClient,
    ReacJack_PerformDeviceConfigurationChange,
    ReacJack_AbortDeviceConfigurationChange,
    ReacJack_HasProperty,
    ReacJack_IsPropertySettable,
    ReacJack_GetPropertyDataSize,
    ReacJack_GetPropertyData,
    ReacJack_SetPropertyData,
    ReacJack_StartIO,
    ReacJack_StopIO,
    ReacJack_GetZeroTimeStamp,
    ReacJack_WillDoIOOperation,
    ReacJack_BeginIOOperation,
    ReacJack_DoIOOperation,
    ReacJack_EndIOOperation};

void *ReacJack_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

void *ReacJack_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
  (void)inAllocator;
  if (inRequestedTypeUUID == NULL ||
      !CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
    return NULL;
  }
  return gDriverRef;
}
