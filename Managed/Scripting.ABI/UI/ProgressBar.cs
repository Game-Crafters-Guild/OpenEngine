namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A native &lt;progress&gt; bar. <see cref="FindElement"/> returns this type for one.
        /// </summary>
        public class ProgressBar : Element
        {
            internal ProgressBar(ulong instanceId)
                : base(instanceId)
            {
            }

            /// <summary>Build a custom control over an existing progress bar.</summary>
            protected ProgressBar(ProgressBar source)
                : base(source)
            {
            }

            /// <summary>
            /// The bar's value, from 0 to <see cref="Max"/>. A value outside the range is
            /// clamped natively and reads back clamped. A bar announces no change event. Reads 0
            /// when the element does not resolve; a write on a dead element is a no-op.
            /// </summary>
            public float Value
            {
                get => GE_UIElement_GetValueFloat(InstanceId, out float value) == 0 ? value : 0f;
                set => GE_UIElement_SetValueFloat(InstanceId, value, 0);
            }

            /// <summary>
            /// The end of the range; 1 unless the layout's <c>max</c> sets it. A new max clamps
            /// <see cref="Value"/> again. A max of 0 or less, or one that is not finite, is
            /// refused and the range stays as it was. Reads 0 when the element does not resolve.
            /// </summary>
            public float Max
            {
                get => GE_UIElement_GetProgressMax(InstanceId, out float max) == 0 ? max : 0f;
                set => GE_UIElement_SetProgressMax(InstanceId, value);
            }

            /// <summary>
            /// True while the bar shows a load with no known end: a short segment at the start
            /// edge instead of the value. The value is kept and shows again when this is false.
            /// </summary>
            public bool Indeterminate
            {
                get => GE_UIElement_GetProgressIndeterminate(InstanceId, out int on) == 0 && on != 0;
                set => GE_UIElement_SetProgressIndeterminate(InstanceId, value ? 1 : 0);
            }
        }
    }
}
