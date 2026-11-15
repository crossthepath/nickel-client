#include <d3d12sdklayers.h>
// In-process DXGI/ImGui renderer: hooks the game's swap chain, draws the Nickel menu and HUD, and routes input.
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_dx12.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <filesystem>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <thread>
#include "menu.hpp"
#include "probe_api.hpp"
#include "game_materials.hpp"
using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

namespace {
HMODULE ownModule{};HWND gameWindow{};WNDPROC previousProc{};IDXGISwapChain* activeSwap{};
std::recursive_mutex renderMutex;std::mutex queueMutex;auto& observedQueue=*new ComPtr<ID3D12CommandQueue>;
std::atomic<bool> toggleRequested{},closeRequested{},rendererReady{};thread_local bool insidePresent=false;
HANDLE readyEvent{};ULONGLONG submittedFrames=0;std::wstring directory;std::ofstream logFile;std::mutex logMutex;
enum class Backend{None,DX11,DX12};Backend backend=Backend::None;
ImGuiContext* context{};ImTextureID logo{};unsigned imageWidth{},imageHeight{};std::vector<unsigned char> pixels;
// Process-lifetime state: do not release DirectX interfaces under loader lock
// during process teardown. Explicit probe shutdown releases them before exit.
auto& device11=*new ComPtr<ID3D11Device>;auto& context11=*new ComPtr<ID3D11DeviceContext>;auto& target11=*new ComPtr<ID3D11RenderTargetView>;auto& image11=*new ComPtr<ID3D11ShaderResourceView>;
auto& device12=*new ComPtr<ID3D12Device>;auto& queue12=*new ComPtr<ID3D12CommandQueue>;auto& rtvHeap=*new ComPtr<ID3D12DescriptorHeap>;auto& srvHeap=*new ComPtr<ID3D12DescriptorHeap>;auto& list12=*new ComPtr<ID3D12GraphicsCommandList>;auto& fence12=*new ComPtr<ID3D12Fence>;auto& image12=*new ComPtr<ID3D12Resource>;
UINT64 fenceValue=0;HANDLE fenceEvent{};UINT descriptorSize{};DXGI_FORMAT format{};UINT backendFrames=0; // swap chain buffer count the ImGui DX12 backend was initialised with
struct Frame{ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12Resource> buffer;D3D12_CPU_DESCRIPTOR_HANDLE rtv{};UINT64 fence=0;};auto& frames=*new std::vector<Frame>;
using Present=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using Present1=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
using Resize=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
// IDXGISwapChain3::ResizeBuffers1 (vtable index 39): Minecraft DX12 calls it when its window regains focus.
// It must be hooked so our back-buffer references are released first, otherwise DXGI returns 0x887a0001
// (DXGI_ERROR_INVALID_CALL) and the game aborts.
using Resize1=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown*const*);
using Execute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList*const*);
Present originalPresent[2]{};Present1 originalPresent1[2]{};Resize originalResize[2]{};Resize1 originalResize1[2]{};Execute originalExecute{};
void Log(const char* message){std::lock_guard<std::mutex> lock(logMutex);if(logFile)logFile<<GetTickCount64()<<" "<<message<<std::endl;}
bool Check(HRESULT result,const char* where){if(FAILED(result)){Log(where);return false;}return true;}
bool OwnProbeProcess(){wchar_t image[32768]{};GetModuleFileNameW(nullptr,image,32768);auto name=wcsrchr(image,L'\\');return !_wcsicmp(name?name+1:image,L"Nickel.OverlayProbe.exe");}
bool FenceWait(UINT64 value){if(!value||fence12->GetCompletedValue()>=value)return true;if(FAILED(fence12->SetEventOnCompletion(value,fenceEvent)))return false;return WaitForSingleObject(fenceEvent,2000)==WAIT_OBJECT_0;}
bool Flush12(){if(!queue12||!fence12)return true;auto value=++fenceValue;return SUCCEEDED(queue12->Signal(fence12.Get(),value))&&FenceWait(value);}
void DecodeLogo(){
 if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return;
 {ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
 auto resource=FindResourceW(ownModule,MAKEINTRESOURCEW(101),RT_RCDATA);auto data=(BYTE*)LockResource(LoadResource(ownModule,resource));auto size=SizeofResource(ownModule,resource);
 if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromMemory(data,size))&&SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder))&&SUCCEEDED(decoder->GetFrame(0,&frame))&&SUCCEEDED(factory->CreateFormatConverter(&converter))&&SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))&&SUCCEEDED(converter->GetSize(&imageWidth,&imageHeight))){pixels.resize(imageWidth*imageHeight*4);if(FAILED(converter->CopyPixels(nullptr,imageWidth*4,(UINT)pixels.size(),pixels.data())))pixels.clear();}}
 CoUninitialize();
}
// Input message queue: filled by the window procedure, drained by the render thread right before NewFrame.
// Calling ImGui directly from the window procedure would need the render lock, which is held almost constantly,
// so most clicks would be dropped.
struct InputMessage{HWND hwnd;UINT message;WPARAM wparam;LPARAM lparam;};std::mutex inputQueueMutex;std::vector<InputMessage> inputQueue;
bool InputForMenu(UINT message){return (message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)||(message>=WM_KEYFIRST&&message<=WM_KEYLAST)||message==WM_MOUSELEAVE||message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_INPUTLANGCHANGE;}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam){
 const bool menuKey=(message==WM_KEYDOWN||message==WM_SYSKEYDOWN)&&!(lparam&(1LL<<30))&&overlay::MenuKeyMatches(wparam,lparam);
 if(menuKey&&!overlay::RecordingKey()&&overlay::CanToggleNow()){toggleRequested.store(true);return 0;}
 if(message==WM_KILLFOCUS||message==WM_DESTROY)closeRequested.store(true);
 if(message==WM_KEYDOWN&&wparam==VK_ESCAPE&&overlay::MenuIsOpen()&&!overlay::RecordingKey()){closeRequested.store(true);return 0;}
 if(InputForMenu(message)){std::lock_guard<std::mutex> lock(inputQueueMutex);
  // Consecutive mouse moves collapse into the last one (only the final position matters); buttons, keys, wheel and focus events are never dropped, only an absurd backlog evicts the oldest move first.
  if(message==WM_MOUSEMOVE&&!inputQueue.empty()&&inputQueue.back().message==WM_MOUSEMOVE)inputQueue.back()={hwnd,message,wparam,lparam};
  else{if(inputQueue.size()>=4096){auto oldest=std::find_if(inputQueue.begin(),inputQueue.end(),[](const InputMessage& queued){return queued.message==WM_MOUSEMOVE;});if(oldest!=inputQueue.end())inputQueue.erase(oldest);}if(inputQueue.size()<16384)inputQueue.push_back({hwnd,message,wparam,lparam});}}
 if(overlay::MenuIsOpen()){
  if(message==WM_INPUT)return DefWindowProcW(hwnd,message,wparam,lparam);
  if((message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)||message==WM_KEYDOWN||message==WM_CHAR)return 0;
 }
 return CallWindowProcW(previousProc,hwnd,message,wparam,lparam);
}
bool InitPlatform(HWND hwnd){
 context=ImGui::CreateContext();ImGui::SetCurrentContext(context);ImGui::StyleColorsDark();overlay::InitializeMenu(ownModule);overlay::SetWindow(hwnd);ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
 if(!ImGui_ImplWin32_Init(hwnd)){ImGui::DestroyContext(context);context=nullptr;return false;}
 gameWindow=hwnd;SetLastError(0);previousProc=(WNDPROC)SetWindowLongPtrW(hwnd,GWLP_WNDPROC,(LONG_PTR)WindowProc);
 if(!previousProc&&GetLastError()){ImGui_ImplWin32_Shutdown();ImGui::DestroyContext(context);context=nullptr;gameWindow=nullptr;return false;}return true;
}
bool BuildFrames(IDXGISwapChain* swap){
 DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(swap->GetDesc(&desc))||desc.BufferCount<1||desc.BufferCount>16)return false;
 frames.clear();frames.resize(desc.BufferCount);D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=desc.BufferCount;
 if(!Check(device12->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtvHeap)),"RTV heap creation failed"))return false;
 auto handle=rtvHeap->GetCPUDescriptorHandleForHeapStart();descriptorSize=device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
 for(UINT i=0;i<desc.BufferCount;++i){auto& frame=frames[i];if(FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&frame.allocator)))||FAILED(swap->GetBuffer(i,IID_PPV_ARGS(&frame.buffer))))return false;frame.rtv=handle;device12->CreateRenderTargetView(frame.buffer.Get(),nullptr,handle);handle.ptr+=descriptorSize;}
 return true;
}
bool UploadImage12(){
 if(pixels.empty())return true;
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=imageWidth;texture.Height=imageHeight;texture.DepthOrArraySize=1;texture.MipLevels=1;texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.SampleDesc.Count=1;
 if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image12))))return false;
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device12->GetCopyableFootprints(&texture,0,1,0,&footprint,nullptr,nullptr,&bytes);
 heap.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 ComPtr<ID3D12Resource> upload;if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload))))return false;
 void* mapped{};D3D12_RANGE range{};if(FAILED(upload->Map(0,&range,&mapped)))return false;for(UINT y=0;y<imageHeight;++y)memcpy((BYTE*)mapped+y*footprint.Footprint.RowPitch,pixels.data()+y*imageWidth*4,imageWidth*4);upload->Unmap(0,nullptr);
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> command;if(FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command))))return false;
 D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=image12.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;command->CopyTextureRegion(&to,0,0,0,&from,nullptr);
 D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=image12.Get();barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;command->ResourceBarrier(1,&barrier);command->Close();ID3D12CommandList* lists[]={command.Get()};queue12->ExecuteCommandLists(1,lists);if(!Flush12())return false;
 auto cpu=srvHeap->GetCPUDescriptorHandleForHeapStart();auto gpu=srvHeap->GetGPUDescriptorHandleForHeapStart();auto increment=device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);cpu.ptr+=increment;gpu.ptr+=increment;
 D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=texture.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device12->CreateShaderResourceView(image12.Get(),&view,cpu);logo=(ImTextureID)gpu.ptr;return true;
}
// ---- Background blur (Settings): blurs the game image behind the menu. The game frame is copied, downsampled
// (1/2, 1/4, 1/8) with a small 5-tap shader, then drawn stretched. Everything runs in an ImGui callback during menu
// rendering, so it lands in the same command list. Resources exist only while the switch is on (off by default).
const char* blurShader=R"(Texture2D t:register(t0);SamplerState s:register(s0);
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V vs(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}
float4 ps(V i):SV_Target{uint w,h;t.GetDimensions(w,h);float2 d=1.5/float2(w,h);
 float3 c=t.Sample(s,i.uv).rgb*4+t.Sample(s,i.uv+d).rgb+t.Sample(s,i.uv-d).rgb+t.Sample(s,i.uv+float2(d.x,-d.y)).rgb+t.Sample(s,i.uv+float2(-d.x,d.y)).rgb;return float4(c/8,1);})";
