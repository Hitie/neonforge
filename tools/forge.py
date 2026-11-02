"""Original NEONFORGE package, diagnostics and SDK utilities. Standard library only."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import sys
import tempfile
import zipfile

class ForgeError(RuntimeError):
    pass

ID=re.compile(r'[a-z][a-z0-9-]*(?:\.[a-z][a-z0-9-]*)+')
SHA=re.compile(r'[0-9a-f]{64}')
VERSION=re.compile(r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)')

def _pairs(items):
    out={}
    for key,value in items:
        if key in out:raise ForgeError('duplicate JSON key: '+key)
        out[key]=value
    return out

def _json(data):
    try:return json.loads(data,object_pairs_hook=_pairs,parse_constant=lambda value:(_ for _ in ()).throw(ForgeError('nonfinite JSON: '+value)))
    except (ValueError,UnicodeError,RecursionError) as e:raise ForgeError('invalid JSON: '+str(e)) from e

def _encode(value):
    try:return (json.dumps(value,ensure_ascii=False,allow_nan=False,sort_keys=True,separators=(',',':'))+'\n').encode('utf-8')
    except (ValueError,UnicodeError,TypeError) as e:raise ForgeError('cannot encode JSON: '+str(e)) from e

def _safe(name):
    if not isinstance(name,str) or not name or len(name)>240 or '\\' in name:raise ForgeError('invalid relative package path')
    for part in name.split('/'):
        if not part or part in ('.','..') or part[-1] in '. ' or any(ord(c)<32 or c in ':<>"|?*' for c in part):raise ForgeError('unsafe package path: '+name)
        base=part.split('.')[0].upper()
        if base in ('CON','PRN','AUX','NUL') or re.fullmatch(r'(?:COM|LPT)[1-9\u00b9\u00b2\u00b3]',base):raise ForgeError('reserved Windows filename: '+name)
    return name

def _no_links(path):
    p=Path(path).absolute()
    for current in reversed([p,*p.parents]):
        try:info=current.lstat()
        except FileNotFoundError:continue
        if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&0x400:raise ForgeError('reparse/symlink path refused: '+str(current))
    return p

def _hash(path):
    _no_links(path)
    h=hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda:source.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def _manifest(value):
    if not isinstance(value,dict):raise ForgeError('manifest must be an object')
    if set(value)-{'id','name','version','api','entry','requires','before','after','capabilities'}:raise ForgeError('unknown manifest fields')
    if not isinstance(value.get('id'),str) or not ID.fullmatch(value['id']) or len(value['id'])>128:raise ForgeError('invalid mod ID')
    def version(text):
        if not isinstance(text,str) or not VERSION.fullmatch(text):raise ForgeError('invalid semantic version')
        parts=tuple(int(n) for n in text.split('.'))
        if any(n>0xffffffff for n in parts):raise ForgeError('version component exceeds uint32')
        return parts
    version(value.get('version'))
    if type(value.get('api')) is not int or value['api']!=1:raise ForgeError('unsupported API major')
    if not isinstance(value.get('name'),str) or not value['name'] or '\0' in value['name'] or len(value['name'].encode('utf-8'))>256:raise ForgeError('invalid mod name')
    _safe(value.get('entry'))
    if not value['entry'].endswith('.dll'):raise ForgeError('native entry must be .dll')
    def identifier(text):
        if not isinstance(text,str) or not ID.fullmatch(text) or len(text)>128:raise ForgeError('invalid dotted identifier')
        return text
    for field in ('before','after'):
        items=value.get(field,[])
        if not isinstance(items,list):raise ForgeError(field+' must be an array')
        for item in items:identifier(item)
    dependencies=value.get('requires',[])
    if not isinstance(dependencies,list):raise ForgeError('requires must be an array')
    seen=set()
    for dependency in dependencies:
        if not isinstance(dependency,dict) or set(dependency)-{'id','min','max_exclusive'}:raise ForgeError('invalid dependency fields')
        key=identifier(dependency.get('id'))
        if key in seen:raise ForgeError('duplicate dependency ID')
        seen.add(key);minimum=version(dependency.get('min'))
        if 'max_exclusive' in dependency and version(dependency['max_exclusive'])<=minimum:raise ForgeError('empty dependency version range')
    capabilities=value.get('capabilities',[])
    if not isinstance(capabilities,list):raise ForgeError('capabilities must be an array')
    seen=set()
    for capability in capabilities:
        if not isinstance(capability,dict) or set(capability)-{'name','version','required'}:raise ForgeError('invalid capability fields')
        key=identifier(capability.get('name'))
        if key in seen:raise ForgeError('duplicate capability')
        seen.add(key);number=capability.get('version')
        if type(number) is not int or not 1<=number<=0xffffffff:raise ForgeError('capability version must be positive uint32')
        if 'required' in capability and type(capability['required']) is not bool:raise ForgeError('capability required must be boolean')
    return value

def inspect_package(package):
    _no_links(package)
    try:
        with zipfile.ZipFile(package) as archive:
            if len(archive.infolist())>10000:raise ForgeError('package has more than 10000 entries')
            total=0;files={};seen=set()
            for info in archive.infolist():
                key=_safe(info.filename.rstrip('/'))
                if key.casefold() in seen:raise ForgeError('duplicate/case-colliding package path: '+key)
                seen.add(key.casefold())
                if stat.S_ISLNK(info.external_attr>>16) or info.external_attr&0x400 or info.flag_bits&1:raise ForgeError('links/encrypted entries are unsupported')
                if info.is_dir():continue
                total+=info.file_size
                if info.file_size>256*1024*1024 or total>512*1024*1024:raise ForgeError('package exceeds uncompressed size limits')
                h=hashlib.sha256()
                with archive.open(info) as source:
                    for chunk in iter(lambda:source.read(1024*1024),b''):h.update(chunk)
                files[key]=h.hexdigest()
            if 'manifest.json' not in files or archive.getinfo('manifest.json').file_size>1048576:raise ForgeError('missing/oversized manifest.json')
            manifest=_manifest(_json(archive.read('manifest.json')))
            if manifest['entry'] not in files:raise ForgeError('declared native entry is absent')
            return {'manifest':manifest,'files':files,'uncompressed_bytes':total,'sha256':_hash(package)}
    except (OSError,zipfile.BadZipFile,RuntimeError) as e:
        if isinstance(e,ForgeError):raise
        raise ForgeError('cannot inspect package: '+str(e)) from e

def _receipt(path,mod_id):
    if not path.exists():return {'format':1,'id':mod_id,'files':{}}
    _no_links(path)
    if path.stat().st_size>2*1024*1024:raise ForgeError('receipt exceeds size limit')
    d=_json(path.read_bytes())
    if not isinstance(d,dict) or d.get('format')!=1 or d.get('id')!=mod_id or not isinstance(d.get('files'),dict):raise ForgeError('invalid ownership receipt')
    seen=set()
    for name,digest in d['files'].items():
        _safe(name)
        if name.casefold() in seen or not isinstance(digest,str) or not SHA.fullmatch(digest):raise ForgeError('invalid receipt file/hash')
        seen.add(name.casefold())
    return d

@contextmanager
def _lock(root):
    base=_no_links(root/'neonforge');base.mkdir(exist_ok=True)
    lock=base/'.manager.lock';_no_links(lock)
    try:fd=os.open(lock,os.O_CREAT|os.O_EXCL|os.O_WRONLY)
    except FileExistsError as e:raise ForgeError('manager lock already exists; another operation or interrupted process needs attention') from e
    try:
        os.write(fd,str(os.getpid()).encode('ascii'));os.fsync(fd);os.close(fd);fd=None
        yield
    finally:
        if fd is not None:os.close(fd)
        lock.unlink()

def _atomic(path,data):
    path=_no_links(path);path.parent.mkdir(parents=True,exist_ok=True);_no_links(path.parent)
    fd,temp=tempfile.mkstemp(prefix='.nf-write-',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as out:out.write(data);out.flush();os.fsync(out.fileno())
        _no_links(path);os.replace(temp,path)
    finally:
        if Path(temp).exists():Path(temp).unlink()

def _apply(root,writes,deletes):
    base=_no_links(root/'neonforge/.transactions');base.mkdir(parents=True,exist_ok=True)
    tx=Path(tempfile.mkdtemp(prefix='nf-',dir=base));backups={};applied=[];retain=False
    try:
        targets=sorted(set(writes)|set(deletes),key=str)
        for i,target in enumerate(targets):
            _no_links(target)
            if target.exists() and not target.is_file():raise ForgeError('target is not a regular file: '+str(target))
            old=target.read_bytes() if target.exists() else None;backups[target]=old
            if old is not None:(tx/f'{i}.before').write_bytes(old)
        journal={'format':1,'state':'prepared','targets':[{'path':str(p.relative_to(root)).replace('\\','/'),'backup':f'{i}.before' if backups[p] is not None else None,'sha256':hashlib.sha256(backups[p]).hexdigest() if backups[p] is not None else None} for i,p in enumerate(targets)]}
        (tx/'journal.json').write_bytes(_encode(journal))
        for target in targets:
            applied.append(target)
            if target in writes:_atomic(target,writes[target])
            elif target.exists():target.unlink()
    except Exception as original:
        try:
            for target in reversed(applied):
                old=backups[target]
                if old is None:
                    _no_links(target)
                    if target.exists():target.unlink()
                else:_atomic(target,old)
        except Exception as rollback:
            retain=True
            raise ForgeError(f'operation failed and rollback needs recovery; retained {tx}: {rollback}') from original
        raise ForgeError('operation rolled back: '+str(original)) from original
    finally:
        if not retain:
            resolved=tx.resolve();allowed=base.resolve()
            if resolved.parent!=allowed or not resolved.name.startswith('nf-'):raise ForgeError('refusing cleanup outside owned transaction directory')
            shutil.rmtree(resolved)

def install(package,root,dry_run=False):
    root=_no_links(root)
    if not root.is_dir():raise ForgeError('installation root must already be a directory')
    candidate=inspect_package(package);m=candidate['manifest'];target=root/'neonforge/mods'/m['id'];receipt=root/'neonforge/receipts'/(m['id']+'.json')
    def plan():
        _no_links(target);_no_links(receipt);previous=_receipt(receipt,m['id']);old=previous['files'];writes={};deletes=[];preserved=[]
        for name,digest in candidate['files'].items():
            file=_no_links(target/name)
            if file.exists() and (name not in old or _hash(file)!=old[name]):raise ForgeError('unowned or locally modified file preserved: '+name)
            writes[file]=name
        residual={}
        for name,digest in old.items():
            if name in candidate['files']:continue
            file=_no_links(target/name)
            if file.exists():
                if _hash(file)==digest:deletes.append(file)
                else:preserved.append(name);residual[name]=digest
        result={'action':'update' if receipt.exists() else 'install','id':m['id'],'version':m['version'],'owned_files':len(candidate['files']),'preserved':preserved,'dry_run':dry_run}
        return writes,deletes,residual,result
    if dry_run:return plan()[3]
    with _lock(root):
        writes,deletes,residual,result=plan()
        with zipfile.ZipFile(package) as archive:
            contents={path:archive.read(name) for path,name in writes.items()}
        for path,data in contents.items():
            if hashlib.sha256(data).hexdigest()!=candidate['files'][str(path.relative_to(target)).replace('\\','/')]:raise ForgeError('package changed during installation')
        contents[receipt]=_encode({'format':1,'id':m['id'],'version':m['version'],'files':{**residual,**candidate['files']}})
        _apply(root,contents,deletes)
        return result

def uninstall(mod_id,root,dry_run=False):
    if not ID.fullmatch(mod_id):raise ForgeError('invalid mod ID')
    root=_no_links(root);target=root/'neonforge/mods'/mod_id;receipt=root/'neonforge/receipts'/(mod_id+'.json')
    def plan():
        _no_links(receipt);old=_receipt(receipt,mod_id);deletes=[];preserved={}
        for name,digest in old['files'].items():
            file=_no_links(target/name)
            if file.exists():
                if _hash(file)==digest:deletes.append(file)
                else:preserved[name]=digest
        result={'action':'uninstall','id':mod_id,'removed':len(deletes),'preserved':sorted(preserved),'dry_run':dry_run}
        writes={}
        if preserved:writes[receipt]=_encode({**old,'files':preserved})
        elif receipt.exists():deletes.append(receipt)
        return writes,deletes,result
    if dry_run:return plan()[2]
    with _lock(root):
        writes,deletes,result=plan();_apply(root,writes,deletes);return result

def migrate_settings(file,from_version,to_version,renames,defaults):
    file=_no_links(file)
    if file.stat().st_size>1048576:raise ForgeError('settings exceed 1 MiB')
    old=_json(file.read_bytes())
    if not isinstance(old,dict) or type(from_version) is not int or type(to_version) is not int or old.get('schema_version')!=from_version or to_version<=from_version:raise ForgeError('settings schema version mismatch')
    if not isinstance(renames,dict) or not isinstance(defaults,dict) or 'schema_version' in renames or 'schema_version' in renames.values() or 'schema_version' in defaults:raise ForgeError('invalid migration map')
    result=dict(old)
    for source,dest in renames.items():
        if not isinstance(source,str) or not isinstance(dest,str) or not source or not dest:raise ForgeError('invalid migration key')
        if source in result:
            if dest in result and source!=dest:raise ForgeError('migration destination already exists: '+dest)
            result[dest]=result.pop(source)
    for key,value in defaults.items():result.setdefault(key,value)
    result['schema_version']=to_version;_atomic(file,_encode(result));return result

def scaffold(destination,mod_id,name):
    if not ID.fullmatch(mod_id) or len(mod_id)>128:raise ForgeError('invalid mod ID')
    destination=_no_links(destination)
    if destination.exists():raise ForgeError('project destination already exists')
    if not name or len(name.encode('utf-8'))>256:raise ForgeError('invalid display name')
    manifest={'id':mod_id,'name':name,'version':'0.1.0','api':1,'entry':'plugin.dll','capabilities':[{'name':'nf.core','version':1,'required':True}]}
    destination.mkdir(parents=True);(destination/'src').mkdir()
    (destination/'manifest.json').write_bytes(_encode(manifest))
    code='''#include <neonforge/api.h>
#include <string.h>
static nf_str text(const char*s){nf_str v={s,strlen(s)};return v;}
__declspec(dllexport) nf_status NF_CALL nf_plugin_query(nf_plugin_info*out){if(!out||out->struct_size!=sizeof(*out))return NF_E_ABI;out->abi_version=1;out->id=text("MOD_ID");out->version_major=0;out->version_minor=1;out->version_patch=0;return NF_OK;}
__declspec(dllexport) nf_status NF_CALL nf_plugin_start(const nf_api*a,nf_runtime*r,nf_context c,nf_plugin_lifecycle*out){if(!a||a->struct_size!=sizeof(*a)||a->abi_version!=1||!out||out->struct_size!=sizeof(*out))return NF_E_ABI;return a->log(r,c,1,text("Original NEONFORGE plugin started"));}
'''.replace('MOD_ID',mod_id)
    (destination/'src/plugin.c').write_text(code,encoding='utf-8')
    (destination/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.25)\nproject(NeonForgeMod LANGUAGES C)\nif(NOT NEONFORGE_SDK)\n message(FATAL_ERROR "Set NEONFORGE_SDK to the extracted SDK directory")\nendif()\nadd_library(plugin SHARED src/plugin.c)\ntarget_include_directories(plugin PRIVATE "${NEONFORGE_SDK}/include")\nset_target_properties(plugin PROPERTIES PREFIX "")\n',encoding='utf-8')
    return {'id':mod_id,'directory':str(destination),'authoring_route':'native C ABI; engine services unavailable'}

def resolve_candidate(sections,entries,primary_hash,secondary_hash=None):
    matches=[e for e in entries if e.get('hash')==str(primary_hash) and (secondary_hash is None or e.get('secondary hash')==secondary_hash)]
    if len(matches)!=1:raise ForgeError(f'address identity is missing or ambiguous ({len(matches)} matches); use a full secondary hash')
    entry=matches[0]
    try:
        section_text,offset_text=entry['offset'].split(':');index=int(section_text,16)-1;offset=int(offset_text,16)
        if index<0 or index>=len(sections):raise ValueError('section outside image')
        section=sections[index]
        if offset<0 or offset>=min(section['virtual_size'],section['raw_size']):raise ValueError('offset is not file-backed in section')
        return {'rva':section['rva']+offset,'file_offset':section['raw_offset']+offset,'secondary_hash':entry.get('secondary hash'),'abi_verified':False,'instruction_fingerprint_verified':False}
    except (ValueError,KeyError,TypeError) as e:raise ForgeError('invalid/out-of-bounds address: '+str(e)) from e

def inspect_pe(file):
    file=_no_links(file)
    if file.stat().st_size>512*1024*1024:raise ForgeError('PE file exceeds inspection limit')
    data=file.read_bytes()
    def get(fmt,offset):
        if offset<0 or offset+struct.calcsize(fmt)>len(data):raise ForgeError('PE structure outside file')
        return struct.unpack_from(fmt,data,offset)
    if data[:2]!=b'MZ':raise ForgeError('missing DOS header')
    pe=get('<I',0x3c)[0]
    if data[pe:pe+4]!=b'PE\0\0':raise ForgeError('missing PE signature')
    machine,count,timestamp,_,_,optional_size,_=get('<HHIIIHH',pe+4)
    optional=pe+24
    if get('<H',optional)[0]!=0x20b or optional_size<112:raise ForgeError('only PE32+ images are supported')
    if count>96:raise ForgeError('excessive PE section count')
    image_base=get('<Q',optional+24)[0];image_size=get('<I',optional+56)[0];sections=[]
    for index in range(count):
        offset=optional+optional_size+index*40
        name=get('<8s',offset)[0].rstrip(b'\0').decode('ascii',errors='replace')
        virtual_size,rva,raw_size,raw_offset=get('<IIII',offset+8)
        flags=get('<I',offset+36)[0]
        if raw_offset+raw_size>len(data) or rva+virtual_size>image_size:raise ForgeError('PE section outside file/image')
        sections.append({'name':name,'rva':rva,'virtual_size':virtual_size,'raw_size':raw_size,'raw_offset':raw_offset,'flags':flags})
    return {'machine':hex(machine),'timestamp':timestamp,'image_base':image_base,'image_size':image_size,'sections':sections,'sha256':hashlib.sha256(data).hexdigest()}

def doctor(game_root,baseline):
    root=_no_links(game_root);identity=_json(Path(baseline).read_bytes());exe=root/'bin/x64/Cyberpunk2077.exe';addresses=root/'bin/x64/cyberpunk2077_addresses.json'
    pe=inspect_pe(exe);address_hash=_hash(addresses);metadata=_json(addresses.read_bytes());entries=metadata.get('Addresses')
    if not isinstance(entries,list):raise ForgeError('address metadata has no Addresses array')
    from collections import Counter
    primary=Counter(e.get('hash') for e in entries);named=sum('symbol' in e for e in entries)
    timestamp=metadata.get('Linker map timestamp','').split(' ',1)[0]
    try:timestamp_matches=int(timestamp,16)==pe['timestamp']
    except ValueError:timestamp_matches=False
    matches=pe['sha256']==identity.get('executable_sha256') and address_hash==identity.get('address_metadata_sha256') and pe['machine']=='0x8664' and timestamp_matches
    return {'game_root':str(root),'identity_matches_baseline':matches,'executable_sha256':pe['sha256'],'address_metadata_sha256':address_hash,'pe_timestamp':pe['timestamp'],'linker_timestamp_matches':timestamp_matches,'address_records':len(entries),'unique_primary_hashes':len(primary),'colliding_primary_hashes':sum(n>1 for n in primary.values()),'named_symbols':named,'sections':pe['sections'],'engine_adapter_ready':False,'game_tested':False,'status':'Exact file identity and metadata inspection only. Internal layouts, calling conventions, fingerprints and engine lifecycle are unverified.'}

def main():
    parser=argparse.ArgumentParser(description='NEONFORGE original package/SDK tools. Does not enable game hooks.')
    sub=parser.add_subparsers(dest='command',required=True)
    p=sub.add_parser('inspect');p.add_argument('package',type=Path)
    for command in ('install','uninstall'):
        p=sub.add_parser(command);p.add_argument('package' if command=='install' else 'mod_id');p.add_argument('--root',type=Path,required=True);p.add_argument('--dry-run',action='store_true')
    p=sub.add_parser('new');p.add_argument('directory',type=Path);p.add_argument('--id',required=True);p.add_argument('--name',required=True)
    p=sub.add_parser('doctor');p.add_argument('game_root',type=Path);p.add_argument('--baseline',type=Path,default=Path(__file__).resolve().parents[1]/'adapters/steam-2.5-26161058.json')
    p=sub.add_parser('migrate-settings');p.add_argument('file',type=Path);p.add_argument('--from-version',type=int,required=True);p.add_argument('--to-version',type=int,required=True);p.add_argument('--renames',default='{}');p.add_argument('--defaults',default='{}')
    args=parser.parse_args()
    try:
        if args.command=='inspect':result=inspect_package(args.package)
        elif args.command=='install':result=install(Path(args.package),args.root,args.dry_run)
        elif args.command=='uninstall':result=uninstall(args.mod_id,args.root,args.dry_run)
        elif args.command=='new':result=scaffold(args.directory,args.id,args.name)
        elif args.command=='doctor':result=doctor(args.game_root,args.baseline)
        else:result=migrate_settings(args.file,args.from_version,args.to_version,_json(args.renames),_json(args.defaults))
        print(json.dumps(result,indent=2,ensure_ascii=False));return 0
    except (ForgeError,OSError) as e:print('NEONFORGE: '+str(e),file=sys.stderr);return 1
if __name__=='__main__':raise SystemExit(main())
