#include <d3d12sdklayers.h>
// Own-process renderer lifecycle check. Never opens or modifies Minecraft.
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <wincodec.h>
#include "probe_api.hpp"
#include <cstdio>
#include <string>
#include <fstream>
using Microsoft::WRL::ComPtr;
namespace {
struct Graphics {
 bool dx12=false;
 ComPtr<IDXGISwapChain> swap;
 ComPtr<ID3D11Device> device11;
 ComPtr<ID3D11DeviceContext> context11;
 ComPtr<ID3D11RenderTargetView> target11;
 ComPtr<ID3D12Device> device12;
 ComPtr<ID3D12CommandQueue> queue;
 ComPtr<IDXGISwapChain3> chain;
 ComPtr<ID3D12DescriptorHeap> heap;
 ComPtr<ID3D12Resource> buffers[2];
 ComPtr<ID3D12CommandAllocator> allocator;
 ComPtr<ID3D12GraphicsCommandList> command;
 ComPtr<ID3D12Fence> fence;
 HANDLE event{};
 UINT64 fenceValue=0;
};
// Export our own renderer's backbuffer, not a desktop or third-party window.
bool SaveOwnFrame(Graphics& g){
 if(g.dx12)return false;ComPtr<ID3D11Resource> resource;g.target11->GetResource(&resource);ComPtr<ID3D11Texture2D> source;if(FAILED(resource.As(&source)))return false;
 D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;ComPtr<ID3D11Texture2D> staging;if(FAILED(g.device11->CreateTexture2D(&desc,nullptr,&staging)))return false;g.context11->CopyResource(staging.Get(),source.Get());D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(g.context11->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)))return false;
 HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool ok=false;{ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmap> bitmap;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IWICFormatConverter> convert;
 if(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->CreateBitmapFromMemory(desc.Width,desc.Height,GUID_WICPixelFormat32bppRGBA,mapped.RowPitch,mapped.RowPitch*desc.Height,(BYTE*)mapped.pData,&bitmap))&&SUCCEEDED(factory->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromFilename(L"menu-review.png",GENERIC_WRITE))&&SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))&&SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))&&SUCCEEDED(frame->SetSize(desc.Width,desc.Height))){auto format=GUID_WICPixelFormat32bppBGRA;if(SUCCEEDED(frame->SetPixelFormat(&format))&&SUCCEEDED(factory->CreateFormatConverter(&convert))&&SUCCEEDED(convert->Initialize(bitmap.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))&&SUCCEEDED(frame->WriteSource(convert.Get(),nullptr))&&SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit()))ok=true;}}
 if(SUCCEEDED(com))CoUninitialize();g.context11->Unmap(staging.Get(),0);return ok;
}
bool Present(Graphics& g){
 MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
 const float color[4]={.07f,.06f,.09f,1};
 if(!g.dx12){
  auto target=g.target11.Get();g.context11->OMSetRenderTargets(1,&target,nullptr);g.context11->ClearRenderTargetView(target,color);
 }else{
  auto index=g.chain->GetCurrentBackBufferIndex();if(index>=2)return true;
  if(FAILED(g.allocator->Reset())||FAILED(g.command->Reset(g.allocator.Get(),nullptr)))return false;
  D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=g.buffers[index].Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;
  g.command->ResourceBarrier(1,&barrier);auto cpu=g.heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=index*g.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  g.command->ClearRenderTargetView(cpu,color,0,nullptr);
  barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
  g.command->ResourceBarrier(1,&barrier);if(FAILED(g.command->Close()))return false;
  ID3D12CommandList* lists[]={g.command.Get()};g.queue->ExecuteCommandLists(1,lists);
  if(FAILED(g.queue->Signal(g.fence.Get(),++g.fenceValue)))return false;
  if(g.fence->GetCompletedValue()<g.fenceValue&&(FAILED(g.fence->SetEventOnCompletion(g.fenceValue,g.event))||WaitForSingleObject(g.event,3000)!=WAIT_OBJECT_0))return false;
 }
 return SUCCEEDED(g.swap->Present(1,0));
}
bool GpuIdle(Graphics& g){
 ++g.fenceValue;
 if(FAILED(g.queue->Signal(g.fence.Get(),g.fenceValue)))return false;
 if(g.fence->GetCompletedValue()>=g.fenceValue)return true;
 if(FAILED(g.fence->SetEventOnCompletion(g.fenceValue,g.event)))return false;
 return WaitForSingleObject(g.event,3000)==WAIT_OBJECT_0;
}
bool ReleaseTargets(Graphics& g){
 if(!g.dx12){g.context11->ClearState();g.target11.Reset();return true;}
 if(!GpuIdle(g))return false;
 g.command.Reset();g.allocator.Reset();g.heap.Reset();
 for(auto& buffer:g.buffers)buffer.Reset();
 return true;
}
bool CreateTargets(Graphics& g){
 if(!g.dx12){
  ComPtr<ID3D11Texture2D> buffer;
  if(FAILED(g.swap->GetBuffer(0,IID_PPV_ARGS(&buffer))))return false;
  return SUCCEEDED(g.device11->CreateRenderTargetView(buffer.Get(),nullptr,&g.target11));
 }
 D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=2;
 if(FAILED(g.device12->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&g.heap))))return false;
 auto cpu=g.heap->GetCPUDescriptorHandleForHeapStart();
 for(int i=0;i<2;++i){if(FAILED(g.swap->GetBuffer(i,IID_PPV_ARGS(&g.buffers[i]))))return false;g.device12->CreateRenderTargetView(g.buffers[i].Get(),nullptr,cpu);cpu.ptr+=g.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);}
 if(FAILED(g.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&g.allocator))))return false;
 if(FAILED(g.device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,g.allocator.Get(),nullptr,IID_PPV_ARGS(&g.command))))return false;
 return SUCCEEDED(g.command->Close());
}
bool InitDx11(Graphics& g,HWND hwnd){
 DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=2;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=hwnd;desc.SampleDesc.Count=1;desc.Windowed=TRUE;
 if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&g.swap,&g.device11,nullptr,&g.context11)))return false;
 return CreateTargets(g);
}
bool InitDx12(Graphics& g,HWND hwnd){
 if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&g.device12))))return false;
 D3D12_COMMAND_QUEUE_DESC queue{};if(FAILED(g.device12->CreateCommandQueue(&queue,IID_PPV_ARGS(&g.queue))))return false;
 ComPtr<IDXGIFactory4> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return false;
 DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=1104;desc.Height=641;desc.BufferCount=2;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
 ComPtr<IDXGISwapChain1> first;if(FAILED(factory->CreateSwapChainForHwnd(g.queue.Get(),hwnd,&desc,nullptr,nullptr,&first)))return false;
 if(FAILED(first.As(&g.swap))||FAILED(first.As(&g.chain))||!CreateTargets(g))return false;
 if(FAILED(g.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&g.fence))))return false;
 g.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
 return g.event!=nullptr;
}
using StateFn=BOOL(WINAPI*)(NickelProbeState*);
struct Check {
 HWND hwnd{};bool dx12=false,visible=false,review=false,motion=false,motionVerified=false;ULONGLONG started=0;
 Graphics g;HMODULE dll{};HANDLE ready{};StateFn getState{};
 std::wstring readyName;
 unsigned presents=0;ULONGLONG framesBefore=0,framesAfter=0;HRESULT resizeResult=E_FAIL;
 bool observed=false,opened=false,drawn=false,resized=false,clearedAfterResize=false,reinitialized=false,reopened=false,framesAdvanced=false,menuReopened=false,stopped=false,clearedAfterStop=false,gpuDrained=true;
 std::string failure;
 ~Check(){if(ready)CloseHandle(ready);if(g.event)CloseHandle(g.event);if(hwnd)DestroyWindow(hwnd);}
 DWORD budget()const{return review?180000:visible?60000:8000;}
 DWORD phase()const{return visible?20000:5000;}
 bool ReadyNow()const{return ready&&WaitForSingleObject(ready,0)==WAIT_OBJECT_0;}
 bool Frame(){if(!Present(g))return Fail("present","host frame submission or GPU wait failed");++presents;return true;}
 bool State(NickelProbeState& s){s=NickelProbeState{};s.size=sizeof(s);s.version=NickelProbeStateVersion;return getState&&getState(&s)!=FALSE;}
 bool Call(const char* name){if(!dll)return false;auto fn=(BOOL(*)())GetProcAddress(dll,name);return fn&&fn()!=FALSE;}
 bool Fail(const char* stage,const char* detail){if(failure.empty())failure=std::string(stage)+": "+detail;printf("FAIL %s - %s\n",stage,detail);fflush(stdout);return false;}
 void Mark(const char* stage,bool ok){if(ok){printf("PASS %s\n",stage);fflush(stdout);}}
 bool WaitReady(const char* stage,DWORD ms){
  auto deadline=GetTickCount64()+ms;
  while(GetTickCount64()<deadline){if(!Frame())return false;if(!ready)ready=OpenEventW(SYNCHRONIZE,FALSE,readyName.c_str());if(ReadyNow())return true;Sleep(10);}
  return Fail(stage,"ready event not signaled within budget");
 }
 bool WaitDrawn(){
  NickelProbeState s{};auto deadline=GetTickCount64()+phase();
  while(GetTickCount64()<deadline){if(!Frame())return false;if(State(s)&&s.rendererReady&&s.menuOpen&&s.submittedFrames>0){framesBefore=s.submittedFrames;return true;}Sleep(10);}
  return Fail("menu_drawn","menuOpen with submittedFrames>0 not observed within budget");
 }
 bool WaitAdvanced(){
  NickelProbeState s{};auto deadline=GetTickCount64()+phase();
  while(GetTickCount64()<deadline){if(!Frame())return false;if(State(s)){framesAfter=s.submittedFrames;if(s.menuOpen&&s.submittedFrames>framesBefore){framesAdvanced=menuReopened=true;return true;}}Sleep(10);}
  return Fail("frames_advanced","submittedFrames did not advance past the pre-resize count with the menu open");
 }
 // review: NICKEL_REVIEW_HOLD=ms keeps the menu up longer (1500 by default, at most 60000), e.g. for a Nickel AI answer
 // to arrive; the frame is saved 300 ms before the end (600 ms after the start by default)
 bool HoldVisible(){
  if(!visible)return true;
  DWORD hold=review?1500:10000,saveAt=600;{wchar_t text[16]{};if(review&&GetEnvironmentVariableW(L"NICKEL_REVIEW_HOLD",text,16)){const DWORD ms=(DWORD)_wtoi(text);if(ms>=1000){hold=ms>60000?60000:ms;saveAt=hold-300;}}}
  auto start=GetTickCount64();auto deadline=start+hold;bool saved=false;
  while(GetTickCount64()<deadline){if(!Frame())return false;if(review&&!saved&&GetTickCount64()-start>saveAt)saved=SaveOwnFrame(g);Sleep(10);}
  return true;
 }
 bool DoResize(){
  if(!ReleaseTargets(g))return Fail("resize_release","host backbuffer release failed");
  // DX12: ResizeBuffers1, the path Minecraft takes when its window regains focus.
  if(g.dx12){ComPtr<IDXGISwapChain3> chain3;UINT masks[2]{0,0};IUnknown* queues[2]{g.queue.Get(),g.queue.Get()};
   resizeResult=SUCCEEDED(g.swap->QueryInterface(IID_PPV_ARGS(&chain3)))?chain3->ResizeBuffers1(2,800,600,DXGI_FORMAT_UNKNOWN,0,masks,queues):E_NOINTERFACE;}
  else resizeResult=g.swap->ResizeBuffers(0,800,600,DXGI_FORMAT_UNKNOWN,0);
  resized=SUCCEEDED(resizeResult);
  if(!resized)return Fail("resize_buffers","ResizeBuffers returned an error");
  if(!CreateTargets(g))return Fail("resize_rebuild","host backbuffer rebuild failed");
  NickelProbeState s{};
  clearedAfterResize=!ReadyNow()&&(State(s)&&!s.rendererReady&&!s.menuOpen);
  if(!clearedAfterResize)return Fail("resize_state","ready event or renderer state still set before the next Present");
  return true;
 }
 bool Shutdown(){
  gpuDrained=!g.dx12||!g.queue.Get()||!g.fence.Get()||!g.event||GpuIdle(g);
  if(!gpuDrained)Fail("gpu_idle","host DX12 queue did not drain before stop");
  stopped=Call("NickelProbeStop");
  if(!stopped)return Fail("shutdown","NickelProbeStop rejected");
  NickelProbeState s{};
  clearedAfterStop=!ReadyNow()&&(State(s)&&!s.rendererReady&&!s.menuOpen);
  if(!clearedAfterStop)return Fail("shutdown_state","ready event or renderer state still set after stop");
  return true;
 }
 bool Passed()const{return observed&&opened&&drawn&&resized&&clearedAfterResize&&reinitialized&&framesAdvanced&&menuReopened&&stopped&&clearedAfterStop&&gpuDrained&&(!motion||motionVerified);}
 int Run(){
  observed=WaitReady("ready",budget());Mark("ready",observed);
  if(observed){
   opened=Call("NickelProbeOpenMenu");if(!opened)Fail("open_menu","NickelProbeOpenMenu rejected");Mark("open_menu",opened);
   if(opened){drawn=WaitDrawn()&&HoldVisible();Mark("menu_drawn",drawn);}
  }
  if(drawn&&DoResize()){
   Mark("resize_buffers",true);
   reinitialized=WaitReady("reinitialize",phase());Mark("reinitialize",reinitialized);
   if(reinitialized){NickelProbeState s;reinitialized=State(s)&&s.rendererReady!=FALSE;if(!reinitialized)Fail("reinitialize","state.rendererReady false after the ready event");}
   if(reinitialized){
    reopened=Call("NickelProbeOpenMenu");if(!reopened)Fail("reopen_menu","NickelProbeOpenMenu rejected after resize");Mark("reopen_menu",reopened);
    if(reopened){bool advanced=WaitAdvanced();framesAdvanced=advanced;Mark("frames_advanced",advanced);}
   }
  }
  if(motion){motionVerified=Call("NickelProbeMotionReady");Mark("motion_pass_after_resize",motionVerified);if(!motionVerified)Fail("motion_blur","Motion blur shader did not draw after resize");}
  Shutdown();
  return Passed()?0:1;
 }
};
void Report(const Check& c,const char* path){
 char hr[16];snprintf(hr,sizeof(hr),"0x%08lX",(unsigned long)c.resizeResult);
 auto flag=[](bool v){return v?"true":"false";};
 std::ofstream out(path,std::ios::trunc);
 out<<"{\n";
 out<<" \"status\":\""<<(c.Passed()?"passed":"failed")<<"\",\n";
 out<<" \"scope\":\"own_process_only\",\n";
 out<<" \"backend\":\""<<(c.dx12?"dx12":"dx11")<<"\",\n";
 out<<" \"renderer_ready\":"<<flag(c.observed)<<",\n";
 out<<" \"menu_open_requested\":"<<flag(c.opened)<<",\n";
 out<<" \"menu_drawn\":"<<flag(c.drawn)<<",\n";
 out<<" \"submitted_frames_before_resize\":"<<c.framesBefore<<",\n";
 out<<" \"submitted_frames_after_resize\":"<<c.framesAfter<<",\n";
 out<<" \"resize_buffers_result\":\""<<hr<<"\",\n";
 out<<" \"ready_event_cleared_after_resize\":"<<flag(c.clearedAfterResize)<<",\n";
 out<<" \"renderer_reinitialized\":"<<flag(c.reinitialized)<<",\n";
 out<<" \"menu_open_requested_after_resize\":"<<flag(c.reopened)<<",\n";
 out<<" \"frames_advanced\":"<<flag(c.framesAdvanced)<<",\n";
 out<<" \"menu_reopened\":"<<flag(c.menuReopened)<<",\n";
 out<<" \"shutdown_completed\":"<<flag(c.stopped)<<",\n";
 out<<" \"state_clear_after_stop\":"<<flag(c.clearedAfterStop)<<",\n";
 out<<" \"presents\":"<<c.presents<<",\n";
 out<<" \"duration_ms\":"<<(unsigned long long)(GetTickCount64()-c.started)<<",\n";
 out<<" \"failure\":\""<<c.failure<<"\"\n";
 out<<"}\n";
 out.flush();
}
}
int wmain(int argc,wchar_t** argv){
 Check c;c.started=GetTickCount64();
 for(int i=1;i<argc;++i){auto arg=std::wstring(argv[i]);if(arg==L"--motion"){c.motion=true;SetEnvironmentVariableW(L"NICKEL_REVIEW_MOTION",L"1");}else if(arg==L"--dx12")c.dx12=true;else if(arg==L"--visible")c.visible=true;else if(arg==L"--review"){c.visible=true;c.review=true;}}
 c.g.dx12=c.dx12;c.readyName=L"Local\\NickelOverlay.Rendered."+std::to_wstring(GetCurrentProcessId());
 auto own=GetModuleHandleW(nullptr);
 WNDCLASSEXW wc{sizeof(wc)};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=own;wc.lpszClassName=L"NickelOwnProbe";RegisterClassExW(&wc);
 // review: NICKEL_REVIEW_SIZE=WxH sets the client area (the game window size to check), e.g. 1280x720
 RECT frame{0,0,c.review?1480:1120,680};bool exact=false;{wchar_t size[32]{};int w=0,h=0;if(c.review&&GetEnvironmentVariableW(L"NICKEL_REVIEW_SIZE",size,32)&&swscanf_s(size,L"%dx%d",&w,&h)==2&&w>=320&&h>=240){frame={0,0,w,h};AdjustWindowRect(&frame,WS_OVERLAPPEDWINDOW,FALSE);exact=true;}}
 c.hwnd=CreateWindowExW(0,wc.lpszClassName,L"Nickel overlay renderer check",WS_OVERLAPPEDWINDOW,exact?0:100,exact?0:100,frame.right-frame.left,frame.bottom-frame.top,nullptr,nullptr,own,nullptr);
 int rc=2;
 if(c.hwnd){
  if(c.visible)ShowWindow(c.hwnd,SW_SHOWNOACTIVATE);   // shown without taking the keyboard from the player's window
  if(c.motion&&c.dx12){ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();}
  bool gfx=c.dx12?InitDx12(c.g,c.hwnd):InitDx11(c.g,c.hwnd);
  c.dll=LoadLibraryW(L"Nickel.Overlay.dll");
  if(c.dll)c.getState=(StateFn)GetProcAddress(c.dll,"NickelProbeGetState");
  if(gfx&&c.dll&&c.getState)rc=c.Run();
  else{
   if(!gfx)c.Fail("graphics","host device or swapchain creation failed");
   else if(!c.dll)c.Fail("load","Nickel.Overlay.dll not found beside the probe");
   else c.Fail("exports","NickelProbeGetState export missing");
   if(c.dll)c.Shutdown();
   rc=1;
  }
 }else c.Fail("window","probe window creation failed");
 Report(c,c.dx12?"probe-dx12.json":"probe-dx11.json");
 return rc;
}