struct BlurState{UINT width=0,height=0;bool failed=false;
 ComPtr<ID3D11Texture2D> capture11,level11[4];ComPtr<ID3D11ShaderResourceView> captureView11,view11[4];ComPtr<ID3D11RenderTargetView> levelTarget11[4];ComPtr<ID3D11VertexShader> vs11;ComPtr<ID3D11PixelShader> ps11;ComPtr<ID3D11SamplerState> sampler11;ComPtr<ID3D11RasterizerState> raster11;
 ComPtr<ID3D12Resource> capture12,level12[4];ComPtr<ID3D12DescriptorHeap> rtv12;ComPtr<ID3D12RootSignature> root12;ComPtr<ID3D12PipelineState> pso12;};
auto& blur=*new BlurState;Frame* currentFrame=nullptr;size_t currentFrameIndex=0; // index of currentFrame in its own swap chain (game or Stream Hide)
bool hideFrame=false; // set by Draw before the menu runs: this frame goes to the Stream Hide chain, where the game image cannot be captured
void ReleaseBlur(){blur=BlurState{};}
UINT BlurSize(UINT full,int level){return std::max(1u,full>>(level<3?level+1:3));}
// Pass order: game image -> 1/2 -> 1/4 -> 1/8, then two ping-pong passes at 1/8 to smooth; result in level 3.
const int blurPasses[6][2]={{-1,0},{0,1},{1,2},{2,3},{3,2},{2,3}};
// Compiled once per process (render thread only, under renderMutex); a failed compile stays failed.
bool CompileBlur(ComPtr<ID3DBlob>& vs,ComPtr<ID3DBlob>& ps){static auto& cachedVs=*new ComPtr<ID3DBlob>;static auto& cachedPs=*new ComPtr<ID3DBlob>;static bool tried=false;
 if(!tried){tried=true;ComPtr<ID3DBlob> v,p;if(SUCCEEDED(D3DCompile(blurShader,strlen(blurShader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&v,nullptr))&&SUCCEEDED(D3DCompile(blurShader,strlen(blurShader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&p,nullptr))){cachedVs=v;cachedPs=p;}}
 if(!cachedVs||!cachedPs)return false;vs=cachedVs;ps=cachedPs;return true;}
bool CreateBlur11(UINT w,UINT h,DXGI_FORMAT pixelFormat){
 D3D11_TEXTURE2D_DESC texture{};texture.Width=w;texture.Height=h;texture.MipLevels=1;texture.ArraySize=1;texture.Format=pixelFormat;texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_SHADER_RESOURCE;
 if(FAILED(device11->CreateTexture2D(&texture,nullptr,&blur.capture11))||FAILED(device11->CreateShaderResourceView(blur.capture11.Get(),nullptr,&blur.captureView11)))return false;
 texture.BindFlags|=D3D11_BIND_RENDER_TARGET;
 for(int i=0;i<4;++i){texture.Width=BlurSize(w,i);texture.Height=BlurSize(h,i);if(FAILED(device11->CreateTexture2D(&texture,nullptr,&blur.level11[i]))||FAILED(device11->CreateShaderResourceView(blur.level11[i].Get(),nullptr,&blur.view11[i]))||FAILED(device11->CreateRenderTargetView(blur.level11[i].Get(),nullptr,&blur.levelTarget11[i])))return false;}
 ComPtr<ID3DBlob> vs,ps;if(!CompileBlur(vs,ps)||FAILED(device11->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&blur.vs11))||FAILED(device11->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&blur.ps11)))return false;
 D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampler.ComparisonFunc=D3D11_COMPARISON_ALWAYS;sampler.MaxLOD=D3D11_FLOAT32_MAX;
 D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
 return SUCCEEDED(device11->CreateSamplerState(&sampler,&blur.sampler11))&&SUCCEEDED(device11->CreateRasterizerState(&raster,&blur.raster11));
}
void BlurCallback11(const ImDrawList*,const ImDrawCmd*){
 ComPtr<ID3D11Texture2D> buffer;if(!activeSwap||!blur.capture11||!target11||FAILED(activeSwap->GetBuffer(0,IID_PPV_ARGS(&buffer))))return;auto* c=context11.Get();
 c->CopyResource(blur.capture11.Get(),buffer.Get());c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(blur.vs11.Get(),nullptr,0);c->PSSetShader(blur.ps11.Get(),nullptr,0);
 auto* sampler=blur.sampler11.Get();c->PSSetSamplers(0,1,&sampler);c->OMSetBlendState(nullptr,nullptr,0xffffffff);c->OMSetDepthStencilState(nullptr,0);c->RSSetState(blur.raster11.Get());ID3D11ShaderResourceView* none=nullptr;
 for(auto& pass:blurPasses){c->PSSetShaderResources(0,1,&none);auto* target=blur.levelTarget11[pass[1]].Get();c->OMSetRenderTargets(1,&target,nullptr);
  D3D11_VIEWPORT view{0,0,(float)BlurSize(blur.width,pass[1]),(float)BlurSize(blur.height,pass[1]),0,1};c->RSSetViewports(1,&view);auto* source=pass[0]<0?blur.captureView11.Get():blur.view11[pass[0]].Get();c->PSSetShaderResources(0,1,&source);c->Draw(3,0);}
 c->PSSetShaderResources(0,1,&none);auto* target=target11.Get();c->OMSetRenderTargets(1,&target,nullptr);
}
D3D12_GPU_DESCRIPTOR_HANDLE BlurSrv(int slot){auto handle=srvHeap->GetGPUDescriptorHandleForHeapStart();handle.ptr+=(UINT64)slot*device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);return handle;}
bool CreateBlur12(UINT w,UINT h,DXGI_FORMAT pixelFormat){
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=w;texture.Height=h;texture.DepthOrArraySize=1;texture.MipLevels=1;texture.Format=pixelFormat;texture.SampleDesc.Count=1;
 if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&blur.capture12))))return false;
 D3D12_DESCRIPTOR_HEAP_DESC targets{};targets.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;targets.NumDescriptors=4;if(FAILED(device12->CreateDescriptorHeap(&targets,IID_PPV_ARGS(&blur.rtv12))))return false;
 auto rtv=blur.rtv12->GetCPUDescriptorHandleForHeapStart();auto srv=srvHeap->GetCPUDescriptorHandleForHeapStart();const auto srvStep=device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),rtvStep=device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
 D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=pixelFormat;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
 srv.ptr+=2*srvStep;device12->CreateShaderResourceView(blur.capture12.Get(),&view,srv);texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
 for(int i=0;i<4;++i){texture.Width=BlurSize(w,i);texture.Height=BlurSize(h,i);if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&blur.level12[i]))))return false;
  srv.ptr+=srvStep;device12->CreateShaderResourceView(blur.level12[i].Get(),&view,srv);device12->CreateRenderTargetView(blur.level12[i].Get(),nullptr,rtv);rtv.ptr+=rtvStep;}
 D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter.DescriptorTable.NumDescriptorRanges=1;parameter.DescriptorTable.pDescriptorRanges=&range;parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
 D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
 D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=1;signature.pParameters=&parameter;signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;ComPtr<ID3DBlob> packed;
 if(FAILED(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&packed,nullptr))||FAILED(device12->CreateRootSignature(0,packed->GetBufferPointer(),packed->GetBufferSize(),IID_PPV_ARGS(&blur.root12))))return false;
 ComPtr<ID3DBlob> vs,ps;if(!CompileBlur(vs,ps))return false;D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};state.pRootSignature=blur.root12.Get();state.VS={vs->GetBufferPointer(),vs->GetBufferSize()};state.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
 state.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;state.SampleMask=UINT_MAX;state.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;state.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;state.RasterizerState.DepthClipEnable=TRUE;
 state.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;state.NumRenderTargets=1;state.RTVFormats[0]=pixelFormat;state.SampleDesc.Count=1;
 return SUCCEEDED(device12->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&blur.pso12)));
}
void BlurCallback12(const ImDrawList*,const ImDrawCmd*){
 if(!currentFrame||!blur.pso12)return;auto* l=list12.Get();
 auto move=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=before;barrier.Transition.StateAfter=after;l->ResourceBarrier(1,&barrier);};
 move(currentFrame->buffer.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);l->CopyResource(blur.capture12.Get(),currentFrame->buffer.Get());move(currentFrame->buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
 move(blur.capture12.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);l->SetGraphicsRootSignature(blur.root12.Get());l->SetPipelineState(blur.pso12.Get());l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 const auto rtvStep=device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
 for(auto& pass:blurPasses){auto* target=blur.level12[pass[1]].Get();move(target,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);auto handle=blur.rtv12->GetCPUDescriptorHandleForHeapStart();handle.ptr+=(SIZE_T)pass[1]*rtvStep;l->OMSetRenderTargets(1,&handle,FALSE,nullptr);
  const UINT w=BlurSize(blur.width,pass[1]),h=BlurSize(blur.height,pass[1]);D3D12_VIEWPORT view{0,0,(float)w,(float)h,0,1};l->RSSetViewports(1,&view);D3D12_RECT cut{0,0,(LONG)w,(LONG)h};l->RSSetScissorRects(1,&cut);
  l->SetGraphicsRootDescriptorTable(0,BlurSrv(pass[0]<0?2:3+pass[0]));l->DrawInstanced(3,1,0,0);move(target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);}
 move(blur.capture12.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);l->OMSetRenderTargets(1,&currentFrame->rtv,FALSE,nullptr);
}
// Called by the menu when Background blur is on: sizes the resources to the game image, appends the blur
// callback to the given draw list and returns the blurred texture (0 if unavailable; the menu then keeps its dim overlay).
ImTextureID RequestBlur(ImDrawList* list){
 if(!activeSwap||blur.failed||backend==Backend::None||hideFrame)return 0;UINT w=0,h=0;DXGI_FORMAT pixelFormat=DXGI_FORMAT_UNKNOWN;bool single=true;
 if(backend==Backend::DX11){ComPtr<ID3D11Texture2D> buffer;D3D11_TEXTURE2D_DESC desc{};if(FAILED(activeSwap->GetBuffer(0,IID_PPV_ARGS(&buffer))))return 0;buffer->GetDesc(&desc);w=desc.Width;h=desc.Height;pixelFormat=desc.Format;single=desc.SampleDesc.Count==1;}
 else{if(frames.empty()||!frames[0].buffer)return 0;auto desc=frames[0].buffer->GetDesc();w=(UINT)desc.Width;h=desc.Height;pixelFormat=desc.Format;single=desc.SampleDesc.Count==1;}
 if(blur.width!=w||blur.height!=h){
  if(backend==Backend::DX12&&blur.width!=0)Flush12();ReleaseBlur();
  if(!single||w<16||h<16||!(backend==Backend::DX11?CreateBlur11(w,h,pixelFormat):CreateBlur12(w,h,pixelFormat))){ReleaseBlur();blur.failed=true;Log("Background blur unavailable: resources could not be created");return 0;}
  blur.width=w;blur.height=h;Log(backend==Backend::DX11?"Background blur ready (DX11)":"Background blur ready (DX12)");}
 list->AddCallback(backend==Backend::DX11?BlurCallback11:BlurCallback12,nullptr);list->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
 return backend==Backend::DX11?(ImTextureID)(uintptr_t)blur.view11[3].Get():(ImTextureID)BlurSrv(6).ptr;
}
#include "post_process.hpp"
// ---- Dynamic HUD images (Target HUD head, Spotify Manager cover, MiniMap terrain): RGBA pixels supplied by the
// menu and uploaded to our own textures. DX12 uses heap descriptors 8 to 12 and 15. Creating a slot or changing its size
// waits on the queue (rare); a same-size update only stores a CPU copy and the copy is recorded into the frame's own
// command list by RecordUploads (no extra submission, no GPU wait). Released by CleanupRenderer.
struct DynamicImage{ComPtr<ID3D11ShaderResourceView> view11;ComPtr<ID3D12Resource> texture12,upload12;int width=0,height=0;ImTextureID id=0;
 std::vector<unsigned char> cpu;bool pending=false,recorded=false;UINT64 uploadFence=0;D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;};DynamicImage* dynamicImages=new DynamicImage[6];
