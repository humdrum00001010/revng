#!/usr/bin/env python3
from pathlib import Path
import json,subprocess,hashlib,os,argparse
parser=argparse.ArgumentParser();parser.add_argument('--preserve-dwarf',action='store_true');args=parser.parse_args()
root=Path(__file__).resolve().parent
binary_name='repro-with-dwarf.dll' if args.preserve_dwarf else 'repro.dll'
receipt_name='build-dwarf-receipt.json' if args.preserve_dwarf else 'build-receipt.json'
llvm=Path(os.environ.get('LLVM_BIN','/opt/homebrew/opt/llvm@17/bin'))
version=subprocess.check_output([str(llvm/'clang'),'--version'],text=True)
commands=[]
if (root/'gdi32.def').exists():commands.append([str(llvm/'llvm-dlltool'),'-m','i386','-d',str(root/'gdi32.def'),'-l',str(root/'gdi32.lib'),'--kill-at'])
commands.append([str(llvm/'clang'),'--target=i686-w64-windows-gnu','-O1','-std=gnu2x','-g','-gdwarf-4','-fdebug-compilation-dir=/repro','-ffreestanding','-fno-inline','-fno-stack-protector','-ffile-prefix-map='+str(root)+'/=/repro/','-fdebug-prefix-map='+str(root)+'/=/repro/','-c','repro.c','-o','repro.obj'])
commands.append([str(llvm/'lld-link'),'/DLL','/lldmingw','/Brepro',*(['/DEBUG:DWARF'] if args.preserve_dwarf else []),'/ENTRY:DllMain@12','/NODEFAULTLIB','/SAFESEH:NO','/MACHINE:X86','/OUT:'+str(root/binary_name),str(root/'repro.obj'),*([str(root/'gdi32.lib')] if (root/'gdi32.def').exists() else [])])
receipts=[]
for i,cmd in enumerate(commands):
 p=subprocess.run(cmd,cwd=root,capture_output=True,text=True);(root/(('build-dwarf-' if args.preserve_dwarf else 'build-')+str(i)+'.log')).write_text((p.stdout+p.stderr).replace(str(root),'<PACKAGE>'));receipts.append({'argv':[arg.replace(str(root),'<PACKAGE>').replace(str(llvm),'<LLVM_BIN>') for arg in cmd],'exit':p.returncode})
 if p.returncode:print(p.stderr);break
result={'compiler_version':version,'commands':receipts,'independently_authored_source':True,'private_source_fragments':False,'source_sha256':hashlib.sha256((root/'repro.c').read_bytes()).hexdigest()}
if (root/binary_name).exists():result['binary_sha256']=hashlib.sha256((root/binary_name).read_bytes()).hexdigest()
(root/receipt_name).write_text(json.dumps(result,indent=2)+'\n')
print(root.name,'build',receipts[-1]['exit'])
raise SystemExit(receipts[-1]['exit'])
