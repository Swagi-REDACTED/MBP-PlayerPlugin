#pragma once

// The two included headers are emitted by the Visual C++ HLSL build step.
// ShaderLoader itself never opens files or invokes a compiler at runtime.
#include "FullscreenVS.bytecode.h"
#include "LoadingPS.bytecode.h"
#include "LauncherGlowPS.bytecode.h"

#include <array>
#include "ShaderLoader.h"

namespace EmbeddedShaderRegistry
{
    inline const std::array<ShaderLoader::Shader, 3> shaders = {{
        {
            "fullscreen_vs",
            "VSMain",
            "vs_6_0",
            { g_FullscreenVS, sizeof(g_FullscreenVS) }
        },
        {
            "loading_ps",
            "main",
            "ps_6_0",
            { g_LoadingPS, sizeof(g_LoadingPS) }
        },
        {
            "launcher_glow_ps",
            "main",
            "ps_6_0",
            { g_LauncherGlowPS, sizeof(g_LauncherGlowPS) }
        }
    }};
}
