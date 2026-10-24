// Copyright Contributors to the Moonray Project
// SPDX-License-Identifier: Apache-2.0

// Translates RenderMan material networks into Moonray ones.
//
// A material authored for RenderMan carries an "ri" network
// (outputs:ri:surface) that Moonray cannot render, so hdMoonray falls back to
// the material's universal network, usually a UsdPreviewSurface. When a
// material has an ri surface and no moonray network, this scene index adds a
// moonray network built around a DwaBaseMaterial.
//
// Only the nodes and inputs _Translator checks for are translated. Any other
// network is left untranslated with one warning, and the material renders as
// it did before.
// Set HDMOONRAY_RENDERMAN_MATERIALS=0 to turn the translation off.
//
// RenderMan defaults below are from RenderMan 27.3's shipped .oso and .args
// files.

#include <pxr/pxr.h>

#if PXR_VERSION >= 2308

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/getenv.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndexPlugin.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/assetPath.h>

#include <scene_rdl2/render/logging/logging.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

// Must match "displayName" of HdMoonrayRendererPlugin in plugInfo.json.
static const char* const kRendererDisplayName = "Moonray";

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (ri)
    (moonray)

    (PxrLayerSurface)
    (PxrSurface)
    (PxrLayerMixer)
    (PxrLayer)
    (PxrMetallicWorkflow)
    (PxrTexture)
    (PxrNormalMap)
    (PxrRemap)
    (PxrColorCorrect)
    (PxrInvert)

    (pxrMaterialOut)
    (rendering)
    (srgb_texture)
    (data)
    (resultRGB)
    (resultR)
    (resultN)
    (resultDiffuseRGB)
    (resultSpecularFaceRGB)
    (resultSpecularEdgeRGB)

    (DwaBaseMaterial)
    (ImageMap)
    (OpMap)
    (ColorCorrectHsvMap)
    (ClampMap)
    (TransformNormalMap)
    (RgbToNormalMap)
    (out)
);

namespace {

using _Connection = HdMaterialNetworkInterface::InputConnection;

// Where a DwaBaseMaterial input gets its value: an ImageMap, or a constant.
struct _Source
{
    TfToken texture;
    VtValue value;

    bool operator==(const _Source& other) const
    {
        return texture == other.texture && value == other.value;
    }
};

// An ImageMap: a PxrTexture, with its colorScale and colorOffset and any
// PxrRemap or PxrColorCorrect after it folded into ImageMap's gain and offset.
struct _Image
{
    std::string filename;
    int linearize = 0;
    GfVec3f gain = GfVec3f(1.0f);
    GfVec3f offset = GfVec3f(0.0f);
    GfVec3f missing = GfVec3f(1.0f, 0.0f, 1.0f);
};

// A ColorCorrectHsvMap or ClampMap after the ImageMap called input. The hue
// offset and saturation factor are PxrColorCorrect's, for ColorCorrectHsvMap.
struct _Correction
{
    TfToken type;
    TfToken input;
    float hue = 0.0f;
    float saturation = 1.0f;
};

// An OpMap: op1 * op1Factor added to or multiplied by op2 * op2Factor.
struct _Op
{
    bool multiply;
    _Source op1, op1Factor, op2, op2Factor;
};

// The DwaBaseMaterial inputs, and the PxrNormalMap's ImageMap if there is one.
struct _Material
{
    int enableDiffuse = 1;
    _Source diffuse;
    float radius;
    GfVec3f radiusColor;
    _Source albedo;
    _Source metallic;
    int enableSpecular = 0;
    _Source specular;
    _Source roughness;
    _Source transmission;
    float rrRoughness = 0.1f;
    float rrIor = 1.5f;
    _Source presence;
    TfToken normalMap;
};

// A string, token or asset path value as a string; an asset path resolves
// when it can.
std::string
_AsString(const VtValue& value)
{
    if (value.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& path = value.UncheckedGet<SdfAssetPath>();
        return path.GetResolvedPath().empty() ? path.GetAssetPath()
                                              : path.GetResolvedPath();
    }
    if (value.IsHolding<TfToken>()) {
        return value.UncheckedGet<TfToken>().GetString();
    }
    if (value.IsHolding<std::string>()) {
        return value.UncheckedGet<std::string>();
    }
    return std::string();
}

// Reads an ri network and writes the equivalent moonray network, or explains
// why it can't.
class _Translator
{
public:
    explicit _Translator(const HdMaterialNetworkInterface& ri) : _ri(ri) {}

    // The moonray network, or null with the reason in GetError().
    HdContainerDataSourceHandle Translate();

    const std::string& GetError() const { return _error; }

private:
    bool _Fail(const TfToken& node, const std::string& reason);
    bool _CheckType(const TfToken& node, const TfToken& type);
    bool _CheckInputs(const TfToken& node,
                      std::initializer_list<const char*> allowed);
    bool _IsAuthored(const TfToken& node, const char* input) const;
    bool _GetConnection(const TfToken& node, const char* input,
                        _Connection* connection) const;
    bool _GetUpstream(const TfToken& node, const char* input,
                      const TfToken& type, const TfToken& output,
                      TfToken* upstream);
    template <typename T>
    bool _GetValue(const TfToken& node, const char* input, const T& fallback,
                   T* value);
    template <typename T>
    bool _GetSource(const TfToken& node, const char* input,
                    const T& fallback, _Source* source);
    bool _GetAdjustedImage(const TfToken& node, const char* input,
                           const TfToken& type, const TfToken& output,
                           _Source* source);
    bool _GetAdjustment(const TfToken& node, float* scale, float* offset,
                        bool* clamp);
    bool _GetImage(const TfToken& node, const char* input,
                   const TfToken& output, _Image* image);
    bool _GetFilename(const TfToken& node, const TfToken& colorspace,
                      std::string* filename);
    bool _GetSurface(const TfToken& surface, TfToken* layer, TfToken* glass,
                     _Source* mask);
    bool _GetLayer(const TfToken& mixer, TfToken* layer, TfToken* glass,
                   _Source* mask);
    bool _GetBase(const TfToken& layer, _Source* albedo, _Source* metallic,
                  _Source* specular, bool* hasWorkflow);
    bool _GetGlass(const TfToken& layer, int* enableRR, _Source* transmission);
    bool _GetMaskedGlass(const TfToken& glass, const _Source& mask, int enableRR,
                         _Material* material);
    bool _GetSubsurface(const TfToken& layer, const _Source& diffuse,
                        _Source* albedo, float* radius, GfVec3f* color);
    bool _GetNormalMap(const TfToken& layer, TfToken* normalMap);
    _Source _Combine(const TfToken& name, bool multiply,
                     const _Source& op1, const _Source& op1Factor,
                     const _Source& op2, const _Source& op2Factor);

