// rive_mask.h
//
// The pieces behind the Mask output: a second render of the same frame in
// which every object is drawn as grey = its world opacity, combined with max,
// so the result is "where is something, and how faded is it" - independent of
// the colours and alphas the artist painted with. A translucent glass card
// that is fully faded in masks as 1.0; the same card at 50% opacity masks as
// 0.5, and so does the text on it (max, not src-over, so stacked elements of
// one fading group do not add up). The mask is greyscale in RGB on opaque
// black; alpha is 1.
//
// Why it needs its own factory: by the time a draw reaches a Renderer, Rive
// has already multiplied the object's opacity into the paint colour, and a
// RenderPaint has setters only - there is no reading its stroke width or
// feather back. So while the mask is on, the file is imported through
// TrackingFactory, which hands Rive a TrackedPaint for every paint it makes.
// A TrackedPaint forwards to the real paint and remembers what was set on it,
// which is enough to build a matching grey paint for the mask pass.
//
// The catch: Rive's renderer silently skips a paint that is not its own
// concrete type. EVERY draw of a tracked file therefore has to go through a
// PassRenderer, which swaps the TrackedPaint back for the real one (normal
// pass) or for its grey twin (mask pass). That is also why the tracking
// factory is only used while the mask is on - with it off, nothing here is in
// the path at all.

#pragma once

#include <cstdint>

#include "rive/factory.hpp"
#include "rive/renderer.hpp"

namespace rive {
class ArtboardInstance;
class Artboard;
class ShapePaint;
} // namespace rive

namespace tdrive {

// A RenderPaint that forwards to the real one and records its state.
class TrackedPaint final : public rive::RenderPaint {
public:
    TrackedPaint(rive::rcp<rive::RenderPaint> inner, rive::Factory* real)
        : mInner(std::move(inner)), mReal(real) {}

    rive::RenderPaint* inner() const { return mInner.get(); }

    // The grey twin used by the mask pass, set to grey level 'alpha' (0..1).
    rive::RenderPaint* maskPaint(float alpha);

    // The ShapePaint this paint belongs to, stamped by tagPaintOwners(); null
    // for paints no ShapePaint owns (pooled text-run paints, scripted paints).
    // Safe to hold raw: the ShapePaint owns this paint, so the paint cannot
    // outlive it.
    rive::ShapePaint* owner() const { return mOwner; }
    void owner(rive::ShapePaint* sp) { mOwner = sp; }

    // What Rive last set: the colour already carries the object's opacity.
    rive::ColorInt color() const { return mColor; }
    bool hasShader() const { return mHasShader; }
    float featherAmount() const { return mFeather; }

    void style(rive::RenderPaintStyle v) override  { mStyle = v; mInner->style(v); }
    void color(rive::ColorInt v) override          { mColor = v; mInner->color(v); }
    void thickness(float v) override               { mThickness = v; mInner->thickness(v); }
    void join(rive::StrokeJoin v) override         { mJoin = v; mInner->join(v); }
    void cap(rive::StrokeCap v) override           { mCap = v; mInner->cap(v); }
    void feather(float v) override                 { mFeather = v; mInner->feather(v); }
    void additiveness(float v) override            { mInner->additiveness(v); }
    void blendMode(rive::BlendMode v) override     { mInner->blendMode(v); }
    void shader(rive::rcp<rive::RenderShader> s) override
    {
        mHasShader = s != nullptr;
        mInner->shader(std::move(s));
    }
    void invalidateStroke() override               { mInner->invalidateStroke(); }
    void modulatedImage(const rive::RenderImage* i, rive::ImageSampler s,
                        const rive::Mat2D& m) override
    {
        mInner->modulatedImage(i, s, m);
    }

private:
    rive::rcp<rive::RenderPaint> mInner;
    rive::Factory*               mReal;   // makes the mask twin
    rive::ShapePaint*            mOwner = nullptr;