ImTextureID GetImage(int slot){return slot>=0&&slot<6?dynamicImages[slot].id:0;}
ImTextureID UpdateImage(int slot,const unsigned char* rgba,int w,int h){
if(slot<0||slot>5||!rgba||w<1||h<1||w>2048||h>2048)return 0;auto& image=dynamicImages[slot];
 if(backend==Backend::DX11){
  if(image.view11&&image.width==w&&image.height==h){ComPtr<ID3D11Resource> resource;image.view11->GetResource(&resource);context11->UpdateSubresource(resource.Get(),0,nullptr,rgba,w*4,0);return image.id;}
  D3D11_TEXTURE2D_DESC desc{};desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA data{};data.pSysMem=rgba;data.SysMemPitch=w*4;
  ComPtr<ID3D11Texture2D> texture;image=DynamicImage{};if(FAILED(device11->CreateTexture2D(&desc,&data,&texture))||FAILED(device11->CreateShaderResourceView(texture.Get(),nullptr,&image.view11)))return 0;
  image.width=w;image.height=h;image.id=(ImTextureID)(uintptr_t)image.view11.Get();return image.id;}
 if(backend!=Backend::DX12||!device12||!queue12||!srvHeap)return 0;
 if(image.texture12&&image.width==w&&image.height==h){image.cpu.assign(rgba,rgba+(size_t)w*h*4);image.pending=true;return image.id;}
 D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=w;texture.Height=h;texture.DepthOrArraySize=1;texture.MipLevels=1;texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.SampleDesc.Count=1;
 ComPtr<ID3D12Resource> fresh,upload;if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&fresh))))return 0;
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device12->GetCopyableFootprints(&texture,0,1,0,&footprint,nullptr,nullptr,&bytes);
 heap.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
 if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload))))return 0;
 void* mapped{};D3D12_RANGE range{};if(FAILED(upload->Map(0,&range,&mapped)))return 0;for(int y=0;y<h;++y)memcpy((BYTE*)mapped+y*footprint.Footprint.RowPitch,rgba+(size_t)y*w*4,(size_t)w*4);upload->Unmap(0,nullptr);
 ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> command;if(FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&command))))return 0;
 D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=fresh.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;command->CopyTextureRegion(&to,0,0,0,&from,nullptr);
 D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=fresh.Get();barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;command->ResourceBarrier(1,&barrier);command->Close();
 ID3D12CommandList* lists[]={command.Get()};queue12->ExecuteCommandLists(1,lists);if(!Flush12())return 0;
 auto cpu=srvHeap->GetCPUDescriptorHandleForHeapStart();auto gpu=srvHeap->GetGPUDescriptorHandleForHeapStart();const auto increment=device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);const int descriptor=slot<5?8+slot:15;cpu.ptr+=(SIZE_T)increment*descriptor;gpu.ptr+=(UINT64)increment*descriptor;
 D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=texture.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device12->CreateShaderResourceView(fresh.Get(),&view,cpu);
 image.texture12=fresh;image.upload12.Reset();image.cpu.clear();image.pending=false;image.recorded=false;image.uploadFence=0;image.footprint=footprint;image.bytes=bytes;image.width=w;image.height=h;image.id=(ImTextureID)gpu.ptr;return image.id;
}
// Records the pending same-size DX12 uploads into list12 (called right after list12->Reset, before the overlay draws, so
// the textures are valid when sampled). The slot's upload buffer is persistent and only rewritten once the fence of its
// previous copy has passed. Slots stay pending until StampUploads confirms the list was submitted.
void RecordUploads(){
 D3D12_RESOURCE_BARRIER toCopy[6]{},toRead[6]{};int slots[6]{};UINT count=0;
 for(int i=0;i<6;++i){auto& image=dynamicImages[i];image.recorded=false;if(!image.pending||!image.texture12||image.cpu.size()<(size_t)image.width*image.height*4)continue;
  if(!FenceWait(image.uploadFence))continue;
  if(!image.upload12){D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=image.bytes;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   if(FAILED(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&image.upload12)))){image.upload12.Reset();image.pending=false;continue;}}
  void* mapped{};D3D12_RANGE none{};if(FAILED(image.upload12->Map(0,&none,&mapped))){image.pending=false;continue;}
  for(int y=0;y<image.height;++y)memcpy((BYTE*)mapped+(size_t)y*image.footprint.Footprint.RowPitch,image.cpu.data()+(size_t)y*image.width*4,(size_t)image.width*4);image.upload12->Unmap(0,nullptr);
  auto& before=toCopy[count];before.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;before.Transition.pResource=image.texture12.Get();before.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;before.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;before.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
  auto& after=toRead[count];after=before;std::swap(after.Transition.StateBefore,after.Transition.StateAfter);image.recorded=true;slots[count++]=i;}
 if(!count)return;auto* l=list12.Get();l->ResourceBarrier(count,toCopy);
 for(UINT k=0;k<count;++k){auto& image=dynamicImages[slots[k]];D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=image.upload12.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=image.footprint;D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=image.texture12.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;l->CopyTextureRegion(&to,0,0,0,&from,nullptr);}
 l->ResourceBarrier(count,toRead);
}
// After the list that carried the recorded copies was submitted: the slots are up to date and their upload buffers are busy until `fence`.
void StampUploads(UINT64 fence){for(int i=0;i<6;++i){auto& image=dynamicImages[i];if(image.recorded){image.recorded=false;image.pending=false;image.uploadFence=fence;}}}
void CleanupRenderer(){
 ReleaseBlur();ReleaseMotion();for(int i=0;i<6;++i)dynamicImages[i]=DynamicImage{};currentFrame=nullptr;backendFrames=0;
 rendererReady=false;if(readyEvent)ResetEvent(readyEvent);if(context){ImGui::SetCurrentContext(context);overlay::CloseMenu(gameWindow);if(ImGui::GetIO().BackendRendererUserData){if(backend==Backend::DX11)ImGui_ImplDX11_Shutdown();if(backend==Backend::DX12)ImGui_ImplDX12_Shutdown();}ImGui_ImplWin32_Shutdown();ImGui::DestroyContext(context);context=nullptr;}
 if(gameWindow&&previousProc&&IsWindow(gameWindow))SetWindowLongPtrW(gameWindow,GWLP_WNDPROC,(LONG_PTR)previousProc);previousProc=nullptr;gameWindow=nullptr;
 target11.Reset();image11.Reset();context11.Reset();device11.Reset();frames.clear();rtvHeap.Reset();srvHeap.Reset();list12.Reset();image12.Reset();fence12.Reset();queue12.Reset();device12.Reset();if(fenceEvent){CloseHandle(fenceEvent);fenceEvent=nullptr;}backend=Backend::None;logo=0;activeSwap=nullptr;
}
// Each Initialize failure reason is written once to overlay.log; renderer behaviour is unchanged.
std::atomic<unsigned> reportedReasons{0};
// The DX12 command queue is read from INSIDE the swap chain: DXGI keeps the pointer to the queue it was created with.
// The probe knows its own queue, so it finds the offset where that pointer is stored (queueOffset); the same offset
// on the game's swap chain yields the game's queue. The candidate is used only if its vtable comes from the same
// module as the probe queue's (queueModule), and is then checked with QueryInterface.
size_t queueOffset=0;HMODULE queueModule{};
bool Readable(const void* address){MEMORY_BASIC_INFORMATION info{};return address&&VirtualQuery(address,&info,sizeof(info))&&info.State==MEM_COMMIT&&!(info.Protect&(PAGE_NOACCESS|PAGE_GUARD));}
ComPtr<ID3D12CommandQueue> QueueInside(IDXGISwapChain* swap){
 ComPtr<ID3D12CommandQueue> queue;if(!queueOffset||!queueModule)return queue;auto slot=(void**)((char*)swap+queueOffset);if(!Readable(slot))return queue;void* candidate=*slot;if(!Readable(candidate))return queue;
 void* table=*(void**)candidate;HMODULE owner{};if(!Readable(table)||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)table,&owner)||owner!=queueModule)return queue;
 if(FAILED(((IUnknown*)candidate)->QueryInterface(IID_PPV_ARGS(&queue))))queue.Reset();return queue;
}
bool Why(unsigned bit,const std::string& text){if(!(reportedReasons.fetch_or(1u<<bit)&(1u<<bit)))Log(("Renderer not started: "+text).c_str());return false;}
bool Initialize(IDXGISwapChain* swap){
 DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(swap->GetDesc(&desc)))return Why(0,"swap chain description unavailable");if(!desc.OutputWindow)return Why(1,"swap chain has no output window ("+std::to_string(desc.BufferDesc.Width)+"x"+std::to_string(desc.BufferDesc.Height)+")");DWORD pid{};GetWindowThreadProcessId(desc.OutputWindow,&pid);RECT client{};GetClientRect(desc.OutputWindow,&client);if(pid!=GetCurrentProcessId())return Why(2,"swap chain window belongs to another process");if(client.right<400||client.bottom<300)return Why(3,"swap chain window too small ("+std::to_string(client.right)+"x"+std::to_string(client.bottom)+")");
 if(SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&device11)))){
  backend=Backend::DX11;device11->GetImmediateContext(&context11);if(!InitPlatform(desc.OutputWindow)||!ImGui_ImplDX11_Init(device11.Get(),context11.Get())){CleanupRenderer();return Why(4,"DX11 backend initialization failed");}
  if(!pixels.empty()){D3D11_TEXTURE2D_DESC image{};image.Width=imageWidth;image.Height=imageHeight;image.MipLevels=1;image.ArraySize=1;image.Format=DXGI_FORMAT_R8G8B8A8_UNORM;image.SampleDesc.Count=1;image.Usage=D3D11_USAGE_IMMUTABLE;image.BindFlags=D3D11_BIND_SHADER_RESOURCE;D3D11_SUBRESOURCE_DATA data{};data.pSysMem=pixels.data();data.SysMemPitch=imageWidth*4;ComPtr<ID3D11Texture2D> texture;if(SUCCEEDED(device11->CreateTexture2D(&image,&data,&texture))&&SUCCEEDED(device11->CreateShaderResourceView(texture.Get(),nullptr,&image11)))logo=(ImTextureID)(uintptr_t)image11.Get();}
 }else{
  if(FAILED(swap->GetDevice(IID_PPV_ARGS(&device12))))return Why(5,"swap chain exposes neither a DX11 nor a DX12 device");
  {ComPtr<ID3D12CommandQueue> own;ComPtr<ID3D12Device> ownDevice;auto inside=QueueInside(swap);
   if(inside&&inside->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&SUCCEEDED(inside->GetDevice(IID_PPV_ARGS(&ownDevice)))&&ownDevice.Get()==device12.Get()){queue12=inside;if(!(reportedReasons.fetch_or(1u<<22)&(1u<<22)))Log("DX12: command queue read inside the swap chain");}else

   // Hooking ExecuteCommandLists alone is not enough on Minecraft 1.26.62.01: no queue is ever observed there.
   if(SUCCEEDED(swap->GetDevice(IID_PPV_ARGS(&own)))&&own&&own->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&SUCCEEDED(own->GetDevice(IID_PPV_ARGS(&ownDevice)))&&ownDevice.Get()==device12.Get()){queue12=own;if(!(reportedReasons.fetch_or(1u<<20)&(1u<<20)))Log("DX12: command queue taken from the swap chain");}
   else{std::lock_guard<std::mutex> lock(queueMutex);if(!observedQueue)return Why(6,"DX12: swap chain gives no command queue and none was observed yet");ComPtr<ID3D12Device> owner;if(FAILED(observedQueue->GetDevice(IID_PPV_ARGS(&owner)))||owner.Get()!=device12.Get())return Why(7,"DX12: observed command queue belongs to another device");queue12=observedQueue;if(!(reportedReasons.fetch_or(1u<<21)&(1u<<21)))Log("DX12: command queue taken from the ExecuteCommandLists hook");}}
  backend=Backend::DX12;format=desc.BufferDesc.Format;D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=16;   /* 0 font, 1 logo, 2..6 background blur, 8..12 dynamic HUD images, 13..14 motion blur, 15 icon atlas */heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if(FAILED(device12->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srvHeap)))||FAILED(device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence12)))||!(fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr))||!BuildFrames(swap)||FAILED(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,frames[0].allocator.Get(),nullptr,IID_PPV_ARGS(&list12)))){CleanupRenderer();return Why(8,"DX12 frame resources could not be created");}list12->Close();
  if(!InitPlatform(desc.OutputWindow)){CleanupRenderer();return Why(9,"DX12 platform initialization failed");}
  ImGui_ImplDX12_InitInfo info{};info.Device=device12.Get();info.CommandQueue=queue12.Get();info.NumFramesInFlight=(int)frames.size();backendFrames=(UINT)frames.size();info.RTVFormat=format;info.SrvDescriptorHeap=srvHeap.Get();info.LegacySingleSrvCpuDescriptor=srvHeap->GetCPUDescriptorHandleForHeapStart();info.LegacySingleSrvGpuDescriptor=srvHeap->GetGPUDescriptorHandleForHeapStart();
  if(!ImGui_ImplDX12_Init(&info)||!UploadImage12()){CleanupRenderer();return Why(10,"DX12 backend initialization failed");}
 }
 overlay::RequestBlur=RequestBlur;overlay::RequestMotionBlur=RequestMotionBlur;overlay::UpdateImage=UpdateImage;overlay::GetImage=GetImage;overlay::backendName=backend==Backend::DX11?"D3D11":"D3D12";overlay::WriteLog=Log;activeSwap=swap;rendererReady=true;if(readyEvent)SetEvent(readyEvent);Log(backend==Backend::DX11?"DX11 renderer ready; Right Shift opens the menu":"DX12 renderer ready; Right Shift opens the menu");return true;
}
// ---- Stream Hide: a STATIC window (WS_EX 0x082800a0, WS_POPUP|WS_DISABLED) created on a dedicated thread,
// opacity 255, WDA_EXCLUDEFROMCAPTURE, with a DirectComposition swap chain (CreateSwapChainForComposition on the game's
// queue: 2 buffers, FLIP_SEQUENTIAL, premultiplied alpha, game format if it is 10 or 0x57, else 0x1c) bound to a visual.
// The UI is drawn there instead of into the game image, so screen capture never sees it. DX12 renderer only.
struct HideState{HWND hwnd=nullptr;std::atomic<bool> windowReady{false},windowFailed{false};ComPtr<IDCompositionDevice> dcomp;ComPtr<IDCompositionTarget> target;ComPtr<IDCompositionVisual> visual;
 ComPtr<IDXGISwapChain3> chain;std::vector<Frame> frames;ComPtr<ID3D12DescriptorHeap> rtv;UINT w=0,h=0;RECT placed{};bool failed=false,shown=false;};
