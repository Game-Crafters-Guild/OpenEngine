using System.Numerics;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The generated Input class through the real GameEngine.Native: the table binds for the committed hash, and every
    /// crossing shape (a bool out, a Vector2 out, an enum out, a record out, enums and a float in, an id in) reaches the
    /// thunk it names. The process is a standalone engine with no input system, so a query answers its default and a request
    /// does nothing; an id of 0 is refused by the thunk of the function called, whose name the message carries.
    /// </summary>
    public class InputBindingTests
    {
        private static readonly StringId Gameplay = StringId.Of("Gameplay");
        private static readonly StringId Jump = StringId.Of("Gameplay.Jump");

        /// <summary>The queries answer their defaults.</summary>
        [Test]
        public void QueriesAnswerTheirDefaultsWithNoInputSystem()
        {
            Assert.That(Input.IsKeyDown(Key.W), Is.False);
            Assert.That(Input.WasMouseButtonPressed(MouseButton.Left), Is.False);
            Assert.That(Input.GetMouseDelta(), Is.EqualTo(Vector2.Zero));
            Assert.That(Input.GetPointerLockState(), Is.EqualTo(PointerLockState.Unlocked));
            ActionState state = Input.GetActionState(Jump);
            Assert.That(state.Pressed || state.JustPressed || state.JustReleased, Is.False);
            Assert.That(state.Value, Is.Zero);
        }

        /// <summary>The requests, the typed bindings among them, cross and do nothing.</summary>
        [Test]
        public void RequestsCrossAndDoNothingWithNoInputSystem()
        {
            Assert.DoesNotThrow(() =>
            {
                Input.RegisterAction(Gameplay, Jump, ActionKind.Button);
                Input.BindKey(Gameplay, Jump, Key.Space, forbiddenMods: ModifierFlags.Control | ModifierFlags.Alt);
                Input.BindKey(Gameplay, Jump, Key.A, -1f);
                Input.BindMouseButton(Gameplay, Jump, MouseButton.Back, 0.5f);
                Input.BindMouseAxis(Gameplay, Jump, MouseAxis.ScrollY, 0.5f);
                Input.BindGamepadButton(Gameplay, Jump, GamepadButton.A);
                Input.BindGamepadAxis(Gameplay, Jump, GamepadAxis.RightTrigger);
                Input.LockPointer();
                Input.UnlockPointer();
            });
        }

        /// <summary>An id of 0 throws ArgumentException with the text of the thunk the call reached.</summary>
        [TestCase("Input.GetActionState")]
        [TestCase("Input.RegisterAction")]
        [TestCase("Input.BindKey")]
        [TestCase("Input.PushContext")]
        public void IdZeroIsRefusedByTheThunkOfTheCall(string function)
        {
            StringId none = new StringId(0);
            TestDelegate call = function switch
            {
                "Input.GetActionState" => () => Input.GetActionState(none),
                "Input.RegisterAction" => () => Input.RegisterAction(none, Jump, ActionKind.Button),
                "Input.BindKey" => () => Input.BindKey(none, Jump, Key.Space),
                _ => () => Input.PushContext(none),
            };

            ArgumentException? error = Assert.Throws<ArgumentException>(call);

            Assert.That(error!.Message, Does.StartWith(function + ": `"));
            Assert.That(error.Message, Does.Contain("is 0, which names no id"));
        }
    }
}
