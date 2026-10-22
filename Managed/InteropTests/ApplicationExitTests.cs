using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    [TestFixture]
    internal class ApplicationExitTests
    {
        [TestCase("Application.RequestExit();")]
        [TestCase("Application.RequestExit(23);")]
        [TestCase("MainThread.Post(() => Application.RequestExit(-7));")]
        public void AGameScriptCanRequestExit(string statement)
        {
            var source = "using GameEngine.Scripting; public static class QuitGame { public static void Run() { " + statement + " } }";
            var result = ScriptCompileHelper.CompileCustomSources(source,
                extraReferencePaths: new[] { typeof(GameSystem).Assembly.Location });

            Assert.That(result.asm, Is.Not.Empty);
        }
    }
}
