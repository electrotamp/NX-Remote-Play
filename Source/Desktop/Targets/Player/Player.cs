using FFmpeg.AutoGen;
using NxRemotePlay.Client.Core;
using NxRemotePlay.Client.Sources;
using System;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using static FFmpeg.AutoGen.ffmpeg;
using static SDL2.SDL;

namespace NxRemotePlay.Client.Targets.Player
{
    unsafe class DecoderContext
    {
        public AVCodecContext* CodecCtx { get; init; }

        public AVFrame* RenderFrame;
        public AVFrame* ReceiveFrame;

        public AVFrame* Frame1 { get; init; }
        public AVFrame* Frame2 { get; init; }

        public object CodecLock { get; init; }

        public StreamSynchronizationHelper SyncHelper;
        public AutoResetEvent OnFrameEvent;
    }

    unsafe struct FormatConverterContext
    {
        public SwsContext* Converter { get; init; }
        public AVFrame* Frame { get; init; }
    }

    // Guarantees that the audio and video stream are compatible with the player
    class PlayerManager : StreamManager
    {
        internal new DecodeH264Target VideoTarget;
        internal new BaseAudioPlayerTarget AudioTarget;

        static OutStream? MakeAudioStream(bool hasAudio)
        {
            if (!hasAudio)
                return null;

            var useCompat = Program.Options.AudioPlayerMode switch
            {
                SDLAudioMode.Compatible => true,
                SDLAudioMode.Default => false,
                _ => false
            };

            if (useCompat)
                return new QueuedStreamAudioTarget();
            else
                return new AudioPlayerTarget();
        }

        static OutStream? MakeVideoStream(bool hasVideo)
        {
            if (!hasVideo)
                return null;

            return new DecodeH264Target();
        }

        public void UseSyncManager(StreamSynchronizationHelper manager)
        {
            if (AudioTarget is AudioPlayerTarget)
            {
                ((AudioPlayerTarget)AudioTarget).SyncHelper = manager;
            }
        }

        public PlayerManager(StreamingSource source, CancellationTokenSource cancel) :
            base(source, MakeVideoStream(source.Options.HasVideo), MakeAudioStream(source.Options.HasAudio), cancel)
        {
            VideoTarget = base.VideoTarget as DecodeH264Target;
            AudioTarget = base.AudioTarget as BaseAudioPlayerTarget;
        }
    }

    class AudioPlayer : IDisposable
    {
        readonly uint DeviceID;

        // Are we using the MacOS strategy ?
        readonly bool IsCompatiblePlayer;

        // Keep a reference to the callback to prevent GC from collecting it
        SDL_AudioCallback? CallbackDelegate;

        // Manually pin the Target object so it can be used as opaque pointer for the native code
        GCHandle TargetHandle;

        public const ushort AudioFormat = AUDIO_S16LSB;

        public AudioPlayer(BaseAudioPlayerTarget target)
        {
            Program.SdlCtx.BugCheckThreadId();

            IsCompatiblePlayer = target is QueuedStreamAudioTarget;

            SDL_AudioSpec wantedSpec = new SDL_AudioSpec()
            {
                channels = StreamInfo.AudioChannels,
                format = AudioFormat,
                freq = StreamInfo.AudioSampleRate,
                // StreamInfo.MinAudioSamplesPerPayload * 2 was the default until nxremoteplay 5.4
                // however SDL will pick its preferred buffer size since we pass SDL_AUDIO_ALLOW_SAMPLES_CHANGE,
                // this is fine since we have our own buffering.
                samples = StreamInfo.MinAudioSamplesPerPayload,
            };

            if (!IsCompatiblePlayer)
            {
                TargetHandle = GCHandle.Alloc(target, GCHandleType.Normal);
                CallbackDelegate = AudioStreamTargetNative.SDLCallback;
                wantedSpec.callback = CallbackDelegate;
                wantedSpec.userdata = GCHandle.ToIntPtr(TargetHandle);
            }

            DeviceID = SDL_OpenAudioDevice(IntPtr.Zero, 0, ref wantedSpec, out var obtained, (int)SDL_AUDIO_ALLOW_SAMPLES_CHANGE);

            DeviceID.AssertNotZero(SDL_GetError);

            Program.DebugLog($"SDL_Audio: requested samples per callback={wantedSpec.samples} obtained={obtained.samples}");

            if (IsCompatiblePlayer)
            {
                ((QueuedStreamAudioTarget)(target)).DeviceID = DeviceID;
            }
        }

        public void Pause()
        {
            SDL_PauseAudioDevice(DeviceID, 1);
        }

        public void Resume()
        {
            SDL_PauseAudioDevice(DeviceID, 0);
        }

        public void Dispose()
        {
            Pause();
            SDL_CloseAudioDevice(DeviceID);

            if (TargetHandle.IsAllocated)
                TargetHandle.Free();
        }
    }

