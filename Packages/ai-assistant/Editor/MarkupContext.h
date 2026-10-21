#pragma once

#include "ConversationMessage.h"
#include "ECS/ECS.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace GameEngine
{
namespace Editor
{
class EditorMarkupRequests;
}

/// A mark-up a prompt names: `@markup:<tag>` (the first of its tags, MarkupContext::TagOf) or
/// `@markup:"<title>"`, where a backslash before a quote or a backslash in the title stands for
/// that character.
struct MarkupToken
{
    /// The token as typed, "@markup:" included.
    std::string Text;
    /// The tag or the title the token names; exactly one is set.
    std::string Tag;
    std::string Title;
};

/// The token that names one mark-up apart from the others, or why none does.
struct MarkupPick
{
    /// The token that names the mark-up; empty when no token can name it apart.
    std::string Token;
    /// Why no token names it, with the fix; empty when Token is set.
    std::string Refusal;
};

/// What the token of one mark-up depends on: its title, its tag and how many mark-ups share each.
struct MarkupNameFacts
{
    uint32_t EntityId = 0;
    std::string_view Title;
    /// The first of its tags (MarkupContext::TagOf); empty when it has none.
    std::string_view Tag;
    /// The mark-ups with this title, this one included.
    std::size_t SameTitle = 1;
    /// The mark-ups whose first tag is this tag, this one included.
    std::size_t SameTag = 1;
};

/// What a prompt's tokens add to its turn.
struct MarkupTurnContext
{
    /// What follows the prompt in the turn the agent receives; empty for a prompt without tokens.
    std::string Text;
    /// For the panel's status line: why a token was not shown (the last one); empty when all were.
    std::string Notice;
    /// One per token, with the title of the mark-up it names now (empty when it names none).
    std::vector<MarkupMention> Mentions;
};

/// The mark-up context of a prompt: the `<markup>` blocks the AI Assistant appends to the
/// user's turn, one per mark-up the prompt names, so the agent reads the mark-ups' shapes and
/// threads without a tool call. The block format is documented in docs/editor/ai-assistant.mdx.
class MarkupContext
{
public:
    /// The most a block holds. The shape and the members keep their room; the description is cut
    /// first, then the thread, and a last line counts what was left out.
    static constexpr std::size_t kMaxBlockBytes = 8 * 1024;
    /// The most a turn's context holds, unknown-token lines included; past it, the remaining
    /// tokens are counted on one line, not shown.
    static constexpr std::size_t kMaxTurnBytes = 32 * 1024;
    /// The most points a region's outline, a member's or a path's line has in a block.
    static constexpr std::size_t kMaxShapePoints = 64;
    /// The thread entries a block shows, the newest ones, each cut to kMaxEntryBytes.
    static constexpr std::size_t kMaxEntries = 10;
    static constexpr std::size_t kMaxEntryBytes = 500;

    /// The tokens `prompt` holds, in order, each mark-up once.
    static std::vector<MarkupToken> FindTokens(std::string_view prompt);
    /// The tag a token names a mark-up by: the first of a markup_list or markup_get row's `tags`,
    /// the first the inspector's Tags row lists; empty when it has none.
    static std::string TagOf(const nlohmann::json& row);
    /// The token that names a mark-up apart from the others: its tag when no other mark-up's first
    /// tag is the same and the tag reads back from a prompt (no space in it), else its title quoted
    /// (a quote or a backslash in it escaped) when no other mark-up has the title; else the refusal,
    /// which names the mark-up's entity id and the fix.
    static MarkupPick NameOf(const MarkupNameFacts& facts);
    /// The block for `markup`, a markup_get answer's "markup" object, at most kMaxBlockBytes.
    /// Attribute values escape `&`, `<`, `>` and `"`, body text `&`, `<` and `>`, so nothing in a
    /// title, a description or a thread can close the block or forge another.
    static std::string Block(const nlohmann::json& markup);
    /// The row of the mark-up `token` names among markup_list's `rows`; when titles or tags repeat,
    /// the first row, the one changed most recently (markup_list's order). Null for none.
    static const nlohmann::json* Resolve(const MarkupToken& token, const nlohmann::json& rows);
    /// The mark-up a markup_list row names.
    static ECS::EntityHandle EntityOf(const nlohmann::json& row);
    /// The context of `prompt`: one block per token, an "[unknown mark-up <token>]" line for a
    /// token that names none, the refusal for every token when mark-ups cannot be read (play
    /// mode), and the tokens past kMaxTurnBytes counted, not shown.
    static MarkupTurnContext ForPrompt(std::string_view prompt, const Editor::EditorMarkupRequests& requests);
};
} // namespace GameEngine
