#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <span>
#include <string_view>

// HLSL is compiled by the build and linked into this library as bytecode.
// No device, file access, runtime compiler, or initialization is needed to load it.
namespace ShaderLoader
{
    struct Shader
    {
        std::string_view name;
        std::string_view entryPoint;
        std::string_view profile;
        D3D12_SHADER_BYTECODE bytecode;
    };

    // All returned views and bytecode pointers remain valid for the process lifetime.
    std::span<const Shader> List() noexcept;
    const Shader* Find(std::string_view name) noexcept;
    // Throws std::runtime_error with the shader name if it wasn't registered in CMake.
    const Shader& Get(std::string_view name);
    D3D12_SHADER_BYTECODE Load(std::string_view name);
}