    class VideoPlayer : IDisposable
    {
        public const AVPixelFormat TargetDecodingFormat = AVPixelFormat.AV_PIX_FMT_YUV420P;
        public readonly uint TargetTextureFormat = SDL_PIXELFORMAT_IYUV;

        public DecoderContext Decoder { get; private set; }
        FormatConverterContext Converter; // Initialized only when the decoder output format doesn't match the SDL texture format

        public IntPtr TargetTexture;
        public SDL_Rect TargetTextureSize;

        // Screenshot fallback (see StreamManager.OnScreenshotFrame / capture.h on the
        // sysmodule side): a separate MJPEG decode path reusing the same FFmpeg/sws_scale
        // machinery as the real H264 pipeline, rather than pulling in a whole separate
        // image library for something this small. Decoding happens in DecodeFrame() on the
        // render thread, same as the real video path - this field is just the handoff.
        unsafe AVCodecContext* jpegCodecCtx;
        unsafe AVFrame* jpegDecodeFrame;
        unsafe AVFrame* jpegYuvFrame;
        unsafe SwsContext* jpegToYuvConverter;
        unsafe AVPacket* jpegPacket;
        byte[]? pendingScreenshotJpeg;

        public VideoPlayer(string? preferredDecoderName)
        {
            InitVideoDecoder(preferredDecoderName);
            InitSDLRenderTexture();
            InitScreenshotDecoder();
        }

        // Not fatal if this fails - screenshot fallback frames just won't display, real
        // video streaming is unaffected either way.
        unsafe void InitScreenshotDecoder()
        {
            try
            {
                AVCodec* jpegCodec = avcodec_find_decoder(AVCodecID.AV_CODEC_ID_MJPEG);
                if (jpegCodec == null)
                    return;

                jpegCodecCtx = avcodec_alloc_context3(jpegCodec);
                if (jpegCodecCtx == null)
                    return;

                if (avcodec_open2(jpegCodecCtx, jpegCodec, null) != 0)
                    return;

                jpegDecodeFrame = av_frame_alloc();
                jpegPacket = av_packet_alloc();
            }
            catch (Exception ex)
            {
                Program.DebugLog($"Screenshot decoder init failed (non-fatal): {ex.Message}");
            }
        }

        // Called from any thread when a screenshot packet arrives - just hands off the
        // bytes, the actual decode happens on the render thread in DecodeFrame(). The
        // timestamp isn't currently used (screenshots aren't synced against anything) but
        // is accepted to match StreamManager.OnScreenshotFrame's signature.
        public void SubmitScreenshotFrame(byte[] jpegData, ulong timestamp)
        {
            Interlocked.Exchange(ref pendingScreenshotJpeg, jpegData);
        }

        unsafe bool TryDecodePendingScreenshot()
        {
            var jpeg = Interlocked.Exchange(ref pendingScreenshotJpeg, null);
            if (jpeg is null || jpegCodecCtx == null)
                return false;

            fixed (byte* data = jpeg)
            {
                jpegPacket->data = data;
                jpegPacket->size = jpeg.Length;

                if (avcodec_send_packet(jpegCodecCtx, jpegPacket) != 0)
                    return false;

                if (avcodec_receive_frame(jpegCodecCtx, jpegDecodeFrame) != 0)
                    return false;
            }

            if (jpegYuvFrame == null)
            {
                jpegYuvFrame = av_frame_alloc();
                jpegYuvFrame->format = (int)TargetDecodingFormat;
                jpegYuvFrame->width = StreamInfo.VideoWidth;
                jpegYuvFrame->height = StreamInfo.VideoHeight;
                av_frame_get_buffer(jpegYuvFrame, 32).AssertZero("Couldn't allocate the buffer for the screenshot frame");
            }

            if (jpegToYuvConverter == null || jpegDecodeFrame->width != lastJpegWidth || jpegDecodeFrame->height != lastJpegHeight)
            {
                if (jpegToYuvConverter != null)
                    sws_freeContext(jpegToYuvConverter);

                lastJpegWidth = jpegDecodeFrame->width;
                lastJpegHeight = jpegDecodeFrame->height;

                jpegToYuvConverter = sws_getContext(
                    jpegDecodeFrame->width, jpegDecodeFrame->height, (AVPixelFormat)jpegDecodeFrame->format,
                    StreamInfo.VideoWidth, StreamInfo.VideoHeight, TargetDecodingFormat,
                    SWS_FAST_BILINEAR, null, null, null);

                if (jpegToYuvConverter == null)
                    return false;
            }

            sws_scale(jpegToYuvConverter, jpegDecodeFrame->data, jpegDecodeFrame->linesize, 0, jpegDecodeFrame->height,
                jpegYuvFrame->data, jpegYuvFrame->linesize);

            UpdateSDLTexture(jpegYuvFrame);
            return true;
        }

