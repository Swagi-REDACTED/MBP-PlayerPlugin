using System;
using System.IO;
using System.IO.Pipes;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using MovieBoxPlayerMod.Bridge;

namespace MovieBoxPlayerMod.Player;

internal sealed class BridgeClient : IDisposable
{
    private readonly NamedPipeClientStream pipe;
    private StreamReader? reader;
    private StreamWriter? writer;
    private readonly SemaphoreSlim gate = new(1, 1);
    internal BridgeClient(string name) => pipe = new(".", name, PipeDirection.InOut, PipeOptions.Asynchronous);

    internal async Task ConnectAsync()
    {
        await pipe.ConnectAsync(10000);
        reader = new StreamReader(pipe);
        writer = new StreamWriter(pipe) { AutoFlush = true };
    }

    internal async Task<PlaybackReply> SendAsync(PlaybackRequest request)
    {
        await gate.WaitAsync();
        try
        {
            await writer!.WriteLineAsync(JsonSerializer.Serialize(request));
            string? line = await reader!.ReadLineAsync().WaitAsync(TimeSpan.FromSeconds(30));
            return line == null ? throw new IOException("MovieBoxPro disconnected.") :
                JsonSerializer.Deserialize<PlaybackReply>(line) ?? throw new IOException("Invalid playback response.");
        }
        catch { pipe.Dispose(); throw; } // A timed-out response must never be mistaken for the next request's reply.
        finally { gate.Release(); }
    }

    public void Dispose()
    {
        try { writer?.Dispose(); } catch (IOException) { } catch (ObjectDisposedException) { }
        try { reader?.Dispose(); } catch (IOException) { } catch (ObjectDisposedException) { }
        pipe.Dispose();
    }
}
