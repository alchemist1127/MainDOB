#ifndef MAINDOB_DOB_TYPES_H
#define MAINDOB_DOB_TYPES_H

#include <sys/types.h>

/* Status codes */
typedef enum
{
    DOB_OK          = 0,
    DOB_ERR_NOT_FOUND   = -1,
    DOB_ERR_DENIED      = -3,
    DOB_ERR_INVALID     = -4,
    DOB_ERR_NO_MEMORY   = -5,
    DOB_ERR_DEAD        = -6,
    DOB_ERR_INTERNAL    = -7,
    DOB_ERR_NO_SPACE    = -8,
    /* Fine-grained errors used by the floppy driver and reusable by
     * any future removable-media driver. */
    DOB_ERR_NO_MEDIA       = -9,   /* drive present but empty         */
    DOB_ERR_HW_FAULT       = -10,  /* controller/drive non-responsive */
    DOB_ERR_BAD_FS         = -11,  /* media present, unreadable FS    */
    DOB_ERR_MEDIA_CHANGED  = -12,  /* media swapped mid-session       */
    /* Operation rejected by the filesystem driver for a domain-
     * specific reason that the driver can describe in words. The
     * reply payload, if non-empty, is a NUL-terminated UTF-8 string
     * carrying that reason (driver-supplied, already in the user's
     * language — DobFiles surfaces it verbatim in a modal popup).
     *
     * Use cases: floppy full ("Disco pieno"), CD read-only ("CD di
     * sola lettura"), per-file ACL violation, future quota systems.
     * Distinct from DOB_ERR_NO_SPACE / DOB_ERR_DENIED because those
     * are bare codes without a textual explanation — keep them for
     * machine-readable contexts where the caller wants to branch on
     * the precise reason rather than display a string. */
    DOB_ERR_REJECTED       = -13
} dob_status_t;

#endif /* MAINDOB_DOB_TYPES_H */