    rive::RenderPaintStyle mStyle     = rive::RenderPaintStyle::fill;
    rive::ColorInt         mColor     = 0xFF000000;
    float                  mThickness = 1.0f;
    rive::StrokeJoin       mJoin      = rive::StrokeJoin::miter;
    rive::StrokeCap        mCap       = rive::StrokeCap::butt;
    float                  mFeather   = 0.0f;
    bool                   mHasShader = false;

    // The twin, and the state last pushed into it - setters only run on a
    // change, because a stroke parameter change invalidates cached geometry.
    rive::rcp<rive::RenderPaint> mMask;
    bool                   mMaskInit  = false;
    rive::RenderPaintStyle mMStyle    = rive::RenderPaintStyle::fill;
    rive::ColorInt         mMColor    = 0;
    float                  mMThickness = -1.0f;
    rive::StrokeJoin       mMJoin     = rive::StrokeJoin::miter;
    rive::StrokeCap        mMCap      = rive::StrokeCap::butt;
    float                  mMFeather  = -1.0f;
};

// Forwards everything to the real factory, except that paints come back
// wrapped in a TrackedPaint.
class TrackingFactory final : public rive::Factory {
public:
    explicit TrackingFactory(rive::Factory* real) : mReal(real) {}

    rive::rcp<rive::RenderBuffer> makeRenderBuffer(rive::RenderBufferType t,
                                                   rive::RenderBufferFlags f,
                                                   size_t n) override
    {
        return mReal->makeRenderBuffer(t, f, n);
    }
    rive::rcp<rive::RenderShader> makeLinearGradient(
        float sx, float sy, float ex, float ey, const rive::ColorInt colors[],
        const float stops[], size_t count) override
    {
        return mReal->makeLinearGradient(sx, sy, ex, ey, colors, stops, count);
    }
    rive::rcp<rive::RenderShader> makeRadialGradient(
        float cx, float cy, float radius, const rive::ColorInt colors[],
        const float stops[], size_t count) override
    {
        return mReal->makeRadialGradient(cx, cy, radius, colors, stops, count);
    }
    rive::rcp<rive::RenderPath> makeRenderPath(rive::RawPath& p,
                                               rive::FillRule r) override
    {
        return mReal->makeRenderPath(p, r);
    }
    rive::rcp<rive::RenderPath> makeEmptyRenderPath() override
    {
        return mReal->makeEmptyRenderPath();
    }
    rive::rcp<rive::RenderPaint> makeRenderPaint() override
    {
        ++mPaintsMade;
        return rive::make_rcp<TrackedPaint>(mReal->makeRenderPaint(), mReal);
    }
    // Bumps whenever a paint is made - i.e. whenever there may be a paint
    // that tagPaintOwners() has not stamped yet.
    uint64_t paintsMade() const { return mPaintsMade; }
    rive::rcp<rive::RenderImage> decodeImage(rive::Span<const uint8_t> b) override
    {
        return mReal->decodeImage(b);
    }
    rive::ore::Context* ore() override { return mReal->ore(); }
    rive::Factory* renderContext() override { return mReal->renderContext(); }
    rive::cmd::DeferredCanvasHost* deferredCanvasHost() override
    {
        return mReal->deferredCanvasHost();
    }
    rive::cmd::DeferredCanvasHost* canvasContentHost() override
    {
        return mReal->canvasContentHost();
    }

private:
    rive::Factory* mReal;
    uint64_t       mPaintsMade = 0;
};

// How the mask pass weighs a paint's own (authored) alpha.
enum class MaskMode {
    Presence,  // object opacity only: a 10%-alpha fill masks as fully present
    Alpha,     // object opacity x the paint's own alpha
};

// Stamps every tracked paint reachable from 'root' - nested artboards and
// data-bound artboard lists included - with the ShapePaint that owns it, so
// the mask pass can read the object's world opacity separately from the
// paint's colour. Only needs re-running when TrackingFactory::paintsMade()
// moves: a paint made after the last walk is the only kind still unstamped.
void tagPaintOwners(rive::ArtboardInstance* root);

// The renderer every draw of a tracked file goes through. Normal pass: swaps
// each TrackedPaint for the real paint. Mask pass: swaps it for its grey twin
// (see TrackedPaint::maskPaint), so the mask is a greyscale image on opaque
// black holding max(coverage x opacity) of everything drawn there.
//
// Images cannot be tinted, so in the mask pass a plain image draws as its
// RECTANGLE at its opacity - a transparent PNG masks as a box. Image meshes
// (deformed images) draw as-is with lighten, an approximation: their bright
// pixels count as presence.
class PassRenderer final : public rive::Renderer {
public:
    PassRenderer(rive::Renderer* inner, bool mask, MaskMode mode,
                 rive::Factory* real)
        : mInner(inner), mMask(mask), mMode(mode), mReal(real) {}

