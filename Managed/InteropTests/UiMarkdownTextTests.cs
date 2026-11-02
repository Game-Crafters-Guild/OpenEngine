using System;
using System.Runtime.InteropServices;
using System.Text;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    internal class UiMarkdownTextTests
    {
        private const string kLib = "GameEngine.Native";

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_AddElement(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string elementId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outInstanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_DestroyElement(ulong instanceId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_GetMarkdownText(ulong instanceId, [Out] byte[]? buffer,
            int bufferLen, out int outLen);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Dispatch(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName, float x, float y, int mods,
            out int outInvoked);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_TestUI_Reset();

        [SetUp]
        public void Reset() => Assert.That(GE_TestUI_Reset(), Is.Zero);

        private static Ui.MarkdownText AddNotes(out ulong instanceId)
        {
            Assert.That(GE_TestUI_AddElement("notes", "markdowntext", out instanceId), Is.Zero);
            Ui.Element? element = Ui.FindElement("notes");
            Assert.That(element, Is.InstanceOf<Ui.MarkdownText>());
            return (Ui.MarkdownText)element!;
        }

        private static string ReadMarkdown(ulong instanceId)
        {
            Assert.That(GE_TestUI_GetMarkdownText(instanceId, null, 0, out int length), Is.Zero);
            byte[] buffer = new byte[length];
            Assert.That(GE_TestUI_GetMarkdownText(instanceId, buffer, buffer.Length, out int copied), Is.Zero);
            Assert.That(copied, Is.EqualTo(length));
            return Encoding.UTF8.GetString(buffer);
        }

        [Test]
        public void FindElementReturnsTheSameMarkdownTextWrapper()
        {
            Ui.MarkdownText notes = AddNotes(out _);
            Assert.That(Ui.FindElement("notes"), Is.SameAs(notes));
        }

        [Test]
        public void SetTextPreservesUtf8AndClearsEmptyOrNullText()
        {
            Ui.MarkdownText notes = AddNotes(out ulong instanceId);
            string markdown = "## Café 🎮\n**" + new string('界', 300) + "**";
            Assert.That(notes.SetText(markdown), Is.True);
            Assert.That(ReadMarkdown(instanceId), Is.EqualTo(markdown));
            Assert.That(notes.SetText(string.Empty), Is.True);
            Assert.That(ReadMarkdown(instanceId), Is.Empty);
            Assert.That(notes.SetText(markdown), Is.True);
            Assert.That(notes.SetText(null), Is.True);
            Assert.That(ReadMarkdown(instanceId), Is.Empty);
        }

        [Test]
        public void DestroyedWrapperCannotWriteToItsReplacement()
        {
            Ui.MarkdownText oldNotes = AddNotes(out ulong oldId);
            Assert.That(GE_TestUI_DestroyElement(oldId), Is.Zero);
            Ui.MarkdownText replacement = AddNotes(out ulong replacementId);
            Assert.That(replacement.SetText("replacement"), Is.True);
            Assert.That(oldNotes.IsAlive, Is.False);
            Assert.That(oldNotes.SetText("stale"), Is.False);
            Assert.That(ReadMarkdown(replacementId), Is.EqualTo("replacement"));
        }

        [Test]
        public void PointerEnteredUsesTheElementSubscriptionLifecycle()
        {
            Ui.MarkdownText notes = AddNotes(out ulong instanceId);
            int calls = 0;
            Ui.PointerArgs seen = default;
            Action<Ui.PointerArgs> handler = args =>
            {
                ++calls;
                seen = args;
            };
            notes.PointerEntered += handler;
            Assert.That(GE_TestUI_Dispatch(instanceId, "UI.MouseEnter", 12f, 34f, 0, out int invoked), Is.Zero);
            Assert.That(invoked, Is.EqualTo(1));
            Assert.That(calls, Is.EqualTo(1));
            Assert.That(seen.X, Is.EqualTo(12f));
            Assert.That(seen.Y, Is.EqualTo(34f));
            notes.PointerEntered -= handler;
            Assert.That(GE_TestUI_Dispatch(instanceId, "UI.MouseEnter", 12f, 34f, 0, out invoked), Is.Zero);
            Assert.That(invoked, Is.Zero);
        }
    }
}
