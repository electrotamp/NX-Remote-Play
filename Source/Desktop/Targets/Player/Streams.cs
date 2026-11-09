using FFmpeg.AutoGen;
using SDL2;
using NxRemotePlay.Client.Core;
using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using static FFmpeg.AutoGen.ffmpeg;

namespace NxRemotePlay.Client.Targets.Player
{
    abstract class BaseAudioPlayerTarget : OutStream
    {
        float _volume = 1;
        int SDL_Volume = SDL.SDL_MIX_MAXVOLUME;

        public float Volume
        {
            get => _volume;
            set
            {
                _volume = Math.Clamp(value, 0, 1);
                SDL_Volume = (int)(_volume * SDL.SDL_MIX_MAXVOLUME);
            }
        }

        static readonly ArrayPool<byte> EncodingPool = ArrayPool<byte>.Create();

        protected ref struct MixResult
        {
            public Span<byte> Span;
            public bool Owned;
            public byte[] PoolObject;
        }

        protected unsafe void MixToBuffer(Span<byte> data, Span<byte> destination)
        {

            // fast path
            if (_volume == 1)
            {
                data.CopyTo(destination);
                return;
            }

            destination.Fill(0);
            fixed (byte* output = destination, input = data)
            {
                SDL.SDL_MixAudioFormat(new IntPtr(output), new IntPtr(input), AudioPlayer.AudioFormat, (uint)data.Length, SDL_Volume);
            }
        }

        protected unsafe MixResult MixToTempBuffer(Span<byte> data)
        {
            // fast path
            if (_volume == 1)
                return new MixResult { Span = data, Owned = false };

            var poolObject = EncodingPool.Rent(data.Length);
            var result = poolObject.AsSpan(0, data.Length);
            MixToBuffer(data, result);

            return new MixResult { Span = result, Owned = true, PoolObject = poolObject };
        }

        protected void FreeTempMix(ref MixResult result)
        {
            if (result.Owned)
                EncodingPool.Return(result.PoolObject);
        }
    }

    // On some devices (mainly mac os) audio doesn't work with our AudioStreamTarget implementation
    // It's unclear why but SDL_QueueAudio seems to always work
    // However we lose control over A/V syncrhonization so this is really just a workaround for some platforms
    class QueuedStreamAudioTarget : BaseAudioPlayerTarget
    {
        // This is set by the SDL audio manager during initialization
        internal uint DeviceID;

        protected unsafe override void SendDataImpl(PoolBuffer block, ulong ts)
        {
            var mix = MixToTempBuffer(block.Span);

            fixed (byte* ptr = mix.Span)
                SDL.SDL_QueueAudio(DeviceID, ptr, (uint)mix.Span.Length);

            FreeTempMix(ref mix);
            block.Free();
        }
    }

    static class AudioStreamTargetNative
    {
        public static unsafe void SDLCallback(IntPtr userdata, IntPtr buf, int len) =>
            ((AudioPlayerTarget)GCHandle.FromIntPtr(userdata).Target).SDLCallback(new Span<byte>(buf.ToPointer(), len));
    }

    class AudioPlayerTarget : BaseAudioPlayerTarget
    {
        // Only as debug info
        readonly bool log = Program.Options.Debug.Log;

        public StreamSynchronizationHelper SyncHelper;

        // Sample queue to be submitted to SDL
        readonly BlockingCollection<(PoolBuffer, ulong)> samples = new BlockingCollection<(PoolBuffer, ulong)>(20);

        // The block we are now submitting (already removed from the queue)
        PoolBuffer? currentBlock;

        // The offset in currentBlock. -1 means we need to get a new block
        int currentOffset = -1;

        public void SDLCallback(Span<byte> buffer)
        {
            // This function is tricky because we need to handle feeding SDL exactly the amount of data it needs
            // this means we might need to dynamically dequeue blocks if the current one is not enough

            // Where there is data to push
            while (buffer.Length != 0 && !Cancel.IsCancellationRequested)
            {
                // If the current block is not empty
                if (currentOffset != -1)
                {
                    // Do not overflow either buffer
                    int toCopy = Math.Min(currentBlock.Length - currentOffset, buffer.Length);

                    // Perform the copy
                    var source = currentBlock.Span.Slice(currentOffset, toCopy);
                    MixToBuffer(source, buffer.Slice(0, toCopy));

                    // Cut the SDL buffer to the remainig free space
                    buffer = buffer.Slice(toCopy);

                    // Advance the offset in the current block
                    currentOffset += toCopy;
                }

                // If the block is exhausted get rid of it
                if (currentBlock is not null && currentOffset >= currentBlock.Length)
                {
                    currentOffset = -1;
                    currentBlock.Free();
                    currentBlock = null;
                }

                // If there is no current block and we still need to push more data
                if (currentOffset == -1 && buffer.Length != 0)
                {
                    try
                    {
                    again:
                        // Get the next block
                        var (block, ts) = samples.Take(Cancel);

                        // Make sure we can use it (note when SyncHelper is disabled this is always true)
                        if (!SyncHelper.CheckTimestamp(false, ts))
                        {
                            if (log)
                                Console.WriteLine($"Dropping audio packet with ts {ts}");

                            block.Free();
                            goto again;
                        }

                        currentBlock = block;
                        currentOffset = 0;
                    }
                    catch (OperationCanceledException)
                    {
                        return;
                    }
                }
            }
        }

