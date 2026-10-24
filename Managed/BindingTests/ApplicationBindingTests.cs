using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The generated Application class through the real GameEngine.Native: the binding acquires its table
    /// and calls the thunk. The test host starts the standalone engine and runs no application, so a
    /// request has nothing to end and does nothing. EngineApplicationExitTests pins the Player's answer and
    /// the thread refusals; PlayerManagedExitTests runs a C# script that ends a Player.
    /// </summary>
    [NonParallelizable]
    public class ApplicationBindingTests
    {
        /// <summary>A request with no application returns without an error, with and without a code.</summary>
        [Test]
        public void ARequestWithNoApplicationDoesNothing()
        {
            Assert.DoesNotThrow(() => Application.RequestExit());
            Assert.DoesNotThrow(() => Application.RequestExit(23));
        }
    }
}
