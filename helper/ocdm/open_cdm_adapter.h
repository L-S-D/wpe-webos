/* SPDX-License-Identifier: Apache-2.0 */
/*
 * OpenCDM GStreamer adapter (as in Thunder's open_cdm_adapter.h), C version.
 *
 * In TVF's implementation "decrypting" only tags the buffer: the sample stays
 * encrypted and gets a GstProtectionMeta "application/x-tvf-cenc" (a copy of
 * the demuxer's meta plus the session id); the TVF sinks hand it to TVF,
 * whose CDM turns it into the TV's secure-video-path input.
 */
#ifndef TVF_OPEN_CDM_ADAPTER_H
#define TVF_OPEN_CDM_ADAPTER_H

#include "open_cdm.h"

struct _GstBuffer;
typedef struct _GstBuffer GstBuffer;
struct _GstCaps;
typedef struct _GstCaps GstCaps;

#ifdef __cplusplus
extern "C" {
#endif

/* reads the GstProtectionMeta of buffer (WebKit with THUNDER_HAS_OCDM_DECRYPT_BUFFER) */
EXTERNAL OpenCDMError opencdm_gstreamer_session_decrypt_buffer(struct OpenCDMSession *session, GstBuffer *buffer,
                                                               GstCaps *caps);
/* older form with the parts passed separately */
EXTERNAL OpenCDMError opencdm_gstreamer_session_decrypt(struct OpenCDMSession *session, GstBuffer *buffer,
                                                        GstBuffer *subSample, const uint32_t subSampleCount,
                                                        GstBuffer *IV, GstBuffer *keyID, uint32_t initWithLast15);

#ifdef __cplusplus
}
#endif

#endif /* TVF_OPEN_CDM_ADAPTER_H */
