/* D3D9 interface method tables: name + stdcall argument count ('this' included) */
#ifndef D3D9_TABLES_H
#define D3D9_TABLES_H
typedef struct { const char *name; unsigned char nargs; } Method;

static const Method M_D3D9[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"RegisterSoftwareDevice",2},
 {"GetAdapterCount",1},{"GetAdapterIdentifier",4},{"GetAdapterModeCount",3},
 {"EnumAdapterModes",5},{"GetAdapterDisplayMode",3},{"CheckDeviceType",6},
 {"CheckDeviceFormat",7},{"CheckDeviceMultiSampleType",7},{"CheckDepthStencilMatch",6},
 {"CheckDeviceFormatConversion",5},{"GetDeviceCaps",4},{"GetAdapterMonitor",2},
 {"CreateDevice",7},
};

static const Method M_DEVICE[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"TestCooperativeLevel",1},
 {"GetAvailableTextureMem",1},{"EvictManagedResources",1},{"GetDirect3D",2},
 {"GetDeviceCaps",2},{"GetDisplayMode",3},{"GetCreationParameters",2},
 {"SetCursorProperties",4},{"SetCursorPosition",4},{"ShowCursor",2},
 {"CreateAdditionalSwapChain",3},{"GetSwapChain",3},{"GetNumberOfSwapChains",1},
 {"Reset",2},{"Present",5},{"GetBackBuffer",5},{"GetRasterStatus",3},
 {"SetDialogBoxMode",2},{"SetGammaRamp",4},{"GetGammaRamp",3},
 {"CreateTexture",9},{"CreateVolumeTexture",10},{"CreateCubeTexture",8},
 {"CreateVertexBuffer",7},{"CreateIndexBuffer",7},{"CreateRenderTarget",9},
 {"CreateDepthStencilSurface",9},{"UpdateSurface",5},{"UpdateTexture",3},
 {"GetRenderTargetData",3},{"GetFrontBufferData",3},{"StretchRect",6},
 {"ColorFill",4},{"CreateOffscreenPlainSurface",7},{"SetRenderTarget",3},
 {"GetRenderTarget",3},{"SetDepthStencilSurface",2},{"GetDepthStencilSurface",2},
 {"BeginScene",1},{"EndScene",1},{"Clear",7},{"SetTransform",3},{"GetTransform",3},
 {"MultiplyTransform",3},{"SetViewport",2},{"GetViewport",2},{"SetMaterial",2},
 {"GetMaterial",2},{"SetLight",3},{"GetLight",3},{"LightEnable",3},{"GetLightEnable",3},
 {"SetClipPlane",3},{"GetClipPlane",3},{"SetRenderState",3},{"GetRenderState",3},
 {"CreateStateBlock",3},{"BeginStateBlock",1},{"EndStateBlock",2},
 {"SetClipStatus",2},{"GetClipStatus",2},{"GetTexture",3},{"SetTexture",3},
 {"GetTextureStageState",4},{"SetTextureStageState",4},{"GetSamplerState",4},
 {"SetSamplerState",4},{"ValidateDevice",2},{"SetPaletteEntries",3},
 {"GetPaletteEntries",3},{"SetCurrentTexturePalette",2},{"GetCurrentTexturePalette",2},
 {"SetScissorRect",2},{"GetScissorRect",2},{"SetSoftwareVertexProcessing",2},
 {"GetSoftwareVertexProcessing",1},{"SetNPatchMode",2},{"GetNPatchMode",1},
 {"DrawPrimitive",4},{"DrawIndexedPrimitive",7},{"DrawPrimitiveUP",5},
 {"DrawIndexedPrimitiveUP",9},{"ProcessVertices",7},{"CreateVertexDeclaration",3},
 {"SetVertexDeclaration",2},{"GetVertexDeclaration",2},{"SetFVF",2},{"GetFVF",2},
 {"CreateVertexShader",3},{"SetVertexShader",2},{"GetVertexShader",2},
 {"SetVertexShaderConstantF",4},{"GetVertexShaderConstantF",4},
 {"SetVertexShaderConstantI",4},{"GetVertexShaderConstantI",4},
 {"SetVertexShaderConstantB",4},{"GetVertexShaderConstantB",4},
 {"SetStreamSource",5},{"GetStreamSource",5},{"SetStreamSourceFreq",3},
 {"GetStreamSourceFreq",3},{"SetIndices",2},{"GetIndices",2},
 {"CreatePixelShader",3},{"SetPixelShader",2},{"GetPixelShader",2},
 {"SetPixelShaderConstantF",4},{"GetPixelShaderConstantF",4},
 {"SetPixelShaderConstantI",4},{"GetPixelShaderConstantI",4},
 {"SetPixelShaderConstantB",4},{"GetPixelShaderConstantB",4},
 {"DrawRectPatch",4},{"DrawTriPatch",4},{"DeletePatch",2},{"CreateQuery",3},
};

