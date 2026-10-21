// Copyright Contributors to the Moonray Project
// SPDX-License-Identifier: Apache-2.0

// Converts implicit surfaces (sphere, cube, cone, cylinder, capsule, plane)
// to meshes for the Moonray render delegate.
//
// With Hydra 1 scene delegates, UsdImaging's adapters did this conversion
// whenever the render delegate didn't list the implicit type as a supported
// rprim. With scene index emulation, the default in Houdini 21 Solaris, that
// fallback is gone, and implicit prims such as the default Sphere LOP never
// reached Moonray. This is the same approach Storm takes in
// HdSt_ImplicitSurfaceSceneIndexPlugin. Planes need USD 24.11 or later.

#include <pxr/pxr.h>

#if PXR_VERSION >= 2308

#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPlugin.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>

PXR_NAMESPACE_OPEN_SCOPE

// Must match "displayName" of HdMoonrayRendererPlugin in plugInfo.json.
static const char* const kRendererDisplayName = "Moonray";

// Not in an anonymous namespace: the TfType name derived from it must equal
// the type name declared in plugInfo.json.
class HdMoonray_ImplicitSurfaceSceneIndexPlugin : public HdSceneIndexPlugin
{
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr& inputScene,
        const HdContainerDataSourceHandle& inputArgs) override
    {
        const HdDataSourceBaseHandle toMesh =
            HdRetainedTypedSampledDataSource<TfToken>::New(
                HdsiImplicitSurfaceSceneIndexTokens->toMesh);

        const TfToken types[] = {
            HdPrimTypeTokens->sphere,
            HdPrimTypeTokens->cube,
            HdPrimTypeTokens->cone,
            HdPrimTypeTokens->cylinder,
            HdPrimTypeTokens->capsule,
#if PXR_VERSION >= 2411
            HdPrimTypeTokens->plane,
#endif
        };
        constexpr size_t count = sizeof(types) / sizeof(types[0]);
        HdDataSourceBaseHandle modes[count];
        for (size_t i = 0; i < count; ++i) {
            modes[i] = toMesh;
        }

        const HdContainerDataSourceHandle args =
            HdRetainedContainerDataSource::New(count, types, modes);

        return HdsiImplicitSurfaceSceneIndex::New(inputScene, args);
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdMoonray_ImplicitSurfaceSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    // Phase 0, at start: convert before any other scene index sees the prims,
    // matching where Storm inserts its equivalent.
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        kRendererDisplayName,
        TfToken("HdMoonray_ImplicitSurfaceSceneIndexPlugin"),
        nullptr,
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_VERSION >= 2308
