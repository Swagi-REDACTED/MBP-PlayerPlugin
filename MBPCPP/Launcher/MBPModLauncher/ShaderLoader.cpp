#include "ShaderLoader.h"
#include "ShaderRegistry.generated.h"

#include <stdexcept>
#include <string>

namespace ShaderLoader
{
    std::span<const Shader> List() noexcept
    {
        return EmbeddedShaderRegistry::shaders;
    }

    const Shader* Find(std::string_view name) noexcept
    {
        for (const auto& shader : List())
            if (shader.name == name)
                return &shader;
        return nullptr;
    }

    const Shader& Get(std::string_view name)
    {
        if (const auto* shader = Find(name))
            return *shader;
        throw std::runtime_error("Embedded shader not found: '" + std::string(name) + "'.");
    }

    D3D12_SHADER_BYTECODE Load(std::string_view name)
    {
        return Get(name).bytecode;
    }
}
