// Derived from QwenAudio/SenseVoice runtime/llama.cpp/funasr-sensevoice at
// runtime-llamacpp-v0.1.9 (MIT). YanFlow changes: persistent model lifetime and
// a versioned binary stdin/stdout protocol for 16 kHz PCM16 utterances.
//
// funasr-sensevoice: SenseVoiceSmall (SAN-M encoder + CTC) on ggml.
//   fbank.bin (T x 560) -> CMVN -> prepend 4 query tokens -> SAN-M encoder ->
//   CTC head -> greedy CTC decode -> token ids (stdout).
// The encoder is the same SAN-M arch as Fun-ASR-Nano (shared forward).
// Detokenize the printed ids with the SentencePiece bpe model (Python side for now).

#define NOMINMAX
#include "ggml.h"
#include "ggml-cpu.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "gguf.h"
#include "../asr-protocol.h"
#include "short-vad.h"

#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

static const float LN_EPS = 1e-5f;

// ---- audio loader: any wav/mp3/flac, any rate/channels -> 16k mono (miniaudio) ----
#define FUNASR_AUDIO_IMPLEMENTATION
#include "funasr_audio.h"
#include "funasr_vad.h"     // built-in FSMN-VAD front end (--vad segmentation)
#include <utility>
static const int FS=16000,WINLEN=400,SHIFT=160,NFFT=512,NMEL=80,LFR_M=7,LFR_N=6;
static const float PREEMPH=0.97f,LOWF=20.0f,HIGHF=8000.0f;
static inline float melf(float f){return 1127.0f*logf(1.0f+f/700.0f);}
static void fftc(std::vector<float>&re,std::vector<float>&im,int n){
  for(int i=1,j=0;i<n;i++){int b=n>>1;for(;j&b;b>>=1)j^=b;j^=b;if(i<j){std::swap(re[i],re[j]);std::swap(im[i],im[j]);}}
  for(int len=2;len<=n;len<<=1){double a=-2.0*M_PI/len;float wr=cosf(a),wi=sinf(a);
    for(int i=0;i<n;i+=len){float cr=1,ci=0;for(int k=0;k<len/2;k++){float ur=re[i+k],ui=im[i+k];
      float vr=re[i+k+len/2]*cr-im[i+k+len/2]*ci,vi=re[i+k+len/2]*ci+im[i+k+len/2]*cr;
      re[i+k]=ur+vr;im[i+k]=ui+vi;re[i+k+len/2]=ur-vr;im[i+k+len/2]=ui-vi;float nc=cr*wr-ci*wi;ci=cr*wi+ci*wr;cr=nc;}}}
}
static std::vector<float> compute_fbank(std::vector<float> wav,int&T_out){
  for(auto&v:wav)v*=32768.0f; std::vector<float> win(WINLEN);
  for(int i=0;i<WINLEN;i++)win[i]=0.54f-0.46f*cosf(2.0f*M_PI*i/(WINLEN-1));
  const int NBIN=NFFT/2+1; float bw=(float)FS/NFFT,ml=melf(LOWF),mh=melf(HIGHF),dm=(mh-ml)/(NMEL+1);
  std::vector<std::vector<float>> fb(NMEL,std::vector<float>(NBIN,0.0f));
  for(int m=0;m<NMEL;m++){float L=ml+m*dm,C=ml+(m+1)*dm,R=ml+(m+2)*dm;
    for(int k=0;k<NBIN;k++){float mf=melf(bw*k); if(mf>L&&mf<R)fb[m][k]=mf<=C?(mf-L)/(C-L):(R-mf)/(R-C);}}
  int N=wav.size(),T=(N-WINLEN)/SHIFT+1; std::vector<std::vector<float>> feat(T,std::vector<float>(NMEL));
  std::vector<float> re(NFFT),im(NFFT),fr(WINLEN); const float fl=1.1920929e-07f;
  for(int t=0;t<T;t++){const float*s=wav.data()+t*SHIFT; double mn=0; for(int i=0;i<WINLEN;i++)mn+=s[i]; mn/=WINLEN;
    for(int i=0;i<WINLEN;i++)fr[i]=s[i]-(float)mn; for(int i=WINLEN-1;i>0;i--)fr[i]-=PREEMPH*fr[i-1]; fr[0]-=PREEMPH*fr[0];
    for(int i=0;i<NFFT;i++){re[i]=i<WINLEN?fr[i]*win[i]:0.0f;im[i]=0.0f;} fftc(re,im,NFFT);
    for(int m=0;m<NMEL;m++){float e=0;for(int k=0;k<NBIN;k++)if(fb[m][k]>0)e+=fb[m][k]*(re[k]*re[k]+im[k]*im[k]); feat[t][m]=logf(e>fl?e:fl);}}
  const int pad=(LFR_M-1)/2; int Tl=(T+LFR_N-1)/LFR_N; std::vector<std::vector<float>> pd; pd.reserve(T+pad+LFR_M);
  for(int i=0;i<pad;i++)pd.push_back(feat[0]); for(int t=0;t<T;t++)pd.push_back(feat[t]);
  while((int)pd.size()<(Tl-1)*LFR_N+LFR_M)pd.push_back(feat[T-1]);
  int D=LFR_M*NMEL; std::vector<float> out((size_t)Tl*D);
  for(int i=0;i<Tl;i++)for(int j=0;j<LFR_M;j++)memcpy(&out[(size_t)i*D+j*NMEL],pd[i*LFR_N+j].data(),NMEL*sizeof(float));
  T_out=Tl; return out;
}

