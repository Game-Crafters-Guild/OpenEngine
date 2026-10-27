cmake_minimum_required(VERSION 3.20)

project(PageStreaming LANGUAGES CXX)

# Page streaming: the .gepage store of a terrain field (a pyramid of fixed-size pages), its
# codecs, cooker, reader and asynchronous page loader, the generated provider of noise and flat bases,
# and the shipped terrain container. Device-free: the GPU half (physical caches, page tables, the
# resolve) lives with the terrain renderer in TerrainECS and CBTTerrain.
add_library(PageStreaming OBJECT
    Source/PageAddress.cpp
    Source/PageStore/PageStoreFormat.cpp
    Source/PageStore/PageStoreSerialization.cpp
    Source/HeightPageCodec.cpp
    Source/PageCodec.cpp
    Source/BytePageCodec.cpp
    Source/Bc5PageCodec.cpp
    Source/PageStore/PageStoreAccess.cpp
    Source/PageStore/PageStoreWriter.cpp
    Source/PageStore/PageStoreReader.cpp
    Source/PageLoader.cpp
    Source/PageCache.cpp
    Source/PageResidency.cpp
    Source/GeneratedHeightPageProvider.cpp
    Source/HeightPageOverlay.cpp
    Source/HeightStoreKey.cpp
    Source/HeightStoreCache.cpp
    Source/HeightSourceRows.cpp
    Source/SourceRangeReader.cpp
    Source/PageFieldFilter.cpp
    Source/PagePyramidBuilder.cpp
    Source/PageStore/PageStorePatcher.cpp
    Source/HeightStepPlanner.cpp
    Source/HeightCookBands.cpp
    Source/HeightPageCooker.cpp
    Source/PlanetPageCooker.cpp
    Source/PageFaceSeams.cpp
    Source/TerrainPageContainer.cpp
    Source/NonInheritedFile.cpp
)

target_compile_features(PageStreaming PUBLIC cxx_std_20)
set_target_properties(PageStreaming PROPERTIES FOLDER "Engine/Modules")

target_include_directories(PageStreaming
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(PageStreaming
    PUBLIC
        Types
        AssetCore
        # Mathematics::Vector3 in the public headers (PageStoreFormat.h, HeightPageCooker.h).
        Mathematics
    PRIVATE
        # The loader's reader threads (Assets/AssetIOService.h); objects link once into Engine.
        AssetRuntime
        JobSystem
        FileSystem
        FileWatcher
        Terrain
        Logger
        # The 16-bit PNG heightmap decode.
        Image
)