HideState& Hide(){static auto* h=new HideState;return *h;}
void HideWindowThread(){auto& h=Hide();h.hwnd=CreateWindowExW(0x082800a0,L"STATIC",L"",WS_POPUP|WS_DISABLED,0,0,1,1,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 if(!h.hwnd){h.windowFailed=true;return;}SetLayeredWindowAttributes(h.hwnd,0,255,LWA_ALPHA);SetWindowDisplayAffinity(h.hwnd,0x11);h.windowReady=true;
 MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}}
bool HideBuildFrames(HideState& h){h.frames.clear();h.frames.resize(2);D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=2;
 if(FAILED(device12->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&h.rtv))))return false;auto handle=h.rtv->GetCPUDescriptorHandleForHeapStart();
 for(UINT i=0;i<2;++i){auto& f=h.frames[i];if(FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&f.allocator)))||FAILED(h.chain->GetBuffer(i,IID_PPV_ARGS(&f.buffer))))return false;
  f.rtv=handle;device12->CreateRenderTargetView(f.buffer.Get(),nullptr,handle);handle.ptr+=descriptorSize;}return true;}
bool HidePrepare(UINT w,UINT h12){auto& h=Hide();if(h.failed)return false;
 if(!h.windowReady){static bool started=false;if(!started&&!h.windowFailed){started=true;std::thread(HideWindowThread).detach();}return false;}
 if(!h.chain){ComPtr<IDXGIFactory2> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))){h.failed=true;Log("Stream Hide: DXGI factory unavailable");return false;}
  DXGI_SWAP_CHAIN_DESC1 d{};d.Width=w;d.Height=h12;d.Format=(format==DXGI_FORMAT_R16G16B16A16_FLOAT||format==DXGI_FORMAT_B8G8R8A8_UNORM)?format:DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;
  d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;d.Scaling=DXGI_SCALING_STRETCH;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;d.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
  ComPtr<IDXGISwapChain1> one;if(d.Format!=format){h.failed=true;Log("Stream Hide: game format differs from the composition format, not started");return false;}
  if(FAILED(factory->CreateSwapChainForComposition(queue12.Get(),&d,nullptr,&one))||FAILED(one.As(&h.chain))){h.failed=true;Log("Stream Hide: composition swap chain not created");return false;}
  if(FAILED(DCompositionCreateDevice(nullptr,IID_PPV_ARGS(&h.dcomp)))||FAILED(h.dcomp->CreateTargetForHwnd(h.hwnd,TRUE,&h.target))||FAILED(h.dcomp->CreateVisual(&h.visual))||
     FAILED(h.visual->SetContent(h.chain.Get()))||FAILED(h.target->SetRoot(h.visual.Get()))||FAILED(h.dcomp->Commit())||!HideBuildFrames(h)){h.failed=true;Log("Stream Hide: DirectComposition setup failed");return false;}
  h.w=w;h.h=h12;Log("Stream Hide: capture-excluded window ready");}
 if(w!=h.w||h12!=h.h){if(!Flush12())return false;h.frames.clear();if(FAILED(h.chain->ResizeBuffers(2,w,h12,DXGI_FORMAT_UNKNOWN,0))||!HideBuildFrames(h)){h.failed=true;return false;}h.w=w;h.h=h12;}
 RECT client{};POINT origin{};if(GetClientRect(gameWindow,&client)&&ClientToScreen(gameWindow,&origin)){RECT r{origin.x,origin.y,origin.x+client.right,origin.y+client.bottom};
  if(!h.shown||memcmp(&r,&h.placed,sizeof(r))){SetWindowPos(h.hwnd,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE|SWP_SHOWWINDOW);h.placed=r;h.shown=true;}}
 return true;}
