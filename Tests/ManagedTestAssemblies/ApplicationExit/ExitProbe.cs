using System;
using System.Threading;
using GameEngine.Scripting;

public class ExitProbe : GameSystem
{
    private int m_Updates;

    public override void OnUpdate(float deltaTime)
    {
        ++m_Updates;
        Console.WriteLine("EXIT_PROBE_UPDATE " + m_Updates);
        if (m_Updates != 3)
            return;

        switch (Environment.GetEnvironmentVariable("EXIT_PROBE_MODE"))
        {
            case "Default":
                Application.RequestExit();
                break;
            case "Posted":
                var postingWorker = new Thread(() => MainThread.Post(() => Application.RequestExit(23)));
                postingWorker.Start();
                postingWorker.Join();
                break;
            case "Refused":
                var refusedWorker = new Thread(RequestExitFromWorker);
                refusedWorker.Start();
                refusedWorker.Join();
                Application.RequestExit(23);
                break;
            default:
                Application.RequestExit(23);
                break;
        }

        Console.WriteLine("EXIT_PROBE_AFTER_REQUEST");
    }

    public override void OnDestroy()
    {
        Console.WriteLine("EXIT_PROBE_DESTROY " + m_Updates);
    }

    private static void RequestExitFromWorker()
    {
        try
        {
            Application.RequestExit(91);
            Console.WriteLine("EXIT_PROBE_UNEXPECTED_ACCEPT");
        }
        catch (EngineCallException exception)
        {
            Console.WriteLine("EXIT_PROBE_REFUSED " + exception.Message);
        }
    }
}
