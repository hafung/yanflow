// Derived from the pinned SenseVoice runtime-llamacpp-v0.1.9 FSMN-VAD (MIT).
// Uses the same neural graph and features, with a stricter short-hold decision.
// Pinned upstream sources/models are never patched.
#define NOMINMAX
#include <algorithm>
#include "funasr_vad.h"
#include "short-vad.h"

bool yanflow_short_vad_segments(const std::string& gguf_path, const std::vector<float>& wav,
                                int max_seg_ms, std::vector<std::pair<int,int>>& segs, int nthreads){
  (void)max_seg_ms;
  using namespace funasr_vad_impl;
  segs.clear();
  vad m; gguf_init_params ip={false,&m.ctx}; gguf_context*gg=gguf_init_from_file(gguf_path.c_str(),ip);
  if(!gg){fprintf(stderr,"vad: cannot load %s\n",gguf_path.c_str());return false;}
  auto rd=[&](const char*k,int d){int i=gguf_find_key(gg,k);return i<0?d:(int)gguf_get_val_u32(gg,i);};
  int idim=rd("vad.input_dim",400),pd=rd("vad.proj_dim",128),nl=rd("vad.fsmn_layers",4),lorder=rd("vad.lorder",20),
      od=rd("vad.output_dim",248),lm=rd("vad.lfr_m",5),ln=rd("vad.lfr_n",1);
  for(int i=0;i<gguf_get_n_tensors(gg);i++){const char*nm=gguf_get_tensor_name(gg,i);m.t[nm]=ggml_get_tensor(m.ctx,nm);}
  gguf_free(gg);

  // fail fast (not segfault) if the GGUF is missing tensors the graph dereferences
  auto need=[&](const std::string&n){ return m.g(n)!=nullptr; };
  bool ok_t = need("cmvn.shift")&&need("cmvn.scale")&&need("encoder.in_linear1.linear.weight")
            &&need("encoder.in_linear2.linear.weight")&&need("encoder.out_linear1.linear.weight")
            &&need("encoder.out_linear2.linear.weight");
  for(int i=0;i<nl&&ok_t;i++){std::string p="encoder.fsmn."+std::to_string(i)+".";
    ok_t=need(p+"linear.linear.weight")&&need(p+"fsmn_block.conv_left.weight")&&need(p+"affine.linear.weight");}
  if(!ok_t){fprintf(stderr,"vad: gguf missing required tensors\n"); if(m.ctx)ggml_free(m.ctx); return false;}

  auto feat=fbank80(wav); int T=0; auto feats=lfr(feat,lm,ln,T);   // [T,400]
  if(T<1){if(m.ctx)ggml_free(m.ctx);return true;}                  // too short -> no speech
  float*shift=(float*)m.g("cmvn.shift")->data,*scale=(float*)m.g("cmvn.scale")->data;
  for(int t=0;t<T;t++)for(int d=0;d<idim;d++)feats[(size_t)t*idim+d]=(feats[(size_t)t*idim+d]+shift[d])*scale[d];

  ggml_backend_t be=ggml_backend_cpu_init();
  if(!be){if(m.ctx)ggml_free(m.ctx);return false;}
  // no_alloc=true -> ctx holds only tensor/graph metadata (the real compute buffer is
  // allocated by gallocr below), so a few MB is plenty regardless of clip length.
  ggml_init_params cp={(size_t)16*1024*1024,nullptr,true}; ggml_context*c=ggml_init(cp);
  if(!c){ggml_backend_free(be);if(m.ctx)ggml_free(m.ctx);return false;}
  ggml_tensor*x=ggml_new_tensor_2d(c,GGML_TYPE_F32,idim,T); ggml_set_input(x);
  ggml_tensor*h=lin(c,m.g("encoder.in_linear1.linear.weight"),m.g("encoder.in_linear1.linear.bias"),x);
  h=lin(c,m.g("encoder.in_linear2.linear.weight"),m.g("encoder.in_linear2.linear.bias"),h); h=ggml_relu(c,h);
  for(int i=0;i<nl;i++){std::string p="encoder.fsmn."+std::to_string(i)+".";
    ggml_tensor*z=ggml_mul_mat(c,m.g(p+"linear.linear.weight"),h);
    ggml_tensor*fk=m.g(p+"fsmn_block.conv_left.weight"); ggml_tensor*zp=ggml_pad_ext(c,z,0,0,lorder-1,0,0,0,0,0); ggml_tensor*acc=z;
    // sl is a full-row slice of the contiguous padded tensor -> already contiguous, no ggml_cont needed
    for(int j=0;j<lorder;j++){auto sl=ggml_view_2d(c,zp,pd,T,zp->nb[1],(size_t)j*zp->nb[1]);auto wj=ggml_view_1d(c,fk,pd,(size_t)j*fk->nb[1]);acc=ggml_add(c,acc,ggml_mul(c,sl,wj));}
    ggml_tensor*a=lin(c,m.g(p+"affine.linear.weight"),m.g(p+"affine.linear.bias"),acc); h=ggml_relu(c,a);}
  h=lin(c,m.g("encoder.out_linear1.linear.weight"),m.g("encoder.out_linear1.linear.bias"),h);
  h=lin(c,m.g("encoder.out_linear2.linear.weight"),m.g("encoder.out_linear2.linear.bias"),h);
  h=ggml_soft_max(c,h); ggml_set_output(h);
  ggml_cgraph*gf=ggml_new_graph(c); ggml_build_forward_expand(gf,h);
  ggml_gallocr_t ga=ggml_gallocr_new(ggml_backend_cpu_buffer_type()); ggml_gallocr_alloc_graph(ga,gf);
  ggml_backend_tensor_set(x,feats.data(),0,ggml_nbytes(x)); ggml_backend_cpu_set_n_threads(be,nthreads);
  bool ok=ggml_backend_graph_compute(be,gf)==GGML_STATUS_SUCCESS;
  std::vector<float> sc((size_t)od*T); if(ok)ggml_backend_tensor_get(h,sc.data(),0,ggml_nbytes(h));
  ggml_gallocr_free(ga);ggml_free(c);ggml_backend_free(be);if(m.ctx)ggml_free(m.ctx);
  if(!ok)return false;

  // Explicit hold intent permits a shorter neural speech run, but requires
  // posterior >= 0.90 (the ordinary path uses >= 0.75). Eight consecutive
  // 10 ms frames reject isolated taps. No ASR inference on an unconfirmed clip.
  int start=-1, run=0;
  for(int t=0;t<=T;t++){
    const bool speech=t<T && std::isfinite(sc[(size_t)t*od]) && sc[(size_t)t*od]<=0.10f;
    if(speech){if(run==0)start=t;run++;}
    else{
      if(run>=8){
        const int begin=std::max(0,(start-6)*160);
        const int finish=std::min((int)wav.size(),(t+10)*160);
        segs.push_back({begin/16,finish/16});
      }
      run=0;
    }
  }
  if(!segs.empty()){
    // VAD confirms speech, but a short clip must retain unvoiced consonants
    // before/after the posterior run. Infer the original (at most 1 s) clip.
    segs.assign(1,{0,(int)wav.size()/16});
  }
  return true;
}