void HideOff(){auto& h=Hide();if(h.shown&&h.hwnd){ShowWindow(h.hwnd,SW_HIDE);h.shown=false;}}
void HideRender(){auto& h=Hide();auto index=h.chain->GetCurrentBackBufferIndex();if(index>=h.frames.size())return;auto& frame=h.frames[index];
 if(!FenceWait(frame.fence)||FAILED(frame.allocator->Reset())||FAILED(list12->Reset(frame.allocator.Get(),nullptr)))return;
 RecordUploads();D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=frame.buffer.Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
 barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;list12->ResourceBarrier(1,&barrier);
 const float clear[4]={0,0,0,0};list12->ClearRenderTargetView(frame.rtv,clear,0,nullptr);list12->OMSetRenderTargets(1,&frame.rtv,FALSE,nullptr);ID3D12DescriptorHeap* heaps[]={srvHeap.Get()};list12->SetDescriptorHeaps(1,heaps);
 currentFrameIndex=index;currentFrame=&frame;ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),list12.Get());currentFrame=nullptr;std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list12->ResourceBarrier(1,&barrier);
 if(FAILED(list12->Close()))return;ID3D12CommandList* lists[]={list12.Get()};queue12->ExecuteCommandLists(1,lists);frame.fence=++fenceValue;if(SUCCEEDED(queue12->Signal(fence12.Get(),frame.fence)))++submittedFrames;StampUploads(frame.fence);
 // Do not block the game thread on the compositor: a dropped overlay frame is harmless, the next one re-renders into the same buffer.
 if(h.chain->Present(0,DXGI_PRESENT_DO_NOT_WAIT)==DXGI_ERROR_WAS_STILL_DRAWING)return;}
