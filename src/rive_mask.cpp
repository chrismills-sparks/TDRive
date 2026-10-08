// rive_mask.cpp - see rive_mask.h.

#include "rive_mask.h"

#include "rive/artboard.hpp"
#include "rive/math/aabb.hpp"
#include "rive/artboard_component_list.hpp"
#include "rive/nested_artboard.hpp"
#include "rive/shapes/paint/color.hpp"
#include "rive/shapes/paint/shape_paint.hpp"
#include "rive/shapes/paint/solid_color.hpp"

namespace tdrive {

rive::RenderPaint* TrackedPaint::maskPaint(float alpha)
{
    if (!mMask) mMask = mReal->makeRenderPaint();
    rive::RenderPaint* m = mMask.get();
    if (!mMaskInit) {
        // Opaque grey drawn with lighten onto opaque black is max(): a
        // card and the text on it, fading together at 0.3, mask as 0.3 -
        // not the 0.51 that src-over would stack them to - and two
        // unrelated objects overlapping keep the stronger of the two.
        m->blendMode(rive::BlendMode::lighten);
        m->shader(nullptr);
        mMaskInit = true;
    }
    if (mStyle != mMStyle || mMThickness < 0.0f) { m->style(mStyle); mMStyle = mStyle; }
    if (mThickness != mMThickness) { m->thickness(mThickness); mMThickness = mThickness; }
    if (mJoin != mMJoin)           { m->join(mJoin);           mMJoin = mJoin; }
    if (mCap != mMCap)             { m->cap(mCap);             mMCap = mCap; }
    if (mFeather != mMFeather)     { m->feather(mFeather);     mMFeather = mFeather; }

    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const int g = (int)(a * 255.0f + 0.5f);
    const rive::ColorInt c = rive::colorARGB(255, g, g, g);
    if (c != mMColor) { m->color(c); mMColor = c; }
    return m;
}

namespace {
void tagArtboard(rive::Artboard* ab, int depth)
{
    // Nesting can chain; the bound stops a malformed cycle.
    if (!ab || depth > 16) return;
    for (rive::Core* obj : ab->objects()) {
        if (!obj) continue;
        if (obj->is<rive::ShapePaint>()) {
            auto* sp = obj->as<rive::ShapePaint>();
            // Every paint of a tracked file is a TrackedPaint.
            if (auto* rp = sp->renderPaint())
                static_cast<TrackedPaint*>(rp)->owner(sp);
        } else if (obj->is<rive::NestedArtboard>()) {
            tagArtboard(obj->as<rive::NestedArtboard>()->artboardInstance(), depth + 1);
        } else if (obj->is<rive::ArtboardComponentList>()) {
            auto* list = obj->as<rive::ArtboardComponentList>();
            for (size_t i = 0; i < list->artboardCount(); ++i)
                tagArtboard(list->artboardInstance((int)i), depth + 1);
        }
    }
}
} // namespace

void tagPaintOwners(rive::ArtboardInstance* root)
{
    tagArtboard(root, 0);
}

void PassRenderer::drawPath(rive::RenderPath* path, rive::RenderPaint* paint)
{
    // Every paint of a tracked file is a TrackedPaint - TrackingFactory is
    // the only thing that makes them.
    auto* tp = static_cast<TrackedPaint*>(paint);
    if (!mMask) {
        mInner->drawPath(path, tp->inner());
        return;
    }

    float alpha;
    if (rive::ShapePaint* sp = tp->owner()) {
        // World opacity - every fading parent, nested-artboard host and data
        // binding folded in - without the paint's own alpha.
        alpha = sp->renderOpacity();
        // Feathered paints are shadows and glows: soft effects whose strength
        // IS their alpha. At full presence a 25% drop shadow would mask as a
        // solid halo, so they always keep their authored alpha.
        if (mMode == MaskMode::Alpha || tp->featherAmount() > 0.0f) {
            rive::Component* mut = sp->paint();
            if (mut && mut->is<rive::SolidColor>())
                alpha *= rive::colorOpacity(mut->as<rive::SolidColor>()->colorValue());
        }
    } else {
        // No owner (pooled text-run paints, scripted paints): all that is
        // known is the final colour, which already carries the opacity.
        alpha = tp->hasShader() ? 1.0f : rive::colorOpacity(tp->color());
    }
    if (!(alpha > 0.0f)) return;
    mInner->drawPath(path, tp->maskPaint(alpha));
}

void PassRenderer::maskImageRect(const rive::RenderImage* img, float opacity)
{
    if (!img || !(opacity > 0.0f)) return;
    if (!mImagePaint) {
        mImagePaint = mReal->makeRenderPaint();
        mImagePaint->style(rive::RenderPaintStyle::fill);
        mImagePaint->blendMode(rive::BlendMode::lighten);
    }
    const float a = opacity > 1.0f ? 1.0f : opacity;
    const int   g = (int)(a * 255.0f + 0.5f);
    mImagePaint->color(rive::colorARGB(255, g, g, g));
    // drawImage() draws the image over (0,0)-(width,height) in the current
    // transform, so this rectangle lands exactly where the image would.
    auto rect = mReal->makeRenderPath(
        rive::AABB(0.0f, 0.0f, (float)img->width(), (float)img->height()));
    mInner->drawPath(rect.get(), mImagePaint.get());
}

} // namespace tdrive
