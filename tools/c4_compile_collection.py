#!/usr/bin/env python3
"""Build-only GLSL -> SPIR-V -> HLSL5 -> FXC. No runtime compiler dependency.
Incremental by source+tool hashes. A failing pass aborts registry publication.
No downloaded binary is executed by this script; tool locations are explicit.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess,sys,re,os,tempfile

def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def run(args):
 p=subprocess.run(list(map(str,args)),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace',timeout=180)
 if p.returncode:raise RuntimeError('Command failed: '+' '.join(map(str,args))+'\n'+p.stdout)
 return p.stdout

def write_changed(p,text):
 if p.exists() and p.read_text(encoding='utf-8')==text:return
 p.parent.mkdir(parents=True,exist_ok=True)
 fd,tmp=tempfile.mkstemp(prefix='.c4-',dir=p.parent)
 try:
  with os.fdopen(fd,'w',encoding='utf-8',newline='\n') as f:f.write(text);f.flush();os.fsync(f.fileno())
  os.replace(tmp,p)
 finally:
  if os.path.exists(tmp):os.unlink(tmp)

def validate_graphs(graphs):
 if not isinstance(graphs,list) or not 1<=len(graphs)<=32:raise ValueError('Preset count')
 seen=set()
 for p in graphs:
  if not re.fullmatch(r'[A-Za-z0-9_]{1,32}',p['id']) or p['id'] in seen:raise ValueError('Preset id')
  seen.add(p['id']);passes=p['passes']
  if not 1<=len(passes)<=8 or not passes[-1].get('final') or passes[-1]['slot']!=7:raise ValueError('Missing final Main')
  if sum(bool(x.get('final')) for x in passes)!=1:raise ValueError('Final count')
  if not 64<=p['maxEdge']<=2048 or not 1<=p['rate']<=60:raise ValueError('Invalid preset limits')
  keys=set();outputs={}
  for q in passes:
   if not re.fullmatch(r'[A-Za-z0-9_]{1,32}',q['key']) or q['key'] in keys:raise ValueError('Pass key')
   keys.add(q['key'])
   if q['stage'] not in ('fragment','compute') or q['slot'] not in range(-1,8):raise ValueError('Stage/output')
   if q['stage']=='fragment' and q['slot']<0:raise ValueError('Fragment needs target')
   if q['scale'] not in (1,2,4) or not 1<=q['repeat']<=20:raise ValueError('Pass bounds')
   if q['slot']>=0:
    if q['slot'] in outputs:raise ValueError('Output slot has two writers')
    outputs[q['slot']]=True
   slots=set()
   for b in q['bindings']:
    if b['binding'] not in range(11) or b['binding'] in slots:raise ValueError('Resource slot')
    slots.add(b['binding'])
    if b['kind'] not in ('pass','selfMain','audio','noise','keyboard'):raise ValueError('Unsupported resource')
  for q in passes:
   for b in q['bindings']:
    if b['kind']=='pass' and b['index'] not in outputs:raise ValueError('Unknown pass dependency')
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--source',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
 ap.add_argument('--target',choices=['player','renderer','client-probe','reference'],default='player')
 for t in ['glslang','spirv-cross','fxc']:ap.add_argument('--'+t,type=Path,required=True)
 a=ap.parse_args();src=a.source.resolve();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
 tools={'glslang':a.glslang.resolve(),'cross':a.spirv_cross.resolve(),'fxc':a.fxc.resolve()}
 for k,t in tools.items():
  if not t.is_file():raise RuntimeError(f'{k} tool missing: {t}')
 versions={k:{'path':str(t),'sha256':digest(t)} for k,t in tools.items()}
 graphs=json.loads((src/'catalogue.json').read_text(encoding='utf-8'));validate_graphs(graphs)
 if a.target in ('renderer','client-probe'):graphs=[p for p in graphs if p['id']=='wXdfzj']
 stages=[];includes=[];table=[]
 for preset in graphs:
  passes=[]
  for p in preset['passes']:
   symbols=[]
   for mode in ['original','music']:
    if a.target in ('player','client-probe') and preset['id']=='wXdfzj':
     symbols.append(None);continue
    name=f"{preset['id']}_{p['key']}_{mode}";ext='comp' if p['stage']=='compute' else 'frag';source=src/'shaders'/f'{name}.{ext}'
    symbol='kC4_'+re.sub('[^A-Za-z0-9_]','_',name);header=out/(name+'.h');meta=out/(name+'.stamp.json')
    profile='cs_5_0' if ext=='comp' else 'ps_5_0';key=hashlib.sha256((digest(source)+json.dumps(versions,sort_keys=True)+profile+'v4').encode()).hexdigest()
    cached=json.loads(meta.read_text()) if meta.is_file() else {}
    if cached.get('key')!=key or not header.is_file() or cached.get('headerSHA256')!=digest(header):
     spv=out/(name+'.spv');hlsl=out/(name+'.hlsl');reflection=out/(name+'.reflect.json');log=[]
     log.append(run([tools['glslang'],'-V','--target-env','vulkan1.0','-S',ext,'-e','main','-o',spv,source]))
     log.append(run([tools['cross'],spv,'--hlsl','--shader-model','50','--output',hlsl]))
     reflection.write_text(run([tools['cross'],spv,'--reflect']),encoding='utf-8')
     log.append(run([tools['fxc'],'/nologo','/Ges','/O3','/T',profile,'/E','main','/Vn',symbol,'/Fh',header,hlsl]))
     if not header.is_file() or symbol not in header.read_text(errors='replace'):raise RuntimeError('FXC header not produced '+name)
     meta.write_text(json.dumps({'key':key,'headerSHA256':digest(header),'sourceSHA256':digest(source),'profile':profile,'log':'\n'.join(log)},indent=2),encoding='utf-8')
    stages.append({'name':name,'profile':profile,'headerSHA256':digest(header),'sourceSHA256':digest(source),'status':'FXC_PASS'})
    includes.append(f'#include "{name}.h"');symbols.append(symbol)
   refs=[-1]*11
   for b in p['bindings']:
    kind=b['kind'];slot=b['binding']
    if kind=='external':raise RuntimeError('Unresolved external resource '+preset['id']+str(b))
    refs[slot]= b['index'] if kind=='pass' else 7 if kind=='selfMain' else {'audio':8,'noise':9,'keyboard':10}[kind]
   flags=','.join('true' if x else 'false' for x in [p.get('once',False),p.get('format')=='rgba32f',p.get('final',False)])
   passes.append('{"'+p['key']+'",Stage::'+p['stage']+f",{p['slot']},{p['scale']},{p['repeat']},{p.get('dispatch',0)},"+flags+',{{'+','.join(map(str,refs))+'}},'+str(p.get('wrapMask',0))+','+','.join('{'+s+',sizeof('+s+')}' if s else '{}' for s in symbols)+'}')
  pid=re.sub('[^A-Za-z0-9_]','_',preset['id']);table.append('static const Pass passes_'+pid+'[]={\n'+',\n'.join(passes)+'\n};')
 # Two small host shaders are authored HLSL, not translated source programs.
 host={
  'vertex':('vs_5_0','kC4Vertex','''struct Out {float4 p:SV_Position;float2 uv:TEXCOORD0;};Out main(uint id:SV_VertexID){float2 q=float2((id<<1)&2,id&2);Out o;o.p=float4(q.x*2-1,1-q.y*2,.5,1);o.uv=q;return o;}'''),
  'resample':('ps_5_0','kC4Resample','''cbuffer Params:register(b0){float4 Scale;}Texture2D<float4> Source:register(t0);SamplerState Linear:register(s0);float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return Source.SampleLevel(Linear,uv,0)*Scale;}''')}
 for name,(profile,symbol,code) in host.items():
  source=out/f'host_{name}.hlsl';header=out/f'host_{name}.h';write_changed(source,code)
  key=digest(source)+versions['fxc']['sha256'];stamp=out/f'host_{name}.stamp.json'
  cached=json.loads(stamp.read_text()) if stamp.exists() else {}
  if cached.get('key')!=key or not header.exists() or cached.get('header')!=digest(header):
   run([tools['fxc'],'/nologo','/Ges','/O3','/T',profile,'/E','main','/Vn',symbol,'/Fh',header,source]);write_changed(stamp,json.dumps({'key':key,'header':digest(header)}))
  includes.append(f'#include "host_{name}.h"')
 defs=[]
 for p in graphs:
  pid=re.sub('[^A-Za-z0-9_]','_',p['id']);title=p['title'].replace('\\','\\\\').replace('"','\\"')
  vals=[p.get('stateful',False),p.get('flipY',True),p.get('alreadyAudio',False),p.get('experimental',False),p.get('storage',False)]
  defs.append('{"'+p['id']+'",L"'+title+'",passes_'+pid+f",{len(p['passes'])},{p['maxEdge']},{p.get('alignment',1)},{p['rate']},"+','.join('true'if v else'false'for v in vals)+'}')
 registry='// Generated atomically only after every shader succeeded.\n'+ '\n'.join(includes)+'\nnamespace nekorox::component::collection {\n'+ '\n'.join(table)+ '\nstatic const Preset definitions[]={\n'+',\n'.join(defs)+'\n};\nstd::span<const Preset> presets()noexcept{return definitions;}\nBlob fullscreenVertexShader()noexcept{return {kC4Vertex,sizeof(kC4Vertex)};}\nBlob resamplePixelShader()noexcept{return {kC4Resample,sizeof(kC4Resample)};}\n}\n'
 write_changed(out/'c4_registry.generated.h',registry)
 (out/'C4_COMPILER_RECEIPT.json').write_text(json.dumps({'target':a.target,'tools':versions,'programs':stages,'registrySHA256':digest(out/'c4_registry.generated.h'),'nativeExecution':'NOT_RUN_BY_COMPILER'},indent=2),encoding='utf-8')
 print('All',len(stages),'collection programs compiled; registry committed')
if __name__=='__main__':
 try:main()
 except Exception as e:print(str(e),file=sys.stderr);sys.exit(1)
