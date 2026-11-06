#include "Graph/GraphSelectionHistory.h"
#include "Graph/GraphModel.h"

#include <cstdint>
#include <initializer_list>
#include <string>

#include <gtest/gtest.h>

namespace GameEngine::Editor
{
namespace
{
class GraphSelectionHistoryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Graph::Node first;
        first.Id = "a";
        first.TypeId = "Test";
        Graph::Node second;
        second.Id = "b";
        second.TypeId = "Test";
        m_Model.Nodes = {first, second};
    }

    bool Select(std::initializer_list<std::string> nodes)
    {
        return m_Selection.UpdateSelection(nodes, &m_History);
    }

    UndoRedoService::SnapshotTarget Target()
    {
        UndoRedoService::SnapshotTarget target;
        target.Identity = {reinterpret_cast<std::uintptr_t>(&m_Model)};
        target.Capture = [this](auto& bytes)
        {
            if (!m_CaptureAllowed)
                return false;
            const std::string json = Graph::ToJson(m_Model);
            bytes.assign(json.begin(), json.end());
            return true;
        };
        target.Apply = [this](const auto& bytes)
        {
            return Graph::FromJson(std::string(bytes.begin(), bytes.end()), m_Model);
        };
        return target;
    }

    void Move(std::initializer_list<std::string> nodes, float delta)
    {
        auto edit = m_History.BeginInteractiveEdit("Move Nodes", Target());
        ASSERT_TRUE(static_cast<bool>(edit));
        edit.Preview([this, &nodes, delta]
        {
            for (const auto& nodeId : nodes)
                m_Model.FindNode(nodeId)->PositionX += delta;
        });
        edit.Commit();
    }

    Graph::Model m_Model;
    GraphSelectionHistory m_Selection;
    UndoRedoService m_History;
    bool m_CaptureAllowed = true;
};

TEST_F(GraphSelectionHistoryTest, ChangingNodesSeparatesTheirMoves)
{
    ASSERT_TRUE(Select({"a"}));
    Move({"a"}, 10.0f);
    ASSERT_TRUE(Select({"b"}));
    Move({"b"}, 20.0f);
    m_History.Undo();
    EXPECT_FLOAT_EQ(m_Model.FindNode("a")->PositionX, 10.0f);
    EXPECT_FLOAT_EQ(m_Model.FindNode("b")->PositionX, 0.0f);
    m_History.Undo();
    EXPECT_FLOAT_EQ(m_Model.FindNode("a")->PositionX, 0.0f);
}

TEST_F(GraphSelectionHistoryTest, DeselectingAndReselectingTheSameNodeEndsItsMove)
{
    ASSERT_TRUE(Select({"a"}));
    Move({"a"}, 10.0f);
    ASSERT_TRUE(Select({}));
    ASSERT_TRUE(Select({"a"}));
    Move({"a"}, 20.0f);
    m_History.Undo();
    EXPECT_FLOAT_EQ(m_Model.FindNode("a")->PositionX, 10.0f);
}

TEST_F(GraphSelectionHistoryTest, ChangingMembershipOfAMultiSelectionEndsTheMove)
{
    ASSERT_TRUE(Select({"a", "b"}));
    Move({"a", "b"}, 10.0f);
    ASSERT_TRUE(Select({"a"}));
    Move({"a"}, 20.0f);
    m_History.Undo();
    EXPECT_FLOAT_EQ(m_Model.FindNode("a")->PositionX, 10.0f);
    EXPECT_FLOAT_EQ(m_Model.FindNode("b")->PositionX, 10.0f);
}

TEST_F(GraphSelectionHistoryTest, RefreshingTheSameSelectionKeepsOnePendingAction)
{
    ASSERT_TRUE(Select({"a"}));
    Move({"a"}, 10.0f);
    ASSERT_TRUE(Select({"a"}));
    EXPECT_TRUE(m_History.GetHistoryNodes().empty());
    Move({"a"}, 20.0f);
    m_History.Undo();
    EXPECT_FLOAT_EQ(m_Model.FindNode("a")->PositionX, 0.0f);
    EXPECT_EQ(m_History.GetHistoryNodes().size(), 1u);
}

TEST_F(GraphSelectionHistoryTest, SelectionIterationOrderDoesNotEndTheMove)
{
    ASSERT_TRUE(Select({"a", "b"}));
    Move({"a", "b"}, 10.0f);
    ASSERT_TRUE(Select({"b", "a"}));
    EXPECT_TRUE(m_History.GetHistoryNodes().empty());
    Move({"a", "b"}, 20.0f);
    m_History.Undo();
    EXPECT_FLOAT_EQ(m_Model.FindNode("a")->PositionX, 0.0f);
    EXPECT_FLOAT_EQ(m_Model.FindNode("b")->PositionX, 0.0f);
}

TEST_F(GraphSelectionHistoryTest, AFailedCaptureCanRetryTheSameSelectionChange)
{
    ASSERT_TRUE(Select({"a"}));
    auto edit = m_History.BeginInteractiveEdit("Move Nodes", Target());
    edit.Preview([this] { m_Model.FindNode("a")->PositionX = 10.0f; });
    m_CaptureAllowed = false;
    EXPECT_FALSE(Select({"b"}));
    EXPECT_TRUE(m_History.GetHistoryNodes().empty());
    m_CaptureAllowed = true;
    ASSERT_TRUE(Select({"b"}));
    EXPECT_EQ(m_History.GetHistoryNodes().size(), 1u);
    m_History.Undo();
    EXPECT_FLOAT_EQ(m_Model.FindNode("a")->PositionX, 0.0f);
}
} // namespace
} // namespace GameEngine::Editor
