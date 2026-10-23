// Copyright Contributors to the Moonray Project
// SPDX-License-Identifier: Apache-2.0

// Translates RenderMan material networks into Moonray ones.
//
// A material authored for RenderMan carries an "ri" network
// (outputs:ri:surface) that Moonray cannot render, so hdMoonray falls back to
// the material's universal network, usually a UsdPreviewSurface. When a
// material has an ri surface and no moonray network, this scene index adds a
// moonray network built from DwaBaseMaterial, ImageMap and ImageNormalMap.
//
// Only one shape of network is recognised: PxrLayerSurface <- PxrLayerMixer
// <- PxrLayer, with optional PxrMetallicWorkflow, PxrTexture, PxrRemap,
// PxrNormalMap and PxrInvert nodes, and only the inputs listed in
// _Translator. Any other network, node or input is left untranslated with
// one warning, and the material renders as it did before. Set
// HDMOONRAY_RENDERMAN_MATERIALS=0 to turn the translation off.
//
// RenderMan defaults below are from RenderMan 27.3's shipped .oso files.

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
    (PxrLayerMixer)
    (PxrLayer)
    (PxrMetallicWorkflow)
    (PxrTexture)
    (PxrNormalMap)
    (PxrRemap)
    (PxrInvert)

    (inputMaterial)
    (pxrMaterialOut)
    (baselayer)
    (enableDiffuse)
    (diffuseColor)
    (diffuseGain)
    (enableSpecular)
    (specularGain)
    (specularFaceColor)
    (specularEdgeColor)
    (specularRoughness)
    (bumpNormal)
    (enableRR)
    (rrRefractionGain)
    (rrReflectionGain)
    (rrRoughness)
    (rrIor)
    (baseColor)
    (metallic)
    (filename)
    (filename_colorspace)
    (rendering)
    (data)
    (inputRGB)
    (resultRGB)
    (resultR)
    (resultN)
    (resultDiffuseRGB)
    (resultSpecularFaceRGB)
    (resultSpecularEdgeRGB)

    (DwaBaseMaterial)
    (ImageMap)
    (ImageNormalMap)
    (out)
);

namespace {

using _Connection = HdMaterialNetworkInterface::InputConnection;

// Where a DwaBaseMaterial input gets its value: the ImageMap made from a
// PxrTexture, or a constant.
struct _Source
{
    TfToken texture;
    VtValue value;

    bool operator==(const _Source& other) const
    {
        return texture == other.texture && value == other.value;
    }
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
    bool _IsAuthored(const TfToken& node, const TfToken& input) const;
    bool _GetConnection(const TfToken& node, const TfToken& input,
                        _Connection* connection) const;
    template <typename T>
    bool _GetValue(const TfToken& node, const TfToken& input, const T& fallback,
                   T* value);
    template <typename T>
    bool _GetSource(const TfToken& node, const TfToken& input,
                    const T& fallback, _Source* source);
    bool _GetTexture(const TfToken& node, const TfToken& input,
                     const _Connection& connection, const TfToken& output,
                     _Source* source);
    bool _GetFilename(const TfToken& node, const TfToken& colorspace,
                      std::string* filename);
    bool _GetLayer(const TfToken& mixer, TfToken* layer);
    bool _GetGlass(const TfToken& layer, int* enableRR, _Source* transmission);

    void _Set(const TfToken& node, const char* input, const _Source& source);
    void _Set(const TfToken& node, const char* input, const VtValue& value);

