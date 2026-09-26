// stubs-loader.cpp — CurlSSLHandle::platformInitialize stub for UWP (Win version excluded from build).
#include "config.h"

#include "CurlSSLHandle.h"

namespace WebCore {

void CurlSSLHandle::platformInitialize()
{
    // Provide non-null default strings to avoid null StringImpl* dereference
    // during Curl SSL setup in AppContainer. The desktop (Win/Linux) platform
    // init sets these; UWP was a no-op, causing crashes when Curl tries to
    // read null String fields.
    setCipherList(CString("DEFAULT:!DH:!aNULL:!eNULL:!LOW:!MEDIUM:!EXP:!RC4:!MD5:!DES"));
    setECCurves(CString("X25519:P-256:P-384:P-521"));
}

} // namespace WebCore
