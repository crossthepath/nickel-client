// Subtitles: synced lyrics for the track currently playing (Spotify Manager).
// Included by menu.cpp inside its anonymous namespace (after Place / Label / Find / Flag; winhttp.h is included at the top).
// Fetching (WinHTTP, user agent "Nickel", lrclib.net:443): GET /api/get?track_name={}&artist_name={}[&album_name={}]
// [&duration={}], then /api/search?track_name={}&artist_name={} if nothing is found; the title is stripped of "(feat",
// "(with", "[feat". JSON: syncedLyrics, else plainLyrics; "instrumental" true = no lyrics.
// Display: current line + next line (Show Next Line), progressive karaoke highlight (Highlight Color) on the current
// line, Transition ms fade between lines, Offset ms shift. Sky mode places the text in the sky (Direction = yaw in
// degrees, Elevation = degrees above the horizon; Anchor FollowView makes Direction relative to the view), size Size;
// it is drawn as projected 2D over the game.
void (*lyricsLog)(const char*)=nullptr;   // log sink (bound to WriteLog by DrawSubtitles)
struct LyricLine{double at;std::string text;};
struct LyricsState{std::mutex guard;std::string wanted,loaded;std::vector<LyricLine> lines;bool busy=false,plain=false;std::string status;};LyricsState lyricsState;
std::wstring LyricsWide(const std::string& s){return Wide(s);}
std::string UrlEncode(const std::string& s){std::string out;char b[4];for(unsigned char c:s){if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=(char)c;else{snprintf(b,sizeof(b),"%%%02X",c);out+=b;}}return out;}
std::string CleanTitle(std::string t){for(const char* cut:{" (feat"," (with"," [feat"}){const size_t at=t.find(cut);if(at!=std::string::npos)t.resize(at);}return t;}
bool HttpGet(const std::wstring& path,std::string& body,DWORD& status){
 body.clear();status=0;HINTERNET s=WinHttpOpen(L"Nickel",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!s)return false;
 HINTERNET c=WinHttpConnect(s,L"lrclib.net",INTERNET_DEFAULT_HTTPS_PORT,0);HINTERNET r=c?WinHttpOpenRequest(c,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE):nullptr;bool ok=false;
 if(r&&WinHttpSendRequest(r,L"Lrclib-Client: Nickel\r\n",(DWORD)-1L,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(r,nullptr)){DWORD size=sizeof(status);WinHttpQueryHeaders(r,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX);
  char buf[8192];DWORD got=0;while(WinHttpReadData(r,buf,sizeof(buf),&got)&&got&&body.size()<4*1024*1024){body.append(buf,got);got=0;}ok=true;}
 if(r)WinHttpCloseHandle(r);if(c)WinHttpCloseHandle(c);WinHttpCloseHandle(s);return ok;}
// JSON value of a key (first occurrence): decoded string, or literal (true/false/null/number).
bool JsonValue(const std::string& j,size_t from,const char* key,std::string& out,bool* isString=nullptr){
 const std::string k=std::string("\"")+key+"\"";size_t at=j.find(k,from);if(at==std::string::npos)return false;at=j.find(':',at+k.size());if(at==std::string::npos)return false;++at;while(at<j.size()&&isspace((unsigned char)j[at]))++at;out.clear();
 if(at<j.size()&&j[at]=='"'){if(isString)*isString=true;for(++at;at<j.size()&&j[at]!='"';++at){char c=j[at];if(c=='\\'&&at+1<j.size()){c=j[++at];if(c=='n')out+='\n';else if(c=='t')out+='\t';else if(c=='r'){}else if(c=='u'&&at+4<j.size()){unsigned v=strtoul(j.substr(at+1,4).c_str(),nullptr,16);at+=4;
     if(v<0x80)out+=(char)v;else if(v<0x800){out+=(char)(0xc0|(v>>6));out+=(char)(0x80|(v&63));}else{out+=(char)(0xe0|(v>>12));out+=(char)(0x80|((v>>6)&63));out+=(char)(0x80|(v&63));}}else out+=c;}else out+=c;}return true;}
 if(isString)*isString=false;while(at<j.size()&&j[at]!=','&&j[at]!='}'&&j[at]!=']')out+=j[at++];while(!out.empty()&&isspace((unsigned char)out.back()))out.pop_back();return true;}
std::vector<LyricLine> ParseLrc(const std::string& text){std::vector<LyricLine> lines;size_t from=0;
 while(from<text.size()){size_t end=text.find('\n',from);if(end==std::string::npos)end=text.size();std::string line=text.substr(from,end-from);from=end+1;std::vector<double> stamps;size_t p=0;
  while(p<line.size()&&line[p]=='['){const size_t close=line.find(']',p);if(close==std::string::npos)break;int mm=0;double ss=0;if(sscanf_s(line.substr(p+1,close-p-1).c_str(),"%d:%lf",&mm,&ss)==2)stamps.push_back(mm*60+ss);p=close+1;}
  std::string words=line.substr(p);while(!words.empty()&&isspace((unsigned char)words.front()))words.erase(words.begin());for(double t:stamps)lines.push_back({t,words});}
 std::sort(lines.begin(),lines.end(),[](const LyricLine& a,const LyricLine& b){return a.at<b.at;});return lines;}
void FetchLyrics(std::string title,std::string artist,std::string album,double duration){
 std::string body;DWORD status=0;std::string path="/api/get?track_name="+UrlEncode(title)+"&artist_name="+UrlEncode(artist);if(!album.empty())path+="&album_name="+UrlEncode(album);if(duration>0)path+="&duration="+std::to_string((int)std::lround(duration));
 std::vector<LyricLine> lines;bool plain=false;std::string note="No lyrics found";
 auto take=[&](const std::string& j,size_t from){std::string v,inst;bool str=false;if(JsonValue(j,from,"instrumental",inst)&&inst=="true"){note="Instrumental";return true;}
  if(JsonValue(j,from,"syncedLyrics",v,&str)&&str&&!v.empty()){lines=ParseLrc(v);if(!lines.empty())return true;}
  if(JsonValue(j,from,"plainLyrics",v,&str)&&str&&!v.empty()){size_t f=0;while(f<v.size()){size_t e=v.find('\n',f);if(e==std::string::npos)e=v.size();lines.push_back({-1,v.substr(f,e-f)});f=e+1;}plain=true;return true;}return false;};
 // Order: exact /api/get, /api/get with the first artist only ("Daft Punk, Pharrell Williams" -> "Daft Punk"),
 // then /api/search with each; a SYNCED version is preferred over plain lyrics (web players often report a duration
 // or album that does not exactly match the LRCLIB entry).
 std::vector<LyricLine> best;bool bestPlain=false;int score=0;
 auto consider=[&](const std::string& j,size_t from){lines.clear();plain=false;if(!take(j,from)||lines.empty())return;const int sc=plain?1:2;if(sc>score){score=sc;best=lines;bestPlain=plain;}};
 std::string primary=artist;for(const char* cut:{", "," & "," et "," and "," feat"," x "," X "}){const size_t at=primary.find(cut);if(at!=std::string::npos&&at>0)primary.resize(at);}
 if(HttpGet(LyricsWide(path),body,status)&&status==200)consider(body,0);
 if(score<2&&primary!=artist){std::string p2="/api/get?track_name="+UrlEncode(title)+"&artist_name="+UrlEncode(primary);if(duration>0)p2+="&duration="+std::to_string((int)std::lround(duration));if(HttpGet(LyricsWide(p2),body,status)&&status==200)consider(body,0);}
 for(const std::string& who:{artist,primary}){if(score>=2)break;const std::string search="/api/search?track_name="+UrlEncode(title)+"&artist_name="+UrlEncode(who);
  if(HttpGet(LyricsWide(search),body,status)&&status==200){size_t at=0;for(int i=0;i<10&&score<2;++i){at=body.find("{\"id\"",at);if(at==std::string::npos)break;consider(body,at);++at;}}if(who==primary)break;}
 const bool found=score>0;lines=std::move(best);plain=bestPlain;
 if(lyricsLog){const std::string text="Subtitles: \""+title+"\" by "+artist+" -> "+(score==2?"synced lyrics, ":score==1?"plain lyrics only, ":"no lyrics, ")+std::to_string(lines.size())+" lines";lyricsLog(text.c_str());}
 std::lock_guard<std::mutex> lock(lyricsState.guard);lyricsState.lines=std::move(lines);lyricsState.plain=plain;lyricsState.busy=false;lyricsState.status=found?"":note;}
void DrawSubtitles(std::vector<std::string>& drawn){
 static Module* mod=ModuleById("subtitles");if(!mod||!mod->enabled)return;media::Start();const media::State st=media::Get();if(!st.available||st.title.empty())return;
 const std::string key=st.title+"\n"+st.artist;{std::lock_guard<std::mutex> lock(lyricsState.guard);
  if(lyricsState.wanted!=key&&!lyricsState.busy){lyricsState.wanted=key;lyricsState.busy=true;lyricsState.lines.clear();lyricsState.status="";std::thread(FetchLyrics,CleanTitle(st.title),st.artist,st.album,st.duration).detach();}}
 std::vector<LyricLine> lines;bool plain;{std::lock_guard<std::mutex> lock(lyricsState.guard);if(lyricsState.busy||lyricsState.wanted!=key)return;lines=lyricsState.lines;plain=lyricsState.plain;}if(lines.empty()||plain)return;
 auto num=[&](const char* id,float fallback){auto s=Find(*mod,id);return s?(float)s->number:fallback;};auto text=[&](const char* id){auto s=Find(*mod,id);return s?s->text:std::string();};
 const double t=st.position+num("offset",0)/1000.0;int cur=-1;for(int i=0;i<(int)lines.size();++i)if(lines[i].at<=t)cur=i;if(cur<0)return;
 const double start=lines[cur].at,next=cur+1<(int)lines.size()?lines[cur+1].at:start+5;const float progress=(float)std::clamp((t-start)/std::max(.05,next-start),0.0,1.0);
 const float transition=num("transition",350)/1000.f;const float fade=transition>0?(float)std::clamp((t-start)/transition,0.0,1.0):1.f;
 const ImU32 ink=ImGui::ColorConvertFloat4ToU32(ParseColor(text("text_color").empty()?"#ffffffff":text("text_color"))),glow=ImGui::ColorConvertFloat4ToU32(ParseColor(text("highlight_color").empty()?"#59d9ffff":text("highlight_color")));
 auto alpha=[](ImU32 c,float a){return (c&0x00ffffff)|((ImU32)std::clamp((float)((c>>24)&255)*a,0.f,255.f)<<24);};const bool karaoke=Flag(*mod,"karaoke_highlight",true),showNext=Flag(*mod,"show_next_line",true);
 const std::string mode=text("mode");auto* d=ImGui::GetBackgroundDrawList();const std::string& line=lines[cur].text;const std::string upcoming=cur+1<(int)lines.size()?lines[cur+1].text:std::string();
 auto drawLine=[&](ImVec2 center,float size,bool current){const std::string& s=current?line:upcoming;if(s.empty())return;const float w=TextWidth(semibold,size,s.c_str());const ImVec2 p(std::round(center.x-w/2),std::round(center.y));
  const ImU32 base=current?alpha(ink,fade):alpha(ink,.55f*fade);Label(d,semibold,size,ImVec2(p.x+1,p.y+1),s.c_str(),alpha(IM_COL32(0,0,0,255),.6f*fade));Label(d,semibold,size,p,s.c_str(),base);
  if(current&&karaoke){d->PushClipRect(ImVec2(p.x,p.y-size),ImVec2(p.x+w*progress,p.y+size*2),true);Label(d,semibold,size,p,s.c_str(),alpha(glow,fade));d->PopClipRect();}};
 if(mode!="Sky"){auto& place=Place("subtitles",ImGui::GetIO().DisplaySize.x/2-200,ImGui::GetIO().DisplaySize.y*.72f);const float k=place.scale,size=22*k;
  const float w=std::max(TextWidth(semibold,size,line.c_str()),TextWidth(semibold,size*.8f,upcoming.c_str()));const float boxW=std::max(400.f*k,w+20*k),boxH=size*(showNext?2.4f:1.4f);
  drawLine(ImVec2(place.x+boxW/2,place.y),size,true);if(showNext)drawLine(ImVec2(place.x+boxW/2,place.y+size*1.3f),size*.8f,false);place.lo=ImVec2(place.x,place.y);place.hi=ImVec2(place.x+boxW,place.y+boxH);drawn.push_back("subtitles");}
 if(mode=="Sky"||mode=="Both"){auto& view=game::View();float m[16],origin[3];uint64_t tick;AcquireSRWLockShared(&view.lock);memcpy(m,view.m,sizeof(m));memcpy(origin,view.origin,sizeof(origin));tick=view.tick;ReleaseSRWLockShared(&view.lock);InterlockedExchange(&view.wanted,1);
  if(!tick||GetTickCount64()-tick>250)return;float yaw=num("direction",0);if(text("anchor")!="Fixed"){float look{};if(game::ReadYaw(&look))yaw+=look;}const float pitch=num("elevation",22)*.0174532925f,y=yaw*.0174532925f,dist=100;
  const float p[3]={origin[0]-std::sin(y)*std::cos(pitch)*dist,origin[1]+std::sin(pitch)*dist,origin[2]+std::cos(y)*std::cos(pitch)*dist};nickel::visual::Point at{};
  const ImVec2 screen=ImGui::GetIO().DisplaySize;if(!nickel::visual::project(m,origin,p,screen.x,screen.y,&at))return;const float size=num("size",6)*screen.y/220.f;
  drawLine(ImVec2(at.x,at.y),size,true);if(showNext)drawLine(ImVec2(at.x,at.y+size*1.3f),size*.8f,false);}
}