    const HdMaterialNetworkInterface& _ri;
    std::unique_ptr<HdDataSourceMaterialNetworkInterface> _out;
    std::map<TfToken, std::string> _textures;  // PxrTexture name -> filename
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
_Translator::_IsAuthored(const TfToken& node, const TfToken& input) const
{
    _Connection connection;
    return !_ri.GetNodeParameterValue(node, input).IsEmpty() ||
           _GetConnection(node, input, &connection);
}

bool
_Translator::_GetConnection(const TfToken& node, const TfToken& input,
                            _Connection* connection) const
{
    const auto connections = _ri.GetNodeInputConnection(node, input);
    if (connections.empty()) {
        return false;
    }
    *connection = connections[0];
    return true;
}

// The value of an unconnected input, or fallback when it is unauthored.
template <typename T>
bool
_Translator::_GetValue(const TfToken& node, const TfToken& input,
                       const T& fallback, T* value)
{
    _Connection connection;
    if (_GetConnection(node, input, &connection)) {
        return _Fail(node, "input " + input.GetString() + " must not be connected");
    }
    const VtValue authored = _ri.GetNodeParameterValue(node, input);
    if (authored.IsEmpty()) {
        *value = fallback;
        return true;
    }
    const VtValue cast = VtValue::Cast<T>(authored);
    if (cast.IsEmpty()) {
        return _Fail(node, "input " + input.GetString() + " has an unexpected type");
    }
    *value = cast.UncheckedGet<T>();
    return true;
}

// A float or colour input: a texture, possibly through an identity PxrRemap,
// or a constant.
template <typename T>
bool
_Translator::_GetSource(const TfToken& node, const TfToken& input,
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

    const TfToken& upstream = connection.upstreamNodeName;
    if (_ri.GetNodeType(upstream) == _tokens->PxrRemap) {
        // A PxrRemap with only inputRGB authored is the identity.
        if (connection.upstreamOutputName != output) {
            return _Fail(node, "input " + input.GetString() + " must use " +
                               upstream.GetString() + "." + output.GetString());
        }
        if (!_CheckInputs(upstream, {"inputRGB"})) {
            return false;
        }
        if (!_GetConnection(upstream, _tokens->inputRGB, &connection)) {
            return _Fail(upstream, "input inputRGB must be connected");
        }
        return _GetTexture(upstream, _tokens->inputRGB, connection,
                           _tokens->resultRGB, source);
    }
    return _GetTexture(node, input, connection, output, source);
}

bool
_Translator::_GetTexture(const TfToken& node, const TfToken& input,
                         const _Connection& connection, const TfToken& output,
                         _Source* source)
{
    const TfToken& texture = connection.upstreamNodeName;
    if (_ri.GetNodeType(texture) != _tokens->PxrTexture ||
        connection.upstreamOutputName != output) {
        return _Fail(node, "input " + input.GetString() +
                           " must be connected to a PxrTexture " +
                           output.GetString());
    }
    std::string filename;
    if (!_GetFilename(texture, TfToken(), &filename)) {
        return false;
    }
    _textures[texture] = filename;
    *source = {texture, VtValue()};
    return true;
}

// The filename of a PxrTexture or PxrNormalMap whose filename_colorspace is
// colorspace, or either "rendering" or "data" when colorspace is empty.
// RenderMan reads texels as stored for both; the pipeline bakes colour maps
// to the rendering space when it makes the .tex files.
bool
_Translator::_GetFilename(const TfToken& node, const TfToken& colorspace,
                          std::string* filename)
{
    if (!_CheckInputs(node, {"filename", "filename_colorspace"})) {
        return false;
    }
    const TfToken space(
        _AsString(_ri.GetNodeParameterValue(node, _tokens->filename_colorspace)));
    if (colorspace.IsEmpty() ? (space != _tokens->rendering && space != _tokens->data)
                             : space != colorspace) {
        return _Fail(node, "filename_colorspace '" + space.GetString() +
                           "' is not supported");
    }
    *filename = _AsString(_ri.GetNodeParameterValue(node, _tokens->filename));
    if (filename->empty()) {
        return _Fail(node, "filename is empty");
    }
    return true;
}

// The layer RenderMan shows: the highest-numbered enabled layer, else the
// base layer. Masks default to 1, so a layer covers everything below it.
bool
_Translator::_GetLayer(const TfToken& mixer, TfToken* layer)
{
    if (!_CheckInputs(mixer, {"baselayer",
                              "layer1", "layer1Enabled", "layer2", "layer2Enabled",
                              "layer3", "layer3Enabled", "layer4", "layer4Enabled"})) {
        return false;
    }
    _Connection chosen;
    if (!_GetConnection(mixer, _tokens->baselayer, &chosen)) {
        return _Fail(mixer, "baselayer must be connected");
    }
    for (int i = 1; i <= 4; ++i) {
        const std::string name = TfStringPrintf("layer%d", i);
        int enabled = 0;
        if (!_GetValue(mixer, TfToken(name + "Enabled"), i == 1 ? 1 : 0, &enabled)) {
            return false;
        }
        if (enabled && !_GetConnection(mixer, TfToken(name), &chosen)) {
            return _Fail(mixer, name + " is enabled but not connected");
        }
    }
    if (_ri.GetNodeType(chosen.upstreamNodeName) != _tokens->PxrLayer ||
        chosen.upstreamOutputName != _tokens->pxrMaterialOut) {
        return _Fail(mixer, "layers must be connected to a PxrLayer pxrMaterialOut");
    }
    *layer = chosen.upstreamNodeName;
    return true;
}

// Glass (enableRR). The pipeline drives refraction and reflection from one
// transparency map and diffuseGain from its inverse. DwaBaseMaterial layers
// transmission over diffuse, so that inverse is implied and dropped.
bool
_Translator::_GetGlass(const TfToken& layer, int* enableRR, _Source* transmission)
{
    if (!_GetValue(layer, _tokens->enableRR, 0, enableRR)) {
        return false;
    }
    if (!*enableRR) {
        // RenderMan ignores the rr inputs, but not diffuseGain.
        *transmission = {TfToken(), VtValue(0.0f)};
        return !_IsAuthored(layer, _tokens->diffuseGain) ||
               _Fail(layer, "diffuseGain is only supported with enableRR");
    }

    _Source reflection;
    if (!_GetSource(layer, _tokens->rrRefractionGain, 0.0f, transmission) ||
        !_GetSource(layer, _tokens->rrReflectionGain, 0.0f, &reflection)) {
        return false;
    }
    if (!(reflection == *transmission)) {
        return _Fail(layer, "rrReflectionGain must match rrRefractionGain");
    }

    if (!_IsAuthored(layer, _tokens->diffuseGain)) {
        return true;
    }
    _Connection connection;
    if (!_GetConnection(layer, _tokens->diffuseGain, &connection) ||
        _ri.GetNodeType(connection.upstreamNodeName) != _tokens->PxrInvert ||
        connection.upstreamOutputName != _tokens->resultR) {
        return _Fail(layer, "diffuseGain must be connected to a PxrInvert resultR");
    }
    const TfToken& invert = connection.upstreamNodeName;
    _Source inverted;
    if (!_CheckInputs(invert, {"inputRGB"}) ||
        !_GetSource(invert, _tokens->inputRGB, GfVec3f(0.0f), &inverted)) {
        return false;
    }
    if (inverted.texture.IsEmpty() || inverted.texture != transmission->texture) {
        return _Fail(invert, "inputRGB must be the rrRefractionGain texture");
    }
    return true;
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
_Translator::Translate()
{
    for (const TfToken& terminal : {HdMaterialTerminalTokens->displacement,
                                    HdMaterialTerminalTokens->volume}) {
        if (_ri.GetTerminalConnection(terminal).first) {
            _error = "has a " + terminal.GetString() + " terminal";
            return nullptr;
        }
    }

    // PxrLayerSurface <- PxrLayerMixer <- PxrLayer
    const TfToken surface =
        _ri.GetTerminalConnection(HdMaterialTerminalTokens->surface)
            .second.upstreamNodeName;
    _Connection connection;
    if (!_CheckType(surface, _tokens->PxrLayerSurface) ||
        !_CheckInputs(surface, {"inputMaterial"})) {
        return nullptr;
    }
    if (!_GetConnection(surface, _tokens->inputMaterial, &connection) ||
        _ri.GetNodeType(connection.upstreamNodeName) != _tokens->PxrLayerMixer ||
        connection.upstreamOutputName != _tokens->pxrMaterialOut) {
        _Fail(surface, "inputMaterial must be connected to a PxrLayerMixer");
        return nullptr;
    }
    TfToken layer;
    if (!_GetLayer(connection.upstreamNodeName, &layer) ||
        !_CheckInputs(layer, {"enableDiffuse", "enableSpecular", "specularGain",
                              "diffuseColor", "specularFaceColor",
                              "specularEdgeColor", "specularRoughness",
                              "bumpNormal", "enableRR", "rrRefractionGain",
                              "rrReflectionGain", "rrRoughness", "rrIor",
                              "diffuseGain"})) {
        return nullptr;
    }

    // Base colour and metallic. PxrMetallicWorkflow feeds diffuse
    // base*(1-m) and specular face/edge mix(0.04, base, m) / mix(1, base, m);
    // DwaBaseMaterial computes the same from albedo and metallic.
    _Source albedo;
    _Source metallic = {TfToken(), VtValue(0.0f)};
    _Source specular;
    const bool hasWorkflow =
        _GetConnection(layer, _tokens->diffuseColor, &connection) &&
        _ri.GetNodeType(connection.upstreamNodeName) == _tokens->PxrMetallicWorkflow;
    if (hasWorkflow) {
        const TfToken workflow = connection.upstreamNodeName;
        const std::pair<TfToken, TfToken> outputs[] = {
            {_tokens->diffuseColor, _tokens->resultDiffuseRGB},
            {_tokens->specularFaceColor, _tokens->resultSpecularFaceRGB},
            {_tokens->specularEdgeColor, _tokens->resultSpecularEdgeRGB},
        };
        for (const auto& [input, output] : outputs) {
            if (!_GetConnection(layer, input, &connection) ||
                connection.upstreamNodeName != workflow ||
                connection.upstreamOutputName != output) {
                _Fail(layer, input.GetString() + " must be connected to " +
                             workflow.GetString() + "." + output.GetString());
                return nullptr;
            }
        }
        if (!_CheckInputs(workflow, {"baseColor", "metallic"}) ||
            !_GetSource(workflow, _tokens->baseColor, GfVec3f(0.2f, 0.5f, 0.8f), &albedo) ||
            !_GetSource(workflow, _tokens->metallic, 0.0f, &metallic) ||
            !_GetSource(layer, _tokens->specularGain, 0.0f, &specular)) {
            return nullptr;
        }
    } else {
        // Without a workflow, only the default black specular colours are
        // supported: RenderMan renders no specular from them.
        GfVec3f face, edge;
        if (!_GetSource(layer, _tokens->diffuseColor, GfVec3f(0.18f), &albedo) ||
            !_GetValue(layer, _tokens->specularFaceColor, GfVec3f(0.0f), &face) ||
            !_GetValue(layer, _tokens->specularEdgeColor, GfVec3f(0.0f), &edge)) {
            return nullptr;
        }
        if (face != GfVec3f(0.0f) || edge != GfVec3f(0.0f)) {
            _Fail(layer, "specular colours need a PxrMetallicWorkflow");
            return nullptr;
        }
        specular = {TfToken(), VtValue(0.0f)};
    }

    int enableDiffuse = 1;
    int enableSpecular = 0;
    _Source roughness;
    _Source transmission;
    int enableRR = 0;
    float rrRoughness = 0.1f;
    float rrIor = 1.5f;
    if (!_GetValue(layer, _tokens->enableDiffuse, 1, &enableDiffuse) ||
        !_GetValue(layer, _tokens->enableSpecular, 0, &enableSpecular) ||
        !_GetSource(layer, _tokens->specularRoughness, 0.2f, &roughness) ||
        !_GetGlass(layer, &enableRR, &transmission) ||
        !_GetValue(layer, _tokens->rrRoughness, 0.1f, &rrRoughness) ||
        !_GetValue(layer, _tokens->rrIor, 1.5f, &rrIor)) {
        return nullptr;
    }
    // RenderMan's glass reflects even without specular colours or
    // enableSpecular; DwaBaseMaterial reflects only through its specular lobe.
    // rrReflectionGain matches rrRefractionGain (see _GetGlass).
    if (enableRR && !hasWorkflow) {
        enableSpecular = 1;
        specular = transmission;
    }

    TfToken normalMap;
    std::string normalFilename;
    if (_IsAuthored(layer, _tokens->bumpNormal)) {
        if (!_GetConnection(layer, _tokens->bumpNormal, &connection) ||
            _ri.GetNodeType(connection.upstreamNodeName) != _tokens->PxrNormalMap ||
            connection.upstreamOutputName != _tokens->resultN) {
            _Fail(layer, "bumpNormal must be connected to a PxrNormalMap resultN");
            return nullptr;
        }
        normalMap = connection.upstreamNodeName;
        if (!_GetFilename(normalMap, _tokens->data, &normalFilename)) {
            return nullptr;
        }
    }

    // Write the moonray network. Nodes keep their RenderMan names.
    _out = std::make_unique<HdDataSourceMaterialNetworkInterface>(
        _ri.GetMaterialPrimPath(), HdRetainedContainerDataSource::New(), nullptr);

    const TfToken& material = surface;
    _out->SetNodeType(material, _tokens->DwaBaseMaterial);
    _Set(material, "show_diffuse", VtValue(enableDiffuse != 0));
    _Set(material, "albedo", albedo);
    _Set(material, "diffuse_roughness", VtValue(0.0f));
    _Set(material, "metallic", metallic);
    _Set(material, "metallic_color", albedo);
    _Set(material, "metallic_edge_color", albedo);
    _Set(material, "show_specular", VtValue(enableSpecular != 0));
    _Set(material, "specular", specular);
    _Set(material, "refractive_index", VtValue(1.5f));
    _Set(material, "specular_model", VtValue(TfToken("Beckmann")));
    _Set(material, "roughness", roughness);
    // PxrLayer rrRefractionColor 1, thinGlass 0.
    _Set(material, "show_transmission", VtValue(true));
    _Set(material, "transmission", transmission);
    _Set(material, "transmission_color", VtValue(GfVec3f(1.0f)));
    _Set(material, "use_independent_transmission_roughness", VtValue(true));
    _Set(material, "independent_transmission_roughness", VtValue(rrRoughness));
    _Set(material, "use_independent_transmission_refractive_index", VtValue(true));
    _Set(material, "independent_transmission_refractive_index", VtValue(rrIor));
    _Set(material, "thin_geometry", VtValue(false));
    _out->SetTerminalConnection(HdMaterialTerminalTokens->surface,
                                {material, HdMaterialTerminalTokens->surface});

    // A missing texture renders magenta, like PxrTexture's missingColor.
    for (const auto& [texture, filename] : _textures) {
        _out->SetNodeType(texture, _tokens->ImageMap);
        _Set(texture, "texture", VtValue(filename));
        _Set(texture, "gamma", VtValue(TfToken("off")));
        _Set(texture, "wrap_around", VtValue(false));
        _Set(texture, "use_default_color_when_missing", VtValue(true));
        _Set(texture, "default_color", VtValue(GfVec3f(1.0f, 0.0f, 1.0f)));
    }

    // PxrNormalMap decodes texels as 2*t-1, with bumpScale 1.
    if (!normalMap.IsEmpty()) {
        _out->SetNodeType(normalMap, _tokens->ImageNormalMap);
        _Set(normalMap, "tangent_space_normal_texture", VtValue(normalFilename));
        _Set(normalMap, "normal_encoding", VtValue(TfToken("[0,1]")));
        _Set(normalMap, "use_default_value_when_missing", VtValue(true));
        _Set(normalMap, "default_value", VtValue(GfVec3f(0.0f, 0.0f, 1.0f)));
        _out->SetNodeInputConnection(material, TfToken("input_normal"),
                                     {{normalMap, _tokens->out}});
        _Set(material, "input_normal_dial", VtValue(1.0f));
    }

    return _out->Finish();
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