struct cfg { int d_model=512,n_head=4,num_blocks=50,tp_blocks=20,kernel=11,vocab=25055,blank=0; };
struct model { cfg c; ggml_context*ctx_w=nullptr; std::map<std::string,ggml_tensor*> t;
  ggml_tensor* g(const std::string&n){auto it=t.find(n);if(it==t.end()){fprintf(stderr,"missing %s\n",n.c_str());exit(1);}return it->second;} };

static ggml_tensor* lin(ggml_context*c,ggml_tensor*w,ggml_tensor*b,ggml_tensor*x){auto y=ggml_mul_mat(c,w,x);return b?ggml_add(c,y,b):y;}
static ggml_tensor* lnorm(ggml_context*c,ggml_tensor*x,ggml_tensor*g,ggml_tensor*b){return ggml_add(c,ggml_mul(c,ggml_norm(c,x,LN_EPS),g),b);}
static ggml_tensor* sanm_attn(ggml_context*c,model&m,const std::string&p,ggml_tensor*x,int T){
  const int D=m.c.d_model,H=m.c.n_head,dk=D/H,K=m.c.kernel;
  ggml_tensor*qkv=lin(c,m.g(p+"linear_q_k_v.weight"),m.g(p+"linear_q_k_v.bias"),x); size_t nb1=qkv->nb[1];
  ggml_tensor*q=ggml_cont(c,ggml_view_2d(c,qkv,D,T,nb1,0));
  ggml_tensor*k=ggml_cont(c,ggml_view_2d(c,qkv,D,T,nb1,(size_t)D*sizeof(float)));
  ggml_tensor*v=ggml_cont(c,ggml_view_2d(c,qkv,D,T,nb1,(size_t)2*D*sizeof(float)));
  const int pad=(K-1)/2; ggml_tensor*fk=m.g(p+"fsmn_block.weight");
  ggml_tensor*vp=ggml_pad_ext(c,v,0,0,pad,pad,0,0,0,0); ggml_tensor*fsmn=v;
  for(int j=0;j<K;j++){auto sl=ggml_view_2d(c,vp,D,T,vp->nb[1],(size_t)j*vp->nb[1]);
    auto wj=ggml_view_1d(c,fk,D,(size_t)j*fk->nb[1]); fsmn=ggml_add(c,fsmn,ggml_mul(c,ggml_cont(c,sl),wj));}
  q=ggml_permute(c,ggml_reshape_3d(c,q,dk,H,T),0,2,1,3); k=ggml_permute(c,ggml_reshape_3d(c,k,dk,H,T),0,2,1,3);
  ggml_tensor*vh=ggml_cont(c,ggml_permute(c,ggml_reshape_3d(c,v,dk,H,T),1,2,0,3));
  ggml_tensor*kq=ggml_soft_max(c,ggml_scale(c,ggml_mul_mat(c,k,q),1.0f/sqrtf((float)dk)));
  ggml_tensor*o=ggml_cont_2d(c,ggml_permute(c,ggml_mul_mat(c,vh,kq),0,2,1,3),D,T);
  return ggml_add(c,lin(c,m.g(p+"linear_out.weight"),m.g(p+"linear_out.bias"),o),fsmn);
}
static ggml_tensor* sanm_layer(ggml_context*c,model&m,const std::string&p,ggml_tensor*x,int T,bool res){
  auto r=x; auto h=lnorm(c,x,m.g(p+"norm1.weight"),m.g(p+"norm1.bias"));
  auto sa=sanm_attn(c,m,p+"self_attn.",h,T); x=res?ggml_add(c,r,sa):sa; r=x;
  h=lnorm(c,x,m.g(p+"norm2.weight"),m.g(p+"norm2.bias"));
  h=lin(c,m.g(p+"feed_forward.w_1.weight"),m.g(p+"feed_forward.w_1.bias"),h); h=ggml_relu(c,h);
  h=lin(c,m.g(p+"feed_forward.w_2.weight"),m.g(p+"feed_forward.w_2.bias"),h); return ggml_add(c,r,h);
}
static void add_posenc(std::vector<float>&x,int T,int depth){
  double inc=log(10000.0)/(depth/2.0-1.0);
  for(int t=0;t<T;t++){double pos=t+1;for(int i=0;i<depth/2;i++){double its=exp(i*-inc),st=pos*its;
    x[(size_t)t*depth+i]+=(float)sin(st);x[(size_t)t*depth+depth/2+i]+=(float)cos(st);}}
}

