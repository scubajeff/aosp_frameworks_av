LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)


LOCAL_SRC_FILES:=                         \
        ACodec.cpp                        \
        AACExtractor.cpp                  \
        AACWriter.cpp                     \
        AMRExtractor.cpp                  \
        AMRWriter.cpp                     \
        AudioPlayer.cpp                   \
        AudioSource.cpp                   \
        CallbackDataSource.cpp            \
        CameraSource.cpp                  \
        CameraSourceTimeLapse.cpp         \
        CodecBase.cpp                     \
        DataConverter.cpp                 \
        DataSource.cpp                    \
        DataURISource.cpp                 \
        DRMExtractor.cpp                  \
        ESDS.cpp                          \
        FileSource.cpp                    \
        FLACExtractor.cpp                 \
        FrameRenderTracker.cpp            \
        HTTPBase.cpp                      \
        HevcUtils.cpp                     \
        JPEGSource.cpp                    \
        MP3Extractor.cpp                  \
        MPEG2TSWriter.cpp                 \
        MPEG4Extractor.cpp                \
        MPEG4Writer.cpp                   \
        MediaAdapter.cpp                  \
        MediaClock.cpp                    \
        MediaCodec.cpp                    \
        MediaCodecList.cpp                \
        MediaCodecListOverrides.cpp       \
        MediaCodecSource.cpp              \
        MediaDefs.cpp                     \
        MediaExtractor.cpp                \
        MediaSync.cpp                     \
        MidiExtractor.cpp                 \
        http/MediaHTTP.cpp                \
        MediaMuxer.cpp                    \
        MediaSource.cpp                   \
        NuCachedSource2.cpp               \
        NuMediaExtractor.cpp              \
        OMXClient.cpp                     \
        OggExtractor.cpp                  \
        ProcessInfo.cpp                   \
        SampleIterator.cpp                \
        SampleTable.cpp                   \
        SimpleDecodingSource.cpp          \
        SkipCutBuffer.cpp                 \
        StagefrightMediaScanner.cpp       \
        StagefrightMetadataRetriever.cpp  \
        SurfaceMediaSource.cpp            \
        SurfaceUtils.cpp                  \
        ThrottledSource.cpp               \
        Utils.cpp                         \
        VBRISeeker.cpp                    \
        VideoFrameScheduler.cpp           \
        WAVExtractor.cpp                  \
        WVMExtractor.cpp                  \
        XINGSeeker.cpp                    \
        avc_utils.cpp                     \

LOCAL_C_INCLUDES:= \
        $(TOP)/frameworks/av/include/media/ \
        $(TOP)/frameworks/av/include/media/stagefright/timedtext \
        $(TOP)/frameworks/native/include/media/hardware \
        $(TOP)/frameworks/native/include/media/openmax \
        $(TOP)/external/flac/include \
        $(TOP)/external/tremolo \
        $(TOP)/external/libvpx/libwebm \
        $(TOP)/system/netd/include \
        $(call include-path-for, audio-utils)

LOCAL_SHARED_LIBRARIES := \
        libaudioutils \
        libbinder \
        libcamera_client \
        libcutils \
        libdl \
        libdrmframework \
        libexpat \
        libgui \
        libicui18n \
        libicuuc \
        liblog \
        libmedia \
        libmediautils \
        libnetd_client \
        libopus \
        libsonivox \
        libssl \
        libstagefright_omx \
        libstagefright_yuv \
        libsync \
        libui \
        libutils \
        libvorbisidec \
        libz \
        libpowermanager

ifeq ($(BOARD_CANT_REALLOCATE_OMX_BUFFERS),true)
LOCAL_CFLAGS += -DBOARD_CANT_REALLOCATE_OMX_BUFFERS
endif

LOCAL_STATIC_LIBRARIES := \
        libstagefright_color_conversion \
        libyuv_static \
        libstagefright_aacenc \
        libstagefright_matroska \
        libstagefright_mediafilter \
        libstagefright_webm \
        libstagefright_timedtext \
        libvpx \
        libwebm \
        libstagefright_mpeg2ts \
        libstagefright_id3 \
        libFLAC \
        libmedia_helper \

#+++>
ifeq ($(BOARD_USE_S3D_SUPPORT), true)
ifeq ($(BOARD_USES_HWC_SERVICES), true)
LOCAL_CFLAGS += -DUSE_S3D_SUPPORT -DHWC_SERVICES
LOCAL_C_INCLUDES += \
        $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr/include \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/openmax/include/exynos \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/$(TARGET_BOARD_PLATFORM)/libhwcService \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/$(TARGET_BOARD_PLATFORM)/libhwc \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/$(TARGET_BOARD_PLATFORM)/include \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/$(TARGET_SOC)/libhwcmodule \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/$(TARGET_SOC)/include \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/exynos/libexynosutils \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/exynos/include \
        $(TOP)/hardware/samsung_slsi-$(TARGET_SLSI_VARIANT)/exynos/libhwc

LOCAL_ADDITIONAL_DEPENDENCIES := \
        $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr

LOCAL_SHARED_LIBRARIES += \
        libExynosHWCService
endif
endif
#--->

LOCAL_SHARED_LIBRARIES += \
        libstagefright_enc_common \
        libstagefright_avc_common \
        libstagefright_foundation \
        libdl \
        libRScpp \

ifeq ($(BOARD_USE_SAMSUNG_CAMERAFORMAT_NV21), true)
# This needs flag requires the following string constant in
# CameraParametersExtra.h:
#
# const char CameraParameters::PIXEL_FORMAT_YUV420SP_NV21[] = "nv21";
LOCAL_CFLAGS += -DUSE_SAMSUNG_CAMERAFORMAT_NV21
endif

ifneq ($(TARGET_USES_MEDIA_EXTENSIONS),true)
ifeq ($(TARGET_HAS_LEGACY_CAMERA_HAL1),true)
LOCAL_CFLAGS += -DCAMCORDER_GRALLOC_SOURCE
endif
endif

ifeq ($(TARGET_OMX_LEGACY_RESCALING),true)
LOCAL_CFLAGS += -DUSE_LEGACY_RESCALING
endif

LOCAL_CFLAGS += -Wno-multichar -Werror -Wno-error=deprecated-declarations -Wall

# enable experiments only in userdebug and eng builds
ifneq (,$(filter userdebug eng,$(TARGET_BUILD_VARIANT)))
LOCAL_CFLAGS += -DENABLE_STAGEFRIGHT_EXPERIMENTS
endif

LOCAL_CLANG := true
LOCAL_SANITIZE := unsigned-integer-overflow signed-integer-overflow

LOCAL_MODULE:= libstagefright

LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(call all-makefiles-under,$(LOCAL_PATH))
