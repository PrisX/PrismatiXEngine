// Effects for the WebGL2 renderer: offline translations of the native SPIR-V
// artifact, run on SDL's own WebGL2 context. SDL continues to own composition,
// clipping, transforms and presentation. A WebGPU path exists only on explicit
// opt-in (?webgpu-effects or initialize(gl, {webgpu: true})): it copies every
// pass between the WebGL canvas and WebGPU and is several times slower. The
// WebGPU renderer does not initialize this class; it only reads sdlGpuSource().
export class WebEffects {
  constructor(manifest, files, base) {
    this.manifest = manifest; this.files = files; this.base = base;
    this.programs = new Map(); this.next = 1; this.backend = "webgl2";
  }
  async initialize(gl, {webgpu = new URL(location.href).searchParams.has("webgpu-effects")} = {}) {
    if (!(gl instanceof WebGL2RenderingContext)) throw new Error("完整特效需要 WebGL2");
    this.gl = gl;
    this.framebuffer = gl.createFramebuffer(); this.vao = gl.createVertexArray();
    this.vertices = gl.createBuffer(); this.uniforms = gl.createBuffer();
    this.white = gl.createTexture();
    this.withState(() => {
      gl.bindTexture(gl.TEXTURE_2D, this.white);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255,255,255,255]));
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    });
    if (webgpu && navigator.gpu) {
      try {
        const adapter = await navigator.gpu.requestAdapter();
        this.device = adapter && await adapter.requestDevice();
        if (this.device) {
          this.backend = "webgpu";
          this.device.lost.then(info => { if(info.reason!=="destroyed")this.fallback(`WebGPU device lost: ${info.message}`); });
          this.device.addEventListener("uncapturederror", event => this.fallback(event.error.message));
        }
      } catch (error) { this.fallback(error.message); }
    }
    const response = await fetch(new URL("web-shaders.json", this.base));
    if (!response.ok) throw new Error("無法載入 Web 特效");
    const shaders = await response.json();
    await this.compile(shaders.compositor); // Handle 1 is the built-in stage pass.
    console.info(`Web effects initialized backend=${this.backend}`);
  }
  fallback(reason) { console.warn(`WebGPU fallback: ${reason}`); this.backend = "webgl2"; }
  withState(action) {
    const gl = this.gl;
    const state = {program: gl.getParameter(gl.CURRENT_PROGRAM), vao: gl.getParameter(gl.VERTEX_ARRAY_BINDING),
      buffer: gl.getParameter(gl.ARRAY_BUFFER_BINDING), uniform: gl.getParameter(gl.UNIFORM_BUFFER_BINDING),
      uniform0: gl.getIndexedParameter(gl.UNIFORM_BUFFER_BINDING, 0),
      read: gl.getParameter(gl.READ_FRAMEBUFFER_BINDING), draw: gl.getParameter(gl.DRAW_FRAMEBUFFER_BINDING),
      viewport: gl.getParameter(gl.VIEWPORT), scissor: gl.getParameter(gl.SCISSOR_BOX),
      active: gl.getParameter(gl.ACTIVE_TEXTURE), textures: [],
      enabled: [gl.BLEND,gl.SCISSOR_TEST,gl.DEPTH_TEST,gl.CULL_FACE].map(cap => gl.isEnabled(cap)),
      factors: [gl.BLEND_SRC_RGB,gl.BLEND_DST_RGB,gl.BLEND_SRC_ALPHA,gl.BLEND_DST_ALPHA].map(key => gl.getParameter(key)),
      equations: [gl.getParameter(gl.BLEND_EQUATION_RGB),gl.getParameter(gl.BLEND_EQUATION_ALPHA)],
      flip: gl.getParameter(gl.UNPACK_FLIP_Y_WEBGL), premultiply: gl.getParameter(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL),
      pack: gl.getParameter(gl.PACK_ALIGNMENT), unpack: gl.getParameter(gl.UNPACK_ALIGNMENT)};
    for (let i=0;i<2;i++) { gl.activeTexture(gl.TEXTURE0+i); state.textures.push(gl.getParameter(gl.TEXTURE_BINDING_2D)); }
    try { return action(state); }
    finally {
      gl.useProgram(state.program); gl.bindVertexArray(state.vao); gl.bindBuffer(gl.ARRAY_BUFFER,state.buffer);
      gl.bindBufferBase(gl.UNIFORM_BUFFER,0,state.uniform0); gl.bindBuffer(gl.UNIFORM_BUFFER,state.uniform);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER,state.read); gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER,state.draw);
      gl.viewport(...state.viewport); gl.scissor(...state.scissor);
      [gl.BLEND,gl.SCISSOR_TEST,gl.DEPTH_TEST,gl.CULL_FACE].forEach((cap,i)=>state.enabled[i]?gl.enable(cap):gl.disable(cap));
      gl.blendFuncSeparate(...state.factors); gl.blendEquationSeparate(...state.equations);
      for(let i=0;i<2;i++){gl.activeTexture(gl.TEXTURE0+i);gl.bindTexture(gl.TEXTURE_2D,state.textures[i]);}
      gl.activeTexture(state.active); gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,state.flip);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL,state.premultiply);
      gl.pixelStorei(gl.PACK_ALIGNMENT,state.pack); gl.pixelStorei(gl.UNPACK_ALIGNMENT,state.unpack);
    }
  }
  shader(type, source) {
    const gl=this.gl, shader=gl.createShader(type); gl.shaderSource(shader,source); gl.compileShader(shader);
    if(!gl.getShaderParameter(shader,gl.COMPILE_STATUS)){const error=gl.getShaderInfoLog(shader);gl.deleteShader(shader);throw new Error(error);}
    return shader;
  }
  async compile(shader) {
    const gl=this.gl;
    const outputs=[...shader.glsl.matchAll(/\bin\s+(?:(?:highp|mediump|lowp)\s+)?(vec[24])\s+(\w+)\s*;/gu)]
      .map(match=>({type:match[1],name:match[2]}));
    const vertex=`#version 300 es
      precision highp float;
      layout(location=0) in vec2 pxPosition;
      layout(location=1) in vec4 pxColor;
      layout(location=2) in vec2 pxUv;
      uniform vec3 pxOutput;
      ${outputs.map(o=>`out ${o.type} ${o.name};`).join("\n")}
      void main(){vec2 p=pxPosition/pxOutput.xy*2.0-1.0;gl_Position=vec4(p.x,p.y*pxOutput.z,0,1);
      ${outputs.map(o=>`${o.name}=${o.type==="vec4"?"pxColor":"pxUv"};`).join("\n")}}`;
    const vs=this.shader(gl.VERTEX_SHADER,vertex),fs=this.shader(gl.FRAGMENT_SHADER,shader.glsl);
    const program=gl.createProgram(); gl.attachShader(program,vs);gl.attachShader(program,fs);gl.linkProgram(program);
    gl.deleteShader(vs);gl.deleteShader(fs);
    if(!gl.getProgramParameter(program,gl.LINK_STATUS)){const error=gl.getProgramInfoLog(program);gl.deleteProgram(program);throw new Error(error);}
    for(let i=0;i<gl.getProgramParameter(program,gl.ACTIVE_UNIFORM_BLOCKS);i++)gl.uniformBlockBinding(program,i,0);
    const samplers=[...shader.glsl.matchAll(/uniform\s+(?:(?:highp|mediump|lowp)\s+)?sampler2D\s+(\w+)\s*;/gu)]
      .map(match=>({location:gl.getUniformLocation(program,match[1]),
        binding:shader.textures.find(tex=>match[1].includes(tex.name.replace(/\W/gu,"_")))?.binding??0}));
    const handle=this.next++;
    const value={shader,program,output:gl.getUniformLocation(program,"pxOutput"),samplers,pipelines:new Map()};
    this.programs.set(handle,value);
    if(this.device){
      value.fragment=this.device.createShaderModule({code:shader.wgsl});
      const diagnostics=await value.fragment.getCompilationInfo();
      if(diagnostics.messages.some(m=>m.type==="error")) this.fallback(diagnostics.messages.map(m=>m.message).join("\n"));
    }
    return handle;
  }
  async load(id, fingerprint) {
    const mapping=this.manifest.effects?.[id];
    if(!mapping||mapping.fingerprint!==fingerprint)throw new Error(`Web effect artifact mismatch: ${id}`);
    const bytes=await this.files.read(mapping.path,0,this.files.size(mapping.path));
    return this.compile(JSON.parse(new TextDecoder().decode(bytes)));
  }
  // The WebGPU renderer: the effect's WGSL, rebound at export for SDL_GPU.
  async sdlGpuSource(id, fingerprint) {
    const mapping=this.manifest.effects?.[id];
    if(!mapping||mapping.fingerprint!==fingerprint)throw new Error(`Web effect artifact mismatch: ${id}`);
    const bytes=await this.files.read(mapping.path,0,this.files.size(mapping.path));
    const shader=JSON.parse(new TextDecoder().decode(bytes));
    if(typeof shader.sdlGpuWgsl!=="string")throw new Error(`Web effect was not exported for WebGPU: ${id}`);
    return shader.sdlGpuWgsl;
  }
  quad(width,height){return new Float32Array([
    0,0,1,1,1,1,0,0,width,0,1,1,1,1,1,0,width,height,1,1,1,1,1,1,
    0,0,1,1,1,1,0,0,width,height,1,1,1,1,1,1,0,height,1,1,1,1,0,1]);}
  blend(mode) {
    // SDL's predefined modes and packed custom factors/operations.
    const standard={0:[2,1,1,2,1,1],1:[5,6,1,2,6,1],2:[5,2,1,1,2,1],
      4:[1,3,1,1,2,1],8:[7,6,1,1,2,1],16:[2,6,1,2,6,1],32:[2,2,1,1,2,1]};
    return standard[mode]??[(mode>>>4)&15,(mode>>>8)&15,mode&15,(mode>>>20)&15,(mode>>>24)&15,(mode>>>16)&15];
  }
  drawGl(value, source, incoming, vertices, width, height, target, blend, clip, uniforms) {
    const gl=this.gl;
    gl.useProgram(value.program);gl.bindVertexArray(this.vao);gl.bindBuffer(gl.ARRAY_BUFFER,this.vertices);
    gl.bufferData(gl.ARRAY_BUFFER,vertices,gl.STREAM_DRAW);
    for(const [location,count,offset] of [[0,2,0],[1,4,8],[2,2,24]]){
      gl.enableVertexAttribArray(location);gl.vertexAttribPointer(location,count,gl.FLOAT,false,32,offset);
    }
    gl.uniform3f(value.output,width,height,target?1:-1);
    gl.bindBuffer(gl.UNIFORM_BUFFER,this.uniforms);gl.bufferData(gl.UNIFORM_BUFFER,uniforms,gl.STREAM_DRAW);
    gl.bindBufferBase(gl.UNIFORM_BUFFER,0,this.uniforms);
    for(let i=0;i<2;i++){gl.activeTexture(gl.TEXTURE0+i);gl.bindTexture(gl.TEXTURE_2D,(i?incoming:source)??this.white);}
    for(const sampler of value.samplers)gl.uniform1i(sampler.location,sampler.binding);
    gl.viewport(0,0,width,height);gl.disable(gl.DEPTH_TEST);gl.disable(gl.CULL_FACE);
    if(clip){gl.enable(gl.SCISSOR_TEST);gl.scissor(clip[0],target?clip[1]:height-clip[1]-clip[3],clip[2],clip[3]);}else gl.disable(gl.SCISSOR_TEST);
    if(!blend)gl.disable(gl.BLEND);
    else{
      const [sc,dc,co,sa,da,ao]=this.blend(blend);
      const factors=[0,gl.ZERO,gl.ONE,gl.SRC_COLOR,gl.ONE_MINUS_SRC_COLOR,gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA,gl.DST_COLOR,gl.ONE_MINUS_DST_COLOR,gl.DST_ALPHA,gl.ONE_MINUS_DST_ALPHA];
      const ops=[0,gl.FUNC_ADD,gl.FUNC_SUBTRACT,gl.FUNC_REVERSE_SUBTRACT,gl.MIN,gl.MAX];
      gl.enable(gl.BLEND);gl.blendFuncSeparate(factors[sc],factors[dc],factors[sa],factors[da]);gl.blendEquationSeparate(ops[co],ops[ao]);
    }
    gl.drawArrays(gl.TRIANGLES,0,vertices.length/8);
  }
  readTexture(texture,width,height) {
    const gl=this.gl;gl.bindFramebuffer(gl.FRAMEBUFFER,this.framebuffer);
    gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,texture,0);
    if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)throw new Error("Web effect source framebuffer is incomplete");
    const bytes=new Uint8Array(width*height*4);gl.pixelStorei(gl.PACK_ALIGNMENT,1);
    gl.readPixels(0,0,width,height,gl.RGBA,gl.UNSIGNED_BYTE,bytes);return bytes;
  }
  async gpu(value,sources,vertices,width,height,blend,clip,uniforms,background,linear=true) {
    const device=this.device;
    const key=String(blend);
    let pipeline=value.pipelines.get(key);
    if(!pipeline){
      const factors=["","zero","one","src","one-minus-src","src-alpha","one-minus-src-alpha","dst","one-minus-dst","dst-alpha","one-minus-dst-alpha"];
      const ops=["","add","subtract","reverse-subtract","min","max"];
      const [sc,dc,co,sa,da,ao]=this.blend(blend);
      const blendState=blend?{color:{srcFactor:factors[sc],dstFactor:factors[dc],operation:ops[co]},alpha:{srcFactor:factors[sa],dstFactor:factors[da],operation:ops[ao]}}:undefined;
      const varyings=value.shader.inputs.map(input=>({location:input.location,
        type:input.type==="vec4"?"vec4<f32>":"vec2<f32>",name:input.type==="vec4"?"color":"uv"}));
      const vertex=device.createShaderModule({code:`
        struct V{@builtin(position) p:vec4<f32>,${varyings.map(v=>`@location(${v.location}) ${v.name}:${v.type}`).join(",")}};
        @vertex fn main(@location(0) position:vec2<f32>,@location(1) color:vec4<f32>,@location(2) uv:vec2<f32>)->V{
          var v:V;v.p=vec4(position.x*2.0-1.0,1.0-position.y*2.0,0,1);${varyings.map(v=>`v.${v.name}=${v.name};`).join("")}return v;}`});
      device.pushErrorScope("validation");
      pipeline=await device.createRenderPipelineAsync({layout:"auto",vertex:{module:vertex,entryPoint:"main",
        buffers:[{arrayStride:32,attributes:[{shaderLocation:0,offset:0,format:"float32x2"},{shaderLocation:1,offset:8,format:"float32x4"},{shaderLocation:2,offset:24,format:"float32x2"}]}]},
        fragment:{module:value.fragment,entryPoint:"main",targets:[{format:"rgba8unorm",...(blendState?{blend:blendState}:{})}]},primitive:{topology:"triangle-list"}});
      const error=await device.popErrorScope();if(error)throw error;
      value.pipelines.set(key,pipeline);
    }
    const textures=[],buffers=[];
    const makeTexture=(width,height,bytes)=>{
      const texture=device.createTexture({size:[width,height],format:"rgba8unorm",usage:GPUTextureUsage.TEXTURE_BINDING|GPUTextureUsage.COPY_DST|GPUTextureUsage.COPY_SRC|GPUTextureUsage.RENDER_ATTACHMENT});
      textures.push(texture);if(bytes)device.queue.writeTexture({texture},bytes,{bytesPerRow:width*4},[width,height]);return texture;
    };
    const makeBuffer=(bytes,usage)=>{const buffer=device.createBuffer({size:Math.max(16,Math.ceil(bytes.byteLength/4)*4),usage:usage|GPUBufferUsage.COPY_DST});buffers.push(buffer);device.queue.writeBuffer(buffer,0,bytes);return buffer;};
    try {
      const input=sources.map(source=>makeTexture(source.width,source.height,source.bytes));
      const output=makeTexture(width,height,background);
      const sampler=device.createSampler({minFilter:linear?"linear":"nearest",magFilter:linear?"linear":"nearest",addressModeU:"clamp-to-edge",addressModeV:"clamp-to-edge"});
      const resources=[...value.shader.textures.map(tex=>({binding:tex.binding,resource:input[tex.binding].createView()})),
        ...value.shader.samplers.map(s=>({binding:s.binding+16,resource:sampler}))];
      const group2=device.createBindGroup({layout:pipeline.getBindGroupLayout(2),entries:resources});
      const uniform=makeBuffer(uniforms,GPUBufferUsage.UNIFORM);
      const group3=device.createBindGroup({layout:pipeline.getBindGroupLayout(3),entries:[{binding:0,resource:{buffer:uniform}}]});
      const normalized=Float32Array.from(vertices);for(let i=0;i<normalized.length;i+=8){normalized[i]/=width;normalized[i+1]/=height;}
      const vertex=makeBuffer(normalized,GPUBufferUsage.VERTEX);
      const encoder=device.createCommandEncoder(),pass=encoder.beginRenderPass({colorAttachments:[{view:output.createView(),loadOp:background?"load":"clear",clearValue:[0,0,0,0],storeOp:"store"}]});
      pass.setPipeline(pipeline);pass.setBindGroup(2,group2);pass.setBindGroup(3,group3);pass.setVertexBuffer(0,vertex);
      if(clip)pass.setScissorRect(Math.max(0,clip[0]),Math.max(0,clip[1]),Math.max(0,Math.min(width-clip[0],clip[2])),Math.max(0,Math.min(height-clip[1],clip[3])));
      pass.draw(vertices.length/8);pass.end();
      const stride=Math.ceil(width*4/256)*256;
      const result=device.createBuffer({size:stride*height,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});buffers.push(result);
      encoder.copyTextureToBuffer({texture:output},{buffer:result,bytesPerRow:stride},[width,height]);device.queue.submit([encoder.finish()]);
      await result.mapAsync(GPUMapMode.READ);
      const mapped=new Uint8Array(result.getMappedRange()),pixels=new Uint8Array(width*height*4);
      for(let row=0;row<height;row++)pixels.set(mapped.subarray(row*stride,row*stride+width*4),row*width*4);
      result.unmap();return pixels;
    }finally{for(const texture of textures)texture.destroy();for(const buffer of buffers)buffer.destroy();}
  }
  async pass(handle,source,incoming,output,width,height,uniforms) {
    const value=this.programs.get(handle);if(!value)throw new Error("Unknown Web effect");
    if(this.backend==="webgpu"){
      try{
        const sources=this.withState(()=>[source,...(incoming?[incoming]:[])].map(texture=>({width,height,bytes:this.readTexture(texture,width,height)})));
        const bytes=await this.gpu(value,sources,this.quad(width,height),width,height,0,null,uniforms);
        this.withState(()=>{const gl=this.gl;gl.bindTexture(gl.TEXTURE_2D,output);gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,width,height,gl.RGBA,gl.UNSIGNED_BYTE,bytes);});return;
      }catch(error){this.fallback(error.message);}
    }
    this.withState(()=>{const gl=this.gl;gl.bindFramebuffer(gl.FRAMEBUFFER,this.framebuffer);gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,output,0);
      this.drawGl(value,source,incoming,this.quad(width,height),width,height,true,0,null,uniforms);});
  }
  async geometry(handle,texture,vertices,width,height,target,blend,clip,uniforms,textureWidth=width,textureHeight=height,linear=true) {
    // Leaf UVs and vertex colors are preserved, including cropped sprites and
    // transformed geometry. They are not replaced with a full-stage capture.
    const value=this.programs.get(handle);if(!value)throw new Error("Unknown Web effect");
    if(this.backend==="webgpu"&&target){
      try{
        const captured=this.withState(state=>{
          const gl=this.gl;
          const output=gl.getFramebufferAttachmentParameter(gl.DRAW_FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.FRAMEBUFFER_ATTACHMENT_OBJECT_NAME);
          if(!output)throw new Error("SDL effect target has no texture attachment");
          gl.bindFramebuffer(gl.READ_FRAMEBUFFER,state.draw);gl.pixelStorei(gl.PACK_ALIGNMENT,1);
          const background=new Uint8Array(width*height*4);gl.readPixels(0,0,width,height,gl.RGBA,gl.UNSIGNED_BYTE,background);
          const source=texture?{width:textureWidth,height:textureHeight,bytes:this.readTexture(texture,textureWidth,textureHeight)}:
            {width:1,height:1,bytes:new Uint8Array([255,255,255,255])};
          return{output,background,source};
        });
        const bytes=await this.gpu(value,[captured.source],vertices,width,height,blend,clip,uniforms,captured.background,linear);
        this.withState(()=>{const gl=this.gl;gl.bindTexture(gl.TEXTURE_2D,captured.output);gl.pixelStorei(gl.UNPACK_ALIGNMENT,1);
          gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,width,height,gl.RGBA,gl.UNSIGNED_BYTE,bytes);});return;
      }catch(error){this.fallback(error.message);}
    }
    this.withState(()=>this.drawGl(value,texture,null,vertices,width,height,target,blend,clip,uniforms));
  }
  destroy() {
    if(!this.gl)return;const gl=this.gl;
    for(const value of this.programs.values())gl.deleteProgram(value.program);
    this.programs.clear();gl.deleteTexture(this.white);gl.deleteFramebuffer(this.framebuffer);
    gl.deleteVertexArray(this.vao);gl.deleteBuffer(this.vertices);gl.deleteBuffer(this.uniforms);
    this.device?.destroy();this.device=null;this.gl=null;
  }
}