    void _Set(const TfToken& node, const char* input, const _Source& source);
    void _Set(const TfToken& node, const char* input, const VtValue& value);
    HdContainerDataSourceHandle _Write(const TfToken& surface,
                                       const _Material& material);

    const HdMaterialNetworkInterface& _ri;
    std::unique_ptr<HdDataSourceMaterialNetworkInterface> _out;
    std::map<TfToken, _Image> _images;  // ImageMap name -> image
    std::map<TfToken, _Op> _ops;        // OpMap name -> operation
    std::map<TfToken, _Correction> _corrections; // node name -> correction
    std::string _error;
};

bool
_Translator::_Fail(const TfToken& node, const std::string& reason)
{
    _error = TfStringPrintf("%s (%s): %s", node.GetText(),
                            _ri.GetNodeType(node).GetText(), reason.c_str());
    return false;
}

bool
_Translator::_CheckType(const TfToken& node, const TfToken& type)
{
    if (_ri.GetNodeType(node) != type) {
        return _Fail(node, "expected a " + type.GetString());
    }
    return true;
}

bool
_Translator::_CheckInputs(const TfToken& node,
                          std::initializer_list<const char*> allowed)
{
    TfTokenVector inputs = _ri.GetAuthoredNodeParameterNames(node);
    const TfTokenVector connected = _ri.GetNodeInputConnectionNames(node);
    inputs.insert(inputs.end(), connected.begin(), connected.end());
    for (const TfToken& input : inputs) {
        bool found = false;
        for (const char* name : allowed) {
            found = found || input == name;
        }
        if (!found) {
            return _Fail(node, "input " + input.GetString() + " is not supported");
        }
    }
    return true;
}

bool
_Translator::_IsAuthored(const TfToken& node, const char* input) const
{
    _Connection connection;
    return !_ri.GetNodeParameterValue(node, TfToken(input)).IsEmpty() ||
           _GetConnection(node, input, &connection);
}

bool
_Translator::_GetConnection(const TfToken& node, const char* input,
                            _Connection* connection) const
{
    const auto connections = _ri.GetNodeInputConnection(node, TfToken(input));
    if (connections.empty()) {
        return false;
    }
    *connection = connections[0];
    return true;
}

// The node connected to input, which must be the output of a node of type.
bool
_Translator::_GetUpstream(const TfToken& node, const char* input,
                          const TfToken& type, const TfToken& output,
                          TfToken* upstream)
{
    _Connection connection;
    if (!_GetConnection(node, input, &connection) ||
        _ri.GetNodeType(connection.upstreamNodeName) != type ||
        connection.upstreamOutputName != output) {
        return _Fail(node, TfStringPrintf("input %s must be connected to a %s %s", input,
                                          type.GetText(), output.GetText()));
    }
    *upstream = connection.upstreamNodeName;
    return true;
}

// The value of an unconnected input, or fallback when it is unauthored.
template <typename T>
bool
_Translator::_GetValue(const TfToken& node, const char* input,
                       const T& fallback, T* value)
{
    _Connection connection;
    if (_GetConnection(node, input, &connection)) {
        return _Fail(node, TfStringPrintf("input %s must not be connected", input));
    }
    const VtValue authored = _ri.GetNodeParameterValue(node, TfToken(input));
    if (authored.IsEmpty()) {
        *value = fallback;
        return true;
    }
    const VtValue cast = VtValue::Cast<T>(authored);
    if (cast.IsEmpty()) {
        return _Fail(node, TfStringPrintf("input %s has an unexpected type", input));
    }
    *value = cast.UncheckedGet<T>();
    return true;
}

// A float or colour input: a texture, possibly through a PxrRemap or
// PxrColorCorrect, or a constant. The ImageMap is named after the node that
// feeds the input.
template <typename T>
bool
_Translator::_GetSource(const TfToken& node, const char* input,
                        const T& fallback, _Source* source)
{
    const TfToken& output = std::is_same<T, float>::value ?
        _tokens->resultR : _tokens->resultRGB;

    _Connection connection;
    if (!_GetConnection(node, input, &connection)) {
        T value;
        if (!_GetValue(node, input, fallback, &value)) {
            return false;
        }
        *source = {TfToken(), VtValue(value)};
        return true;
    }

    const TfToken& name = connection.upstreamNodeName;
    const TfToken type = _ri.GetNodeType(name);
    if (type == _tokens->PxrRemap || type == _tokens->PxrColorCorrect) {
        return _GetAdjustedImage(node, input, type, output, source);
    }
    _Image image;
    if (!_GetImage(node, input, output, &image)) {
        return false;
    }
    _images[name] = image;
    *source = {name, VtValue()};
    return true;
}

// A texture through a PxrRemap or PxrColorCorrect, as an ImageMap followed by
// a ColorCorrectHsvMap or ClampMap when needed.
bool
_Translator::_GetAdjustedImage(const TfToken& node, const char* input,
                               const TfToken& type, const TfToken& output,
                               _Source* source)
{
    TfToken name;
    _Image image;
    float scale, offset;
    bool clamp;
    if (!_GetUpstream(node, input, type, output, &name) ||
        !_GetAdjustment(name, &scale, &offset, &clamp) ||
        !_GetImage(name, "inputRGB", _tokens->resultRGB, &image)) {
        return false;
    }
    image.gain *= scale;
    image.offset = image.offset * scale + GfVec3f(offset);
    _images[name] = image;
    *source = {name, VtValue()};

    // The HSV correction comes before exposure in PxrColorCorrect, but it
    // scales with its input, so it can follow ImageMap's gain.
    GfVec3f hsv(0.0f, 1.0f, 1.0f);
    if (type == _tokens->PxrColorCorrect &&
        !_GetValue(name, "hsv", hsv, &hsv)) {
        return false;
    }
    if (hsv[2] != 1.0f) {
        return _Fail(name, "only an hsv value of 1 is supported");
    }
    if (hsv[0] != 0.0f || hsv[1] != 1.0f) {
        const TfToken hsvName(name.GetString() + "_hsv");
        _corrections[hsvName] = {_tokens->ColorCorrectHsvMap, name, hsv[0],
                                 hsv[1]};
        *source = {hsvName, VtValue()};
    }
    if (clamp) {
        const TfToken clampName(name.GetString() + "_clamp");
        _corrections[clampName] = {_tokens->ClampMap, name};
        *source = {clampName, VtValue()};
    }
    return true;
}

// A PxrRemap or PxrColorCorrect as scale * x + offset, with clamp set when
// PxrRemap has an input range. Only inputs that keep it linear are supported.
// For inputs in [0, 1], dropping RenderMan's other clamps only changes results
// outside [0, 1], which DwaBaseMaterial clamps.
bool
_Translator::_GetAdjustment(const TfToken& node, float* scale, float* offset,
                            bool* clamp)
{
    *clamp = false;
    if (_ri.GetNodeType(node) == _tokens->PxrRemap) {
        float inMin, inMax, min, max;
        if (!_CheckInputs(node, {"inputRGB", "inputMin", "inputMax",
                                 "outputMin", "outputMax"}) ||
            !_GetValue(node, "inputMin", 0.0f, &inMin) ||
            !_GetValue(node, "inputMax", 1.0f, &inMax) ||
            !_GetValue(node, "outputMin", 0.0f, &min) ||
            !_GetValue(node, "outputMax", 1.0f, &max)) {
            return false;
        }
        if (inMin == inMax) {
            return _Fail(node, "inputMin must differ from inputMax");
        }
        *clamp = inMin != 0.0f || inMax != 1.0f;
        if (*clamp && (min != 0.0f || max != 1.0f)) {
            return _Fail(node, "an input range needs the default output range");
        }
        *scale = (max - min) / (inMax - inMin);
        *offset = min - inMin * *scale;
        return true;
    }
    float exposure;
    if (!_CheckInputs(node, {"inputRGB", "exposure", "hsv"}) ||
        !_GetValue(node, "exposure", 0.0f, &exposure)) {
        return false;
    }
    *scale = std::exp2(exposure);
    *offset = 0.0f;
    return true;
}

// alphaScale and alphaOffset only change resultA, which is never used.
bool
_Translator::_GetImage(const TfToken& node, const char* input,
                       const TfToken& output, _Image* image)
{
    TfToken texture;
    return _GetUpstream(node, input, _tokens->PxrTexture, output, &texture) &&
           _CheckInputs(texture, {"filename", "filename_colorspace", "linearize",
                                  "colorScale", "colorOffset", "missingColor",
                                  "alphaScale", "alphaOffset"}) &&
           _GetFilename(texture, TfToken(), &image->filename) &&
           _GetValue(texture, "linearize", 0, &image->linearize) &&
           _GetValue(texture, "colorScale", GfVec3f(1.0f), &image->gain) &&
           _GetValue(texture, "colorOffset", GfVec3f(0.0f), &image->offset) &&
           _GetValue(texture, "missingColor", GfVec3f(1.0f, 0.0f, 1.0f),
                     &image->missing);
}

// The filename of a PxrTexture or PxrNormalMap whose filename_colorspace is
// colorspace, or "rendering", "data" or unset when colorspace is empty. A .tex
// file may also be "srgb_texture" when colorspace is empty, or unset when it
// is not. RenderMan reads .tex texels as stored whatever the colorspace; the
// pipeline bakes colour maps to the rendering space when it makes the .tex
// files.
bool
_Translator::_GetFilename(const TfToken& node, const TfToken& colorspace,
                          std::string* filename)
{
    *filename = _AsString(_ri.GetNodeParameterValue(node, TfToken("filename")));
    const TfToken space(
        _AsString(_ri.GetNodeParameterValue(node, TfToken("filename_colorspace"))));
    const bool tex = TfStringEndsWith(*filename, ".tex");
    const bool supported = colorspace.IsEmpty() ?
        space.IsEmpty() || space == _tokens->rendering || space == _tokens->data ||
            (space == _tokens->srgb_texture && tex) :
        space == colorspace || (space.IsEmpty() && tex);
    if (!supported) {
        return _Fail(node, "filename_colorspace '" + space.GetString() +
                           "' is not supported");
    }
    if (filename->empty()) {
        return _Fail(node, "filename is empty");
    }
    return true;
}

// PxrLayerSurface <- PxrLayerMixer <- PxrLayer, or a PxrSurface with only
// diffuseColor set, read as a PxrLayer since it renders as one.
// diffuseDoubleSided is ignored: MoonRay lights both sides of double-sided
// geometry, as RenderMan does when it is on.
bool
_Translator::_GetSurface(const TfToken& surface, TfToken* layer, TfToken* glass,
                         _Source* mask)
{
    TfToken mixer;
    if (_ri.GetNodeType(surface) == _tokens->PxrSurface) {
        if (!_CheckInputs(surface,
                          {"diffuseColor", "presence", "diffuseDoubleSided"})) {
            return false;
        }
        *layer = surface;
    } else {
        if (!_CheckType(surface, _tokens->PxrLayerSurface) ||
            !_CheckInputs(surface,
                          {"inputMaterial", "presence", "diffuseDoubleSided"}) ||
            !_GetUpstream(surface, "inputMaterial", _tokens->PxrLayerMixer,
                          _tokens->pxrMaterialOut, &mixer) ||
            !_GetLayer(mixer, layer, glass, mask) ||
            !_CheckInputs(*layer, {"enableDiffuse", "enableSpecular", "specularGain",
                                   "diffuseColor", "specularFaceColor",
                                   "specularEdgeColor", "specularRoughness",
                                   "bumpNormal", "enableRR", "rrRefractionGain",
                                   "rrReflectionGain", "rrRoughness", "rrIor",
                                   "diffuseGain", "enableSubsurface", "subsurfaceGain",
                                   "subsurfaceColor", "subsurfaceDmfp",
                                   "subsurfaceDmfpColor", "subsurfacePostTint",
                                   "subsurfaceTransmitGain", "enableSinglescatter",
                                   "singlescatterGain", "singlescatterColor",
                                   "singlescatterMfp", "singlescatterMfpColor",
                                   "singlescatterDirectionality", "singlescatterIor",
                                   "singlescatterBlur", "singlescatterDirectGain",
                                   "singlescatterDirectGainTint", "enableGlow",
                                   "glowGain", "glowColor"})) {
            return false;
        }
    }
    // RenderMan ignores the single scatter and glow inputs unless their lobe
    // is enabled, and these lobes are not translated.
    for (const char* lobe : {"enableSinglescatter", "enableGlow"}) {
        int enabled = 0;
        if (!_GetValue(*layer, lobe, 0, &enabled)) {
            return false;
        }
        if (enabled) {
            return _Fail(*layer, std::string(lobe) + " is not supported");
        }
    }
    return true;
}

// The layer RenderMan shows: the highest-numbered enabled layer, else the
// base layer. Masks default to 1, so a layer covers everything below it.
// When that is layer1 with a layer1Mask, it is the base layer, with layer1
// as glass blended in by mask (see _GetMaskedGlass).
bool
_Translator::_GetLayer(const TfToken& mixer, TfToken* layer, TfToken* glass,
                       _Source* mask)
{
    if (!_CheckInputs(mixer, {"baselayer", "layer1", "layer1Enabled", "layer1Mask",
                              "layer2", "layer2Enabled",
                              "layer3", "layer3Enabled", "layer4", "layer4Enabled"})) {
        return false;
    }
    _Connection connection;
    if (!_GetConnection(mixer, "baselayer", &connection)) {
        return _Fail(mixer, "baselayer must be connected");
    }
    std::string chosen = "baselayer";
    for (int i = 1; i <= 4; ++i) {
        const std::string name = TfStringPrintf("layer%d", i);
        int enabled = 0;
        if (!_GetValue(mixer, (name + "Enabled").c_str(), i == 1 ? 1 : 0, &enabled)) {
            return false;
        }
        if (enabled) {
            if (!_GetConnection(mixer, name.c_str(), &connection)) {
                return _Fail(mixer, name + " is enabled but not connected");
            }
            chosen = name;
        }
    }
    if (chosen == "layer1" && _IsAuthored(mixer, "layer1Mask")) {
        if (!_GetUpstream(mixer, "layer1", _tokens->PxrLayer,
                          _tokens->pxrMaterialOut, glass) ||
            !_GetSource(mixer, "layer1Mask", 1.0f, mask)) {
            return false;
        }
        chosen = "baselayer";
    }
    return _GetUpstream(mixer, chosen.c_str(), _tokens->PxrLayer,
                        _tokens->pxrMaterialOut, layer);
}

// Base colour and metallic. PxrMetallicWorkflow feeds diffuse
// base*(1-m) and specular face/edge mix(0.04, base, m) / mix(1, base, m);
// DwaBaseMaterial computes the same from albedo and metallic.
bool
_Translator::_GetBase(const TfToken& layer, _Source* albedo, _Source* metallic,
                      _Source* specular, bool* hasWorkflow)
{
    _Connection connection;
    *metallic = {TfToken(), VtValue(0.0f)};
    *hasWorkflow =
        _GetConnection(layer, "diffuseColor", &connection) &&
        _ri.GetNodeType(connection.upstreamNodeName) == _tokens->PxrMetallicWorkflow;
    if (*hasWorkflow) {
        const TfToken workflow = connection.upstreamNodeName;
        const std::pair<const char*, TfToken> outputs[] = {
            {"diffuseColor", _tokens->resultDiffuseRGB},
            {"specularFaceColor", _tokens->resultSpecularFaceRGB},
            {"specularEdgeColor", _tokens->resultSpecularEdgeRGB},
        };
        for (const auto& [input, output] : outputs) {
            if (!_GetConnection(layer, input, &connection) ||
                connection.upstreamNodeName != workflow ||
                connection.upstreamOutputName != output) {
                return _Fail(layer, std::string(input) + " must be connected to " +
                                    workflow.GetString() + "." + output.GetString());
            }
        }
        if (!_CheckInputs(workflow, {"baseColor", "metallic"}) ||
            !_GetSource(workflow, "baseColor", GfVec3f(0.2f, 0.5f, 0.8f), albedo) ||
            !_GetSource(workflow, "metallic", 0.0f, metallic) ||
            !_GetSource(layer, "specularGain", 0.0f, specular)) {
            return false;
        }
    } else {
        // Without a workflow, only the default black specular colours are
        // supported: RenderMan renders no specular from them.
        GfVec3f face, edge;
        if (!_GetSource(layer, "diffuseColor", GfVec3f(0.18f), albedo) ||
            !_GetValue(layer, "specularFaceColor", GfVec3f(0.0f), &face) ||
            !_GetValue(layer, "specularEdgeColor", GfVec3f(0.0f), &edge)) {
            return false;
        }
        if (face != GfVec3f(0.0f) || edge != GfVec3f(0.0f)) {
            return _Fail(layer, "specular colours need a PxrMetallicWorkflow");
        }
        *specular = {TfToken(), VtValue(0.0f)};
    }
    return true;
}

// Glass (enableRR). The pipeline drives refraction and reflection from one
// transparency map and diffuseGain from its inverse. DwaBaseMaterial layers
// transmission over diffuse, so that inverse is implied and dropped.
bool
_Translator::_GetGlass(const TfToken& layer, int* enableRR, _Source* transmission)
{
    if (!_GetValue(layer, "enableRR", 0, enableRR)) {
        return false;
    }
    if (!*enableRR) {
        // RenderMan ignores the rr inputs, but not diffuseGain.
        *transmission = {TfToken(), VtValue(0.0f)};
        return !_IsAuthored(layer, "diffuseGain") ||
               _Fail(layer, "diffuseGain is only supported with enableRR");
    }

    _Source reflection;
    if (!_GetSource(layer, "rrRefractionGain", 0.0f, transmission) ||
        !_GetSource(layer, "rrReflectionGain", 0.0f, &reflection)) {
        return false;
    }
    if (!(reflection == *transmission)) {
        return _Fail(layer, "rrReflectionGain must match rrRefractionGain");
    }

    if (!_IsAuthored(layer, "diffuseGain")) {
        return true;
    }
    TfToken invert;
    _Source inverted;
    if (!_GetUpstream(layer, "diffuseGain", _tokens->PxrInvert,
                      _tokens->resultR, &invert) ||
        !_CheckInputs(invert, {"inputRGB"}) ||
        !_GetSource(invert, "inputRGB", GfVec3f(0.0f), &inverted)) {
        return false;
    }
    if (inverted.texture.IsEmpty() || inverted.texture != transmission->texture) {
        return _Fail(invert, "inputRGB must be the rrRefractionGain texture");
    }
    return true;
}

// A clear glass layer1 blended over the base layer by layer1Mask. The mask,
// times rrRefractionGain, becomes DwaBaseMaterial's transmission, which
// swaps diffuse for refraction. Reflection keeps the base layer's specular
// and roughness, so rrReflectionGain must be 1.
bool
_Translator::_GetMaskedGlass(const TfToken& glass, const _Source& mask,
                             int enableRR, _Material* material)
{
    if (enableRR) {
        return _Fail(glass, "the base layer under a layer1Mask must not have enableRR");
    }
    if (!material->enableSpecular) {
        return _Fail(glass, "the base layer under a layer1Mask needs enableSpecular");
    }
    // diffuseGain does nothing with enableDiffuse off.
    int diffuse, specular, rr;
    float gain, reflection;
    if (!_CheckInputs(glass, {"enableDiffuse", "diffuseGain", "enableSpecular",
                              "enableRR", "rrRefractionGain", "rrReflectionGain",
                              "rrRoughness", "rrIor"}) ||
        !_GetValue(glass, "enableDiffuse", 1, &diffuse) ||
        !_GetValue(glass, "enableSpecular", 0, &specular) ||
        !_GetValue(glass, "enableRR", 0, &rr) ||
        !_GetValue(glass, "rrRefractionGain", 0.0f, &gain) ||
        !_GetValue(glass, "rrReflectionGain", 0.0f, &reflection) ||
        !_GetValue(glass, "rrRoughness", 0.1f, &material->rrRoughness) ||
        !_GetValue(glass, "rrIor", 1.5f, &material->rrIor)) {
        return false;
    }
    if (diffuse || specular || !rr || reflection != 1.0f) {
        return _Fail(glass, "the layer over a layer1Mask must be clear glass: "
                            "enableRR, no diffuse or specular, rrReflectionGain 1");
    }
    if (mask.texture.IsEmpty()) {
        material->transmission = {TfToken(),
                                  VtValue(mask.value.UncheckedGet<float>() * gain)};
    } else if (gain == 1.0f) {
        material->transmission = mask;
    } else {
        material->transmission =
            _Combine(TfToken(glass.GetString() + "_transmission"), true, mask,
                     {TfToken(), VtValue(gain)}, {TfToken(), VtValue(GfVec3f(1.0f))},
                     {TfToken(), VtValue(1.0f)});
    }
    return true;
}

// Subsurface. DwaBaseMaterial has one diffuse lobe, so the whole lobe
// scatters, with albedo diffuse + gain * colour * gain * tint (clamped to 1).
// subsurfaceColor may share diffuseColor's connection, such as a
// PxrMetallicWorkflow's resultDiffuseRGB, and then reads the same as diffuse.
// subsurfaceTransmitGain needs subsurfaceDoubleSided, which is never set here.
// scattering_radius is half RenderMan's mean free path (subsurfaceDmfp *
// subsurfaceDmfpColor * 0.1), which matched its shadow edge blur in tests.
bool
_Translator::_GetSubsurface(const TfToken& layer, const _Source& diffuse,
                            _Source* albedo, float* radius, GfVec3f* color)
{
    int enable = 0;
    if (!_GetValue(layer, "enableSubsurface", 0, &enable)) {
        return false;
    }
    *albedo = diffuse;
    *radius = 0.0f;
    *color = GfVec3f(1.0f);
    if (!enable) {
        return true;
    }

    _Connection fromSubsurface, fromDiffuse;
    const bool shared =
        _GetConnection(layer, "subsurfaceColor", &fromSubsurface) &&
        _GetConnection(layer, "diffuseColor", &fromDiffuse) &&
        fromSubsurface.upstreamNodeName == fromDiffuse.upstreamNodeName &&
        fromSubsurface.upstreamOutputName == fromDiffuse.upstreamOutputName;
    _Source gain, subsurfaceColor = diffuse, tint;
    float dmfp;
    GfVec3f dmfpColor;
    if (!_GetSource(layer, "subsurfaceGain", 0.0f, &gain) ||
        !(shared || _GetSource(layer, "subsurfaceColor",
                               GfVec3f(0.83f, 0.791f, 0.753f), &subsurfaceColor)) ||
        !_GetSource(layer, "subsurfacePostTint", GfVec3f(1.0f), &tint) ||
        !_GetValue(layer, "subsurfaceDmfp", 10.0f, &dmfp) ||
        !_GetValue(layer, "subsurfaceDmfpColor",
                   GfVec3f(0.851f, 0.557f, 0.395f), &dmfpColor)) {
        return false;
    }
    const _Source one = {TfToken(), VtValue(1.0f)};
    const _Source subsurface =
        _Combine(TfToken(layer.GetString() + "_subsurface"), true,
                 subsurfaceColor, gain, tint, gain);
    *albedo = _Combine(TfToken(layer.GetString() + "_albedo"), false,
                       diffuse, one, subsurface, one);

    // scattering_color is clamped to 1, so carry its scale in the radius.
    const float longest = std::max({dmfpColor[0], dmfpColor[1], dmfpColor[2]});
    if (longest > 0.0f) {
        *radius = 0.5f * dmfp * 0.1f * longest;
        *color = dmfpColor / longest;
    }
    return true;
}

// PxrNormalMap decodes texels as 2*t-1, scales red and green by
// bumpScale and, on the left-handed meshes Houdini writes, tilts the
// normal towards +s for red and -t for green with its default orientation
// (2, Custom); orientation 0 (OpenGL) flips red and green from that, and
// 1 (DirectX) flips red. TransformNormalMap tilts it towards +s and +t,
// so the ImageMap applies those signs and the scale. A missing texture or
// tile decodes to the unbent normal.
bool
_Translator::_GetNormalMap(const TfToken& layer, TfToken* normalMap)
{
    if (!_IsAuthored(layer, "bumpNormal")) {
        return true;
    }
    if (!_GetUpstream(layer, "bumpNormal", _tokens->PxrNormalMap,
                      _tokens->resultN, normalMap)) {
        return false;
    }
    _Image& image = _images[*normalMap];
    int orientation;
    float scale;
    if (!_CheckInputs(*normalMap, {"filename", "filename_colorspace",
                                    "orientation", "bumpScale"}) ||
        !_GetFilename(*normalMap, _tokens->data, &image.filename) ||
        !_GetValue(*normalMap, "orientation", 2, &orientation) ||
        !_GetValue(*normalMap, "bumpScale", 1.0f, &scale)) {
        return false;
    }
    if (orientation < 0 || orientation > 2) {
        return _Fail(*normalMap, "orientation must be 0, 1 or 2");
    }
    // Red and green signs for orientations 0, 1 and 2.
    static const float signs[3][2] = {{-1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, -1.0f}};
    const float red = signs[orientation][0] * scale;
    const float green = signs[orientation][1] * scale;
    image.gain = GfVec3f(red, green, 1.0f);
    image.offset = GfVec3f((1.0f - red) / 2.0f, (1.0f - green) / 2.0f, 0.0f);
    image.missing = GfVec3f(0.5f, 0.5f, 1.0f);
    return true;
}

// The source for op1 * op1Factor (op) op2 * op2Factor: a constant when all
// four are constants, otherwise an OpMap called name.
_Source
_Translator::_Combine(const TfToken& name, bool multiply,
                      const _Source& op1, const _Source& op1Factor,
                      const _Source& op2, const _Source& op2Factor)
{
    if (op1.texture.IsEmpty() && op1Factor.texture.IsEmpty() &&
        op2.texture.IsEmpty() && op2Factor.texture.IsEmpty()) {
        const GfVec3f a = op1.value.UncheckedGet<GfVec3f>() *
                          op1Factor.value.UncheckedGet<float>();
        const GfVec3f b = op2.value.UncheckedGet<GfVec3f>() *
                          op2Factor.value.UncheckedGet<float>();
        return {TfToken(), VtValue(multiply ? GfCompMult(a, b) : a + b)};
    }
    _ops[name] = {multiply, op1, op1Factor, op2, op2Factor};
    return {name, VtValue()};
}

void
_Translator::_Set(const TfToken& node, const char* input, const _Source& source)
{
    if (source.texture.IsEmpty()) {
        _Set(node, input, source.value);
    } else {
        _out->SetNodeInputConnection(node, TfToken(input),
                                     {{source.texture, _tokens->out}});
    }
}

void
_Translator::_Set(const TfToken& node, const char* input, const VtValue& value)
{
    _out->SetNodeParameterValue(node, TfToken(input), value);
}

HdContainerDataSourceHandle
_Translator::_Write(const TfToken& surface, const _Material& material)
{
    // Write the moonray network. Nodes keep their RenderMan names.
    _out = std::make_unique<HdDataSourceMaterialNetworkInterface>(
        _ri.GetMaterialPrimPath(), HdRetainedContainerDataSource::New(), nullptr);

    _out->SetNodeType(surface, _tokens->DwaBaseMaterial);
    _Set(surface, "show_diffuse", VtValue(material.enableDiffuse != 0));
    _Set(surface, "albedo", material.diffuse);
    _Set(surface, "diffuse_roughness", VtValue(0.0f));
    _Set(surface, "bssrdf", VtValue(TfToken("normalized diffusion")));
    _Set(surface, "scattering_radius", VtValue(material.radius));
    _Set(surface, "scattering_color", VtValue(material.radiusColor));
    _Set(surface, "metallic", material.metallic);
    _Set(surface, "metallic_color", material.albedo);
    _Set(surface, "metallic_edge_color", material.albedo);
    _Set(surface, "show_specular", VtValue(material.enableSpecular != 0));
    _Set(surface, "specular", material.specular);
    _Set(surface, "refractive_index", VtValue(1.5f));
    _Set(surface, "specular_model", VtValue(TfToken("Beckmann")));
    _Set(surface, "roughness", material.roughness);
    // PxrLayer rrRefractionColor 1, thinGlass 0.
    _Set(surface, "show_transmission", VtValue(true));
    _Set(surface, "transmission", material.transmission);
    _Set(surface, "transmission_color", VtValue(GfVec3f(1.0f)));
    _Set(surface, "use_independent_transmission_roughness", VtValue(true));
    _Set(surface, "independent_transmission_roughness", VtValue(material.rrRoughness));
    _Set(surface, "use_independent_transmission_refractive_index", VtValue(true));
    _Set(surface, "independent_transmission_refractive_index", VtValue(material.rrIor));
    _Set(surface, "thin_geometry", VtValue(false));
    _Set(surface, "presence", material.presence);
    _out->SetTerminalConnection(HdMaterialTerminalTokens->surface,
                                {surface, HdMaterialTerminalTokens->surface});

    // PxrTexture applies colorScale and colorOffset to missingColor too, and
    // so does ImageMap's gain and offset to default_color.
    for (const auto& [name, image] : _images) {
        _out->SetNodeType(name, _tokens->ImageMap);
        _Set(name, "texture", VtValue(image.filename));
        _Set(name, "gamma", VtValue(TfToken("off")));
        _Set(name, "wrap_around", VtValue(false));
        _Set(name, "use_default_color_when_missing", VtValue(true));
        _Set(name, "default_color", VtValue(image.missing));
        // PxrTexture's linearize decodes sRGB before colorScale and
        // colorOffset; ImageMap's gamma comes before its gain and offset and
        // is a 2.2 power, within 0.009 of the sRGB curve.
        if (image.linearize) {
            _Set(name, "gamma_enabled", VtValue(true));
            _Set(name, "gamma_adjust", VtValue(GfVec3f(1.0f / 2.2f)));
        }
        if (image.gain != GfVec3f(1.0f) || image.offset != GfVec3f(0.0f)) {
            _Set(name, "gain_offset_enabled", VtValue(true));
            _Set(name, "gain", VtValue(image.gain));
            _Set(name, "offset_adjust", VtValue(image.offset));
        }
    }

    for (const auto& [name, op] : _ops) {
        _out->SetNodeType(name, _tokens->OpMap);
        _Set(name, "operation", VtValue(TfToken(op.multiply ? "multiply" : "add")));
        _Set(name, "op1", op.op1);
        _Set(name, "op1_factor", op.op1Factor);
        _Set(name, "op2", op.op2);
        _Set(name, "op2_factor", op.op2Factor);
    }

    // RenderMan's hue offset is in turns, ColorCorrectHsvMap's in degrees.
    // RenderMan also clamps the saturation to 1, which this does not.
    for (const auto& [name, correction] : _corrections) {
        _out->SetNodeType(name, correction.type);
        _out->SetNodeInputConnection(name, TfToken("input"),
                                     {{correction.input, _tokens->out}});
        if (correction.type == _tokens->ColorCorrectHsvMap) {
            _Set(name, "hue_shift", VtValue(correction.hue * 360.0f));
            _Set(name, "saturation_factor", VtValue(correction.saturation));
        }
    }

    if (!material.normalMap.IsEmpty()) {
        const TfToken tangent(material.normalMap.GetString() + "_tangent");
        const TfToken normal(material.normalMap.GetString() + "_normal");
        _out->SetNodeType(tangent, _tokens->TransformNormalMap);
        _out->SetNodeInputConnection(tangent, TfToken("input_normal"),
                                     {{material.normalMap, _tokens->out}});
        _out->SetNodeType(normal, _tokens->RgbToNormalMap);
        _out->SetNodeInputConnection(normal, TfToken("input"),
                                     {{tangent, _tokens->out}});
        _out->SetNodeInputConnection(surface, TfToken("input_normal"),
                                     {{normal, _tokens->out}});
        _Set(surface, "input_normal_dial", VtValue(1.0f));
    }

    return _out->Finish();
}

HdContainerDataSourceHandle
_Translator::Translate()
{
    for (const TfToken& terminal : {HdMaterialTerminalTokens->displacement,
                                    HdMaterialTerminalTokens->volume}) {
        if (_ri.GetTerminalConnection(terminal).first) {
            _error = "has a " + terminal.GetString() + " terminal";
            return nullptr;
        }
    }

    const TfToken surface =
        _ri.GetTerminalConnection(HdMaterialTerminalTokens->surface)
            .second.upstreamNodeName;
    TfToken layer, glass;
    _Source mask;
    _Material material;
    bool hasWorkflow;
    if (!_GetSurface(surface, &layer, &glass, &mask) ||
        !_GetSource(surface, "presence", 1.0f, &material.presence) ||
        !_GetBase(layer, &material.albedo, &material.metallic, &material.specular,
                  &hasWorkflow)) {
        return nullptr;
    }

    int enableRR = 0;
    if (!_GetValue(layer, "enableDiffuse", 1, &material.enableDiffuse) ||
        !_GetValue(layer, "enableSpecular", 0, &material.enableSpecular) ||
        !_GetSource(layer, "specularRoughness", 0.2f, &material.roughness) ||
        !_GetGlass(layer, &enableRR, &material.transmission) ||
        !_GetValue(layer, "rrRoughness", 0.1f, &material.rrRoughness) ||
        !_GetValue(layer, "rrIor", 1.5f, &material.rrIor) ||
        (!glass.IsEmpty() && !_GetMaskedGlass(glass, mask, enableRR, &material))) {
        return nullptr;
    }
    if (!_GetSubsurface(layer, material.albedo, &material.diffuse,
                        &material.radius, &material.radiusColor)) {
        return nullptr;
    }
    if (!material.enableDiffuse && !(material.diffuse == material.albedo)) {
        _Fail(layer, "enableSubsurface needs enableDiffuse");
        return nullptr;
    }
    // RenderMan's glass reflects even without specular colours or
    // enableSpecular; DwaBaseMaterial reflects only through its specular lobe.
    // rrReflectionGain matches rrRefractionGain (see _GetGlass).
    if (enableRR && !hasWorkflow) {
        material.enableSpecular = 1;
        material.specular = material.transmission;
    }

    if (!_GetNormalMap(layer, &material.normalMap)) {
        return nullptr;
    }

    return _Write(surface, material);
}

} // anonymous namespace

