using NxRemotePlay.Client.Targets;
using System;
using System.Diagnostics;
using System.Text;

namespace NxRemotePlay.Client.Core
{
    public class DebugOptions
    {
        // Verbose logging
        public bool Log;

        // SDL/Imgui debug window + framerate
        public bool GuiDebug;

        // Decode the content of keyframes and measure delay
        public bool Keyframe;

        // Decode the content of each nal and print the type
        public bool Nal;

        // Disable audio/video synchronization
        public bool NoSync;

        // Log SDL input events
        public bool SDLEvents;

        public bool RequiresH264Analysis => Keyframe || Nal;
    }

    class FramerateCounter
    {
        Stopwatch sw = new();
        uint frames = 0;

        public void Start()
        {
            sw.Restart();
        }

        public void OnFrame()
        {
            unchecked { frames++; }
        }

        public bool GetFps(out int fps)
        {
            if (sw.ElapsedMilliseconds >= 990)
            {
                fps = (int)(frames * 1000.0f / sw.ElapsedMilliseconds);
                frames = 0;
                sw.Restart();
                return true;
            }

            fps = 0;
            return false;
        }
    }

    class H264LoggingTarget : OutStream
    {
        readonly bool checkNal;
        readonly bool checkKeyframes;
        readonly StringBuilder sb = new();

        public H264LoggingTarget()
        {
            checkNal = Program.Options.Debug.Nal;
            checkKeyframes = Program.Options.Debug.Keyframe;
        }

        DateTime lastKeyframe = DateTime.Now;
        DateTime lastNal = DateTime.Now;

        protected override void SendDataImpl(PoolBuffer block, ulong ts)
        {
            sb.Clear();
            sb.Append('[');

            bool firstInSeq = true;
            foreach (var n in H264Util.EnumerateNals(block.ArraySegment))
            {
                var nal = block.Span.Slice(n.Start, n.Length);
                if (checkNal)
                {
                    if (firstInSeq)
                    {
                        var now = DateTime.Now;
                        var diff = (int)(now - lastNal).TotalMilliseconds;
                        sb.AppendFormat("{0}ms ", diff);
                        lastNal = now;
                        firstInSeq = false;
                    }

                    sb.AppendFormat("{0:x} ", nal[0] & 0x1F);
                }

                if (checkKeyframes)
                {
                    // IDR frame
                    if ((nal[0] & 0x1F) == 5)
                    {
                        var now = DateTime.Now;
                        var diff = now - lastKeyframe;
                        sb.AppendFormat("kf {0}ms ", (int)diff.TotalMilliseconds);
                        lastKeyframe = now;
                    }
                }
            }

            if (sb.Length != 1)
            {
                sb.Append("] ");
                Console.Write(sb.ToString());
            }

            block.Free();
        }
    }
}
