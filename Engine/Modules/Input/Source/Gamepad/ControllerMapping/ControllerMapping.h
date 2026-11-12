#pragma once

#include "Input/GamepadCodes.h"
#include "Input/GamepadFrame.h"

#include <cstdint>
#include <span>
#include <string>

namespace GameEngine::Input
{

/**
 * @brief One controller's entry in the community mapping database: which raw control drives each
 * GamepadButton and GamepadAxis. A control with no source reads released or at rest.
 */
struct ControllerMapping
{
    /// One reading of a controller's raw controls, in the mapping database's units: an axis reads -1
    /// to 1, a hat reads a mask of up 1, right 2, down 4 and left 8.
    struct Reading
    {
        std::span<const float> Axes;
        std::span<const bool> Buttons;
        std::span<const std::uint8_t> Hats;
    };

    /// The raw control that drives one gamepad control: a button ("b3"), an axis ("a2", "+a2", "-a2",
    /// "a2~") or one direction of a hat ("h0.4").
    struct Source
    {
        enum class Kind : std::uint8_t
        {
            None,
            Button,
            Axis,
            Hat,
        };
        /// The part of an axis the source reads: all of it, or the half above or below the center.
        enum class AxisRange : std::uint8_t
        {
            Full,
            Positive,
            Negative,
        };

        Kind SourceKind = Kind::None;
        AxisRange Range = AxisRange::Full;
        bool Inverted = false;
        std::uint8_t Index = 0;
        std::uint8_t HatMask = 0;
    };

    std::string Name;
    std::uint16_t VendorId = 0;
    std::uint16_t ProductId = 0;
    Source Buttons[kGamepadButtonCount] = {};
    Source Axes[kGamepadAxisCount] = {};
    /// A source that drives only the half of an axis above ("+leftx") or below ("-leftx") the center.
    Source PositiveHalfAxes[kGamepadAxisCount] = {};
    Source NegativeHalfAxes[kGamepadAxisCount] = {};

    /// Writes `reading` into `slot` in the GamepadButton and GamepadAxis layout and marks the slot
    /// connected. A source past the reading's controls reads released or at rest. A stick's Y is
    /// negated: the database reads it +Y down.
    void Apply(const Reading& reading, GamepadFrame::Slot& slot) const;
};

} // namespace GameEngine::Input