void Draw(IDXGISwapChain* swap,UINT flags){
 if(flags&DXGI_PRESENT_TEST)return;std::lock_guard<std::recursive_mutex> lock(renderMutex);
 if(!context&&!Initialize(swap))return;if(swap!=activeSwap)return;ImGui::SetCurrentContext(context);
 // After a resize only the swap chain buffers were released (ReleaseForResize): rebuild them here. The ImGui DX12 backend bakes the
 // buffer count and the RTV format in, so a change of either needs the full teardown and a fresh Initialize on the next Present.
 if(backend==Backend::DX12&&frames.empty()){DXGI_SWAP_CHAIN_DESC now{};if(FAILED(swap->GetDesc(&now))||now.BufferCount!=backendFrames||now.BufferDesc.Format!=format||!BuildFrames(swap)){Log("Resize: swap chain format or buffer count changed, or its buffers could not be rebuilt; renderer restarted");CleanupRenderer();return;}}
 if(!rendererReady.load()){rendererReady=true;if(readyEvent)SetEvent(readyEvent);}   // ready again after a resize
 hideFrame=backend==Backend::DX12&&overlay::streamHide.load()&&!Hide().failed;
 if(toggleRequested.exchange(false))overlay::ToggleMenu(gameWindow);if(closeRequested.exchange(false))overlay::CloseMenu(gameWindow);
 {std::vector<InputMessage> pending;{std::lock_guard<std::mutex> queueLock(inputQueueMutex);pending.swap(inputQueue);}for(auto& input:pending){if((input.message==WM_KEYDOWN||input.message==WM_SYSKEYDOWN)&&!(input.lparam&(1LL<<30)))overlay::HandleKey((unsigned)input.wparam,input.lparam);ImGui_ImplWin32_WndProcHandler(input.hwnd,input.message,input.wparam,input.lparam);}}
 // new font sizes baked between two frames: drop the font texture, the backend recreates it in NewFrame
 if(overlay::FontsRebuild()){if(backend==Backend::DX11)ImGui_ImplDX11_InvalidateDeviceObjects();else{Flush12();ImGui_ImplDX12_InvalidateDeviceObjects();}}
 if(backend==Backend::DX11)ImGui_ImplDX11_NewFrame();else ImGui_ImplDX12_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::GetIO().MouseDrawCursor=overlay::MenuIsOpen();ImGui::NewFrame();
 if(OwnProbeProcess()&&GetEnvironmentVariableW(L"NICKEL_REVIEW_MOTION",nullptr,0)){float reprojection[16]={1,0,0,0,0,1,0,0,0,0,1,0,.04f,0,0,1};auto* draw=ImGui::GetBackgroundDrawList();if(auto image=RequestMotionBlur(draw,reprojection,.5f,12,40))draw->AddImage(image,ImVec2(0,0),ImGui::GetIO().DisplaySize);}
 float fade=overlay::MenuProgress(ImGui::GetIO().DeltaTime);overlay::DrawMenu(fade,logo);ImGui::Render();if(!ImGui::GetDrawData()->TotalVtxCount)return;
 if(backend==Backend::DX11){
  if(!target11){ComPtr<ID3D11Texture2D> buffer;if(FAILED(swap->GetBuffer(0,IID_PPV_ARGS(&buffer)))||FAILED(device11->CreateRenderTargetView(buffer.Get(),nullptr,&target11)))return;}
  ComPtr<ID3D11RenderTargetView> previous;ComPtr<ID3D11DepthStencilView> depth;context11->OMGetRenderTargets(1,&previous,&depth);auto target=target11.Get();context11->OMSetRenderTargets(1,&target,nullptr);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());++submittedFrames;target=previous.Get();context11->OMSetRenderTargets(1,&target,depth.Get());
 }else{
  if(overlay::streamHide.load()&&!frames.empty()&&frames[0].buffer){auto d=frames[0].buffer->GetDesc();if(HidePrepare((UINT)d.Width,d.Height)){HideRender();return;}}else HideOff();
  ComPtr<IDXGISwapChain3> chain;if(FAILED(swap->QueryInterface(IID_PPV_ARGS(&chain))))return;auto index=chain->GetCurrentBackBufferIndex();if(index>=frames.size())return;auto& frame=frames[index];if(!FenceWait(frame.fence)||FAILED(frame.allocator->Reset())||FAILED(list12->Reset(frame.allocator.Get(),nullptr)))return;
  D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=frame.buffer.Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;list12->ResourceBarrier(1,&barrier);list12->OMSetRenderTargets(1,&frame.rtv,FALSE,nullptr);ID3D12DescriptorHeap* heaps[]={srvHeap.Get()};list12->SetDescriptorHeaps(1,heaps);RecordUploads();currentFrameIndex=index;currentFrame=&frame;ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),list12.Get());currentFrame=nullptr;std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list12->ResourceBarrier(1,&barrier);if(FAILED(list12->Close()))return;ID3D12CommandList* lists[]={list12.Get()};queue12->ExecuteCommandLists(1,lists);frame.fence=++fenceValue;if(SUCCEEDED(queue12->Signal(fence12.Get(),frame.fence)))++submittedFrames;StampUploads(frame.fence);
 }
}
std::atomic<bool> sawPresent{false},sawPresent1{false};
// Vsync Disabler (applied in the Present hook): outside DXGI_PRESENT_TEST and for the game's swap chain, sync
// interval 0; if the swap chain is windowed and was created with ALLOW_TEARING, flags drop TEST/RESTART and add
// ALLOW_TEARING. Recreating a DX11 swap chain without ALLOW_TEARING is not handled.
void VsyncAdjust(IDXGISwapChain* swap,UINT& interval,UINT& flags){if(!overlay::vsyncDisabled.load()||(flags&DXGI_PRESENT_TEST)||!swap||swap!=activeSwap)return;
 DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(swap->GetDesc(&desc)))return;static std::atomic<bool> told{false};if(!told.exchange(true))Log(desc.Flags&DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING?"Vsync Disabler: sync interval 0 with tearing":"Vsync Disabler: sync interval 0 (swap chain without tearing)");
 interval=0;if(desc.Windowed&&(desc.Flags&DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))flags=(flags&~(UINT)(DXGI_PRESENT_TEST|DXGI_PRESENT_RESTART))|DXGI_PRESENT_ALLOW_TEARING;}
// On resize, keep the swap chain's ALLOW_TEARING flag.
UINT KeepTearing(IDXGISwapChain* swap,UINT flags){DXGI_SWAP_CHAIN_DESC desc{};if(swap&&SUCCEEDED(swap->GetDesc(&desc)))flags|=desc.Flags&DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;return flags;}
HRESULT DispatchPresent(IDXGISwapChain* swap,UINT interval,UINT flags,Present original){if(!sawPresent.exchange(true))Log("First game frame seen through Present");if(insidePresent)return original(swap,interval,flags);insidePresent=true;try{Draw(swap,flags);}catch(...){Log("Renderer exception; original Present retained");}VsyncAdjust(swap,interval,flags);auto result=original(swap,interval,flags);insidePresent=false;return result;}
HRESULT DispatchPresent1(IDXGISwapChain1* swap,UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* params,Present1 original){if(!sawPresent1.exchange(true))Log("First game frame seen through Present1");if(insidePresent)return original(swap,interval,flags,params);insidePresent=true;try{Draw(swap,flags);}catch(...){Log("Renderer exception; original Present1 retained");}VsyncAdjust(swap,interval,flags);auto result=original(swap,interval,flags,params);insidePresent=false;return result;}
// Before any resize, release our references to the swap chain buffers (otherwise the game gets
// DXGI_ERROR_INVALID_CALL and stops). A failed GPU wait is not reported to the game, which treats any error as fatal.
// Only what references the swap chain buffers or is sized to the window is released; the ImGui context, fonts, backend,
// descriptor heap, queue objects, dynamic images and the window procedure survive (the menu is still closed, as before).
void ReleaseForResize(IDXGISwapChain* swap){std::lock_guard<std::recursive_mutex> lock(renderMutex);if(swap!=activeSwap)return;insidePresent=true;rendererReady=false;if(readyEvent)ResetEvent(readyEvent);   // not ready until the next Present rebuilds the buffers
 if(backend==Backend::DX12&&!Flush12()&&!Flush12())Log("Resize: GPU wait timed out; overlay resources released anyway");
 ReleaseBlur();ReleaseMotion();currentFrame=nullptr;if(context){ImGui::SetCurrentContext(context);overlay::CloseMenu(gameWindow);}
 target11.Reset();
 // list12 is closed but may still name the old buffers: reset it on an allocator that is about to be destroyed, then close it again.
 if(list12&&!frames.empty()&&frames[0].allocator&&SUCCEEDED(list12->Reset(frames[0].allocator.Get(),nullptr)))list12->Close();
 frames.clear();rtvHeap.Reset();insidePresent=false;}
