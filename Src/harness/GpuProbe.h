#pragma once
// GpuProbe — GPU path-1 feasibility probe. On a given SwapChainPanel it uses ANGLE (EGL/GL ES 2.0 over
// D3D11 FL9_3) to initialize and draw an animated triangle, verifying the GPU pipeline works in the
// Win10M ARM32 App Container (the GPU counterpart of JitProbe's executable-memory check).
// Runs on a background thread (EGL stays on one thread throughout); each step's result plus
// GL_VENDOR/RENDERER/VERSION and eglGetError are written to LocalState\gpuprobe.txt.
namespace Harness {
    void RunGpuProbe(Windows::UI::Xaml::Controls::SwapChainPanel^ panel, Platform::String^ localStateDir);
}