/* common prefix for the IDirect3DResource9 derivatives */
#define RES_HEAD \
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},\
 {"SetPrivateData",5},{"GetPrivateData",4},{"FreePrivateData",2},\
 {"SetPriority",2},{"GetPriority",1},{"PreLoad",1},{"GetType",1}

static const Method M_TEXTURE[] = { RES_HEAD,
 {"SetLOD",2},{"GetLOD",1},{"GetLevelCount",1},{"SetAutoGenFilterType",2},
 {"GetAutoGenFilterType",1},{"GenerateMipSubLevels",1},{"GetLevelDesc",3},
 {"GetSurfaceLevel",3},{"LockRect",5},{"UnlockRect",2},{"AddDirtyRect",2},
};
static const Method M_VBUF[] = { RES_HEAD,
 {"Lock",5},{"Unlock",1},{"GetDesc",2},
};
static const Method M_IBUF[] = { RES_HEAD,
 {"Lock",5},{"Unlock",1},{"GetDesc",2},
};
static const Method M_SURFACE[] = { RES_HEAD,
 {"GetContainer",3},{"GetDesc",2},{"LockRect",4},{"UnlockRect",1},
 {"GetDC",2},{"ReleaseDC",2},
};
/* --- small interfaces: each has its own vtable layout --- */
static const Method M_VSHADER[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"GetFunction",3},
};
static const Method M_VDECL[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"GetDeclaration",3},
};
static const Method M_STATEBLOCK[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"Capture",1},{"Apply",1},
};
static const Method M_SWAPCHAIN[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"Present",6},{"GetFrontBufferData",2},
 {"GetBackBuffer",4},{"GetRasterStatus",2},{"GetDisplayMode",2},{"GetDevice",2},
 {"GetPresentParameters",2},
};
static const Method M_QUERY[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},{"GetType",1},
 {"GetDataSize",1},{"Issue",2},{"GetData",4},
};
/* --- D3DX 9 (d3dx9_26 = D3DX_SDK_VERSION 26): ID3DXEffect and ID3DXEffectPool, answered by the game
 * (game.h d3dx_call: Most Wanted's runtime/mw/d3dxfx_mw.c).  The order is the SDK's d3dx9effect.h:
 * ID3DXBaseEffect 0..56, then ID3DXEffect's own, with DeleteParameterBlock and without SetRawValue
 * (docs/mw/fx-model.md 1.1). --- */