// SenseVoice detok: sentencepiece pieces (no byte-fallback in this vocab) -> join,
// "▁"(U+2581)->space; meta tokens <|lang|>/<|emo|>/<|event|>/<|itn|> dropped unless --keep-tags.
static std::string sv_trim(const std::string&s){size_t a=s.find_first_not_of(' ');if(a==std::string::npos)return "";size_t b=s.find_last_not_of(' ');return s.substr(a,b-a+1);}
static std::string detok_sv(const std::vector<int>&ids,const std::vector<std::string>&vocab,bool keep_tags){
  std::string s; for(int id:ids){ if(id<0||id>=(int)vocab.size())continue; const std::string&p=vocab[id];
    if(!keep_tags && p.size()>=2 && p[0]=='<' && p[1]=='|') continue;   // skip <|...|> meta
    s+=p; }
  const std::string lb="\xe2\x96\x81"; size_t pp; while((pp=s.find(lb))!=std::string::npos)s.replace(pp,3," ");
  return sv_trim(s);
}

namespace {
constexpr uint32_t REQUEST_MAGIC = 0x31514659;  // YFQ1, little endian
constexpr uint32_t RESPONSE_MAGIC = 0x31524659; // YFR1
constexpr uint32_t READY_MAGIC = 0x31574659;    // YFW1
constexpr uint32_t PROTOCOL_VERSION = yanflow::kAsrProtocolVersion;
constexpr uint32_t REQUEST_USE_VAD = 1;
constexpr uint32_t MAX_SAMPLES = FS * 60;

struct request_header { uint32_t magic; uint32_t samples; uint32_t flags; };
struct ready_header { uint32_t magic; uint32_t version; uint32_t status; };
struct timed_token { uint32_t begin, end; std::string piece; };

static bool read_exact(void* destination,size_t bytes){
  return bytes==0 || fread(destination,1,bytes,stdin)==bytes;
}
static bool write_exact(const void* source,size_t bytes){
  if(bytes!=0 && fwrite(source,1,bytes,stdout)!=bytes)return false;
  return fflush(stdout)==0;
}
static bool write_response(uint32_t status,const std::string& text,uint64_t elapsed_us,
    const std::vector<timed_token>* tokens=nullptr){
  const uint32_t magic=RESPONSE_MAGIC,bytes=(uint32_t)text.size();
  if(!(write_exact(&magic,sizeof(magic))&&write_exact(&status,sizeof(status))&&
    write_exact(&bytes,sizeof(bytes))&&write_exact(&elapsed_us,sizeof(elapsed_us))&&
    write_exact(text.data(),text.size())))return false;
  if(tokens){
    const uint32_t count=(uint32_t)tokens->size();if(!write_exact(&count,sizeof(count)))return false;
    for(const auto& token:*tokens){
      yanflow::AsrTokenHeader header{token.begin,token.end,(uint32_t)token.piece.size()};
      if(!write_exact(&header,sizeof(header))||!write_exact(token.piece.data(),token.piece.size()))return false;
    }
  }
  return true;
}

class sensevoice_worker {
public:
  ~sensevoice_worker(){ if(backend_)ggml_backend_free(backend_); if(model_.ctx_w)ggml_free(model_.ctx_w); }

