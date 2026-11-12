#pragma once

#include "Core/Engine.h"
#include "Assets/ShaderProgramAsset.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <filesystem>
#include <utility>

namespace GameEngine
{
struct EngineCore::ShaderHookState
{
    Rendering::Utils::ShaderFileLoaderFunc Loader = Rendering::Utils::GetShaderFileLoader();
    Rendering::Utils::ShaderPathResolverFunc Resolver = Rendering::Utils::GetShaderPathResolver();
    Rendering::ShaderPackageRebuildActionFunc RebuildAction = Rendering::GetShaderPackageRebuildAction();
    ShaderCacheRoots CacheRoots = ShaderProgramAsset::GetShaderCacheRoots();
    Rendering::PrecompiledShaderFiles PrecompiledShaders = Rendering::GetPrecompiledShaderFiles();

    ~ShaderHookState()
    {
        Rendering::Utils::SetShaderFileLoader(Loader);
        Rendering::Utils::SetShaderPathResolver(Resolver);
        Rendering::SetShaderPackageRebuildAction(RebuildAction);
        ShaderProgramAsset::SetShaderCacheRoots(std::move(CacheRoots));
        Rendering::SetPrecompiledShaderFiles(std::move(PrecompiledShaders));
    }
};
} // namespace GameEngine