TF_DECLARE_REF_PTRS(HdMoonray_RenderManMaterialSceneIndex);

class HdMoonray_RenderManMaterialSceneIndex final
    : public HdSingleInputFilteringSceneIndexBase
{
public:
    static HdMoonray_RenderManMaterialSceneIndexRefPtr New(
        const HdSceneIndexBaseRefPtr& inputScene)
    {
        return TfCreateRefPtr(new HdMoonray_RenderManMaterialSceneIndex(inputScene));
    }

    HdSceneIndexPrim GetPrim(const SdfPath& primPath) const override
    {
        HdSceneIndexPrim prim = _GetInputSceneIndex()->GetPrim(primPath);
        if (prim.primType != HdPrimTypeTokens->material || !prim.dataSource) {
            return prim;
        }
        const HdContainerDataSourceHandle material =
            HdMaterialSchema::GetFromParent(prim.dataSource).GetContainer();
        if (!material || material->Get(_tokens->moonray)) {
            return prim;
        }
        const HdContainerDataSourceHandle riNetwork =
            HdContainerDataSource::Cast(material->Get(_tokens->ri));
        if (!riNetwork) {
            return prim;
        }
        const HdDataSourceMaterialNetworkInterface ri(
            primPath, riNetwork, prim.dataSource);
        if (!ri.GetTerminalConnection(HdMaterialTerminalTokens->surface).first) {
            return prim;
        }

        _Translator translator(ri);
        const HdContainerDataSourceHandle network = translator.Translate();
        if (!network) {
            _WarnOnce(primPath, translator.GetError());
            return prim;
        }
        prim.dataSource = HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(
                HdMaterialSchema::GetSchemaToken(),
                HdRetainedContainerDataSource::New(_tokens->moonray, network)),
            prim.dataSource);
        return prim;
    }

    SdfPathVector GetChildPrimPaths(const SdfPath& primPath) const override
    {
        return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
    }