    void save() override                         { mInner->save(); }
    void restore() override                      { mInner->restore(); }
    void transform(const rive::Mat2D& m) override { mInner->transform(m); }
    void clipPath(rive::RenderPath* p) override  { mInner->clipPath(p); }
    void clipStroke(rive::RenderPath* p, const rive::StrokeParams& s) override
    {
        mInner->clipStroke(p, s);
    }
    void modulateOpacity(float o) override       { mInner->modulateOpacity(o); }
    bool currentTransform(rive::Mat2D* out) const override
    {
        return mInner->currentTransform(out);
    }
    bool currentModulatedOpacity(float* out) const override
    {
        return mInner->currentModulatedOpacity(out);
    }

    void drawPath(rive::RenderPath* path, rive::RenderPaint* paint) override;

    void drawImage(const rive::RenderImage* img, rive::ImageSampler s,
                   rive::BlendMode b, float opacity) override
    {
        if (mMask) maskImageRect(img, opacity);
        else       mInner->drawImage(img, s, b, opacity);
    }
    void drawImage(const rive::RenderImage* img, rive::ImageSampler s,
                   rive::BlendMode b, float opacity, float additive) override
    {
        if (mMask) maskImageRect(img, opacity);
        else       mInner->drawImage(img, s, b, opacity, additive);
    }
    void drawImageMesh(const rive::RenderImage* img, rive::ImageSampler s,
                       rive::rcp<rive::RenderBuffer> v,
                       rive::rcp<rive::RenderBuffer> uv,
                       rive::rcp<rive::RenderBuffer> idx, uint32_t vc,
                       uint32_t ic, rive::BlendMode b, float opacity) override
    {
        mInner->drawImageMesh(img, s, std::move(v), std::move(uv),
                              std::move(idx), vc, ic,
                              mMask ? rive::BlendMode::lighten : b, opacity);
    }
    void drawImageMesh(const rive::RenderImage* img, rive::ImageSampler s,
                       rive::rcp<rive::RenderBuffer> v,
                       rive::rcp<rive::RenderBuffer> uv,
                       rive::rcp<rive::RenderBuffer> idx, uint32_t vc,
                       uint32_t ic, rive::BlendMode b, float opacity,
                       float additive) override
    {
        if (mMask)
            mInner->drawImageMesh(img, s, std::move(v), std::move(uv),
                                  std::move(idx), vc, ic,
                                  rive::BlendMode::lighten, opacity);
        else
            mInner->drawImageMesh(img, s, std::move(v), std::move(uv),
                                  std::move(idx), vc, ic, b, opacity, additive);
    }

private:
    void maskImageRect(const rive::RenderImage* img, float opacity);

    rive::Renderer*    mInner;
    bool               mMask;
    MaskMode           mMode;
    rive::Factory*     mReal;
    rive::rcp<rive::RenderPaint> mImagePaint;   // made on first image
};

} // namespace tdrive