HRESULT DispatchResize(IDXGISwapChain* swap,UINT count,UINT width,UINT height,DXGI_FORMAT newFormat,UINT flags,Resize original){ReleaseForResize(swap);std::lock_guard<std::recursive_mutex> lock(renderMutex);return original(swap,count,width,height,newFormat,KeepTearing(swap,flags));}
HRESULT DispatchResize1(IDXGISwapChain3* swap,UINT count,UINT width,UINT height,DXGI_FORMAT newFormat,UINT flags,const UINT* masks,IUnknown*const* queues,Resize1 original){static std::atomic<bool> seen{false};if(!seen.exchange(true))Log("Swap chain resized through ResizeBuffers1; overlay buffers released first");ReleaseForResize(swap);std::lock_guard<std::recursive_mutex> lock(renderMutex);return original(swap,count,width,height,newFormat,KeepTearing(swap,flags),masks,queues);}
HRESULT STDMETHODCALLTYPE PresentA(IDXGISwapChain* s,UINT i,UINT f){return DispatchPresent(s,i,f,originalPresent[0]);}HRESULT STDMETHODCALLTYPE PresentB(IDXGISwapChain* s,UINT i,UINT f){return DispatchPresent(s,i,f,originalPresent[1]);}
HRESULT STDMETHODCALLTYPE Present1A(IDXGISwapChain1* s,UINT i,UINT f,const DXGI_PRESENT_PARAMETERS* p){return DispatchPresent1(s,i,f,p,originalPresent1[0]);}HRESULT STDMETHODCALLTYPE Present1B(IDXGISwapChain1* s,UINT i,UINT f,const DXGI_PRESENT_PARAMETERS* p){return DispatchPresent1(s,i,f,p,originalPresent1[1]);}
HRESULT STDMETHODCALLTYPE Resize1A(IDXGISwapChain3* s,UINT c,UINT w,UINT h,DXGI_FORMAT f,UINT flags,const UINT* m,IUnknown*const* q){return DispatchResize1(s,c,w,h,f,flags,m,q,originalResize1[0]);}HRESULT STDMETHODCALLTYPE Resize1B(IDXGISwapChain3* s,UINT c,UINT w,UINT h,DXGI_FORMAT f,UINT flags,const UINT* m,IUnknown*const* q){return DispatchResize1(s,c,w,h,f,flags,m,q,originalResize1[1]);}
HRESULT STDMETHODCALLTYPE ResizeA(IDXGISwapChain* s,UINT c,UINT w,UINT h,DXGI_FORMAT f,UINT flags){return DispatchResize(s,c,w,h,f,flags,originalResize[0]);}HRESULT STDMETHODCALLTYPE ResizeB(IDXGISwapChain* s,UINT c,UINT w,UINT h,DXGI_FORMAT f,UINT flags){return DispatchResize(s,c,w,h,f,flags,originalResize[1]);}
void STDMETHODCALLTYPE ExecuteHook(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList*const* lists){if(!insidePresent&&!rendererReady.load()&&queue->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT){std::lock_guard<std::mutex> lock(queueMutex);observedQueue=queue;}originalExecute(queue,count,lists);}
bool AddHook(void* target,void* replacement,void** original){auto result=MH_CreateHook(target,replacement,original);if(result!=MH_OK){Log(MH_StatusToString(result));return false;}return true;}
bool HookSwap(IDXGISwapChain* swap,int slot,std::vector<void*>& seen){auto table=*(void***)swap;void* replacements[]={slot?(void*)PresentB:(void*)PresentA,slot?(void*)ResizeB:(void*)ResizeA};int indices[]={8,13};void** originals[]={(void**)&originalPresent[slot],(void**)&originalResize[slot]};bool success=true;for(int i=0;i<2;++i)if(std::find(seen.begin(),seen.end(),table[indices[i]])==seen.end()){success=AddHook(table[indices[i]],replacements[i],originals[i])&&success;seen.push_back(table[indices[i]]);}ComPtr<IDXGISwapChain1> chain;if(SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&chain)))){auto target=(*(void***)chain.Get())[22];if(std::find(seen.begin(),seen.end(),target)==seen.end()){success=AddHook(target,slot?(void*)Present1B:(void*)Present1A,(void**)&originalPresent1[slot])&&success;seen.push_back(target);}}ComPtr<IDXGISwapChain3> chain3;if(SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&chain3)))){auto target=(*(void***)chain3.Get())[39];if(std::find(seen.begin(),seen.end(),target)==seen.end()){success=AddHook(target,slot?(void*)Resize1B:(void*)Resize1A,(void**)&originalResize1[slot])&&success;seen.push_back(target);}}return success;}
// ---------------------------------------------------------------------------------------------------------------
// Game input while the menu is open.
// Blocking WM_INPUT and mouse messages is not enough: the game reads keyboard and mouse through GameInput
// (GameInputRedist.dll). We therefore hook, on the reading object:
// IGameInputReading::GetKeyCount (12), GetKeyState (13), GetMouseState (14)
// These indices and the GameInputMouseState layout are identical in the official v1 to v4 headers.
// With the menu open: no keys, no buttons, position and wheel frozen; on close the accumulated offset is kept so
// the camera does not jump. The game's ClipCursor / SetCursorPos calls are ignored so the cursor stays free.
struct GameInputMouse{uint32_t buttons;uint32_t positions;int64_t positionX,positionY,absolutePositionX,absolutePositionY,wheelX,wheelY;};
using MouseStateFn=bool(STDMETHODCALLTYPE*)(void*,GameInputMouse*);using KeyCountFn=uint32_t(STDMETHODCALLTYPE*)(void*);using KeyStateFn=uint32_t(STDMETHODCALLTYPE*)(void*,uint32_t,void*);
MouseStateFn originalMouseState[4]{};KeyCountFn originalKeyCount[4]{};KeyStateFn originalKeyState[4]{};int mouseSlots=0,keySlots=0;
std::mutex inputMutex;std::atomic<bool> sawMouseState{false};
// Four GetMouseState functions are hooked (one per interface version) and they call each other; without this
// depth counter the state would be filtered twice per read, resetting the offset and making the camera jump.
// Only the outermost call filters.
thread_local int inputDepth=0;
// One freeze state PER DEVICE. The game reads several mouse streams (aggregate and per-device) whose cumulative
// counters differ by a constant; forcing them to a single frozen value shifted one of them (Y position and wheel).
// Each stream therefore keeps its own frozen value and offset. The key is the reading's device
// (IGameInputReading::GetDevice, index 5 in v1 to v4); the object is only used as a key and released immediately.
struct InputStream{bool frozen=false;int64_t offset[4]{},held[4]{};};
std::map<void*,InputStream> inputStreams;
void FilterMouse(void* self,GameInputMouse* state){
 void* device=nullptr;((void(STDMETHODCALLTYPE*)(void*,void**))(*(void***)self)[5])(self,&device);if(device)((IUnknown*)device)->Release();
 std::lock_guard<std::mutex> lock(inputMutex);const bool known=inputStreams.count(device)!=0;auto& stream=inputStreams[device];
 if(!known&&inputStreams.size()<=6)Log(("GameInput: mouse stream "+std::to_string(inputStreams.size())+(device?" (device)":" (no device)")).c_str());
 int64_t raw[4]={state->positionX,state->positionY,state->wheelX,state->wheelY};
 if(overlay::MenuIsOpen()){if(!stream.frozen){stream.frozen=true;for(int i=0;i<4;++i)stream.held[i]=raw[i]-stream.offset[i];}for(int i=0;i<4;++i)stream.offset[i]=raw[i]-stream.held[i];state->buttons=0;}else{stream.frozen=false;state->buttons=overlay::SyntheticButtons(state->buttons);if(!overlay::Playing())for(int i=0;i<4;++i)stream.offset[i]=0;}   // a game screen with a cursor reads absolute positions: no offset left from the menu
 state->positionX=raw[0]-stream.offset[0];state->positionY=raw[1]-stream.offset[1];state->wheelX=raw[2]-stream.offset[2];state->wheelY=raw[3]-stream.offset[3];
}
#define NICKEL_INPUT_SLOT(n) \
 bool STDMETHODCALLTYPE MouseState##n(void* self,GameInputMouse* state){++inputDepth;bool ok=originalMouseState[n](self,state);--inputDepth;if(!sawMouseState.exchange(true))Log("GameInput: first mouse state read by the game");if(ok&&state&&inputDepth==0)FilterMouse(self,state);return ok;} \
 uint32_t STDMETHODCALLTYPE KeyCount##n(void* self){return overlay::MenuIsOpen()?0:originalKeyCount[n](self);} \
 uint32_t STDMETHODCALLTYPE KeyState##n(void* self,uint32_t count,void* states){return overlay::MenuIsOpen()?0:originalKeyState[n](self,count,states);}
