/* SPDX-License-Identifier: Apache-2.0 */
/*
 * OpenCDM client API (the part WPE WebKit's Thunder CDM uses), C version.
 *
 * Names, values and signatures follow Thunder's OpenCDM
 * (rdkcentral/ThunderClientLibraries, Source/ocdm/open_cdm.h) so that
 * WebKit's CDMThunder/CDMProxyThunder build against it unchanged
 * (ENABLE_THUNDER). The implementation is TVF's (../tvf-ocdm.c): it forwards
 * everything to TVF, which hosts the TV's CDMs - the Rialto split, like
 * rialto-ocdm (FindThunder accepts ocdm, ocdmRialto, ClientOCDM).
 */
#ifndef TVF_OPEN_CDM_H
#define TVF_OPEN_CDM_H

#include <stdint.h>

#ifndef EXTERNAL
#define EXTERNAL __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct OpenCDMSystem;
struct OpenCDMSession;

typedef enum {
    Temporary = 0,
    PersistentUsageRecord,
    PersistentLicense
} LicenseType;

typedef enum {
    Usable = 0,
    Expired,
    Released,
    OutputRestricted,
    OutputRestrictedHDCP22,
    OutputDownscaled,
    StatusPending,
    InternalError,
    HWError
} KeyStatus;

typedef enum {
    ERROR_NONE = 0,
    ERROR_UNKNOWN = 1,
    ERROR_MORE_DATA_AVAILABLE = 2,
    ERROR_INTERFACE_NOT_IMPLEMENTED = 3,
    ERROR_BUFFER_TOO_SMALL = 4,
    ERROR_INVALID_ACCESSOR = 0x80000001,
    ERROR_KEYSYSTEM_NOT_SUPPORTED = 0x80000002,
    ERROR_INVALID_SESSION = 0x80000003,
    ERROR_INVALID_DECRYPT_BUFFER = 0x80000004,
    ERROR_OUT_OF_MEMORY = 0x80000005,
    ERROR_METHOD_NOT_IMPLEMENTED = 0x80000006,
    ERROR_FAIL = 0x80004005,
    ERROR_INVALID_ARG = 0x80070057,
    ERROR_SERVER_INTERNAL_ERROR = 0x8004C600,
    ERROR_SERVER_INVALID_MESSAGE = 0x8004C601,
    ERROR_SERVER_SERVICE_SPECIFIC = 0x8004C604,
    ERROR_BUSY_CANNOT_INITIALIZE = 0x8004DD00
} OpenCDMError;

typedef enum {
    OPENCDM_BOOL_FALSE = 0,
    OPENCDM_BOOL_TRUE = 1
} OpenCDMBool;

typedef struct {
    /* license request (or renewal...) to send to the server; the challenge
     * starts with "<message type digit>:Type:" (WebKit's convention) */
    void (*process_challenge_callback)(struct OpenCDMSession *session, void *userData, const char url[],
                                       const uint8_t challenge[], const uint16_t challengeLength);
    /* a key changed its status (opencdm_session_status has the new one) */
    void (*key_update_callback)(struct OpenCDMSession *session, void *userData, const uint8_t keyId[],
                                const uint8_t length);
    void (*error_message_callback)(struct OpenCDMSession *session, void *userData, const char message[]);
    /* after all key changes of one update were reported */
    void (*keys_updated_callback)(const struct OpenCDMSession *session, void *userData);
} OpenCDMSessionCallbacks;

EXTERNAL struct OpenCDMSystem *opencdm_create_system(const char keySystem[]);
EXTERNAL OpenCDMError opencdm_destruct_system(struct OpenCDMSystem *system);
/* zero if supported; mimeType may be empty */
EXTERNAL OpenCDMError opencdm_is_type_supported(const char keySystem[], const char mimeType[]);
/* the session holding keyId (new reference, waits up to waitTime ms), NULL if none */
EXTERNAL struct OpenCDMSession *opencdm_get_system_session(struct OpenCDMSystem *system, const uint8_t keyId[],
                                                           const uint8_t length, const uint32_t waitTime);
EXTERNAL OpenCDMBool opencdm_system_supports_server_certificate(struct OpenCDMSystem *system);
EXTERNAL OpenCDMError opencdm_system_set_server_certificate(struct OpenCDMSystem *system,
                                                            const uint8_t serverCertificate[],
                                                            const uint16_t serverCertificateLength);
/* CDMData/CDMDataLength (no init data): the id of a persistent session to load */
EXTERNAL OpenCDMError opencdm_construct_session(struct OpenCDMSystem *system, const LicenseType licenseType,
                                                const char initDataType[], const uint8_t initData[],
                                                const uint16_t initDataLength, const uint8_t CDMData[],
                                                const uint16_t CDMDataLength, OpenCDMSessionCallbacks *callbacks,
                                                void *userData, struct OpenCDMSession **session);
EXTERNAL OpenCDMError opencdm_destruct_session(struct OpenCDMSession *session);
EXTERNAL OpenCDMError opencdm_session_load(struct OpenCDMSession *session);
EXTERNAL OpenCDMError opencdm_session_update(struct OpenCDMSession *session, const uint8_t keyMessage[],
                                             const uint16_t keyLength);
EXTERNAL OpenCDMError opencdm_session_remove(struct OpenCDMSession *session);
EXTERNAL const char *opencdm_session_id(const struct OpenCDMSession *session);
EXTERNAL uint32_t opencdm_session_has_key_id(struct OpenCDMSession *session, const uint8_t length,
                                             const uint8_t keyId[]);
EXTERNAL KeyStatus opencdm_session_status(const struct OpenCDMSession *session, const uint8_t keyId[],
                                          const uint8_t length);
EXTERNAL OpenCDMError opencdm_session_close(struct OpenCDMSession *session);

#ifdef __cplusplus
}
#endif

#endif /* TVF_OPEN_CDM_H */