  bool load(const std::string& model_path,const std::string& vad_path,int threads){
    vad_path_=vad_path; threads_=threads;
    fprintf(stderr,"yanflow-worker: opening model\n");
    gguf_init_params gp={false,&model_.ctx_w}; gguf_context*gg=gguf_init_from_file(model_path.c_str(),gp);
    if(!gg){fprintf(stderr,"yanflow-worker: load gguf failed\n");return false;}
    fprintf(stderr,"yanflow-worker: reading metadata\n");
    auto rd=[&](const char*k,int d){int i=gguf_find_key(gg,k);return i<0?d:(int)gguf_get_val_u32(gg,i);};
    model_.c.d_model=rd("sv.output_size",512); model_.c.n_head=rd("sv.attention_heads",4);
    model_.c.num_blocks=rd("sv.num_blocks",50); model_.c.tp_blocks=rd("sv.tp_blocks",20);
    model_.c.kernel=rd("sv.kernel_size",11); model_.c.vocab=rd("sv.vocab_size",25055); model_.c.blank=rd("sv.blank_id",0);
    int qi=gguf_find_key(gg,"sv.query_tokens"); nq_=qi<0?0:(int)gguf_get_arr_n(gg,qi);
    qtok_.resize(nq_); for(int i=0;i<nq_;i++)qtok_[i]=((const int32_t*)gguf_get_arr_data(gg,qi))[i];
    int ki=gguf_find_key(gg,"sv.vocab"); if(ki>=0){int nv=gguf_get_arr_n(gg,ki);vocab_.resize(nv);
      for(int i=0;i<nv;i++){const char*s=gguf_get_arr_str(gg,ki,i);vocab_[i]=s?s:"";}}
    for(int i=0;i<gguf_get_n_tensors(gg);i++){const char*nm=gguf_get_tensor_name(gg,i);model_.t[nm]=ggml_get_tensor(model_.ctx_w,nm);}
    gguf_free(gg);
    fprintf(stderr,"yanflow-worker: tensors ready\n");
    if(vocab_.empty()){fprintf(stderr,"yanflow-worker: model has no embedded vocabulary\n");return false;}
    embedding_=(float*)model_.g("embed.weight")->data;
    fprintf(stderr,"yanflow-worker: initializing CPU backend\n");
    backend_=ggml_backend_cpu_init();
    if(!backend_)return false;
    ggml_backend_cpu_set_n_threads(backend_,threads_);
    return true;
  }

