"""Read-only, full-file game inventory; reports never certify publisher integrity."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile

MARKERS=('red4ext','r6/scripts','r6/tweaks','r6/cache/modded','archive/pc/mod','mods','bin/x64/plugins','bin/x64/winmm.dll','bin/x64/version.dll','bin/x64/dxgi.dll','bin/x64/dinput8.dll','bin/x64/global.ini','bin/x64/NeonForge.Core.dll')

def linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info,'st_file_attributes',0)&0x400)

def stable(a,b):
    return (a.st_dev,a.st_ino,a.st_size,a.st_mtime_ns)==(b.st_dev,b.st_ino,b.st_size,b.st_mtime_ns)

def checked_root(root):
    root=Path(root).absolute()
    for part in reversed((root,*root.parents)):
        if linked(part.lstat()):raise ValueError('Game root contains a linked/reparse path')
    if not root.is_dir():raise ValueError('Game root must be a directory')
    return root

def hash_file(path):
    digest=hashlib.sha256();header=b''
    with Path(path).open('rb') as file:
        before=os.fstat(file.fileno())
        if not stat.S_ISREG(before.st_mode):raise ValueError('Not a regular file')
        while block:=file.read(4*1048576):
            if not header:header=block[:16]
            digest.update(block)
        if not stable(before,os.fstat(file.fileno())):raise ValueError('File changed while hashing')
    return dict(sha256=digest.hexdigest(),header_hex=header.hex())

def audit(root,progress=None):
    try:root=checked_root(root)
    except OSError as error:raise ValueError('Cannot access game root: '+str(error)) from error
    report=dict(format=1,started_utc=datetime.now(timezone.utc).isoformat(),files=[],directories=0,files_hashed=0,bytes_hashed=0,errors=[],known_mod_markers=[],complete=False,pristine_install_verified=False,engine_adapter_ready=False,game_tested=False)
    observed=set()
    pending=[root]
    while pending:
        directory=pending.pop()
        try:
            info=directory.lstat()
            if linked(info):raise ValueError('Linked/reparse directory refused')
            entries=sorted(directory.iterdir(),key=lambda path:path.name.casefold())
            report['directories']+=1
        except (OSError,ValueError) as error:
            report['errors'].append(dict(path=directory.relative_to(root).as_posix(),error=str(error)));continue
        children=[]
        for path in entries:
            relative=path.relative_to(root).as_posix();observed.add(relative.casefold())
            try:
                before=path.lstat()
                if linked(before):raise ValueError('Linked/reparse path refused')
                if stat.S_ISDIR(before.st_mode):children.append(path);continue
                if not stat.S_ISREG(before.st_mode):raise ValueError('Non-regular file refused')
                identity=hash_file(path)
                if not stable(before,path.lstat()):raise ValueError('File replaced/changed while hashing')
                report['files'].append(dict(path=relative,bytes=before.st_size,mtime_ns=before.st_mtime_ns,**identity))
                report['files_hashed']+=1;report['bytes_hashed']+=before.st_size
                if progress:progress(report)
            except (OSError,ValueError) as error:
                report['errors'].append(dict(path=relative,error=str(error)))
        pending.extend(reversed(children))
    report['files'].sort(key=lambda entry:entry['path'])
    report['known_mod_markers']=[marker for marker in MARKERS if marker.casefold() in observed]
    report['complete']=not report['errors']
    report['finished_utc']=datetime.now(timezone.utc).isoformat()
    return report

def write_report(root,output,progress=None):
    root=checked_root(root);output=Path(output).absolute()
    if output.resolve().is_relative_to(root.resolve()):raise ValueError('Audit output must be outside the game directory')
    report=audit(root,progress)
    output.parent.mkdir(parents=True,exist_ok=True)
    temporary=None
    try:
        with tempfile.NamedTemporaryFile(mode='w',encoding='utf-8',prefix='.nf-audit-',suffix='.tmp',dir=output.parent,delete=False) as file:
            temporary=Path(file.name)
            json.dump(report,file,indent=2);file.write('\n');file.flush();os.fsync(file.fileno())
        os.replace(temporary,output)
        temporary=None
    finally:
        if temporary is not None:temporary.unlink(missing_ok=True)
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root',type=Path);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        report=write_report(args.root,args.output,lambda result:print(json.dumps({key:result[key] for key in ['files_hashed','bytes_hashed']}),flush=True))
        print(json.dumps({key:report[key] for key in ['complete','files_hashed','bytes_hashed','errors','known_mod_markers','pristine_install_verified']}))
        return 0 if report['complete'] else 1
    except (OSError,ValueError) as error:
        print('NEONFORGE audit: '+str(error));return 1

if __name__=='__main__':raise SystemExit(main())
