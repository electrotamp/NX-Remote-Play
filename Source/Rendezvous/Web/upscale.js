'use strict';
// Picture upscaling in the browser. The Switch sends 1280x720; when the picture is shown larger than that, the browser stretches it with
// plain bilinear filtering. With this on (menu: "Picture upscaling"), each decoded video frame is drawn on a WebGL canvas over the video:
//   FSR   = AMD FidelityFX Super Resolution 1 (EASU edge-adaptive upscale, then RCAS sharpening; needs WebGL 2; fsr.js holds AMD's shader text)
//   Cubic = a lighter Catmull-Rom filter with a little sharpening (WebGL 1; also used when FSR cannot start)
// Nothing is added to the stream. Off by default; any problem (no WebGL, no per-frame callback, lost context) puts the plain video back.
(function () {
  const el = (id) => document.getElementById(id);
  const prefs = {
    get(k, f) {
      try {
        const v = localStorage.getItem('nx.' + k);
        return v === null ? f : v;
      } catch {
        return f;
      }
    },
    set(k, v) {
      try {
        localStorage.setItem('nx.' + k, String(v));
      } catch {}
    },
  };
  const names = ['Off', 'FSR', 'Cubic'];
  let mode = Math.min(2, Math.max(0, parseInt(prefs.get('upscale', '0'), 10) || 0));
  let canvas = null,
    gl = null,
    kind = '',
    callbackId = 0,
    failed = false,
    resizeObserver = null,
    fsrNote = '',
    state = {};
  const video = el('video'),
    viewport = el('viewport');
  const supported = !!(
    video &&
    viewport &&
    'requestVideoFrameCallback' in HTMLVideoElement.prototype
  );
  const RCAS_STOPS = 0.25; // 0 = sharpest, 2 = none; AMD's own default is about 0.2

  const f32 = new Float32Array(1),
    u32 = new Uint32Array(f32.buffer);
  const bits = (v) => {
    f32[0] = v;
    return u32[0];
  };
  function half(v) {
    f32[0] = v;
    const x = u32[0],
      sign = (x >>> 16) & 0x8000;
    let e = ((x >>> 23) & 0xff) - 127 + 15,
      m = x & 0x7fffff;
    if (e <= 0) {
      if (e < -10) return sign;
      m = (m | 0x800000) >> (1 - e);
      return sign | ((m + 0x1000) >> 13);
    }
    if (e >= 31) return sign | 0x7c00;
    return sign | (e << 10) | ((m + 0x1000) >> 13);
  }

  // ---- Cubic (WebGL 1) ----
  const cubicVertex =
    'attribute vec2 p;varying vec2 uv;void main(){gl_Position=vec4(p,0.0,1.0);uv=vec2(p.x*0.5+0.5,0.5-p.y*0.5);}';
  const cubicFragment = `precision highp float;
uniform sampler2D tex;uniform vec2 srcSize;uniform vec2 params;varying vec2 uv;
vec3 cr(vec2 q){
  vec2 pos=q*srcSize;vec2 c=floor(pos-0.5)+0.5;vec2 f=pos-c;
  vec2 w0=f*(-0.5+f*(1.0-0.5*f));vec2 w1=1.0+f*f*(-2.5+1.5*f);vec2 w2=f*(0.5+f*(2.0-1.5*f));vec2 w3=f*f*(-0.5+0.5*f);
  vec2 w12=w1+w2;vec2 t0=(c-1.0)/srcSize;vec2 t12=(c+w2/w12)/srcSize;vec2 t3=(c+2.0)/srcSize;
  vec3 r=vec3(0.0);
  r+=texture2D(tex,vec2(t0.x,t0.y)).rgb*(w0.x*w0.y);r+=texture2D(tex,vec2(t12.x,t0.y)).rgb*(w12.x*w0.y);r+=texture2D(tex,vec2(t3.x,t0.y)).rgb*(w3.x*w0.y);
  r+=texture2D(tex,vec2(t0.x,t12.y)).rgb*(w0.x*w12.y);r+=texture2D(tex,vec2(t12.x,t12.y)).rgb*(w12.x*w12.y);r+=texture2D(tex,vec2(t3.x,t12.y)).rgb*(w3.x*w12.y);
  r+=texture2D(tex,vec2(t0.x,t3.y)).rgb*(w0.x*w3.y);r+=texture2D(tex,vec2(t12.x,t3.y)).rgb*(w12.x*w3.y);r+=texture2D(tex,vec2(t3.x,t3.y)).rgb*(w3.x*w3.y);
  return r;}
void main(){
  vec3 c=cr(uv);
  if(params.x>0.5){
    vec2 px=1.0/srcSize;
    vec3 c0=texture2D(tex,uv).rgb;
    vec3 n=texture2D(tex,uv+vec2(0.0,-px.y)).rgb,s=texture2D(tex,uv+vec2(0.0,px.y)).rgb,e=texture2D(tex,uv+vec2(px.x,0.0)).rgb,w=texture2D(tex,uv+vec2(-px.x,0.0)).rgb;
    vec3 lo=min(c0,min(min(n,s),min(e,w))),hi=max(c0,max(max(n,s),max(e,w)));
    vec3 blur=(n+s+e+w+c0*2.0)/6.0;
    c=clamp(c+params.y*1.5*(c-blur),lo-0.03,hi+0.03);}
  gl_FragColor=vec4(clamp(c,0.0,1.0),1.0);}`;

  // ---- FSR (WebGL 2): AMD's FsrEasuF / FsrRcasF with texelFetch standing in for textureGather (not in GLSL ES 3.00) ----
  const fsrVertex =
    '#version 300 es\nlayout(location=0) in vec2 p;void main(){gl_Position=vec4(p,0.0,1.0);}';
  const fsrHead =
    '#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n#define A_GPU 1\n#define A_GLSL 1\n';
  const easuMid = `#define FSR_EASU_F 1
uniform sampler2D InputTexture;uniform uvec4 con0;uniform uvec4 con1;uniform uvec4 con2;uniform uvec4 con3;
// the 2x2 footprint textureGather would return: (i0,j1),(i1,j1),(i1,j0),(i0,j0)
vec4 gather4(vec2 p,int ch){
  ivec2 sz=textureSize(InputTexture,0);ivec2 b=ivec2(floor(p*vec2(sz)-0.5));ivec2 mx=sz-1;
  vec4 t00=texelFetch(InputTexture,clamp(b,ivec2(0),mx),0),t10=texelFetch(InputTexture,clamp(b+ivec2(1,0),ivec2(0),mx),0);
  vec4 t01=texelFetch(InputTexture,clamp(b+ivec2(0,1),ivec2(0),mx),0),t11=texelFetch(InputTexture,clamp(b+ivec2(1,1),ivec2(0),mx),0);
  if(ch==0)return vec4(t01.r,t11.r,t10.r,t00.r);
  if(ch==1)return vec4(t01.g,t11.g,t10.g,t00.g);
  return vec4(t01.b,t11.b,t10.b,t00.b);}
AF4 FsrEasuRF(AF2 p){return gather4(p,0);}
AF4 FsrEasuGF(AF2 p){return gather4(p,1);}
AF4 FsrEasuBF(AF2 p){return gather4(p,2);}
`;
  const easuTail =
    '\nlayout(location=0) out highp vec4 o;\nvoid main(){AF3 c;FsrEasuF(c,AU2(gl_FragCoord.xy),con0,con1,con2,con3);o=vec4(c,1.0);}\n';
  const rcasMid = `#define FSR_RCAS_F 1
uniform sampler2D InputTexture;uniform uvec4 con;uniform ivec2 sz;
AF4 FsrRcasLoadF(ASU2 p){return texelFetch(InputTexture,clamp(ivec2(p),ivec2(0),sz-1),0);}
void FsrRcasInputF(inout AF1 r,inout AF1 g,inout AF1 b){}
`;
  const rcasTail =
    '\nlayout(location=0) out highp vec4 o;\nvoid main(){ivec2 q=ivec2(gl_FragCoord.xy);q.y=sz.y-1-q.y;AF1 r,g,b;FsrRcasF(r,g,b,AU2(q),con);o=vec4(r,g,b,1.0);}\n';

  function shader(g, type, src, what) {
    const s = g.createShader(type);
    g.shaderSource(s, src);
    g.compileShader(s);
    if (!g.getShaderParameter(s, g.COMPILE_STATUS))
      throw new Error(what + ': ' + g.getShaderInfoLog(s));
    return s;
  }
  function program(g, v, f, what) {
    const p = g.createProgram();
    g.attachShader(p, shader(g, g.VERTEX_SHADER, v, what + ' vertex'));
    g.attachShader(p, shader(g, g.FRAGMENT_SHADER, f, what + ' fragment'));
    g.linkProgram(p);
    if (!g.getProgramParameter(p, g.LINK_STATUS))
      throw new Error(what + ' link: ' + g.getProgramInfoLog(p));
    return p;
  }
  function makeCanvas(contextName) {
    const c = document.createElement('canvas');
    c.id = 'upscaleCanvas';
    c.setAttribute('aria-hidden', 'true');
    c.style.cssText = 'position:absolute;pointer-events:none;display:block';
    const g = c.getContext(contextName, {
      alpha: false,
      antialias: false,
      depth: false,
      stencil: false,
      preserveDrawingBuffer: false,
      desynchronized: true,
      powerPreference: 'high-performance',
    });
    return { c, g };
  }
  function quadBuffer(g) {
    const b = g.createBuffer();
    g.bindBuffer(g.ARRAY_BUFFER, b);
    g.bufferData(g.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), g.STATIC_DRAW);
    g.enableVertexAttribArray(0);
    g.vertexAttribPointer(0, 2, g.FLOAT, false, 0, 0);
  }
  function texture(g, filter) {
    const t = g.createTexture();
    g.bindTexture(g.TEXTURE_2D, t);
    g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MIN_FILTER, filter);
    g.texParameteri(g.TEXTURE_2D, g.TEXTURE_MAG_FILTER, filter);
    g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_S, g.CLAMP_TO_EDGE);
    g.texParameteri(g.TEXTURE_2D, g.TEXTURE_WRAP_T, g.CLAMP_TO_EDGE);
    return t;
  }

  function startFsr() {
    const { c, g } = makeCanvas('webgl2');
    if (!g) throw new Error('WebGL 2 is not available');
    if (!window.NX_FSR) throw new Error('fsr.js did not load');
    const A = window.NX_FSR.ffxA,
      F = window.NX_FSR.ffxFsr1;
    const easu = program(g, fsrVertex, fsrHead + A + easuMid + F + easuTail, 'FSR EASU'),
      rcas = program(g, fsrVertex, fsrHead + A + rcasMid + F + rcasTail, 'FSR RCAS');
    g.bindVertexArray(g.createVertexArray());
    quadBuffer(g);
    const floatTargets = !!g.getExtension('EXT_color_buffer_float');
    state = {
      easu,
      rcas,
      floatTargets,
      videoTex: texture(g, g.NEAREST),
      easuTex: texture(g, g.NEAREST),
      fbo: g.createFramebuffer(),
      easuW: 0,
      easuH: 0,
      u: {
        easu: {
          tex: g.getUniformLocation(easu, 'InputTexture'),
          c: [0, 1, 2, 3].map((i) => g.getUniformLocation(easu, 'con' + i)),
        },
        rcas: {
          tex: g.getUniformLocation(rcas, 'InputTexture'),
          con: g.getUniformLocation(rcas, 'con'),
          sz: g.getUniformLocation(rcas, 'sz'),
        },
      },
    };
    c.addEventListener(
      'webglcontextlost',
      (event) => {
        event.preventDefault();
        stop();
        render();
      },
      false,
    );
    return { c, g };
  }
  function startCubic() {
    const { c, g } = makeCanvas('webgl');
    if (!g) throw new Error('WebGL is not available');
    const p = program(g, cubicVertex, cubicFragment, 'cubic');
    g.useProgram(p);
    const b = g.createBuffer();
    g.bindBuffer(g.ARRAY_BUFFER, b);
    g.bufferData(g.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), g.STATIC_DRAW);
    const a = g.getAttribLocation(p, 'p');
    g.enableVertexAttribArray(a);
    g.vertexAttribPointer(a, 2, g.FLOAT, false, 0, 0);
    state = {
      p,
      tex: texture(g, g.LINEAR),
      u: {
        tex: g.getUniformLocation(p, 'tex'),
        src: g.getUniformLocation(p, 'srcSize'),
        params: g.getUniformLocation(p, 'params'),
      },
    };
    c.addEventListener(
      'webglcontextlost',
      (event) => {
        event.preventDefault();
        stop();
        render();
      },
      false,
    );
    return { c, g };
  }
  function start() {
    let made = null;
    kind = '';
    fsrNote = '';
    if (mode === 1) {
      try {
        made = startFsr();
        kind = 'fsr';
      } catch (error) {
        fsrNote = error.message || String(error);
        console.warn('FSR unavailable, using the cubic filter:', error);
        made = null;
      }
    }
    if (!made) {
      made = startCubic();
      kind = 'cubic';
    }
    canvas = made.c;
    gl = made.g;
    video.after(canvas);
    resizeObserver = new ResizeObserver(layout);
    resizeObserver.observe(viewport);
  }
  // the canvas covers exactly the part of the viewport the video itself occupies (object-fit: contain)
  function layout() {
    if (!canvas || !video.videoWidth) return;
    const bw = viewport.clientWidth,
      bh = viewport.clientHeight,
      ar = video.videoWidth / video.videoHeight;
    let w = bw,
      h = bw / ar;
    if (h > bh) {
      h = bh;
      w = bh * ar;
    }
    const dpr = Math.min(window.devicePixelRatio || 1, 3);
    canvas.style.width = w + 'px';
    canvas.style.height = h + 'px';
    canvas.style.left = (bw - w) / 2 + 'px';
    canvas.style.top = (bh - h) / 2 + 'px';
    const cw = Math.min(3840, Math.round(w * dpr)),
      ch = Math.min(2160, Math.round(h * dpr));
    if (canvas.width !== cw || canvas.height !== ch) {
      canvas.width = cw;
      canvas.height = ch;
    }
  }
  function enlarging() {
    return (
      canvas &&
      video.videoWidth &&
      canvas.width >= video.videoWidth * 1.05 &&
      canvas.height >= video.videoHeight * 1.05
    );
  }

  function drawFsr() {
    const sw = video.videoWidth,
      sh = video.videoHeight,
      w = canvas.width,
      h = canvas.height,
      s = state;
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, s.videoTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, video);
    if (s.easuW !== w || s.easuH !== h) {
      gl.bindTexture(gl.TEXTURE_2D, s.easuTex);
      if (s.floatTargets)
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA16F, w, h, 0, gl.RGBA, gl.HALF_FLOAT, null);
      else gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, s.fbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, s.easuTex, 0);
      if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE)
        throw new Error('framebuffer incomplete');
      s.easuW = w;
      s.easuH = h;
    }
    // EASU: video -> canvas-sized texture
    gl.bindFramebuffer(gl.FRAMEBUFFER, s.fbo);
    gl.viewport(0, 0, w, h);
    gl.useProgram(s.easu);
    gl.bindTexture(gl.TEXTURE_2D, s.videoTex);
    gl.uniform1i(s.u.easu.tex, 0);
    const inv = [1 / sw, 1 / sh];
    gl.uniform4ui(
      s.u.easu.c[0],
      bits(sw / w),
      bits(sh / h),
      bits((0.5 * sw) / w - 0.5),
      bits((0.5 * sh) / h - 0.5),
    );
    gl.uniform4ui(s.u.easu.c[1], bits(inv[0]), bits(inv[1]), bits(inv[0]), bits(-inv[1]));
    gl.uniform4ui(s.u.easu.c[2], bits(-inv[0]), bits(2 * inv[1]), bits(inv[0]), bits(2 * inv[1]));
    gl.uniform4ui(s.u.easu.c[3], 0, bits(4 * inv[1]), 0, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    // RCAS: that texture -> the canvas
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, w, h);
    gl.useProgram(s.rcas);
    gl.bindTexture(gl.TEXTURE_2D, s.easuTex);
    gl.uniform1i(s.u.rcas.tex, 0);
    const sharp = Math.pow(2, -RCAS_STOPS),
      hb = half(sharp);
    gl.uniform4ui(s.u.rcas.con, bits(sharp), (hb | (hb << 16)) >>> 0, 0, 0);
    gl.uniform2i(s.u.rcas.sz, w, h);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }
  function drawCubic() {
    const s = state;
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.bindTexture(gl.TEXTURE_2D, s.tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, video);
    gl.uniform1i(s.u.tex, 0);
    gl.uniform2f(s.u.src, video.videoWidth, video.videoHeight);
    gl.uniform2f(s.u.params, 1, 0.5);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
  }
  function draw() {
    callbackId = 0;
    if (!canvas || !mode) return;
    try {
      layout();
      if (video.videoWidth && enlarging()) {
        if (kind === 'fsr') drawFsr();
        else drawCubic();
        canvas.style.visibility = 'visible';
        video.style.opacity = '0';
      } else {
        canvas.style.visibility = 'hidden';
        video.style.opacity = '';
      } // not enlarging: the plain video is just as good
    } catch (error) {
      fail(error);
      return;
    }
    callbackId = video.requestVideoFrameCallback(draw);
  }
  function stop() {
    if (callbackId && video.cancelVideoFrameCallback) video.cancelVideoFrameCallback(callbackId);
    callbackId = 0;
    if (resizeObserver) {
      resizeObserver.disconnect();
      resizeObserver = null;
    }
    if (canvas) {
      canvas.remove();
      canvas = null;
    }
    gl = null;
    state = {};
    kind = '';
    video.style.opacity = '';
  }
  function fail(error) {
    failed = true;
    console.warn('Upscaling is off:', error);
    stop();
    render();
  }
  function apply() {
    stop();
    if (!mode || failed || !supported) {
      render();
      return;
    }
    try {
      start();
      layout();
      callbackId = video.requestVideoFrameCallback(draw);
    } catch (error) {
      fail(error);
      return;
    }
    render();
  }
  function render() {
    const button = el('upscaleToggle');
    if (!button) return;
    button.setAttribute('aria-pressed', mode ? 'true' : 'false');
    button.querySelector('em').textContent = !supported
      ? 'Not supported here'
      : failed
        ? 'Unavailable'
        : mode === 1 && kind === 'cubic'
          ? 'FSR unavailable: Cubic'
          : names[mode];
    button.title = fsrNote
      ? 'FSR could not start: ' + fsrNote
      : 'Draws the picture with a better enlarging filter when it is shown larger than 1280x720: Off, FSR (AMD FidelityFX Super Resolution 1) or Cubic (lighter)';
  }
  const button = el('upscaleToggle');
  if (button)
    button.addEventListener('click', () => {
      if (!supported || failed) return;
      mode = (mode + 1) % 3;
      prefs.set('upscale', mode);
      apply();
    });
  render();
  apply();
  video.addEventListener('loadedmetadata', layout);
  video.addEventListener('resize', layout);
  window.NxUpscale = {
    get mode() {
      return mode;
    },
    set mode(v) {
      mode = Math.min(2, Math.max(0, v | 0));
      prefs.set('upscale', mode);
      apply();
    },
    get kind() {
      return kind;
    },
    get note() {
      return fsrNote;
    },
  };
})();
