#pragma once

/* Return codes for error paths - set based on DEBUG at compile time */
#ifdef DEBUG
#define RC_MISSING_CONFIG -1
#define RC_UNKNOWN_OPT -2
#define RC_NOT_REMOTE_CTX -3
#define RC_GETENV_FAIL -4
#define RC_WRONG_PARTITION -5
#define RC_NO_NAMESPACE -6
#define RC_NAMESPACE_OPEN_FAIL -7
#define RC_NAMESPACE_NOT_ROOT -8
#define RC_SETNS_FAIL -9
#else
#define RC_MISSING_CONFIG -1      /* Always fail on config error */
#define RC_UNKNOWN_OPT -1         /* Always fail on unknown option */
#define RC_NOT_REMOTE_CTX 0       /* Graceful skip */
#define RC_GETENV_FAIL 0          /* Graceful skip */
#define RC_WRONG_PARTITION 0      /* Graceful skip */
#define RC_NO_NAMESPACE 0         /* Graceful skip */
#define RC_NAMESPACE_OPEN_FAIL 0  /* Graceful skip */
#define RC_NAMESPACE_NOT_ROOT 0   /* Graceful skip */
#define RC_SETNS_FAIL 0           /* Graceful skip */
#endif