protected:
    HdMoonray_RenderManMaterialSceneIndex(const HdSceneIndexBaseRefPtr& inputScene)
        : HdSingleInputFilteringSceneIndexBase(inputScene)
    {
    }

    void _PrimsAdded(const HdSceneIndexBase&,
                     const HdSceneIndexObserver::AddedPrimEntries& entries) override
    {
        for (const auto& entry : entries) {
            _ForgetWarning(entry.primPath);
        }
        _SendPrimsAdded(entries);
    }

    void _PrimsRemoved(const HdSceneIndexBase&,
                       const HdSceneIndexObserver::RemovedPrimEntries& entries) override
    {
        _SendPrimsRemoved(entries);
    }

    // The moonray network is computed from the ri one.
    void _PrimsDirtied(const HdSceneIndexBase&,
                       const HdSceneIndexObserver::DirtiedPrimEntries& entries) override
    {
        static const HdDataSourceLocator riLocator =
            HdMaterialSchema::GetDefaultLocator().Append(_tokens->ri);
        static const HdDataSourceLocator moonrayLocator =
            HdMaterialSchema::GetDefaultLocator().Append(_tokens->moonray);

        std::optional<HdSceneIndexObserver::DirtiedPrimEntries> edited;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].dirtyLocators.Intersects(riLocator)) {
                if (!edited) {
                    edited = entries;
                }
                (*edited)[i].dirtyLocators.insert(moonrayLocator);
                _ForgetWarning(entries[i].primPath);
            }
        }
        _SendPrimsDirtied(edited ? *edited : entries);
    }

