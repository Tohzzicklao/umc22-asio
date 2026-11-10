#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#pragma comment(lib,"ole32.lib")
static void fmt(WAVEFORMATEXTENSIBLE&w,int ch,int sr,int bits,int valid,bool flt){
 ZeroMemory(&w,sizeof w);w.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;w.Format.nChannels=ch;w.Format.nSamplesPerSec=sr;
 w.Format.wBitsPerSample=bits;w.Format.nBlockAlign=ch*bits/8;w.Format.nAvgBytesPerSec=sr*w.Format.nBlockAlign;w.Format.cbSize=22;
 w.Samples.wValidBitsPerSample=valid;w.dwChannelMask=ch==1?SPEAKER_FRONT_CENTER:(SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT);
 w.SubFormat=flt?KSDATAFORMAT_SUBTYPE_IEEE_FLOAT:KSDATAFORMAT_SUBTYPE_PCM;}
int main(){CoInitializeEx(0,COINIT_MULTITHREADED);IMMDeviceEnumerator*e;
 CoCreateInstance(__uuidof(MMDeviceEnumerator),0,CLSCTX_ALL,IID_PPV_ARGS(&e));
 for(int f=0;f<2;f++){IMMDeviceCollection*c;e->EnumAudioEndpoints(f?eCapture:eRender,DEVICE_STATE_ACTIVE,&c);UINT n;c->GetCount(&n);
 for(UINT i=0;i<n;i++){IMMDevice*d;c->Item(i,&d);IPropertyStore*p;d->OpenPropertyStore(STGM_READ,&p);PROPVARIANT v;PropVariantInit(&v);
  p->GetValue(PKEY_Device_FriendlyName,&v);wprintf(L"\n[%s] %s\n",f?L"IN":L"OUT",v.pwszVal);
  IAudioClient3*a;if(FAILED(d->Activate(__uuidof(IAudioClient3),CLSCTX_ALL,0,(void**)&a)))continue;
  WAVEFORMATEX*mix;a->GetMixFormat(&mix);printf("  mix: %d ch %d Hz %d bit\n",mix->nChannels,mix->nSamplesPerSec,mix->wBitsPerSample);
  UINT32 def,fun,mn,mx;if(SUCCEEDED(a->GetSharedModeEnginePeriod(mix,&def,&fun,&mn,&mx)))printf("  shared period frames: def %u fund %u min %u max %u\n",def,fun,mn,mx);
  REFERENCE_TIME dp,mp;a->GetDevicePeriod(&dp,&mp);printf("  device period: def %.2fms min %.2fms\n",dp/1e4,mp/1e4);
  int chs[]={1,2};int srs[]={44100,48000};struct{int b,v;bool f;}bs[]={{16,16,0},{24,24,0},{32,24,0},{32,32,0},{32,32,1}};
  for(int ch:chs)for(int sr:srs)for(auto b:bs){WAVEFORMATEXTENSIBLE w;fmt(w,ch,sr,b.b,b.v,b.f);
   if(a->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&w.Format,0)==S_OK)printf("  EXCL ok: %dch %d %d/%d%s\n",ch,sr,b.b,b.v,b.f?"f":"");}
 }}
}
