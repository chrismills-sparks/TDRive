// backend_d3d11.cpp
//
// Windows Rive renderer backend. Mirrors backend_metal.mm: owns a D3D11
// device + context, an offscreen BGRA8Unorm/RGBA8Unorm render target,
// and a staging texture used for CPU readback.
//
// In CUDA execute mode (NVIDIA GPUs) the render target is additionally
// registered with the CUDA runtime so the finished frame can be copied
// GPU->GPU into the cudaArray TouchDesigner hands us - no CPU round-trip.
// Injected textures (Image1..N params) follow the same pattern in reverse.

#include "IBackend.h"

#include <chrono>

#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>
#include <vector>

#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
// texture.hpp must be included before render_context_d3d_impl.hpp - the D3D
// header instantiates rcp<rive::gpu::Texture> through RenderContextImpl, and
// MSVC needs the full type for that. Metal's path includes it transitively;
// MSVC's path doesn't.
#include "rive/renderer/texture.hpp"
#include "rive/renderer/d3d11/render_context_d3d_impl.hpp"
#include "rive/renderer/rive_render_image.hpp"

#include "cuda_interop_win.h"

using Microsoft::WRL::ComPtr;

namespace tdrive {

class D3D11Backend : public IBackend {
public:
    explicit D3D11Backend(bool cudaMode) : mCUDAMode(cudaMode)
    {
        // BGRA8 matches TouchDesigner's BGRA8Fixed CPU upload without a
        // swizzle. In CUDA mode we use RGBA8 to match the RGBA8Fixed output
        // array we request, and because it is unconditionally UAV-compatible.
        mTargetFormat = cudaMode ? DXGI_FORMAT_R8G8B8A8_UNORM
                                 : DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    ~D3D11Backend() override
    {
        releaseSurface(mMain);
        releaseSurface(mMask);
        for (auto& s : mSlots) releaseSlot(s);
        if (mStream) {
            if (const auto* api = cuda::Get()) api->streamDestroy(mStream);
            mStream = nullptr;
        }
        if (mRenderContext) {
            mRenderContext->releaseResources();
            mRenderContext.reset();
        }
        mContext.Reset();
        mDevice.Reset();
    }

    bool init(std::string& err) override
    {
        ComPtr<IDXGIFactory2> factory;
        HRESULT hr = CreateDXGIFactory(
            __uuidof(IDXGIFactory2),
            reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) { err = "CreateDXGIFactory failed."; return false; }

        // Default: first adapter. In CUDA mode we must create the D3D11
        // device on the adapter CUDA can talk to (hybrid-GPU machines can
        // have the default adapter be the non-NVIDIA one).
        UINT ordinal = 0;
        if (mCUDAMode) {
            int cudaOrdinal = cuda::FindCUDAAdapterOrdinal();
            if (cudaOrdinal < 0) {
                err = "CUDA execute mode active but no DXGI adapter maps to "
                      "a CUDA device.";
                return false;
            }
            ordinal = (UINT)cudaOrdinal;
        }

        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC adapterDesc{};
        if (factory->EnumAdapters(ordinal, &adapter) != DXGI_ERROR_NOT_FOUND) {
            adapter->GetDesc(&adapterDesc);
        }

        rive::gpu::D3DContextOptions opts;
        opts.isIntel = adapterDesc.VendorId == 0x163C ||
                       adapterDesc.VendorId == 0x8086 ||
                       adapterDesc.VendorId == 0x8087;

        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1 };
        UINT creationFlags = 0;
#ifdef _DEBUG
        creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        hr = D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            creationFlags,
            featureLevels,
            (UINT)std::size(featureLevels),
            D3D11_SDK_VERSION,
            mDevice.ReleaseAndGetAddressOf(),
            nullptr,
            mContext.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !mDevice || !mContext) {
            err = "D3D11CreateDevice failed.";
            return false;
        }

        mRenderContext = rive::gpu::RenderContextD3DImpl::MakeContext(
            mDevice, mContext, opts);
        if (!mRenderContext) {
            err = "Failed to create Rive D3D11 render context.";
            mContext.Reset();
            mDevice.Reset();
            return false;
        }

