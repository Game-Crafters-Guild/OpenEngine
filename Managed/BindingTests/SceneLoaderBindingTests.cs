using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The generated SceneLoader class through the real GameEngine.Native: the scene's text and the reveal
    /// policy cross in, the operation crosses out and back, and a refused operation is an EngineCallException.
    /// The test host runs no frame, so a load stays queued. SceneLoaderScriptApiTests polls loads to ready
    /// on a headless engine.
    /// </summary>
    [NonParallelizable]
    public class SceneLoaderBindingTests
    {
        /// <summary>A queued load is loading, at 0 and not ready; the operation of no load answers 0 and false.</summary>
        [Test]
        public void AQueuedLoadIsLoadingAndNotReady()
        {
            SceneLoadOperation load = SceneLoader.Load("Scenes/Grün 森.scene", SceneRevealPolicy.WhenGameReleases);
            Assert.That(load.Id, Is.Not.EqualTo(0UL));
            Assert.That(SceneLoader.IsLoading(), Is.True);
            Assert.That(SceneLoader.Progress(load), Is.EqualTo(0f));
            Assert.That(SceneLoader.IsReadyToShow(load), Is.False);
            Assert.DoesNotThrow(() => SceneLoader.Reveal(load));

            var none = new SceneLoadOperation();
            Assert.That(SceneLoader.Progress(none), Is.EqualTo(0f));
            Assert.That(SceneLoader.IsReadyToShow(none), Is.False);
            Assert.DoesNotThrow(() => SceneLoader.Reveal(none));
        }

        /// <summary>After a newer Load, a call with the old operation throws, and the message names the fix.</summary>
        [Test]
        public void ANewerLoadReplacesTheOperation()
        {
            SceneLoadOperation first = SceneLoader.Load("Scenes/A.scene");
            SceneLoadOperation second = SceneLoader.Load("Scenes/B.scene");
            EngineCallException error = Assert.Throws<EngineCallException>(() => SceneLoader.Progress(first))!;
            Assert.That(error.Message, Does.Contain("SceneLoader.Progress").And.Contain("the newest Load returned"));
            Assert.That(SceneLoader.IsReadyToShow(second), Is.False);
        }
    }
}
