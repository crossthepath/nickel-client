// Included inside overlay.cpp's renderer namespace. Motion blur pixel shader (DXBC);
// RDEF Constants = 224 bytes, motion buffer at offset 144.
struct MotionConstants {float size[4]{},region[4]{},options[4]{},display[4]{},viewport[4]{},reprojection[16]{},motion[4]{},candidates[16]{};};
static_assert(sizeof(MotionConstants)==224);
struct MotionState {
 UINT width=0,height=0;ULONGLONG draws=0;bool failed=false;MotionConstants constants{};
 ComPtr<ID3D11Texture2D> capture11,result11;ComPtr<ID3D11ShaderResourceView> captureView11,resultView11;ComPtr<ID3D11RenderTargetView> resultTarget11;
 ComPtr<ID3D11VertexShader> vs11;ComPtr<ID3D11PixelShader> ps11;ComPtr<ID3D11SamplerState> sampler11;ComPtr<ID3D11RasterizerState> raster11;ComPtr<ID3D11Buffer> constants11;
 ComPtr<ID3D12Resource> capture12,result12;std::vector<ComPtr<ID3D12Resource>> constants12;ComPtr<ID3D12DescriptorHeap> rtv12;ComPtr<ID3D12RootSignature> root12;ComPtr<ID3D12PipelineState> pipeline12;
};
auto& motion=*new MotionState;
void ReleaseMotion(){motion=MotionState{};}
// Motion blur pixel shader (ps_5_0), compiled once at runtime. Each pixel is reprojected with the previous camera
// to get its screen velocity; the blur length is soft-limited with tanh, sampled `motion.z` times along the velocity
// with interleaved-gradient jitter and triangle weights, and faded out inside the rounded rectangle `region` (the HUD
// area kept sharp). `motion.w` re-encodes the result to sRGB.
static const char* kMotionPixel = R"(
cbuffer Constants : register(b0) {
 float4 size;          // xy = render size in pixels
 float4 region;        // sharp rectangle: xy = min, zw = max (pixels)
 float4 options;       // x = edge feather, y = rectangle enabled, z = corner radius factor
 float4 display;
 float4 viewport;      // xy = origin, zw = size
 float4 reprojection[4];
 float4 motion;        // x = strength, y = max length, z = sample count, w = encode sRGB
 float4 candidates[4];
};
Texture2D image : register(t0);
SamplerState linearClamp : register(s0);

float SharpMask(float2 p) {
 if (options.y == 0) return 0;
 float2 extent = region.zw - region.xy;
 float radius = min(extent.x, extent.y) * 0.5 * options.z;
 float2 q = abs(p - (region.xy + region.zw) * 0.5) - extent * 0.5 + radius;
 float distance = length(max(q, 0)) + min(max(q.x, q.y), 0) - radius;
 return saturate(0.5 - distance / options.x);
}

float3 EncodeSrgb(float3 c) {
 float3 high = 1.055 * pow(max(c, 0), 1.0 / 2.4) - 0.055;
 return c >= 0.0031308 ? high : c * 12.92;
}

float4 ps(float4 position : SV_Position) : SV_Target {
 float4 base = image.Load(int3(int2(position.xy), 0));
 float2 ndc = (position.xy - viewport.xy) / viewport.zw * float2(2, -2) + float2(-1, 1);
 float3 previous = reprojection[0].xyw * ndc.x + reprojection[1].xyw * ndc.y + reprojection[2].xyw * 0.5 + reprojection[3].xyw;
 float2 velocity = 0;
 if (previous.z > 0.0001) {
  float2 screen = (previous.xy / previous.z * float2(0.5, -0.5) + 0.5) * viewport.zw + viewport.xy;
  velocity = (position.xy - screen) * motion.x;
 }
 float length0 = length(velocity);
 float sharp = SharpMask(position.xy);
 if (length0 >= 0.5 && sharp < 1) {
  velocity *= tanh(length0 / motion.y) * motion.y / length0;
  uint count = (uint)motion.z;
  float jitter = frac(52.982918 * frac(dot(position.xy, float2(0.06711056, 0.00583715))));
  float4 sum = 0;
  for (uint i = 0; i < count; ++i) {
   float t = (i + jitter) / count - 0.5;
   float2 at = position.xy + velocity * t;
   float weight = (1 - abs(t)) * (1 - SharpMask(at));
   sum += float4(image.SampleLevel(linearClamp, at / size.xy, 0).rgb * weight, weight);
  }
  float3 blurred = sum.w > 0.0001 ? sum.rgb / sum.w : base.rgb;
  base.rgb = lerp(blurred, base.rgb, sharp);
 }
 if (motion.w != 0) base.rgb = EncodeSrgb(base.rgb);
 return base;
})";
const void* MotionShader(DWORD* size){static ComPtr<ID3DBlob> blob;static bool tried=false;
 if(!tried){tried=true;ComPtr<ID3DBlob> errors;if(FAILED(D3DCompile(kMotionPixel,strlen(kMotionPixel),"motion_blur",nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors))){
  blob.Reset();if(errors)Log((std::string("Motion blur shader failed to compile: ")+(const char*)errors->GetBufferPointer()).c_str());}}
 if(!blob)return nullptr;*size=(DWORD)blob->GetBufferSize();return blob->GetBufferPointer();}
