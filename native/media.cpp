// Spotify Manager: Windows media playback state through the WinRT
// GlobalSystemMediaTransportControlsSessionManager. Album art: Thumbnail -> OpenReadAsync -> DataReader (1..8 MB) ->
// WIC as 32-bit RGBA, at most 4096 px (downscaled to 512 px above 1024 for the texture); it is only reloaded when the
// title or artist changes. Everything runs on a separate thread so a slow or failing WinRT call never blocks rendering.
#include "media.hpp"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
namespace media{
namespace{
using namespace winrt::Windows::Media::Control;
std::mutex guard;State shared;std::chrono::steady_clock::time_point stamp;std::atomic<bool> started{false};std::atomic<int> pending{-1};std::atomic<double> pendingFraction{0};
double Seconds(winrt::Windows::Foundation::TimeSpan span){return std::chrono::duration<double>(span).count();}
std::vector<unsigned char> cover;int coverWidth=0,coverHeight=0;unsigned coverSerial=0,serials=0;
bool Decode(const std::vector<unsigned char>& bytes,std::vector<unsigned char>* out,int* width,int* height){
 using Microsoft::WRL::ComPtr;ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;UINT w=0,h=0;
 if(bytes.empty()||FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))||FAILED(factory->CreateStream(&stream))||FAILED(stream->InitializeFromMemory((BYTE*)bytes.data(),(DWORD)bytes.size()))||FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder))||FAILED(decoder->GetFrame(0,&frame))||FAILED(frame->GetSize(&w,&h))||!w||!h||w>4096||h>4096)return false;
 ComPtr<IWICBitmapSource> source=frame;if(w>1024||h>1024){ComPtr<IWICBitmapScaler> scaler;const UINT most=std::max(w,h),nw=std::max(1u,w*512/most),nh=std::max(1u,h*512/most);if(FAILED(factory->CreateBitmapScaler(&scaler))||FAILED(scaler->Initialize(frame.Get(),nw,nh,WICBitmapInterpolationModeFant)))return false;source=scaler;w=nw;h=nh;}
 if(FAILED(factory->CreateFormatConverter(&converter))||FAILED(converter->Initialize(source.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;
 out->resize((size_t)w*h*4);if(FAILED(converter->CopyPixels(nullptr,w*4,(UINT)out->size(),out->data())))return false;*width=(int)w;*height=(int)h;return true;
}
void ReadCover(const GlobalSystemMediaTransportControlsSessionMediaProperties& properties){
 std::vector<unsigned char> pixels;int w=0,h=0;bool ok=false;
 try{if(auto reference=properties.Thumbnail()){auto stream=reference.OpenReadAsync().get();const uint64_t size=stream.Size();if(size>=1&&size<=0x7fffff){winrt::Windows::Storage::Streams::DataReader reader(stream);reader.LoadAsync((uint32_t)size).get();std::vector<unsigned char> bytes((size_t)size);reader.ReadBytes(bytes);ok=Decode(bytes,&pixels,&w,&h);}}}catch(...){ok=false;}   // not a game address: size limit
 std::lock_guard<std::mutex> lock(guard);if(ok){cover.swap(pixels);coverWidth=w;coverHeight=h;coverSerial=++serials;}else{cover.clear();coverSerial=0;}
}
void Worker(){
 try{
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  auto manager=GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
  std::string shownTrack;
  for(;;){
   State next;
   try{
    if(auto session=manager.GetCurrentSession()){
     const int command=pending.exchange(-1);
     if(command==0)session.TryTogglePlayPauseAsync().get();else if(command==1)session.TrySkipPreviousAsync().get();else if(command==2)session.TrySkipNextAsync().get();
     auto timeline=session.GetTimelineProperties();const double start=Seconds(timeline.StartTime()),length=Seconds(timeline.EndTime())-start;
     if(command==3&&length>0)session.TryChangePlaybackPositionAsync((int64_t)((start+std::clamp(pendingFraction.load(),0.0,1.0)*length)*1e7)).get();
     auto info=session.GetPlaybackInfo();auto controls=info.Controls();auto properties=session.TryGetMediaPropertiesAsync().get();
     next.available=true;next.playing=info.PlaybackStatus()==GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
     next.canToggle=controls.IsPlayPauseToggleEnabled()||controls.IsPlayEnabled()||controls.IsPauseEnabled();next.canPrevious=controls.IsPreviousEnabled();next.canNext=controls.IsNextEnabled();next.canSeek=controls.IsPlaybackPositionEnabled();
     next.title=winrt::to_string(properties.Title());next.artist=winrt::to_string(properties.Artist());next.album=winrt::to_string(properties.AlbumTitle());next.duration=std::max(0.0,length);
     if(const std::string track=next.title+"\n"+next.artist;track!=shownTrack){shownTrack=track;ReadCover(properties);}
     timeline=session.GetTimelineProperties();next.position=Seconds(timeline.Position())-start;
     if(next.playing)next.position+=std::chrono::duration<double>(winrt::clock::now()-timeline.LastUpdatedTime()).count();   // Windows only refreshes the position occasionally
     next.position=std::clamp(next.position,0.0,std::max(0.0,next.duration));
    }
    else if(!shownTrack.empty()){shownTrack.clear();std::lock_guard<std::mutex> lock(guard);cover.clear();coverSerial=0;}
   }catch(...){next=State{};next.error=true;}
   {std::lock_guard<std::mutex> lock(guard);shared=next;stamp=std::chrono::steady_clock::now();}
   for(int i=0;i<10&&pending.load()<0;++i)std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
 }catch(...){std::lock_guard<std::mutex> lock(guard);shared=State{};shared.error=true;}
}
}
bool DecodeImage(const unsigned char* bytes,size_t size,std::vector<unsigned char>* rgba,int* width,int* height){const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);bool ok=false;{std::vector<unsigned char> copy(bytes,bytes+size);ok=Decode(copy,rgba,width,height);}if(SUCCEEDED(com))CoUninitialize();return ok;}
void Start(){if(!started.exchange(true))std::thread(Worker).detach();}
State Get(){std::lock_guard<std::mutex> lock(guard);State state=shared;if(state.available&&state.playing)state.position=std::min(state.duration>0?state.duration:1e12,state.position+std::chrono::duration<double>(std::chrono::steady_clock::now()-stamp).count());return state;}
void Command(int which,double fraction){pendingFraction=fraction;pending=which;}
unsigned Cover(std::vector<unsigned char>* rgba,int* width,int* height){std::lock_guard<std::mutex> lock(guard);if(!coverSerial)return 0;*rgba=cover;*width=coverWidth;*height=coverHeight;return coverSerial;}
}
