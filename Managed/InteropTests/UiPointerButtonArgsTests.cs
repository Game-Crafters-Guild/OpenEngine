using System.Collections.Generic;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>
    /// The pointer button events as a script sees them: which button, where, the modifiers, and a
    /// release told apart from a cancel. Driven against the native shim's element double, like the
    /// wrapper tests beside this file; the delivery through a real game UI tree is pinned by
    /// GameViewPointerDeliveryTests.
    /// </summary>
    public class UiPointerButtonArgsTests
    {
        private const string kLib = "GameEngine.Native";

        private const string kMouseDown = "UI.MouseDown";
        private const string kMouseUp = "UI.MouseUp";
        private const string kMouseCancel = "UI.MouseCancel";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_AddElement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string elementId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outInstanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Reset();

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DispatchPress(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName, float x, float y, int button, int mods,
            out int outInvoked);

        /// <summary>Each test builds its own elements, so the double starts empty.</summary>
        [SetUp]
        public void Reset() => Assert.That(GE_TestUI_Reset(), Is.EqualTo(0));

        private static ulong AddElement(string elementId)
        {
            Assert.That(GE_TestUI_AddElement(elementId, "button", out ulong instanceId), Is.EqualTo(0));
            Assert.That(instanceId, Is.Not.Zero);
            return instanceId;
        }

        private static int Dispatch(ulong instanceId, string eventName, float x, float y, MouseButton button,
                                    ModifierFlags mods = ModifierFlags.None)
        {
            Assert.That(GE_TestUI_DispatchPress(instanceId, eventName, x, y, (int)button, (int)mods, out int invoked),
                Is.EqualTo(0));
            return invoked;
        }

        /// <summary>A right press arrives as MouseButton.Right, with its position and modifiers.</summary>
        [Test]
        public void PointerPressed_ReportsTheButtonThePositionAndTheModifiers()
        {
            ulong instanceId = AddElement("grid-cell");
            Ui.PointerButtonArgs? seen = null;
            Ui.Element? cell = Ui.FindElement("grid-cell");
            Assert.That(cell, Is.Not.Null);
            cell!.PointerPressed += args => seen = args;

            Assert.That(Dispatch(instanceId, kMouseDown, 41.5f, 12.25f, MouseButton.Right, ModifierFlags.Shift),
                Is.EqualTo(1),
                "the press reached no listener");

            Assert.That(seen, Is.Not.Null, "the listener was never called");
            Assert.That(seen!.Value.Button, Is.EqualTo(MouseButton.Right));
            Assert.That(seen.Value.X, Is.EqualTo(41.5f));
            Assert.That(seen.Value.Y, Is.EqualTo(12.25f));
            Assert.That(seen.Value.Mods, Is.EqualTo(ModifierFlags.Shift));
        }

        /// <summary>A cancel reaches only PointerCancelled, never PointerReleased.</summary>
        [Test]
        public void PointerReleasedAndPointerCancelled_AreDistinctEvents()
        {
            ulong instanceId = AddElement("grid-cell");
            var heard = new List<string>();
            Ui.Element? cell = Ui.FindElement("grid-cell");
            Assert.That(cell, Is.Not.Null);
            cell!.PointerReleased += args => heard.Add($"released {args.Button}");
            cell.PointerCancelled += args => heard.Add($"cancelled {args.Button}");

            Dispatch(instanceId, kMouseUp, 0f, 0f, MouseButton.Left);
            Dispatch(instanceId, kMouseCancel, -1f, -1f, MouseButton.Middle);

            Assert.That(heard, Is.EqualTo(new[] { "released Left", "cancelled Middle" }));
        }
    }
}