        protected override void SendDataImpl(PoolBuffer block, ulong ts)
        {
            try
            {
                samples.Add((block, ts), Cancel);
                // Free is called by the consumer thread...
            }
            catch
            {
                block.Free();
            }
        }

        protected override void DisposeImpl()
        {
            // Free any remaining elements
            currentOffset = -1;
            currentBlock?.Free();
            samples.ToList().ForEach(x => x.Item1.Free());

            base.Dispose();
        }
    }

    class DecodeH264Target : OutStream
    {
        Task VideoConsumerTask = null!;

        readonly Channel<(PoolBuffer, ulong)> videoBuffer = Channel.CreateBounded<(PoolBuffer, ulong)>(
            new BoundedChannelOptions(50)
            {
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
                SingleWriter = true
            },
            (x) =>
            {
                x.Item1.Free();
            }
        );

        public int Pending;

        DecoderContext ctx;
        int timebase_den;
        unsafe AVPacket* packet;
        StreamSynchronizationHelper sync;
        bool log;
        AutoResetEvent onFrame;

        //  latency catch-up. The UI thread takes ONE decoded frame per vsync and the consumer below only sends the next packet when
        // that frame is taken, so a burst of packets (USB, host catching up after a slow frame) left a standing queue of up to 50 packets
        // that never shrank at a 60 Hz display and a ~58-60 fps stream: permanent extra delay. Since the audio-sync drop was removed
        // nothing trimmed it any more. Now, while more than CatchUpBacklog packets wait, each packet is still fully decoded (references stay
        // intact, so no artifacts) but its picture is thrown away instead of waiting for the next vsync; the queue drains in a few ms and the
        // normal one-frame-per-vsync pacing resumes with at most CatchUpBacklog packets of delay.
        const int CatchUpBacklog = 3;
        unsafe AVFrame* scratch;
        long catchUpFrames, loggedCatchUp, lastBacklogLog;
        int peakBacklog;

        unsafe void DiscardDecodedPicture()
        {
            if (scratch == null) scratch = av_frame_alloc();
            if (scratch == null) return;
            int rc;
            lock (ctx.CodecLock) rc = avcodec_receive_frame(ctx.CodecCtx, scratch);
            if (rc == 0) { av_frame_unref(scratch); catchUpFrames++; }
        }

        unsafe void FreeScratch()
        {
            var f = scratch;
            scratch = null;
            if (f != null) av_frame_free(&f);
        }

        // One line per 10 s in %LOCALAPPDATA%/NX-Remote-Play/Diagnostics/desktop-video.log, only when the queue was ever deeper than the limit.
        void NoteBacklog(int backlog)
        {
            if (backlog > peakBacklog) peakBacklog = backlog;
            long now = Environment.TickCount64;
            if (now - lastBacklogLog < 10000) return;
            lastBacklogLog = now;
            if (peakBacklog > CatchUpBacklog || catchUpFrames != loggedCatchUp)
            {
                try
                {
                    var dir = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "NX-Remote-Play", "Diagnostics");
                    System.IO.Directory.CreateDirectory(dir);
                    System.IO.File.AppendAllText(System.IO.Path.Combine(dir, "desktop-video.log"),
                        $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} video queue: peak {peakBacklog} packets in the last 10 s, {catchUpFrames - loggedCatchUp} pictures skipped to catch up (total {catchUpFrames}){Environment.NewLine}");
                }
                catch (Exception) { }
            }
            VideoFlowStats.Append(VideoFlowStats.TakeWindow());
            loggedCatchUp = catchUpFrames;
            peakBacklog = 0;
        }