private:
    // GetPrim runs many times per material; warn once until it changes.
    void _WarnOnce(const SdfPath& primPath, const std::string& reason) const
    {
        {
            std::lock_guard<std::mutex> lock(_warnedMutex);
            if (!_warned.insert(primPath).second) {
                return;
            }
        }
        scene_rdl2::logging::Logger::warn(
            primPath, ": not translating the RenderMan material, ", reason,
            ". Using its universal material network instead.");
    }

    void _ForgetWarning(const SdfPath& primPath)
    {
        std::lock_guard<std::mutex> lock(_warnedMutex);
        _warned.erase(primPath);
    }

    mutable std::mutex _warnedMutex;
    mutable SdfPathSet _warned;
};

// Not in an anonymous namespace: the TfType name derived from it must equal
// the type name declared in plugInfo.json.
class HdMoonray_RenderManMaterialSceneIndexPlugin : public HdSceneIndexPlugin
{
protected:
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr& inputScene,
        const HdContainerDataSourceHandle& inputArgs) override
    {
        if (!TfGetenvBool("HDMOONRAY_RENDERMAN_MATERIALS", true)) {
            return inputScene;
        }
        return HdMoonray_RenderManMaterialSceneIndex::New(inputScene);
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<HdMoonray_RenderManMaterialSceneIndexPlugin>();
}

TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        kRendererDisplayName,
        TfToken("HdMoonray_RenderManMaterialSceneIndexPlugin"),
        nullptr,
        0,
        HdSceneIndexPluginRegistry::InsertionOrderAtStart);
}

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXR_VERSION >= 2308