bool MotionVertex(ComPtr<ID3DBlob>& vertex){const char* source=R"(struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;float4 color:COLOR0;};
V vs(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.color=float4(1,1,1,1);return o;})";return SUCCEEDED(D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vertex,nullptr));}
bool CreateMotion11(UINT w,UINT h,DXGI_FORMAT format){
 D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=format;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 if(FAILED(device11->CreateTexture2D(&desc,nullptr,&motion.capture11))||FAILED(device11->CreateShaderResourceView(motion.capture11.Get(),nullptr,&motion.captureView11)))return false;
 desc.BindFlags|=D3D11_BIND_RENDER_TARGET;if(FAILED(device11->CreateTexture2D(&desc,nullptr,&motion.result11))||FAILED(device11->CreateShaderResourceView(motion.result11.Get(),nullptr,&motion.resultView11))||FAILED(device11->CreateRenderTargetView(motion.result11.Get(),nullptr,&motion.resultTarget11)))return false;
 DWORD length{};const void* pixel=MotionShader(&length);ComPtr<ID3DBlob> vertex;if(!pixel||!MotionVertex(vertex)||FAILED(device11->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&motion.vs11))||FAILED(device11->CreatePixelShader(pixel,length,nullptr,&motion.ps11)))return false;
 D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(MotionConstants);cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;if(FAILED(device11->CreateBuffer(&cb,nullptr,&motion.constants11)))return false;
 D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sampler.MaxLOD=D3D11_FLOAT32_MAX;
 D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
 return SUCCEEDED(device11->CreateSamplerState(&sampler,&motion.sampler11))&&SUCCEEDED(device11->CreateRasterizerState(&raster,&motion.raster11));
}
void MotionCallback11(const ImDrawList*,const ImDrawCmd*){
 ComPtr<ID3D11Texture2D> back;if(!activeSwap||!motion.capture11||FAILED(activeSwap->GetBuffer(0,IID_PPV_ARGS(&back))))return;auto* c=context11.Get();
 ComPtr<ID3D11Buffer> previous;c->PSGetConstantBuffers(0,1,&previous);c->CopyResource(motion.capture11.Get(),back.Get());c->UpdateSubresource(motion.constants11.Get(),0,nullptr,&motion.constants,0,0);
 c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(motion.vs11.Get(),nullptr,0);c->PSSetShader(motion.ps11.Get(),nullptr,0);
 auto* constants=motion.constants11.Get();auto* sampler=motion.sampler11.Get();auto* source=motion.captureView11.Get();auto* target=motion.resultTarget11.Get();c->PSSetConstantBuffers(0,1,&constants);c->PSSetSamplers(0,1,&sampler);c->PSSetShaderResources(0,1,&source);c->OMSetRenderTargets(1,&target,nullptr);c->OMSetBlendState(nullptr,nullptr,0xffffffff);c->OMSetDepthStencilState(nullptr,0);c->RSSetState(motion.raster11.Get());
 D3D11_VIEWPORT view{0,0,(float)motion.width,(float)motion.height,0,1};c->RSSetViewports(1,&view);c->Draw(3,0);++motion.draws;
 ID3D11ShaderResourceView* none=nullptr;c->PSSetShaderResources(0,1,&none);auto* restored=previous.Get();c->PSSetConstantBuffers(0,1,&restored);target=target11.Get();c->OMSetRenderTargets(1,&target,nullptr);
}
bool CreateMotion12(UINT w,UINT h,DXGI_FORMAT format){
 Log("Motion DX12 create resources");D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=format;desc.SampleDesc.Count=1;
 if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&motion.capture12))))return false;
 desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&motion.result12))))return false;
 D3D12_DESCRIPTOR_HEAP_DESC targetHeap{};targetHeap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;targetHeap.NumDescriptors=1;if(FAILED(device12->CreateDescriptorHeap(&targetHeap,IID_PPV_ARGS(&motion.rtv12))))return false;device12->CreateRenderTargetView(motion.result12.Get(),nullptr,motion.rtv12->GetCPUDescriptorHandleForHeapStart());
 auto cpu=srvHeap->GetCPUDescriptorHandleForHeapStart();const UINT step=device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);cpu.ptr+=13*step;
 D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device12->CreateShaderResourceView(motion.capture12.Get(),&view,cpu);cpu.ptr+=step;device12->CreateShaderResourceView(motion.result12.Get(),&view,cpu);
 Log("Motion DX12 create root signature");D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable.NumDescriptorRanges=1;parameters[0].DescriptorTable.pDescriptorRanges=&range;parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
 D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
 D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;ComPtr<ID3DBlob> packed;
 if(FAILED(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&packed,nullptr))||FAILED(device12->CreateRootSignature(0,packed->GetBufferPointer(),packed->GetBufferSize(),IID_PPV_ARGS(&motion.root12))))return false;
 Log("Motion DX12 create pipeline");DWORD length{};const void* pixel=MotionShader(&length);ComPtr<ID3DBlob> vertex;if(!pixel||!MotionVertex(vertex)){Log("Motion DX12 shaders unavailable");return false;}
 D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};state.pRootSignature=motion.root12.Get();state.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};state.PS={pixel,length};state.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;state.SampleMask=UINT_MAX;state.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;state.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;state.RasterizerState.DepthClipEnable=TRUE;state.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;state.NumRenderTargets=1;state.RTVFormats[0]=format;state.SampleDesc.Count=1;
 auto& blend=state.BlendState.RenderTarget[0];blend.SrcBlend=D3D12_BLEND_ONE;blend.DestBlend=D3D12_BLEND_ZERO;blend.BlendOp=D3D12_BLEND_OP_ADD;blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
 state.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;state.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;state.DepthStencilState.StencilReadMask=D3D12_DEFAULT_STENCIL_READ_MASK;state.DepthStencilState.StencilWriteMask=D3D12_DEFAULT_STENCIL_WRITE_MASK;
 auto& front=state.DepthStencilState.FrontFace;front.StencilFailOp=front.StencilDepthFailOp=front.StencilPassOp=D3D12_STENCIL_OP_KEEP;front.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;state.DepthStencilState.BackFace=front;
 const HRESULT pipelineResult=device12->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&motion.pipeline12));if(FAILED(pipelineResult)){char error[100];snprintf(error,sizeof(error),"Motion DX12 pipeline creation failed: 0x%08lx",(unsigned long)pipelineResult);Log(error);ComPtr<ID3D12InfoQueue> info;if(SUCCEEDED(device12.As(&info))){const auto count=info->GetNumStoredMessages();for(UINT64 i=count>12?count-12:0;i<count;++i){SIZE_T bytes{};info->GetMessage(i,nullptr,&bytes);if(bytes>65536)continue;std::vector<char> storage(bytes);auto* message=(D3D12_MESSAGE*)storage.data();if(SUCCEEDED(info->GetMessage(i,message,&bytes)))Log(message->pDescription);}}return false;}
 heap.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC cb{};cb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;cb.Width=256;cb.Height=1;cb.DepthOrArraySize=1;cb.MipLevels=1;cb.SampleDesc.Count=1;cb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 motion.constants12.resize(frames.size());for(auto& buffer:motion.constants12)if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&cb,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer))))return false;return true;
}
void MotionCallback12(const ImDrawList*,const ImDrawCmd*){
 if(!currentFrame||!motion.pipeline12)return;const size_t index=currentFrameIndex;if(index>=motion.constants12.size())return;auto* constants=motion.constants12[index].Get();void* mapped{};D3D12_RANGE none{};if(FAILED(constants->Map(0,&none,&mapped)))return;memcpy(mapped,&motion.constants,sizeof(MotionConstants));constants->Unmap(0,nullptr);
 auto* command=list12.Get();auto move=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=a;barrier.Transition.StateAfter=b;command->ResourceBarrier(1,&barrier);};
 move(currentFrame->buffer.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);command->CopyResource(motion.capture12.Get(),currentFrame->buffer.Get());move(currentFrame->buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);move(motion.capture12.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);move(motion.result12.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
 command->SetGraphicsRootSignature(motion.root12.Get());command->SetPipelineState(motion.pipeline12.Get());command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);command->SetGraphicsRootDescriptorTable(0,BlurSrv(13));command->SetGraphicsRootConstantBufferView(1,constants->GetGPUVirtualAddress());auto target=motion.rtv12->GetCPUDescriptorHandleForHeapStart();command->OMSetRenderTargets(1,&target,FALSE,nullptr);D3D12_VIEWPORT viewport{0,0,(float)motion.width,(float)motion.height,0,1};D3D12_RECT cut{0,0,(LONG)motion.width,(LONG)motion.height};command->RSSetViewports(1,&viewport);command->RSSetScissorRects(1,&cut);command->DrawInstanced(3,1,0,0);++motion.draws;
 move(motion.result12.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);move(motion.capture12.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);command->OMSetRenderTargets(1,&currentFrame->rtv,FALSE,nullptr);
}
bool MotionStationary(const float* r,float strength,float w,float h){
 for(int iy=-1;iy<=1;++iy)for(int ix=-1;ix<=1;++ix){const float x=(float)ix,y=(float)iy;
  const float px=r[0]*x+r[4]*y+r[8]*.5f+r[12],py=r[1]*x+r[5]*y+r[9]*.5f+r[13],pw=r[3]*x+r[7]*y+r[11]*.5f+r[15];
  if(!(pw>=.5f))return false;
  const float sx=(px/pw*.5f+.5f)*w,sy=(py/pw*-.5f+.5f)*h,ox=(x+1.f)*.5f*w,oy=(1.f-y)*.5f*h,vx=(ox-sx)*strength,vy=(oy-sy)*strength;
  if(!(vx*vx+vy*vy<.0625f))return false;}
 return true;
}
ImTextureID RequestMotionBlur(ImDrawList* draw,const float* reprojection,float strength,int samples,float maximum){
 if(!activeSwap||!reprojection||motion.failed||backend==Backend::None||hideFrame)return 0;UINT w=0,h=0;DXGI_FORMAT pixelFormat{};bool single=true;
 if(backend==Backend::DX11){ComPtr<ID3D11Texture2D> buffer;if(FAILED(activeSwap->GetBuffer(0,IID_PPV_ARGS(&buffer))))return 0;D3D11_TEXTURE2D_DESC desc{};buffer->GetDesc(&desc);w=desc.Width;h=desc.Height;pixelFormat=desc.Format;single=desc.SampleDesc.Count==1;}
 else{if(frames.empty()||!frames[0].buffer)return 0;auto desc=frames[0].buffer->GetDesc();w=(UINT)desc.Width;h=desc.Height;pixelFormat=desc.Format;single=desc.SampleDesc.Count==1;}
 if(motion.width!=w||motion.height!=h){if(backend==Backend::DX12&&motion.width!=0)Flush12();ReleaseMotion();if(!single||w<1||h<1||!(backend==Backend::DX11?CreateMotion11(w,h,pixelFormat):CreateMotion12(w,h,pixelFormat))){ReleaseMotion();motion.failed=true;Log("Original Motion Blur: GPU resource creation failed");return 0;}motion.width=w;motion.height=h;Log("Original Motion Blur: pixel shader and GPU resources ready");}
 // Camera standing still: the reprojection maps every pixel onto itself, the shader would early-out everywhere and the composite would
 // be a no-op, so skip the copy and both passes. Same screen-space velocity as the shader at the corners, edge midpoints and centre,
 // with a margin below its 0.5 px threshold; a near-zero or negative w anywhere counts as moving.
 if(MotionStationary(reprojection,std::clamp(strength,0.f,1.f),(float)w,(float)h))return 0;
 MotionConstants cb{};cb.size[0]=(float)w;cb.size[1]=(float)h;cb.region[2]=(float)w;cb.region[3]=(float)h;cb.options[0]=1;cb.display[2]=cb.display[3]=1;cb.viewport[2]=(float)w;cb.viewport[3]=(float)h;memcpy(cb.reprojection,reprojection,64);cb.motion[0]=std::clamp(strength,0.f,1.f);cb.motion[1]=std::clamp(maximum,8.f,128.f)*h/1080.f;cb.motion[2]=(float)std::clamp(samples,4,32);motion.constants=cb;
 draw->AddCallback(backend==Backend::DX11?MotionCallback11:MotionCallback12,nullptr);draw->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
 return backend==Backend::DX11?(ImTextureID)(uintptr_t)motion.resultView11.Get():(ImTextureID)BlurSrv(14).ptr;
}