        // Diagnostics only: if any query fails to create, renderGpuMs stays 0.
        D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
        for (auto& t : mGpuTimers) {
            if (FAILED(mDevice->CreateQuery(&dj, &t.disjoint)) ||
                FAILED(mDevice->CreateQuery(&ts, &t.begin)) ||
                FAILED(mDevice->CreateQuery(&ts, &t.end))) {
                t = GpuTimer{};
            }
        }
        return true;
    }

    rive::Factory*            factory()       override { return mRenderContext.get(); }
    rive::gpu::RenderContext* renderContext() override { return mRenderContext.get(); }

    bool cudaInterop() const override { return mCUDAMode; }

    // A dedicated non-blocking stream, declared to TouchDesigner, instead of
    // the legacy default stream - which implicitly serializes with every
    // other blocking stream in the process, TouchDesigner's own included.
    void* cudaStream() const override { return mStream; }

    void ensureCudaStream() override
    {
        if (!mCUDAMode || mStream) return;
        if (const auto* api = cuda::Get())
            api->streamCreateWithFlags(&mStream, cuda::kStreamNonBlocking);
    }

    bool ensureRenderTarget(uint32_t w, uint32_t h, std::string& err) override
    {
        return ensureSurface(mMain, w, h, err);
    }

    bool ensureMaskTarget(uint32_t w, uint32_t h, std::string& err) override
    {
        return ensureSurface(mMask, w, h, err);
    }

    void releaseMaskTarget() override
    {
        releaseSurface(mMask);
        mTimings.maskMs = 0.0;
    }

    using Clock = std::chrono::steady_clock;
    tdrive::ReadbackTimings mTimings{};

    bool renderAndReadback(const rive::gpu::RenderContext::FrameDescriptor& fd,
                           const std::function<void(rive::Renderer*)>&      draw,
                           void*                                            dst,
                           std::string&                                     err) override
    {
        return surfaceRenderAndReadback(mMain, fd, draw, dst, err, /*main=*/true);
    }

    bool renderMaskAndReadback(const rive::gpu::RenderContext::FrameDescriptor& fd,
                               const std::function<void(rive::Renderer*)>&      draw,
                               void* dst, std::string& err) override
    {
        const auto t0 = Clock::now();
        const bool ok = surfaceRenderAndReadback(mMask, fd, draw, dst, err,
                                                 /*main=*/false);
        mTimings.maskMs = std::chrono::duration<double, std::milli>(
                              Clock::now() - t0).count();
        return ok;
    }

    tdrive::ReadbackTimings lastTimings() const override { return mTimings; }

    bool renderToCUDA(const rive::gpu::RenderContext::FrameDescriptor& fd,
                      const std::function<void(rive::Renderer*)>&      draw,
                      void* dstCudaArray, std::string& err) override
    {
        return surfaceRenderToCUDA(mMain, fd, draw, dstCudaArray, err,
                                   /*main=*/true);
    }

    bool renderMaskToCUDA(const rive::gpu::RenderContext::FrameDescriptor& fd,
                          const std::function<void(rive::Renderer*)>&      draw,
                          void* dstCudaArray, std::string& err) override
    {
        const auto t0 = Clock::now();
        const bool ok = surfaceRenderToCUDA(mMask, fd, draw, dstCudaArray, err,
                                            /*main=*/false);
        mTimings.maskMs = std::chrono::duration<double, std::milli>(
                              Clock::now() - t0).count();
        return ok;
    }

    rive::rcp<rive::RenderImage> updateImageSlot(
        int slot, uint32_t w, uint32_t h,
        const uint8_t* rgba, std::string& err) override
    {
        if (!ensureSlotTexture(slot, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, err))
            return nullptr;
        Slot& s = mSlots[slot];
        mContext->UpdateSubresource(s.tex.Get(), 0, nullptr,
                                    rgba, w * 4, 0);
        return s.img;
    }

    rive::rcp<rive::RenderImage> updateImageSlotCUDA(
        int slot, uint32_t w, uint32_t h,
        void* srcCudaArray, bool bgra, std::string& err) override
    {
        const auto* api = cuda::Get();
        if (!mCUDAMode || !api) { err = "CUDA interop inactive."; return nullptr; }
        // The slot texture takes the input's channel order, so the raw bytes
        // CUDA copies in are already right and Rive's view of the texture
        // does the swizzle. BGRA8 is a CUDA D3D11-interop format.
        const DXGI_FORMAT fmt = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM
                                     : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!ensureSlotTexture(slot, w, h, fmt, err)) return nullptr;
        Slot& s = mSlots[slot];

