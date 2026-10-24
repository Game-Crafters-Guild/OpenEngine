using System.Runtime.InteropServices;
using System.Text;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedBindingTests
{
    /// <summary>
    /// The generated RenderLayers class through the real GameEngine.Native, against a project that names its layers:
    /// text crosses in (Find) and out (NameOf) as UTF-8, a mask crosses as its bits. BindingTestScene
    /// (Tests/ScriptBindings) reads the project's names, as the editor's project open does.
    /// </summary>
    [NonParallelizable]
    public class RenderLayersBindingTests
    {
        private const string kScene = "BindingTestScene";
        private const string kForest = "Grün Wald 森";

        [DllImport(kScene, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestScene_LoadRenderLayerNames([MarshalAs(UnmanagedType.LPUTF8Str)] string projectRoot);

        private string m_Project = string.Empty;

        /// <summary>A generated call starts the standalone engine; then the project's names are read.</summary>
        [OneTimeSetUp]
        public void OpenProject()
        {
            Assert.That(RenderLayers.NameOf(RenderLayerMask.Default), Is.EqualTo("Default"));
            m_Project = Path.Combine(Path.GetTempPath(), "RenderLayersBindingTests-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Path.Combine(m_Project, ".Editor"));
            File.WriteAllText(Path.Combine(m_Project, ".Editor", "ProjectSettings.json"),
                              "{ \"rendering\": { \"renderLayerNames\": [\"\", \"Props\", \"\", \"" + kForest + "\"] } }",
                              new UTF8Encoding(false));
            Assert.That(GE_TestScene_LoadRenderLayerNames(m_Project), Is.EqualTo(0));
        }

        /// <summary>The names go back to the defaults.</summary>
        [OneTimeTearDown]
        public void CloseProject()
        {
            GE_TestScene_LoadRenderLayerNames(string.Empty);
            Directory.Delete(m_Project, recursive: true);
        }

        /// <summary>A name finds its layer as a mask of that layer; a name no layer has finds no layer.</summary>
        [Test]
        public void FindAnswersTheNamedLayer()
        {
            Assert.That(RenderLayers.Find("Props"), Is.EqualTo(RenderLayerMask.FromLayer(1)));
            Assert.That(RenderLayers.Find(kForest), Is.EqualTo(RenderLayerMask.FromLayer(3)));
            Assert.That(RenderLayers.Find("Default"), Is.EqualTo(RenderLayerMask.FromLayer(0)));
            Assert.That(RenderLayers.Find("Layer 2"), Is.EqualTo(RenderLayerMask.FromLayer(2)));
            Assert.That(RenderLayers.Find("props"), Is.EqualTo(RenderLayerMask.None), "the match is case-sensitive");
            Assert.That(RenderLayers.Find(string.Empty), Is.EqualTo(RenderLayerMask.None));
            Assert.Throws<ArgumentNullException>(() => RenderLayers.Find(null!));
        }

        /// <summary>A mask of one layer has the layer's name; any other mask has none.</summary>
        [Test]
        public void NameOfAnswersTheNameOfOneLayer()
        {
            Assert.That(RenderLayers.NameOf(RenderLayerMask.FromLayer(1)), Is.EqualTo("Props"));
            Assert.That(RenderLayers.NameOf(RenderLayerMask.FromLayer(3)), Is.EqualTo(kForest));
            Assert.That(RenderLayers.NameOf(RenderLayerMask.FromLayer(2)), Is.EqualTo("Layer 2"));
            Assert.That(RenderLayers.NameOf(RenderLayerMask.None), Is.Empty);
            Assert.That(RenderLayers.NameOf(RenderLayerMask.FromLayer(1) | RenderLayerMask.FromLayer(3)), Is.Empty);
        }

        /// <summary>A text longer than the stack buffer of a call goes through a pooled buffer, both ways.</summary>
        [Test]
        public void LongTextTakesAPooledBuffer()
        {
            string text = new string('森', 100);
            int bytes = Encoding.UTF8.GetByteCount(text);
            Assert.That(bytes, Is.GreaterThan(ScriptText.StackBytes));

            ScriptText.In encoded = new ScriptText.In(text, stackalloc byte[ScriptText.StackBytes]);
            try
            {
                Assert.That(Encoding.UTF8.GetString(encoded.Bytes), Is.EqualTo(text));
            }
            finally
            {
                encoded.Dispose();
            }

            ScriptText.Out decoded = new ScriptText.Out(stackalloc byte[ScriptText.StackBytes]);
            try
            {
                Assert.That(decoded.TryRead(bytes, out _), Is.False, "a text that did not fit is asked again");
                Assert.That(decoded.Bytes.Length, Is.GreaterThanOrEqualTo(bytes));
                Encoding.UTF8.GetBytes(text, decoded.Bytes);
                Assert.That(decoded.TryRead(bytes, out string read), Is.True);
                Assert.That(read, Is.EqualTo(text));
            }
            finally
            {
                decoded.Dispose();
            }
        }
    }
}