        int lastJpegWidth = -1, lastJpegHeight = -1;

        void InitSDLRenderTexture()
        {
            Program.SdlCtx.BugCheckThreadId();

            var tex = SDL_CreateTexture(Program.SdlCtx.RendererHandle, TargetTextureFormat,
                (int)SDL_TextureAccess.SDL_TEXTUREACCESS_STREAMING,
                StreamInfo.VideoWidth, StreamInfo.VideoHeight).AssertNotNull(SDL_GetError);

            if (Program.Options.Debug.Log)
            {
                SDL_QueryTexture(tex, out var format, out var a, out var w, out var h);
                Console.WriteLine($"SDL texture info: f = {SDL_GetPixelFormatName(format)} a = {a} w = {w} h = {h}");

                SDL_RendererInfo info;
                SDL_GetRendererInfo(Program.SdlCtx.RendererHandle, out info);
                for (int i = 0; i < info.num_texture_formats; i++) unsafe
                    {
                        Console.WriteLine($"Renderer supports pixel format {SDL_GetPixelFormatName(info.texture_formats[i])}");
                    }
            }

            TargetTextureSize = new SDL_Rect() { x = 0, y = 0, w = StreamInfo.VideoWidth, h = StreamInfo.VideoHeight };
            TargetTexture = tex;
        }

        unsafe void InitVideoDecoder(string? name)
        {
            AVCodec* codec = null;

            if (name is not null)
            {
                codec = avcodec_find_decoder_by_name(name);
            }

            if (codec == null)
                codec = avcodec_find_decoder(AVCodecID.AV_CODEC_ID_H264);

            if (codec == null)
                throw new Exception("Couldn't find any compatible video codecs");

            Decoder = CreateDecoderContext(codec);
        }

        static unsafe DecoderContext CreateDecoderContext(AVCodec* codec)
        {
            if (codec == null)
                throw new Exception("Codec can't be null");

            string codecName = Marshal.PtrToStringAnsi((IntPtr)codec->name);

            Console.WriteLine(string.Format(Program.Strings.Player.PlayerInitializationMessage, codecName));

            var codectx = avcodec_alloc_context3(codec);
            if (codectx == null)
                throw new Exception("Couldn't allocate a codec context");

            // These are set in ffplay
            codectx->codec_id = codec->id;
            codectx->codec_type = AVMediaType.AVMEDIA_TYPE_VIDEO;
            codectx->bit_rate = 0;

            // Some decoders break without this
            codectx->width = StreamInfo.VideoWidth;
            codectx->height = StreamInfo.VideoHeight;

            var (ex, sz) = LibavUtils.AllocateH264Extradata();
            codectx->extradata_size = sz;
            codectx->extradata = (byte*)ex.ToPointer();

            avcodec_open2(codectx, codec, null).AssertZero("Couldn't open the codec.");

            var pic = av_frame_alloc();
            if (pic == null)
                throw new Exception("Couldn't allocate the decoding frame");

            var pic2 = av_frame_alloc();
            if (pic2 == null)
                throw new Exception("Couldn't allocate the decoding frame");

            return new DecoderContext()
            {
                CodecCtx = codectx,
                Frame1 = pic,
                Frame2 = pic2,
                ReceiveFrame = pic,
                RenderFrame = pic2,
                CodecLock = new object(),
                OnFrameEvent = new AutoResetEvent(true)
            };
        }