        // TouchDesigner's cudaArray starts at the bottom row and Rive samples
        // top row first, so CUDA copies into a landing texture and the rows
        // are flipped into the slot texture below. An array-to-array copy
        // can't reverse rows itself.
        if (!s.landing) {
            D3D11_TEXTURE2D_DESC d{};
            s.tex->GetDesc(&d);
            HRESULT hr = mDevice->CreateTexture2D(
                &d, nullptr, s.landing.ReleaseAndGetAddressOf());
            if (FAILED(hr) || !s.landing) {
                err = "Failed to allocate D3D11 landing texture.";
                return nullptr;
            }
        }

        if (!s.cudaRes) {
            cudaError_t ce = api->graphicsD3D11RegisterResource(
                &s.cudaRes, s.landing.Get(), cuda::kGraphicsRegisterFlagsNone);
            if (ce != cuda::kSuccess) {
                err = std::string("cudaGraphicsD3D11RegisterResource(image) "
                                  "failed: ") + api->getErrorString(ce);
                s.cudaRes = nullptr;
                return nullptr;
            }
        }

        cudaError_t ce = api->graphicsMapResources(1, &s.cudaRes, mStream);
        if (ce != cuda::kSuccess) {
            err = std::string("cudaGraphicsMapResources(image) failed: ") +
                  api->getErrorString(ce);
            return nullptr;
        }
        cudaArray* dstArray = nullptr;
        ce = api->graphicsSubResourceGetMappedArray(&dstArray, s.cudaRes, 0, 0);
        if (ce == cuda::kSuccess && dstArray) {
            ce = copyArray(api, dstArray, (cudaArray*)srcCudaArray, w, h);
        }
        api->graphicsUnmapResources(1, &s.cudaRes, mStream);

        if (ce != cuda::kSuccess) {
            err = std::string("CUDA image copy failed: ") +
                  api->getErrorString(ce);
            return nullptr;
        }

        // Unmap orders the CUDA copy before later D3D11 work on the landing
        // texture, so these GPU-side row copies see the new frame.
        for (uint32_t y = 0; y < h; ++y) {
            const D3D11_BOX row{0, y, 0, w, y + 1, 1};
            mContext->CopySubresourceRegion(s.tex.Get(), 0, 0, h - 1 - y, 0,
                                            s.landing.Get(), 0, &row);
        }
        return s.img;
    }