static const Method M_D3DXFX[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDesc",2},{"GetParameterDesc",3},
 {"GetTechniqueDesc",3},{"GetPassDesc",3},{"GetFunctionDesc",3},{"GetParameter",3},
 {"GetParameterByName",3},{"GetParameterBySemantic",3},{"GetParameterElement",3},{"GetTechnique",2},
 {"GetTechniqueByName",2},{"GetPass",3},{"GetPassByName",3},{"GetFunction",2},{"GetFunctionByName",2},
 {"GetAnnotation",3},{"GetAnnotationByName",3},{"SetValue",4},{"GetValue",4},{"SetBool",3},{"GetBool",3},
 {"SetBoolArray",4},{"GetBoolArray",4},{"SetInt",3},{"GetInt",3},{"SetIntArray",4},{"GetIntArray",4},
 {"SetFloat",3},{"GetFloat",3},{"SetFloatArray",4},{"GetFloatArray",4},{"SetVector",3},{"GetVector",3},
 {"SetVectorArray",4},{"GetVectorArray",4},{"SetMatrix",3},{"GetMatrix",3},{"SetMatrixArray",4},
 {"GetMatrixArray",4},{"SetMatrixPointerArray",4},{"GetMatrixPointerArray",4},{"SetMatrixTranspose",3},
 {"GetMatrixTranspose",3},{"SetMatrixTransposeArray",4},{"GetMatrixTransposeArray",4},
 {"SetMatrixTransposePointerArray",4},{"GetMatrixTransposePointerArray",4},{"SetString",3},{"GetString",3},
 {"SetTexture",3},{"GetTexture",3},{"GetPixelShader",3},{"GetVertexShader",3},{"SetArrayRange",4},
 {"GetPool",2},{"SetTechnique",2},{"GetCurrentTechnique",1},{"ValidateTechnique",2},
 {"FindNextValidTechnique",3},{"IsParameterUsed",3},{"Begin",3},{"BeginPass",2},{"CommitChanges",1},
 {"EndPass",1},{"End",1},{"GetDevice",2},{"OnLostDevice",1},{"OnResetDevice",1},{"SetStateManager",2},
 {"GetStateManager",2},{"BeginParameterBlock",1},{"EndParameterBlock",1},{"ApplyParameterBlock",2},
 {"DeleteParameterBlock",2},{"CloneEffect",3},
};
static const Method M_D3DXPOOL[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
};
/* wide empty vtable for simple/unknown objects (prevents unresolvable calls) */
static const Method M_SIMPLE[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetDevice",2},
 {"GetFunction",3},{"GetDeclaration",3},
 {"m6",1},{"m7",1},{"m8",1},{"m9",1},{"m10",1},{"m11",1},{"m12",1},{"m13",1},
 {"m14",1},{"m15",1},
};
/* --- DirectInput8 --- */
static const Method M_DI8[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"CreateDevice",4},{"EnumDevices",5},{"GetDeviceStatus",2},{"RunControlPanel",3},
 {"Initialize",3},{"FindDevice",4},{"EnumDevicesBySemantics",6},{"ConfigureDevices",5},
};
static const Method M_DIDEV[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"GetCapabilities",2},{"EnumObjects",4},{"GetProperty",3},{"SetProperty",3},
 {"Acquire",1},{"Unacquire",1},{"GetDeviceState",3},{"GetDeviceData",5},
 {"SetDataFormat",2},{"SetEventNotification",2},{"SetCooperativeLevel",3},
 {"GetObjectInfo",4},{"GetDeviceInfo",2},{"RunControlPanel",3},{"Initialize",4},
 {"CreateEffect",5},{"EnumEffects",4},{"GetEffectInfo",3},{"GetForceFeedbackState",2},
 {"SendForceFeedbackCommand",2},{"EnumCreatedEffectObjects",4},{"Escape",2},{"Poll",1},
 {"SendDeviceData",5},{"EnumEffectsInFile",5},{"WriteEffectToFile",5},
 {"BuildActionMap",4},{"SetActionMap",4},{"GetImageInfo",2},
};
/* the pad's force-feedback effects (dinput_pad.c) */
static const Method M_DIEFF[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"Initialize",4},{"GetEffectGuid",2},{"GetParameters",3},{"SetParameters",3},
 {"Start",3},{"Stop",1},{"GetEffectStatus",2},{"Download",1},{"Unload",1},{"Escape",2},
};
/* --- DirectSound --- */
static const Method M_DS[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"CreateSoundBuffer",4},{"GetCaps",2},{"DuplicateSoundBuffer",3},
 {"SetCooperativeLevel",3},{"Compact",1},{"GetSpeakerConfig",2},
 {"SetSpeakerConfig",2},{"Initialize",2},
};
static const Method M_DSBUF[] = {
 {"QueryInterface",3},{"AddRef",1},{"Release",1},
 {"GetCaps",2},{"GetCurrentPosition",3},{"GetFormat",4},{"GetVolume",2},
 {"GetPan",2},{"GetFrequency",2},{"GetStatus",2},{"Initialize",3},
 {"Lock",8},{"Play",4},{"SetCurrentPosition",2},{"SetFormat",2},
 {"SetVolume",2},{"SetPan",2},{"SetFrequency",2},{"Stop",1},{"Unlock",5},{"Restore",1},
};
#endif