        public unsafe bool DecodeFrame()
        {
            // A pending screenshot takes priority and updates the same texture the H264
            // path uses - once shown it just stays on screen (nothing to "undo") until
            // either a new screenshot or a real H264 frame overwrites it, so no separate
            // "frozen frame" state is needed here.
            VideoFlowStats.UiCall();
            if (TryDecodePendingScreenshot())
                return true;

            if (DecodeFrameInternal())
            {
                VideoFlowStats.Presented();
                // TODO: this call is needed only with opengl on linux (and not on every linux install i tested) where TextureUpdate must be called by the main thread,
                // Check if are there any performance improvements by moving this to the decoder thread on other OSes
                UpdateSDLTexture(Decoder.RenderFrame);

                return true;
            }
            return false;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        unsafe void UpdateSDLTexture(AVFrame* pic)
        {
            if (pic->linesize[0] > 0 && pic->linesize[1] > 0 && pic->linesize[2] > 0)
            {
                SDL_UpdateYUVTexture(TargetTexture, ref TargetTextureSize,
                    (IntPtr)pic->data[0], pic->linesize[0],
                    (IntPtr)pic->data[1], pic->linesize[1],
                    (IntPtr)pic->data[2], pic->linesize[2]);
            }
            // While this doesn't seem to be handled in ffplay but the texture can be non-planar with some decoders
            else if (pic->linesize[0] > 0 && pic->linesize[1] == 0)
            {
                SDL_UpdateTexture(TargetTexture, ref TargetTextureSize, (nint)pic->data[0], pic->linesize[0]);
            }
            else Console.WriteLine($"Error: Non-positive planar linesizes are not supported, open an issue on Github. {pic->linesize[0]} {pic->linesize[1]} {pic->linesize[2]}");
        }

        bool converterFirstFrameCheck = false;
        unsafe bool DecodeFrameInternal()
        {
            int ret = 0;

            lock (Decoder.CodecLock)
                ret = avcodec_receive_frame(Decoder.CodecCtx, Decoder.ReceiveFrame);

            if (ret == AVERROR(EAGAIN))
            {
                // Try again for the next SDL frame
                return false;
            }
            else if (ret != 0)
            {
                // Should not happen
                Program.DebugLog($"avcodec_receive_frame {ret}");
                return false;
            }
            else
            {
                // On the first frame we get check if we need to use a converter
                if (!converterFirstFrameCheck && Decoder.CodecCtx->pix_fmt != AVPixelFormat.AV_PIX_FMT_NONE)
                {
                    Program.DebugLog($"Decoder.CodecCtx uses pixel format {Decoder.CodecCtx->pix_fmt}");

                    converterFirstFrameCheck = true;
                    if (Decoder.CodecCtx->pix_fmt != TargetDecodingFormat)
                    {
                        Converter = InitializeConverter(Decoder.CodecCtx);
                        // Render to the converted frame
                        Decoder.RenderFrame = Converter.Frame;
                    }
                }

                if (Converter.Converter != null)
                {
                    var source = Decoder.ReceiveFrame;
                    var target = Decoder.RenderFrame;
                    sws_scale(Converter.Converter, source->data, source->linesize, 0, source->height, target->data, target->linesize);
                }
                else
                {
                    // Swap the frames so we can render source
                    var toRender = Decoder.ReceiveFrame;
                    var receiveNext = Decoder.RenderFrame;

                    Decoder.ReceiveFrame = receiveNext;
                    Decoder.RenderFrame = toRender;
                }

                VirtualCameraFeed.Instance?.Offer(Decoder.RenderFrame);   // the camera shows this same decoded picture (cheap unless a program reads it)
                return true;
            }
        }

        unsafe static FormatConverterContext InitializeConverter(AVCodecContext* codecctx)
        {
            AVFrame* dstframe = null;
            SwsContext* swsContext = null;

            Program.DebugLog($"Initializing converter for {codecctx->pix_fmt}");

            dstframe = av_frame_alloc();

            if (dstframe == null)
                throw new Exception("Couldn't allocate the the converted frame");

            dstframe->format = (int)TargetDecodingFormat;
            dstframe->width = StreamInfo.VideoWidth;
            dstframe->height = StreamInfo.VideoHeight;

            av_frame_get_buffer(dstframe, 32).AssertZero("Couldn't allocate the buffer for the converted frame");

            swsContext = sws_getContext(codecctx->width, codecctx->height, codecctx->pix_fmt,
                                        dstframe->width, dstframe->height, (AVPixelFormat)dstframe->format,
                                        SWS_FAST_BILINEAR, null, null, null);

            if (swsContext == null)
                throw new Exception("Couldn't initialize the converter");

            return new FormatConverterContext()
            {
                Converter = swsContext,
                Frame = dstframe
            };
        }

        public unsafe void Dispose()
        {
            var ptr = Decoder.Frame1;
            av_frame_free(&ptr);

            ptr = Decoder.Frame2;
            av_frame_free(&ptr);

            var ptr2 = Decoder.CodecCtx;
            avcodec_free_context(&ptr2);

            if (Converter.Converter != null)
            {
                var ptr3 = Converter.Frame;
                av_frame_free(&ptr3);

                sws_freeContext(Converter.Converter);
            }

            if (TargetTexture != 0)
                SDL_DestroyTexture(TargetTexture);

            Decoder.OnFrameEvent.Dispose();

            if (jpegDecodeFrame != null)
            {
                var jptr = jpegDecodeFrame;
                av_frame_free(&jptr);
            }

            if (jpegYuvFrame != null)
            {
                var jptr = jpegYuvFrame;
                av_frame_free(&jptr);
            }

            if (jpegPacket != null)
            {
                var jptr = jpegPacket;
                av_packet_free(&jptr);
            }

            if (jpegToYuvConverter != null)
                sws_freeContext(jpegToYuvConverter);

            if (jpegCodecCtx != null)
            {
                var jptr = jpegCodecCtx;
                avcodec_free_context(&jptr);
            }
        }
    }
}
