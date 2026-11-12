// A module that DbgHelpSearchPathTests copies to a new folder and symbolizes.
// The build puts it in a folder of its own, which no loaded module shares.

extern "C" __declspec(dllexport) int DbgHelpSearchPathProbe(int value)
{
    return value + 1;
}