NICKEL_INPUT_SLOT(0) NICKEL_INPUT_SLOT(1) NICKEL_INPUT_SLOT(2) NICKEL_INPUT_SLOT(3)
#undef NICKEL_INPUT_SLOT
void* const mouseStateHooks[4]={(void*)MouseState0,(void*)MouseState1,(void*)MouseState2,(void*)MouseState3};
void* const keyCountHooks[4]={(void*)KeyCount0,(void*)KeyCount1,(void*)KeyCount2,(void*)KeyCount3};
void* const keyStateHooks[4]={(void*)KeyState0,(void*)KeyState1,(void*)KeyState2,(void*)KeyState3};
using ClipCursorFn=BOOL(WINAPI*)(const RECT*);using SetCursorPosFn=BOOL(WINAPI*)(int,int);ClipCursorFn originalClipCursor{};SetCursorPosFn originalSetCursorPos{};
BOOL WINAPI ClipCursorHook(const RECT* area){if(overlay::MenuIsOpen()){overlay::NoteGameCursor(area!=nullptr);if(area)return TRUE;}return originalClipCursor(area);}
BOOL WINAPI SetCursorPosHook(int x,int y){if(overlay::MenuIsOpen()){overlay::NoteGameCursor(true);return TRUE;}return originalSetCursorPos(x,y);}
bool EnableOne(void* target,void* replacement,void** original){return MH_CreateHook(target,replacement,original)==MH_OK&&MH_EnableHook(target)==MH_OK;}
// Returns true when there is nothing left to retry (hooks installed, or GameInput not loaded in the process).
bool InstallInputHooks(std::vector<void*>& seen,bool lastAttempt){
 if(!originalClipCursor){EnableOne((void*)&ClipCursor,(void*)ClipCursorHook,(void**)&originalClipCursor);EnableOne((void*)&SetCursorPos,(void*)SetCursorPosHook,(void**)&originalSetCursorPos);}
 HMODULE library=GetModuleHandleW(L"GameInputRedist.dll");if(!library){Log(GetModuleHandleW(L"GameInput.dll")?"GameInput: only the system GameInput.dll (v0) is loaded; input not intercepted":"GameInput: not loaded in this process; input not intercepted");return true;}
 auto initialize=(HRESULT(WINAPI*)(REFIID,void**))GetProcAddress(library,"GameInputInitialize");auto create=(HRESULT(WINAPI*)(IUnknown**))GetProcAddress(library,"GameInputCreate");
 static const GUID versions[]={{0xF2DAAE4C,0x4D21,0x4250,{0x89,0x53,0x4F,0x79,0x1E,0x62,0x79,0xAB}},{0x20EFC1C7,0x5D9A,0x43BA,{0xB2,0x6F,0xB8,0x07,0xFA,0x48,0x60,0x9C}},{0xBBAA66D2,0x837A,0x40F7,{0xA3,0x03,0x91,0x7D,0x50,0x09,0x55,0xF4}},{0x40FFB7E4,0x6150,0x407A,{0xB4,0x39,0x13,0x2B,0xAD,0xC0,0x8D,0x2D}}};
 int interfaces=0,readings=0;
 for(auto& version:versions){
  ComPtr<IUnknown> input;if(!initialize||FAILED(initialize(version,(void**)input.GetAddressOf()))){input.Reset();ComPtr<IUnknown> legacy;if(create&&SUCCEEDED(create(legacy.GetAddressOf()))&&legacy)legacy->QueryInterface(version,(void**)input.GetAddressOf());}
  if(!input)continue;++interfaces;
  auto current=(HRESULT(STDMETHODCALLTYPE*)(void*,uint32_t,void*,void**))(*(void***)input.Get())[4];   // IGameInput::GetCurrentReading
  for(uint32_t kind:{0x20u,0x10u}){   // GameInputKindMouse, GameInputKindKeyboard
   IUnknown* reading=nullptr;if(FAILED(current(input.Get(),kind,nullptr,(void**)&reading))||!reading)continue;++readings;auto table=*(void***)reading;
   if(std::find(seen.begin(),seen.end(),table[14])==seen.end()&&mouseSlots<4){if(EnableOne(table[14],mouseStateHooks[mouseSlots],(void**)&originalMouseState[mouseSlots]))++mouseSlots;seen.push_back(table[14]);}
   if(std::find(seen.begin(),seen.end(),table[12])==seen.end()&&std::find(seen.begin(),seen.end(),table[13])==seen.end()&&keySlots<4){if(EnableOne(table[12],keyCountHooks[keySlots],(void**)&originalKeyCount[keySlots])&&EnableOne(table[13],keyStateHooks[keySlots],(void**)&originalKeyState[keySlots]))++keySlots;seen.push_back(table[12]);seen.push_back(table[13]);}
   reading->Release();
  }
 }
 if(mouseSlots&&keySlots){Log(("GameInput: input hooks installed ("+std::to_string(interfaces)+" interface versions, "+std::to_string(mouseSlots)+" mouse, "+std::to_string(keySlots)+" keyboard)").c_str());return true;}
 if(lastAttempt)Log(("GameInput: hooks incomplete ("+std::to_string(interfaces)+" interface versions, "+std::to_string(readings)+" readings, "+std::to_string(mouseSlots)+" mouse, "+std::to_string(keySlots)+" keyboard)").c_str());
 return false;
}
DWORD WINAPI Worker(void*){
 HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)&Worker,&pinned);
 wchar_t path[32768]{};GetModuleFileNameW(ownModule,path,32768);directory=path;directory=directory.substr(0,directory.find_last_of(L"\\/"));logFile.open(std::filesystem::path(directory+L"\\overlay.log"),std::ios::app);Log("Overlay component started; no gameplay adapters enabled");{uintptr_t gameBase{};uint32_t gameSize{};const bool validated=game::Supported(&gameBase,&gameSize);   // before any game hook: checks the loaded code too
 Log(validated?(std::string("Game build: ")+game::kGameBuilds[game::currentBuild].name+", code verified").c_str():"Game build: not a validated Minecraft build, game features stay off");}DecodeLogo();
 game::faultLog=Log;   // fault boundaries report from the first hook on, before the menu sets its own log
 auto eventName=L"Local\\NickelOverlay.Rendered."+std::to_wstring(GetCurrentProcessId());readyEvent=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str());
 WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=ownModule;wc.lpszClassName=L"NickelOverlayProbe";RegisterClassExW(&wc);auto window=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,16,16,nullptr,nullptr,ownModule,nullptr);
 if(!window||MH_Initialize()!=MH_OK){Log("Could not initialize render hooks");if(window)DestroyWindow(window);return 1;}
 game::InstallMaterialHook(Log); // must be installed before EditorHighlight.material.bin is first read
 std::vector<void*> seen;bool hooked=false;ComPtr<IDXGISwapChain> swap11;ComPtr<ID3D11Device> probe11;ComPtr<ID3D11DeviceContext> probeContext;DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=2;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferDesc.Width=16;desc.BufferDesc.Height=16;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
 if(SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&swap11,&probe11,nullptr,&probeContext)))hooked=HookSwap(swap11.Get(),0,seen);
 ComPtr<ID3D12Device> probe12;ComPtr<ID3D12CommandQueue> probeQueue;ComPtr<IDXGIFactory4> factory;ComPtr<IDXGISwapChain1> swap12;
 // Separate window for the DX12 probe: a window already owning the DX11 probe swap chain may refuse a second one.
 auto window12=CreateWindowExW(0,wc.lpszClassName,L"",WS_OVERLAPPEDWINDOW,0,0,16,16,nullptr,nullptr,ownModule,nullptr);
 if(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&probe12)))){D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;if(SUCCEEDED(probe12->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&probeQueue)))&&SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))){DXGI_SWAP_CHAIN_DESC1 dx{};dx.Width=16;dx.Height=16;dx.Format=DXGI_FORMAT_R8G8B8A8_UNORM;dx.SampleDesc.Count=1;dx.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;dx.BufferCount=2;dx.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;if(!AddHook((*(void***)probeQueue.Get())[10],(void*)ExecuteHook,(void**)&originalExecute))Log("DX12 probe: ExecuteCommandLists hook not installed");auto created=factory->CreateSwapChainForHwnd(probeQueue.Get(),window12?window12:window,&dx,nullptr,nullptr,&swap12);if(SUCCEEDED(created)){hooked=HookSwap(swap12.Get(),1,seen)||hooked;ComPtr<IDXGISwapChain> base12;if(SUCCEEDED(swap12.As(&base12))){auto words=(void**)base12.Get();for(size_t i=1;i<512;++i){if(!Readable(words+i))break;if(words[i]==(void*)probeQueue.Get()){queueOffset=i*sizeof(void*);break;}}GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)*(void**)probeQueue.Get(),&queueModule);Log(queueOffset?("DX12 probe: command queue pointer found inside the swap chain at offset "+std::to_string(queueOffset)).c_str():"DX12 probe: command queue pointer not found inside the swap chain");}}else Log(("DX12 probe swap chain not created, hresult "+std::to_string((unsigned long)created)).c_str());}else Log("DX12 probe: command queue or factory unavailable");}else Log("DX12 probe: device unavailable");
 if(hooked&&MH_EnableHook(MH_ALL_HOOKS)==MH_OK)Log("DXGI hooks installed; waiting for a game frame");else Log("Hook installation failed; renderer will not be marked ready");
 swap12.Reset();swap11.Reset();if(window12)DestroyWindow(window12);DestroyWindow(window);UnregisterClassW(wc.lpszClassName,ownModule);
 for(int attempt=0;attempt<40&&!InstallInputHooks(seen,attempt==39);++attempt)Sleep(500);
 return 0;
}
}
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){ownModule=module;DisableThreadLibraryCalls(module);auto thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);if(thread)CloseHandle(thread);}return TRUE;}
extern "C" __declspec(dllexport) BOOL NickelProbeOpenMenu(){if(!OwnProbeProcess())return FALSE;toggleRequested=true;return TRUE;}
extern "C" __declspec(dllexport) BOOL NickelProbeGetState(NickelProbeState* state){if(!OwnProbeProcess())return FALSE;if(!state||state->size!=sizeof(NickelProbeState)||state->version!=NickelProbeStateVersion)return FALSE;std::lock_guard<std::recursive_mutex> lock(renderMutex);if(context)ImGui::SetCurrentContext(context);state->rendererReady=rendererReady?TRUE:FALSE;state->menuOpen=overlay::MenuIsOpen()?TRUE:FALSE;state->submittedFrames=submittedFrames;return TRUE;}
extern "C" __declspec(dllexport) BOOL NickelProbeStop(){if(!OwnProbeProcess())return FALSE;MH_DisableHook(MH_ALL_HOOKS);std::lock_guard<std::recursive_mutex> lock(renderMutex);insidePresent=true;if(backend==Backend::DX12&&!Flush12()){insidePresent=false;return FALSE;}CleanupRenderer();{std::lock_guard<std::mutex> queueLock(queueMutex);observedQueue.Reset();}MH_Uninitialize();insidePresent=false;Log("Own-process probe shutdown completed");return TRUE;}

extern "C" __declspec(dllexport) BOOL NickelProbeMotionReady(){if(!OwnProbeProcess())return FALSE;std::lock_guard<std::recursive_mutex> lock(renderMutex);return motion.draws>0&&!motion.failed;}
