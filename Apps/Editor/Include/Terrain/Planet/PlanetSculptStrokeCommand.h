#pragma once

#include "UndoRedo/IEditorCommand.h"

#include "PageStreaming/Height/HeightPageLayer.h"
#include "TerrainECS/TerrainService.h"

#include <utility>
#include <vector>

namespace GameEngine::Editor
{

/// One undo entry for a planet sculpt brush stroke: the pages of the planet's height page layer the
/// stroke changed, as they were before it and after it. Undo and redo put them back byte for byte
/// through TerrainService::RestorePlanetSculptPages, which rewrites the height pages they reach.
class PlanetSculptStrokeCommand final : public IEditorCommand
{
public:
    PlanetSculptStrokeCommand(TerrainECS::TerrainService* service, uint32 planet,
                              std::vector<PageStreaming::HeightPageLayerPage> before,
                              std::vector<PageStreaming::HeightPageLayerPage> after)
        : m_Service(service), m_Planet(planet), m_Before(std::move(before)), m_After(std::move(after))
    {
    }

    const char* GetName() const override { return "Planet Sculpt Stroke"; }
    const char* GetTypeName() const override { return "PlanetSculptStrokeCommand"; }

    void Do() override { Redo(); }
    void Redo() override
    {
        if (m_Service)
            m_Service->RestorePlanetSculptPages(m_Planet, m_After);
    }
    void Undo() override
    {
        if (m_Service)
            m_Service->RestorePlanetSculptPages(m_Planet, m_Before);
    }

private:
    TerrainECS::TerrainService* m_Service = nullptr; // not owned
    uint32 m_Planet = 0;                             // the planet's terrain entity id
    std::vector<PageStreaming::HeightPageLayerPage> m_Before;
    std::vector<PageStreaming::HeightPageLayerPage> m_After;
};

} // namespace GameEngine::Editor
