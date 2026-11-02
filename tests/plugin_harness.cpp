// End-to-end test: loads the built plugin like Mumble does, with a fake Mumble API and a fake Valheim
// server mod on a local TCP port. Linux only. Run via tests/run_tests.sh.
#include "MumblePlugin.h"
#include "Protocol.h"
#include "Settings.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace std::chrono_literals;
static int fails = 0;
#define CHECK(c, msg) do { bool _ok = (c); printf("%s %s\n", _ok ? "  ok  " : "  FAIL", msg); if(!_ok) fails++; } while(0)

static std::string g_desc; static std::vector<std::string> logs;
static const char* dupstr(const std::string&s){ return strdup(s.c_str()); }
static mumble_error_t f_log(mumble_plugin_id_t, const char*m){ printf("    [log] %s\n", m); logs.push_back(m); return MUMBLE_STATUS_OK; }
static mumble_error_t f_set(mumble_plugin_id_t, mumble_settings_key_t k, double*v){ *v = k==MUMBLE_SK_AUDIO_OUTPUT_PA_MINIMUM_DISTANCE?2: k==MUMBLE_SK_AUDIO_OUTPUT_PA_MAXIMUM_DISTANCE?60: 0.3; return MUMBLE_STATUS_OK; }
static mumble_error_t f_conn(mumble_plugin_id_t, mumble_connection_t*c){*c=1;return MUMBLE_STATUS_OK;}
static mumble_error_t f_sync(mumble_plugin_id_t, mumble_connection_t, bool*s){*s=true;return MUMBLE_STATUS_OK;}
static mumble_error_t f_self(mumble_plugin_id_t, mumble_connection_t, mumble_userid_t*u){*u=1;return MUMBLE_STATUS_OK;}
static mumble_error_t f_chan(mumble_plugin_id_t, mumble_connection_t, mumble_userid_t, mumble_channelid_t*c){*c=5;return MUMBLE_STATUS_OK;}
static mumble_error_t f_users(mumble_plugin_id_t, mumble_connection_t, mumble_channelid_t, mumble_userid_t**l, size_t*n){
  *l=(mumble_userid_t*)malloc(5*sizeof(mumble_userid_t)); for(int i=0;i<5;i++)(*l)[i]=i+1; *n=5; return MUMBLE_STATUS_OK;}
static mumble_error_t f_uname(mumble_plugin_id_t, mumble_connection_t, mumble_userid_t u, const char**n){
  const char* names[]={"","Ragnar","bjorn","Sigrid","xX_gamer_Xx","Nobody"}; *n=dupstr(names[u]); return MUMBLE_STATUS_OK;}
static mumble_error_t f_desc(mumble_plugin_id_t, mumble_connection_t, mumble_channelid_t ch, const char**d){
  *d = dupstr(ch==0 ? "<p>Welcome</p>" : g_desc); return MUMBLE_STATUS_OK;}
static mumble_error_t f_free(mumble_plugin_id_t, const void*p){free((void*)p);return MUMBLE_STATUS_OK;}
static std::vector<uint8_t> sent; static std::string sentId;
static mumble_error_t f_send(mumble_plugin_id_t, mumble_connection_t, const mumble_userid_t*,size_t,const uint8_t*d,size_t len,const char*id){ sent.assign(d,d+len); sentId=id; return MUMBLE_STATUS_OK;}
template<class T> T sym(void*h,const char*n){ void*p=dlsym(h,n); if(!p){printf("missing %s\n",n);exit(1);} return (T)p; }