  bool transcribe(const std::vector<float>& wav,bool use_vad,std::string& output,
      std::vector<timed_token>& tokens,bool short_hold){
    output.clear();
    tokens.clear();
    std::vector<std::pair<int,int>> segments;
    if(use_vad){
      if(vad_path_.empty())return false;
      const bool ok=short_hold && wav.size()<=FS
        ? yanflow_short_vad_segments(vad_path_,wav,30000,segments,threads_)
        : funasr_vad_segments(vad_path_,wav,30000,segments,threads_);
      if(!ok)return false;
    }else{
      segments.push_back({0,(int)((int64_t)wav.size()*1000/FS)});
    }
    for(const auto& span:segments){
      int begin=(int)((int64_t)span.first*FS/1000),end=(int)((int64_t)span.second*FS/1000);
      begin=std::max(0,begin);end=std::min((int)wav.size(),end);
      if(end-begin<WINLEN)continue;
      std::vector<float> clip(wav.begin()+begin,wav.begin()+end);
      int frames=0;std::vector<float> features=compute_fbank(std::move(clip),frames);
      std::string text;std::vector<timed_token> part;
      if(!run_segment(features,frames,text,part,end-begin))return false;
      output+=text;
      for(auto& token:part){token.begin+=(uint32_t)begin;token.end+=(uint32_t)begin;tokens.push_back(std::move(token));}
    }
    return true;
  }

private:
  bool run_segment(const std::vector<float>& fb,int T,std::string& output,
      std::vector<timed_token>& tokens,int sample_count){
    const int F=560,D=model_.c.d_model,V=model_.c.vocab,N=nq_+T;
    std::vector<float> input((size_t)N*F);
    for(int i=0;i<nq_;i++)memcpy(&input[(size_t)i*F],&embedding_[(size_t)qtok_[i]*F],F*sizeof(float));
    memcpy(&input[(size_t)nq_*F],fb.data(),(size_t)T*F*sizeof(float));
    float scale=sqrtf((float)D);for(float&value:input)value*=scale;add_posenc(input,N,F);
    ggml_init_params cp={(size_t)256*1024*1024,nullptr,true};ggml_context*c=ggml_init(cp);
    if(!c)return false;
    ggml_tensor*x=ggml_new_tensor_2d(c,GGML_TYPE_F32,F,N);ggml_set_input(x);
    ggml_tensor*h=sanm_layer(c,model_,"encoder.encoders0.0.",x,N,false);
    for(int i=0;i<model_.c.num_blocks-1;i++)h=sanm_layer(c,model_,"encoder.encoders."+std::to_string(i)+".",h,N,true);
    h=lnorm(c,h,model_.g("encoder.after_norm.weight"),model_.g("encoder.after_norm.bias"));
    for(int i=0;i<model_.c.tp_blocks;i++)h=sanm_layer(c,model_,"encoder.tp_encoders."+std::to_string(i)+".",h,N,true);
    h=lnorm(c,h,model_.g("encoder.tp_norm.weight"),model_.g("encoder.tp_norm.bias"));
    ggml_tensor*logits=lin(c,model_.g("ctc.ctc_lo.weight"),model_.g("ctc.ctc_lo.bias"),h);ggml_set_output(logits);
    ggml_cgraph*graph=ggml_new_graph_custom(c,32768,false);ggml_build_forward_expand(graph,logits);
    ggml_gallocr_t allocator=ggml_gallocr_new(ggml_backend_cpu_buffer_type());ggml_gallocr_alloc_graph(allocator,graph);
    ggml_backend_tensor_set(x,input.data(),0,ggml_nbytes(x));
    bool ok=ggml_backend_graph_compute(backend_,graph)==GGML_STATUS_SUCCESS;
    std::vector<int> ids;
    if(ok){std::vector<float> values((size_t)V*N);ggml_backend_tensor_get(logits,values.data(),0,ggml_nbytes(logits));int previous=-1;
      for(int n=0;n<N;n++){const float*column=&values[(size_t)n*V];int best_id=0;float best=column[0];
        for(int v=1;v<V;v++)if(column[v]>best){best=column[v];best_id=v;}
        if(best_id!=previous&&best_id!=model_.c.blank){
          ids.push_back(best_id);
          if(best_id>=0&&best_id<(int)vocab_.size()){
            const auto& piece=vocab_[best_id];
            if(!(piece.size()>=2&&piece[0]=='<'&&piece[1]=='|')){
              const int begin=std::clamp((n-nq_)*LFR_N*SHIFT,0,sample_count);
              const int end=std::clamp((n-nq_+1)*LFR_N*SHIFT,begin,sample_count);
              tokens.push_back({(uint32_t)begin,(uint32_t)end,piece});
            }
          }
        }else if(best_id==previous&&best_id!=model_.c.blank&&best_id>=0&&best_id<(int)vocab_.size()&&!tokens.empty()&&tokens.back().piece==vocab_[best_id]){
          tokens.back().end=(uint32_t)std::clamp((n-nq_+1)*LFR_N*SHIFT,(int)tokens.back().begin,sample_count);
        }
        previous=best_id;}}
    ggml_gallocr_free(allocator);ggml_free(c);
    if(!ok)return false;
    output=detok_sv(ids,vocab_,false);return true;
  }

