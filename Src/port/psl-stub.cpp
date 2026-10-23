// psl-stub.cpp - minimal libpsl surface for the x64 driver link.
//
// The UWP curl build is configured without libpsl, but one of the WebCore
// objects references these four names. The original driver linked a hand-made
// psl.lib whose source was not kept; this file restores that small stub so a
// fresh machine can build the same link. The functions deliberately return
// "no public suffix information" rather than pretending to be a real PSL.
//
// Build with: pwsh -File Src\port\build-psl-stub.ps1 -Arch x64
// The archive is a build artifact and is ignored by git.

extern "C" {
struct psl_ctx_t;

const psl_ctx_t* psl_builtin(void)
{
    return nullptr;
}

void psl_free(psl_ctx_t*)
{
}

int psl_is_public_suffix2(const psl_ctx_t*, const char*, int)
{
    return 0;
}

const char* psl_registrable_domain(const psl_ctx_t*, const char*)
{
    return nullptr;
}
}