// ---- fake Valheim server ----
static std::atomic<bool> srvStop{false}; static std::atomic<int> srvAuthed{0}, srvRejected{0};
static void fakeServer(int lfd, std::string key){
  while(!srvStop){
    fd_set fs; FD_ZERO(&fs); FD_SET(lfd,&fs); timeval tv{0,100000};
    if(select(lfd+1,&fs,0,0,&tv)<=0) continue;
    int c=accept(lfd,0,0); if(c<0) continue;
    uint8_t buf[1100]; int got=0; timeval rt{2,0}; setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&rt,sizeof rt);
    while(got<4){ int n=recv(c,buf+got,sizeof buf-got,0); if(n<=0)break; got+=n; }
    uint32_t len; memcpy(&len,buf,4); while(got<4+(int)len){ int n=recv(c,buf+got,sizeof buf-got,0); if(n<=0)break; got+=n; }
    vr::Reader r(buf+4,len); uint32_t m; uint16_t v; std::string k; r.get(m); r.get(v); r.str(k);
    if(m!=vr::kHelloMagic || k!=key){ srvRejected++; close(c); continue; }
    srvAuthed++;
    while(!srvStop){
      vr::Writer w; w.put(vr::kSnapshotMagic); w.put(vr::kVersion); w.put(uint16_t(0)); w.str("valheim-test");
      float rf[7]={30,60,0.035f,4,1,0.7f,4}; for(float x:rf) w.put(x); uint8_t fl[4]={1,1,1,0}; for(auto b:fl) w.put(b);
      struct P{const char*n; float x,y,z;}; P ps[]={{"Ragnar",10,31.6f,5},{"Bjorn",10,31.6f,10},{"Sigrid",10,31.6f,95},{"Astrid",-80,31.6f,5}};
      w.put(uint16_t(4)); float dir[3]={0,0,1}; for(auto&p:ps){ w.str(p.n); float pos[3]={p.x,p.y,p.z}; w.vec3(pos); w.vec3(dir);} 
      auto f=w.framed(); if(send(c,f.data(),f.size(),MSG_NOSIGNAL)<=0) break; std::this_thread::sleep_for(50ms);
    }
    close(c);
  }
}

static double gainDb(bool(*audio)(float*,uint32_t,uint16_t,uint32_t,bool,mumble_userid_t), mumble_userid_t u, bool*mod){
  double ein=0,eout=0; *mod=false;
  for(int b=0;b<150;b++){ std::vector<float> buf(480); for(int i=0;i<480;i++){ buf[i]=0.1f*sinf((b*480+i)*2*3.14159f*440/48000); ein+=buf[i]*buf[i]; }
    *mod|=audio(buf.data(),480,1,48000,true,u); for(float v:buf) eout+=v*v; }
  return 10*log10(eout/ein);
}