        // The video player consumer may block but we don't want that when streaming over USB
        // since the same thread handles both audio and video, we use a buffer to avoid blocking
        async Task ConsumeVideoAsync()
        {
            var reader = videoBuffer.Reader;
            while (!Cancel.IsCancellationRequested)
            {
                var (buf, ts) = await reader.ReadAsync(Cancel).ConfigureAwait(false);
                bool success = false;

                Interlocked.Decrement(ref Pending);

                // Try at most 5 times ( = 5 frames since we lock on the frame event)
                for (int i = 0; i < 4; i++)
                {
                    // video is never dropped to resync audio. An H.264 P-frame that is thrown away leaves the decoder without its
                    // reference until the next IDR, which shows as black/garbled blocks (seen on the desktop viewer over USB while the host's
                    // frames ran late during Quick Settings: 84-98 ms frames, audio ahead of video by more than the 90 ms threshold). The call
                    // still updates the last video timestamp for the audio side, which drops audio when video is ahead.
                    sync.CheckTimestamp(true, ts);

                    var res = DecodePacket(buf, ts);

                    if (res == AVERROR(EAGAIN))
                    {
                        // Normally this only happens if the UI thread is not pulling video frames like when the window is being dragged
                        // However it also seems to happen for some specific games so simply dropping packets will cause visual artifacts
                        // Wait for the next frame and submit again
                        // Since we don't check this every frame we may wake up too early but only for a frame so it's fine
                        onFrame.WaitOne(500);

                        if (Cancel.IsCancellationRequested)
                            break;
                    }
                    else if (res != 0)
                    {
                        Program.DebugLog($"avcodec_send_packet {res}");
                        break;
                    }
                    else
                    {
                        success = true;
                        break;
                    }
                }

                // the backlog is the real queue length. Pending was incremented on every write but never decremented for the pictures the bounded
                // channel drops when it overflows (DropOldest, 50), so after one long UI stall it stayed above CatchUpBacklog for ever: every picture
                // was then decoded and thrown away and the desktop viewer showed a frozen frame while the stream kept running (seen 09:20, Pending 385).
                int backlog = videoBuffer.Reader.CanCount ? videoBuffer.Reader.Count : Volatile.Read(ref Pending);
                if (success && backlog > CatchUpBacklog) DiscardDecodedPicture();
                NoteBacklog(backlog);

                if (!success && log)
                    Console.WriteLine($"Dropping video packet because the UI thread is too late");

                buf.Free();
            }
        }

        unsafe public DecodeH264Target()
        {
            packet = av_packet_alloc();
            log = Program.Options.Debug.Log;
        }

        unsafe ~DecodeH264Target()
        {
            fixed (AVPacket** p = &packet)
                av_packet_free(p);
        }

        unsafe public void UseContext(DecoderContext ctx)
        {
            this.ctx = ctx;
            timebase_den = ctx.CodecCtx->time_base.den;
            sync = ctx.SyncHelper;
            onFrame = ctx.OnFrameEvent;
        }

        public override void UseCancellationToken(CancellationToken tok)
        {
            base.UseCancellationToken(tok);

            // Start the consumer only after the token is set
            VideoConsumerTask = Task.Run(ConsumeVideoAsync);
        }

        protected override async void DisposeImpl()
        {
            if (!Cancel.IsCancellationRequested)
                throw new Exception("Disposing without cancelling the token first");

            videoBuffer.Writer.Complete();

            try
            {
                // If the cancellation token is not set, the consumer thread has probably already terminated
                await VideoConsumerTask;
            }
            catch { /* ignore */ }

            FreeScratch();

            // Free any remaining buffers
            videoBuffer.Reader.ReadAllAsync()
                .ToBlockingEnumerable()
                .ToList()
                .ForEach(x => x.Item1.Free());

            base.Dispose();
        }

        long firstTs = -1;
        public unsafe int DecodePacket(PoolBuffer data, ulong ts)
        {
            int size = data.Length;

            if (firstTs == -1)
                firstTs = (long)ts;

            fixed (byte* nal_data = data.Span)
            {
                var pkt = packet;
                pkt->data = nal_data;
                pkt->size = size;
                pkt->pts = pkt->dts = (long)(((long)ts - firstTs) / 1E+6 * timebase_den);

                lock (ctx.CodecLock)
                    return avcodec_send_packet(ctx.CodecCtx, pkt);
            }
        }

        protected override void SendDataImpl(PoolBuffer block, ulong ts)
        {
            try
            {
                // Free is called by the consumer thread...
                _ = videoBuffer.Writer.WriteAsync((block, ts), Cancel);
                Interlocked.Increment(ref Pending);
                VideoFlowStats.Arrived();
            }
            catch
            {
                block.Free();
            }
        }
    }
}