private:
    struct Slot {
        ComPtr<ID3D11Texture2D>       tex;
        // CUDA-mode only: where TD's (bottom-up) frame lands before the
        // row flip into 'tex'. Registered with CUDA as cudaRes.
        ComPtr<ID3D11Texture2D>       landing;
        rive::rcp<rive::RenderImage>  img;
        uint32_t                      w = 0, h = 0;
        DXGI_FORMAT                   fmt = DXGI_FORMAT_UNKNOWN;
        cudaGraphicsResource_t        cudaRes = nullptr;
    };

    // One offscreen render target plus what it takes to get its pixels out:
    // two staging textures (CPU path) or a CUDA registration (CUDA path). The
    // main output and the mask output are one each, on the same device and
    // render context.
    struct Surface {
        ComPtr<ID3D11Texture2D>               target;
        // Two staging textures, used round-robin. See surfaceRenderAndReadback().
        ComPtr<ID3D11Texture2D>               staging[2];
        int                                   stagingIdx     = 0;
        bool                                  stagingPending = false;
        cudaGraphicsResource_t                cudaRes = nullptr;
        rive::rcp<rive::gpu::RenderTargetD3D> rt;
        uint32_t                              w = 0, h = 0;
    };

    void releaseSurface(Surface& s)
    {
        if (s.cudaRes) {
            if (const auto* api = cuda::Get())
                api->graphicsUnregisterResource(s.cudaRes);
            s.cudaRes = nullptr;
        }
        s.rt.reset();
        s.target.Reset();
        s.staging[0].Reset();
        s.staging[1].Reset();
        s.stagingIdx     = 0;
        s.stagingPending = false;
        s.w = s.h = 0;
    }

    bool ensureSurface(Surface& s, uint32_t w, uint32_t h, std::string& err)
    {
        if (w == 0 || h == 0) { err = "Render target has zero size."; return false; }
        if (s.target && s.w == w && s.h == h && s.rt &&
            (mCUDAMode ? s.cudaRes != nullptr : s.staging[0] != nullptr))
            return true;

        releaseSurface(s);

        // Offscreen texture, render-target + UAV (Rive renders via UAV in
        // atomic mode, RTV in raster-ordered mode; we enable both so it
        // works regardless of the path the Rive runtime picks).
        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = mTargetFormat;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        d.BindFlags        = D3D11_BIND_RENDER_TARGET |
                             D3D11_BIND_SHADER_RESOURCE |
                             D3D11_BIND_UNORDERED_ACCESS;
        d.CPUAccessFlags   = 0;
        d.MiscFlags        = 0;

        HRESULT hr = mDevice->CreateTexture2D(&d, nullptr,
                                              s.target.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !s.target) {
            err = "Failed to allocate offscreen D3D11 texture.";
            return false;
        }

        if (mCUDAMode) {
            const auto* api = cuda::Get();
            if (!api) { err = "CUDA runtime not loaded."; return false; }
            cudaError_t ce = api->graphicsD3D11RegisterResource(
                &s.cudaRes, s.target.Get(),
                cuda::kGraphicsRegisterFlagsNone);
            if (ce != cuda::kSuccess) {
                err = std::string("cudaGraphicsD3D11RegisterResource(target) "
                                  "failed: ") + api->getErrorString(ce);
                s.cudaRes = nullptr;
                return false;
            }
        } else {
            // Staging texture: CPU-readable copy destination.
            D3D11_TEXTURE2D_DESC st{};
            st.Width            = w;
            st.Height           = h;
            st.MipLevels        = 1;
            st.ArraySize        = 1;
            st.Format           = mTargetFormat;
            st.SampleDesc.Count = 1;
            st.Usage            = D3D11_USAGE_STAGING;
            st.BindFlags        = 0;
            st.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
            st.MiscFlags        = 0;

            for (int i = 0; i < 2; ++i) {
                hr = mDevice->CreateTexture2D(
                    &st, nullptr, s.staging[i].ReleaseAndGetAddressOf());
                if (FAILED(hr) || !s.staging[i]) {
                    err = "Failed to allocate D3D11 staging texture.";
                    return false;
                }
            }
            // Nothing has been copied into either one at the new size yet, so
            // the next readback has to be the synchronous kind.
            s.stagingIdx     = 0;
            s.stagingPending = false;
        }

        auto* impl = mRenderContext->static_impl_cast<rive::gpu::RenderContextD3DImpl>();
        s.rt = impl->makeRenderTarget(w, h);
        s.w = w;
        s.h = h;
        return true;
    }

    bool surfaceRenderAndReadback(Surface& s,
                                  const rive::gpu::RenderContext::FrameDescriptor& fd,
                                  const std::function<void(rive::Renderer*)>& draw,
                                  void* dst, std::string& err, bool main)
    {
        if (!mRenderContext || !s.rt || !s.target || !s.staging[0]) {
            err = "D3D11 backend not initialized.";
            return false;
        }

        const auto t0 = Clock::now();
        renderFrame(s, fd, draw, /*timed=*/main);
        const auto t1 = Clock::now();

        // Queue the GPU->staging copy for the frame we just drew.
        //
        // The two staging textures are used round-robin so that the Map()
        // below reads the copy queued on the PREVIOUS cook, which the GPU has
        // had a whole frame to retire. Mapping the copy we just queued is what
        // made this expensive: Map(D3D11_MAP_READ) blocks until the GPU
        // catches up, and that stall measured 2.17 ms of a 3.39 ms readback at
        // 3840x2160 - 64% of it, against 0.08 ms of actual Rive rendering.
        //
        // The cost is one frame of latency: the texture handed to
        // TouchDesigner is the frame drawn on the previous cook. The first
        // cook after a resize has no previous copy to read, so it falls back
        // to the synchronous path rather than emitting a blank frame.
        const int cur  = s.stagingIdx;
        const int prev = s.stagingIdx ^ 1;
        mContext->CopyResource(s.staging[cur].Get(), s.target.Get());
        // Submit now instead of letting D3D11 batch. Without this the copy sits
        // in the command buffer until something forces a flush - which is the
        // Map() below - so the GPU only starts the copy at the moment we begin
        // waiting for it, and double buffering buys nothing. Flushing here is
        // what actually gives the GPU a whole frame to retire the copy.
        mContext->Flush();
        const auto t2 = Clock::now();

        const int readIdx = s.stagingPending ? prev : cur;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = mContext->Map(s.staging[readIdx].Get(), 0,
                                   D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) { err = "Map(staging) failed."; return false; }
        const auto t3 = Clock::now();

        const uint8_t* src = (const uint8_t*)mapped.pData;
        uint8_t*       d   = (uint8_t*)dst;
        const size_t   rowBytes = (size_t)s.w * 4;
        if (mapped.RowPitch == rowBytes) {
            // Tightly packed - one memcpy instead of a call per scanline.
            std::memcpy(d, src, rowBytes * s.h);
        } else {
            for (uint32_t y = 0; y < s.h; ++y) {
                std::memcpy(d + y * rowBytes,
                            src + (size_t)y * mapped.RowPitch,
                            rowBytes);
            }
        }
        mContext->Unmap(s.staging[readIdx].Get(), 0);
        const auto t4 = Clock::now();

        s.stagingIdx     = prev;
        s.stagingPending = true;

        if (main) {
            auto ms = [](Clock::time_point a, Clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            mTimings.renderMs = ms(t0, t1);
            mTimings.copyMs   = ms(t1, t2);
            mTimings.mapMs    = ms(t2, t3);
            mTimings.memcpyMs = ms(t3, t4);
            mTimings.totalMs  = ms(t0, t4);
        }
        return true;
    }

    bool surfaceRenderToCUDA(Surface& s,
                             const rive::gpu::RenderContext::FrameDescriptor& fd,
                             const std::function<void(rive::Renderer*)>& draw,
                             void* dstCudaArray, std::string& err, bool main)
    {
        const auto* api = cuda::Get();
        if (!mCUDAMode || !api) { err = "CUDA interop inactive."; return false; }
        if (!mRenderContext || !s.rt || !s.target || !s.cudaRes) {
            err = "D3D11 backend not initialized (CUDA).";
            return false;
        }

        const auto t0 = Clock::now();
        renderFrame(s, fd, draw, /*timed=*/main);
        // Make sure the D3D work is submitted before CUDA touches the
        // texture. cudaGraphicsMapResources synchronizes with the device,
        // but only against submitted work.
        mContext->Flush();
        const auto t1 = Clock::now();

        cudaError_t ce = api->graphicsMapResources(1, &s.cudaRes, mStream);
        if (ce != cuda::kSuccess) {
            err = std::string("cudaGraphicsMapResources(target) failed: ") +
                  api->getErrorString(ce);
            return false;
        }
        const auto t2 = Clock::now();
        cudaArray* srcArray = nullptr;
        ce = api->graphicsSubResourceGetMappedArray(&srcArray, s.cudaRes, 0, 0);
        if (ce == cuda::kSuccess && srcArray) {
            ce = copyArray(api, (cudaArray*)dstCudaArray, srcArray, s.w, s.h);
        }
        const auto t3 = Clock::now();
        api->graphicsUnmapResources(1, &s.cudaRes, mStream);
        const auto t4 = Clock::now();

        if (main) {
            auto ms = [](Clock::time_point a, Clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            mTimings.renderMs = ms(t0, t1);
            mTimings.mapMs    = ms(t1, t2);
            mTimings.copyMs   = ms(t2, t3);
            mTimings.unmapMs  = ms(t3, t4);
            mTimings.memcpyMs = 0.0;
            mTimings.totalMs  = ms(t0, t4);
        }

        if (ce != cuda::kSuccess) {
            err = std::string("CUDA target copy failed: ") +
                  api->getErrorString(ce);
            return false;
        }
        return true;
    }

    void renderFrame(Surface& s,
                     const rive::gpu::RenderContext::FrameDescriptor& fd,
                     const std::function<void(rive::Renderer*)>&      draw,
                     bool timed)
    {
        GpuTimer* t = nullptr;
        if (timed) {
            collectGpuTimers();
            GpuTimer& g = mGpuTimers[mGpuTimerIdx];
            mGpuTimerIdx = (mGpuTimerIdx + 1) % kNumGpuTimers;
            // A set whose result hasn't come back yet is skipped, not waited on.
            if (g.disjoint.Get() != nullptr && !g.pending) t = &g;
        }
        if (t) {
            mContext->Begin(t->disjoint.Get());
            mContext->End(t->begin.Get());
        }

        s.rt->setTargetTexture(s.target);
        mRenderContext->beginFrame(fd);
        rive::RiveRenderer renderer(mRenderContext.get());
        draw(&renderer);
        rive::gpu::RenderContext::FlushResources flush;
        flush.renderTarget = s.rt.get();
        flush.externalCommandBuffer = nullptr;
        mRenderContext->flush(flush);

        if (t) {
            mContext->End(t->end.Get());
            mContext->End(t->disjoint.Get());
            t->pending = true;
        }
    }

    void collectGpuTimers()
    {
        for (auto& t : mGpuTimers) {
            if (!t.pending) continue;
            constexpr UINT kNoFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 b = 0, e = 0;
            if (mContext->GetData(t.disjoint.Get(), &dj, sizeof(dj), kNoFlush) != S_OK ||
                mContext->GetData(t.begin.Get(), &b, sizeof(b), kNoFlush) != S_OK ||
                mContext->GetData(t.end.Get(), &e, sizeof(e), kNoFlush) != S_OK)
                continue;
            t.pending = false;
            if (!dj.Disjoint && dj.Frequency && e >= b)
                mTimings.renderGpuMs = (double)(e - b) * 1000.0 / (double)dj.Frequency;
        }
    }

    bool ensureSlotTexture(int slot, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                           std::string& err)
    {
        if (slot < 0 || slot >= kMaxImageSlots) { err = "Bad image slot."; return false; }
        if (w == 0 || h == 0) { err = "Image input has zero size."; return false; }
        Slot& s = mSlots[slot];
        if (s.tex && s.w == w && s.h == h && s.fmt == fmt && s.img) return true;

        releaseSlot(s);

        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = fmt;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DEFAULT;
        d.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags   = 0;
        d.MiscFlags        = 0;

        HRESULT hr = mDevice->CreateTexture2D(&d, nullptr,
                                              s.tex.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !s.tex) {
            err = "Failed to allocate D3D11 image texture.";
            return false;
        }

        auto* impl = mRenderContext->static_impl_cast<rive::gpu::RenderContextD3DImpl>();
        auto riveTex = impl->adoptImageTexture(s.tex, w, h);
        if (!riveTex) { err = "adoptImageTexture failed."; return false; }
        s.img = rive::make_rcp<rive::RiveRenderImage>(std::move(riveTex));
        s.w = w;
        s.h = h;
        s.fmt = fmt;
        return true;
    }

    void releaseSlot(Slot& s)
    {
        if (s.cudaRes) {
            if (const auto* api = cuda::Get())
                api->graphicsUnregisterResource(s.cudaRes);
            s.cudaRes = nullptr;
        }
        s.img.reset();
        s.landing.Reset();
        s.tex.Reset();
        s.w = s.h = 0;
        s.fmt = DXGI_FORMAT_UNKNOWN;
    }

    // RGBA8 array -> array on mStream. Extent is in elements for arrays.
    cudaError_t copyArray(const cuda::Api* api, cudaArray* dst, cudaArray* src,
                          uint32_t w, uint32_t h)
    {
        cuda::Memcpy3DParms p{};
        p.srcArray = src;
        p.dstArray = dst;
        p.extent   = {w, h, 1};
        p.kind     = cuda::kMemcpyDeviceToDevice;
        return api->memcpy3DAsync(&p, mStream);
    }

    bool                         mCUDAMode = false;
    DXGI_FORMAT                  mTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    ComPtr<ID3D11Device>         mDevice;
    ComPtr<ID3D11DeviceContext>  mContext;
    Surface                      mMain;   // the Rive TOP's output
    Surface                      mMask;   // the Mask output (color buffer 1)
    cudaStream_t                 mStream = nullptr;

    struct GpuTimer {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool                pending = false;
    };
    static constexpr int kNumGpuTimers = 4;
    GpuTimer mGpuTimers[kNumGpuTimers];
    int      mGpuTimerIdx = 0;

    Slot mSlots[kMaxImageSlots];

    std::unique_ptr<rive::gpu::RenderContext> mRenderContext;
};

std::unique_ptr<IBackend> CreateBackend(bool cudaMode)
{
    return std::make_unique<D3D11Backend>(cudaMode);
}

} // namespace tdrive