int main(int, char**argv){
  printf("settings parsing:\n");
  auto a=vr::parseAddress("play.example.com:47900/k3y"); CHECK(a && a->host=="play.example.com" && a->port==47900 && a->key=="k3y", "host:port/key");
  a=vr::parseAddress("10.0.0.5"); CHECK(a && a->port==vr::kDefaultPort && a->key.empty(), "host only -> default port");
  CHECK(!vr::parseAddress("host:99999"), "bad port rejected");
  a=vr::findChannelTag("<p>Valheim crew</p><p>valheim-radio=vh.example.org:47812/abc</p>"); CHECK(a && a->host=="vh.example.org" && a->key=="abc", "tag inside HTML");
  CHECK(!vr::findChannelTag("<p>nothing here</p>"), "no tag");
  auto ini=vr::parseIni("[server]\naddress = 1.2.3.4:5000 ; comment\nkey=abc\n[player]\nname = Ragnar the Red\n[radio]\nstart=10\nsquelch=off\n");
  CHECK(ini.server && ini.server->host=="1.2.3.4" && ini.server->port==5000 && ini.server->key=="abc", "ini server");
  CHECK(ini.characterName=="Ragnar the Red" && ini.radio.start && *ini.radio.start==10 && ini.radio.squelch && !*ini.radio.squelch, "ini player/radio");
  CHECK(!vr::parseIni(vr::parseIni("").characterName).server, "empty ini");

  int lfd=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
  sockaddr_in sa{}; sa.sin_family=AF_INET; sa.sin_addr.s_addr=htonl(INADDR_LOOPBACK); bind(lfd,(sockaddr*)&sa,sizeof sa); listen(lfd,4);
  socklen_t sl=sizeof sa; getsockname(lfd,(sockaddr*)&sa,&sl); int port=ntohs(sa.sin_port);
  std::thread srv(fakeServer,lfd,std::string("s3cret"));

  void*h=dlopen(argv[1],RTLD_NOW); if(!h){printf("%s\n",dlerror());return 1;}
  MumbleAPI api{}; api.log=f_log; api.getMumbleSetting_double=f_set; api.getActiveServerConnection=f_conn; api.isConnectionSynchronized=f_sync;
  api.getLocalUserID=f_self; api.getChannelOfUser=f_chan; api.getUsersInChannel=f_users; api.freeMemory=f_free; api.sendData=f_send;
  api.getUserName=f_uname; api.getChannelDescription=f_desc;
  g_desc = "<p>Our server. valheim-radio=127.0.0.1:" + std::to_string(port) + "/wrongkey</p>";
  sym<void(*)(void*)>(h,"mumble_registerAPIFunctions")(&api);
  auto initPD=sym<uint8_t(*)(const char*const*,const uint64_t*,size_t)>(h,"mumble_initPositionalData");
  auto fetch=sym<bool(*)(float*,float*,float*,float*,float*,float*,const char**,const char**)>(h,"mumble_fetchPositionalData");
  auto recvData=sym<bool(*)(mumble_connection_t,mumble_userid_t,const uint8_t*,size_t,const char*)>(h,"mumble_onReceiveData");
  auto audio=sym<bool(*)(float*,uint32_t,uint16_t,uint32_t,bool,mumble_userid_t)>(h,"mumble_onAudioSourceFetched");

  printf("plugin, wrong key in channel description:\n");
  sym<mumble_error_t(*)(mumble_plugin_id_t)>(h,"mumble_init")(7);
  std::this_thread::sleep_for(1500ms);
  CHECK(srvRejected>0 && srvAuthed==0, "server rejected wrong key");
  CHECK(initPD(0,0,0)==MUMBLE_PDEC_ERROR_TEMP, "not linked");

  printf("plugin, correct key (description edited while running):\n");
  g_desc = "<p>Our server. valheim-radio=127.0.0.1:" + std::to_string(port) + "/s3cret</p>";
  std::this_thread::sleep_for(4500ms);
  CHECK(srvAuthed==1, "connected with key");
  CHECK(initPD(0,0,0)==MUMBLE_PDEC_OK, "linked (Mumble name 'Ragnar' found in game)");
  float ap[3],ad[3],ax[3],cp[3],cd[3],cx[3]; const char*ctx,*id;
  bool ok=fetch(ap,ad,ax,cp,cd,cx,&ctx,&id);
  CHECK(ok && ap[0]==10 && ap[2]==5 && cp[1]==31.6f && ad[2]==1 && ax[1]==1 && std::string(ctx)=="valheim-test" && std::string(id)=="Ragnar", "positional data");

  bool mod; double g;
  g=gainDb(audio,2,&mod); CHECK(!mod && fabs(g)<0.01, "user 'bjorn' 5 m away (case-insensitive match): untouched");
  g=gainDb(audio,3,&mod); printf("        sigrid gain %.1f dB\n", g); CHECK(mod && g>3, "user 'Sigrid' 90 m away: radio + volume compensation");
  g=gainDb(audio,4,&mod); CHECK(!mod, "user 'xX_gamer_Xx' (no matching character): untouched");
  auto ann=vr::makeNameAnnouncement("Astrid"); CHECK(recvData(1,4,ann.data(),ann.size(),vr::kNameDataID), "name announcement accepted");
  g=gainDb(audio,4,&mod); CHECK(mod, "after announcing 'Astrid' (90 m away): radio");
  CHECK(!recvData(1,4,(const uint8_t*)"x",1,"other-plugin"), "foreign plugin data ignored");

  printf("server goes away:\n");
  srvStop=true; srv.join(); close(lfd);
  std::this_thread::sleep_for(3500ms);
  CHECK(initPD(0,0,0)==MUMBLE_PDEC_ERROR_TEMP && !fetch(ap,ad,ax,cp,cd,cx,&ctx,&id), "unlinked after snapshots stop");
  sym<void(*)()>(h,"mumble_shutdown")();
  printf("shutdown ok\n%s (%d failures)\n", fails? "FAILED":"ALL PASSED", fails);
  return fails!=0;
}