  model model_;
  ggml_backend_t backend_=nullptr;
  std::string vad_path_;
  int threads_=8;
  int nq_=0;
  std::vector<int> qtok_;
  std::vector<std::string> vocab_;
  float*embedding_=nullptr;
};
} // namespace

static int worker_main(int argc,char**argv){
  setvbuf(stderr,nullptr,_IONBF,0);
  ggml_time_init();
#ifdef _WIN32
  _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
#endif
  std::string model_path,vad_path;int threads=8;
  for(int i=1;i<argc;i++){
    if(!strcmp(argv[i],"-m")&&i+1<argc)model_path=argv[++i];
    else if(!strcmp(argv[i],"--vad")&&i+1<argc)vad_path=argv[++i];
    else if(!strcmp(argv[i],"--threads")&&i+1<argc)threads=std::max(1,atoi(argv[++i]));
    else{fprintf(stderr,"usage: %s -m sensevoice.gguf --vad fsmn-vad.gguf [--threads N]\n",argv[0]);return 2;}
  }
  if(model_path.empty()||vad_path.empty())return 2;
  sensevoice_worker worker;
  int64_t load_started=ggml_time_us();
  if(!worker.load(model_path,vad_path,threads))return 3;
  fprintf(stderr,"yanflow-worker: ready in %.2fs\n",(ggml_time_us()-load_started)/1e6);
  ready_header ready={READY_MAGIC,PROTOCOL_VERSION,0};if(!write_exact(&ready,sizeof(ready)))return 4;
  for(;;){
    request_header request={};if(!read_exact(&request,sizeof(request)))break;
    if(request.magic!=REQUEST_MAGIC||request.samples>MAX_SAMPLES)return 5;
    fprintf(stderr,"yanflow-worker: request samples=%u flags=%u\n",request.samples,request.flags);
    std::vector<int16_t> pcm(request.samples);if(!read_exact(pcm.data(),pcm.size()*sizeof(int16_t)))return 6;
    fprintf(stderr,"yanflow-worker: request body ready\n");
    std::vector<float> wav(pcm.size());for(size_t i=0;i<pcm.size();i++)wav[i]=(float)pcm[i]/32768.0f;
    if(request.flags & ~(yanflow::kAsrUseVad|yanflow::kAsrTimedTokens|yanflow::kAsrShortHold))return 5;
    if((request.flags&yanflow::kAsrShortHold)&&!(request.flags&REQUEST_USE_VAD))return 5;
    int64_t started=ggml_time_us();std::string text;std::vector<timed_token> tokens;
    bool ok=worker.transcribe(wav,(request.flags&REQUEST_USE_VAD)!=0,text,tokens,(request.flags&yanflow::kAsrShortHold)!=0);
    if(!write_response(ok?0u:1u,text,(uint64_t)(ggml_time_us()-started),
        (request.flags&yanflow::kAsrTimedTokens)?&tokens:nullptr))return 7;
  }
  return 0;
}

#ifdef _WIN32
// ggml opens UTF-8 paths with _wfopen. The narrow CRT argv uses the active code
// page, so obtain UTF-16 arguments directly and explicitly encode them as UTF-8.
int wmain(int argc,wchar_t**argv){
  std::vector<std::string> utf8_arguments;
  utf8_arguments.reserve(argc);
  for(int i=0;i<argc;i++){
    int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
    if(bytes==0)return 2;
    std::string argument(bytes,'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,argument.data(),bytes,nullptr,nullptr)==0)return 2;
    argument.pop_back();
    utf8_arguments.push_back(std::move(argument));
  }
  std::vector<char*> arguments;
  arguments.reserve(argc);
  for(auto&argument:utf8_arguments)arguments.push_back(argument.data());
  return worker_main(argc,arguments.data());
}
#else
int main(int argc,char**argv){return worker_main(argc,argv);}
#endif
