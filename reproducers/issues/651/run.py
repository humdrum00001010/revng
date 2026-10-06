#!/usr/bin/env python3
"""Run the official supported whole-binary pipeline; never inject models or IR."""
from pathlib import Path
import argparse,subprocess,json,hashlib,tarfile,shutil,os
parser=argparse.ArgumentParser();parser.add_argument('--variant',default=None);parser.add_argument('--binary',default='repro.dll');parser.add_argument('--quick',action='store_true');args=parser.parse_args()
root=Path(__file__).resolve().parent;out=root/'runs'/(args.variant or ('official-'+__import__('datetime').datetime.now(__import__('datetime').timezone.utc).strftime('%Y%m%dT%H%M%S')));assert not out.exists(),'Keep previous receipts; select a fresh variant name';out.mkdir(parents=True);project=out/'project';project.mkdir();image='revng/revng@sha256:1446ae6f9ba9f2f907af69bb6c3954093327dc0e515fad76f1de8610df4a0fce';tool=Path(os.environ.get('LLVM_BIN','/opt/homebrew/opt/llvm@17/bin'));source=root/args.binary;sha=hashlib.sha256(source.read_bytes()).hexdigest();results={'issue':int(root.name),'image':image,'binary_sha256':sha,'complete_original_binary':True,'manual_model_or_metadata_overrides':False,'function_selectors':False,'IR_or_Clift_inputs_used':False,'commands':[]}
stages = [('quick',['revng','quick','artifact','emit-recompilable-archive','/inputs/repro.dll','-o','recompilable.tar.gz'])] if args.quick else [('init',['revng','project','init','/inputs/repro.dll']),('archive',['revng','project','artifact','emit-recompilable-archive','-o','recompilable.tar.gz'])]
results['supported_pipeline'] = 'revng quick artifact' if args.quick else 'revng project init then artifact'
for stage,arguments in stages:
 name='maintainer-repro-'+root.name+'-'+stage+'-'+sha[:10]+'-'+out.name[-8:];cmd=['docker','run','--platform','linux/amd64','--network','none','--name',name,'--cpus','1','--memory','4g','--memory-swap','4g','--mount','type=bind,src='+str(source)+',dst=/inputs/repro.dll,readonly','--mount','type=bind,src='+str(project)+',dst=/work','--workdir','/work',image,*arguments];p=subprocess.run(cmd,capture_output=True,text=True);(out/(stage+'.stdout.log')).write_text(p.stdout.replace(str(root),'<PACKAGE>'));(out/(stage+'.stderr.log')).write_text(p.stderr.replace(str(root),'<PACKAGE>'));state=json.loads(subprocess.check_output(['docker','inspect',name,'--format','{{json .State}}'],text=True));results['commands'].append({'stage':stage,'argv':[x.replace(str(root),'<PACKAGE>') for x in cmd],'exit':p.returncode,'state':state});(out/'result.json').write_text(json.dumps(results,indent=2)+'\n');print(root.name,stage,p.returncode,flush=True)
 if p.returncode:results['classification']='reproduced' if int(root.name)==651 and ('llvm::alignTo(ByteSize, Alignment) == StructByteSize' in p.stderr or 'FieldSizeInBits % 8 == 0' in p.stderr) else 'inconclusive';results['reason']='Normal whole-binary pipeline stopped before complete C output';(out/'result.json').write_text(json.dumps(results,indent=2)+'\n');raise SystemExit(0)
archive=project/'recompilable.tar.gz';generated=out/'generated';generated.mkdir()
with tarfile.open(archive) as t:
 for member in t.getmembers():
  if not member.isfile():continue
  assert member.name.startswith('decompiled/') and '..' not in Path(member.name).parts
  target=generated/member.name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(t.extractfile(member).read())
c=generated/'decompiled/functions.c';body=c.read_bytes();results['generated_C_sha256']=hashlib.sha256(body).hexdigest();results['archive_sha256']=hashlib.sha256(archive.read_bytes()).hexdigest();cmd=[str(tool/'clang'),'--target=i686-w64-windows-gnu','-std=gnu2x','-ffreestanding','-O0','-I',str(c.parent),'-c',str(c),'-o',str(out/'whole.obj')];p=subprocess.run(cmd,capture_output=True,text=True);diagnostics=(p.stdout+p.stderr).replace(str(root),'<PACKAGE>');(out/'whole-compile.log').write_text(diagnostics);results['commands'].append({'stage':'complete-C-to-COFF','argv':[x.replace(str(root),'<PACKAGE>').replace(str(tool),'<LLVM_BIN>') for x in cmd],'exit':p.returncode});results['complete_generated_C_compile_exit']=p.returncode;assert c.read_bytes()==body
if p.returncode==0:
 nm=subprocess.check_output([str(tool/'llvm-nm'),'--undefined-only',str(out/'whole.obj')],text=True);(out/'whole-undefined-symbols.txt').write_text(nm);results['undefined_symbols']=nm.splitlines()
if int(root.name)==651:
 logs=''.join((out/(stage+'.stderr.log')).read_text() for stage,_ in stages)
 reproduced='llvm::alignTo(ByteSize, Alignment) == StructByteSize' in logs or 'FieldSizeInBits % 8 == 0' in logs
else:
 reproduced=p.returncode!=0 and ('const-qualified type' in diagnostics or 'read-only variable is not assignable' in diagnostics)
results['classification']='reproduced' if reproduced else 'not_reproduced'
results['scope_of_classification']='This independently authored complete binary candidate on the official normal pipeline only'
(out/'result.json').write_text(json.dumps(results,indent=2)+'\n')
print(root.name,results['classification'],'compile',p.returncode,flush=True)
