"""Durable original offline checkpoints and content-settled project watching."""
import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid
import windows_files as files

StoreError = files.FileError


class Conflict(StoreError):
    pass


class InvalidData(StoreError):
    pass


class Retryable(StoreError):
    pass


def _encode(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False) + '\n').encode('utf-8')


def _metadata(data, keys):
    try:
        def pairs(items):
            result = {}
            for key, value in items:
                if key in result: raise ValueError('duplicate metadata key')
                result[key] = value
            return result
        value = json.loads(data.decode('utf-8'), object_pairs_hook=pairs)
        if type(value) is not dict or set(value) != set(keys) or type(value['format']) is not int or value['format'] != 1:
            raise ValueError('metadata shape/version')
        return value
    except (ValueError, UnicodeError, KeyError, RecursionError) as error:
        raise InvalidData('invalid store metadata: '+str(error)) from error


def _counter(value):
    if not isinstance(value, str) or not re.fullmatch('0|[1-9][0-9]{0,19}', value) or int(value) > 2**64-1:
        raise InvalidData('invalid persisted generation')
    return int(value)


class Validator:
    def __init__(self, executable):
        self.executable = Path(executable).absolute()
        if not self.executable.is_file(): raise StoreError('native nf_data validator is missing')

    def _run(self, mode, data, limit):
        if not isinstance(data, bytes) or len(data) > limit: raise StoreError('native input byte limit')
        try:
            result = subprocess.run([str(self.executable), mode, '-'], input=data, capture_output=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise Retryable('native data validator failed: '+str(error)) from error
        if result.returncode or len(result.stdout) > 32 * 1048576 + 1:
            detail = result.stderr.decode('utf-8', errors='replace')[:4096]
            kind = InvalidData if result.returncode == 1 else Retryable
            raise kind(detail or 'native checkpoint validation failed')
        return result.stdout

    def checkpoint(self, project): return self._run('checkpoint', project, 1048576)
    def normalize(self, checkpoint): return self._run('canonicalize', checkpoint, 32 * 1048576)


class Store:
    _reserved = ('owner.json', 'head.json', 'previous.json', 'sequence.json', 'slot-0.json', 'slot-1.json', '.pending-write')

    def __init__(self, root, validator, fault=lambda stage: None):
        self.root = files.absolute(root)
        self.validator = validator
        self.fault = fault

    def _initialize(self):
        if self.root.exists(): return
        with files.anchor(self.root.parent):
            staging = self.root.parent / ('.nf-store-init-' + uuid.uuid4().hex)
            staging.mkdir()
            owner = {'format': 1, 'kind': 'neonforge.checkpoint-store', 'id': uuid.uuid4().hex}
            empty = {'format': 1, 'kind': 'neonforge.checkpoint-head', 'store': owner['id'], 'generation': '0', 'slot': None, 'sha256': None}
            sequence = {'format': 1, 'kind': 'neonforge.checkpoint-sequence', 'store': owner['id'], 'generation': '0'}
            # Leave an unpublished unique sibling on failure. Never clean a path
            # after its verified directory handle has moved away from that name.
            with files.pinned_directory(staging) as pinned:
                if list(staging.iterdir()): raise StoreError('initialization staging directory is not empty')
                try:
                    files.create(staging / 'owner.json', _encode(owner))
                    files.create(staging / 'head.json', _encode(empty))
                    files.create(staging / 'sequence.json', _encode(sequence))
                    files.move(pinned, self.root, replace=False)
                except StoreError:
                    if not self.root.exists(): raise

    @contextmanager
    def _locked(self):
        try:
            self._initialize()
            with files.anchor(self.root):
                owner = _metadata(files.read(self.root / 'owner.json', 4096), ('format','kind','id'))
                if owner['kind'] != 'neonforge.checkpoint-store' or not isinstance(owner['id'], str) or not re.fullmatch('[0-9a-f]{32}', owner['id']):
                    raise StoreError('unrecognized checkpoint store owner')
                with files.exclusive(self.root / 'LOCK'):
                    for name in self._reserved: files.regular_or_missing(self.root / name)
                    files.remove_reserved(self.root / '.pending-write')
                    yield owner
        except OSError as error:
            raise StoreError(str(error)) from error

    def _read_head(self, name, owner):
        raw = files.read(self.root / name, 4096)
        head = _metadata(raw, ('format','kind','store','generation','slot','sha256'))
        generation = _counter(head['generation'])
        if head['kind'] != 'neonforge.checkpoint-head' or head['store'] != owner['id']:
            raise InvalidData('checkpoint marker belongs to another format/store')
        if generation == 0:
            if head['slot'] is not None or head['sha256'] is not None: raise InvalidData('invalid empty commit marker')
            checkpoint = None
        else:
            if type(head['slot']) is not int or head['slot'] not in (0,1) or not isinstance(head['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', head['sha256']):
                raise InvalidData('invalid checkpoint slot/hash marker')
            checkpoint = files.read(self.root / ('slot-'+str(head['slot'])+'.json'), 32 * 1048576)
            if hashlib.sha256(checkpoint).hexdigest() != head['sha256']: raise InvalidData('checkpoint digest mismatch')
            normalized = self.validator.normalize(checkpoint)
            if checkpoint != normalized: raise InvalidData('stored checkpoint is not canonical native output')
        return {'generation': generation, 'checkpoint': checkpoint, 'digest': head['sha256'],
                'slot': head['slot'], 'head': head, 'recovered': name == 'previous.json',
                'token': {key: head[key] for key in ('store','generation','slot','sha256')}}

    def _load(self, owner):
        errors, valid = [], []
        for name in ('head.json', 'previous.json'):
            try: valid.append(self._read_head(name, owner))
            except (InvalidData, FileNotFoundError) as error: errors.append(str(error))
        if not valid: raise StoreError('no valid committed checkpoint marker; '+ '; '.join(errors)[:4096])
        if len(valid) == 2 and valid[0]['generation'] == valid[1]['generation'] and valid[0]['head'] != valid[1]['head']:
            raise InvalidData('ambiguous same-generation commit markers')
        before = max(valid, key=lambda item: item['generation'])
        sequence = _metadata(files.read(self.root/'sequence.json',4096), ('format','kind','store','generation'))
        if sequence['kind'] != 'neonforge.checkpoint-sequence' or sequence['store'] != owner['id']:
            raise InvalidData('invalid store generation reservation')
        reserved = _counter(sequence['generation'])
        if reserved < before['generation']: raise InvalidData('generation reservation predates committed marker')
        before['reserved'] = reserved
        before['recovered'] = before['recovered'] or reserved > before['generation']
        return before

    def load(self):
        with self._locked() as owner: return self._load(owner)

    def _publish(self, name, data, phase):
        pending = self.root / '.pending-write'
        with files.staged(pending, data, self.fault, phase) as pinned:
            files.move(pinned, self.root / name)
            self.fault(phase + ':renamed')

    def save(self, checkpoint, expected=None):
        canonical = self.validator.normalize(checkpoint)
        digest = hashlib.sha256(canonical).hexdigest()
        with self._locked() as owner:
            before = self._load(owner)
            if expected is not None and expected != before['token']:
                raise Conflict('persisted commit identity conflict; restart/resync before writing')
            if before['checkpoint'] == canonical: return before
            if before['reserved'] == 2**64-1: raise StoreError('persisted generation exhausted')
            slot = 0 if before['slot'] is None else 1 - before['slot']
            head = {'format': 1, 'kind': 'neonforge.checkpoint-head', 'store': owner['id'],
                    'generation': str(before['reserved'] + 1), 'slot': slot, 'sha256': digest}
            result = {'generation': before['reserved']+1, 'checkpoint': canonical, 'digest': digest,
                      'slot': slot, 'head': head, 'recovered': before['recovered'],
                      'token': {key: head[key] for key in ('store','generation','slot','sha256')}}
            reservation = {'format':1,'kind':'neonforge.checkpoint-sequence','store':owner['id'],'generation':head['generation']}
            self._publish('sequence.json', _encode(reservation), 'sequence')
            self._publish('previous.json', _encode(before['head']), 'previous')
            self._publish('slot-'+str(slot)+'.json', canonical, 'slot')
            self._publish('head.json', _encode(head), 'head')
            return result


class Watcher:
    def __init__(self, project, store, validator, settle=.5):
        self.project = files.absolute(project)
        self.store, self.validator = store, validator
        if files.is_within(self.project, store.root):
            raise StoreError('watched source must be outside the owned store')
        if not .01 <= settle <= 60: raise StoreError('settle interval must be 10..60000 ms')
        initial = store.load()
        self.generation, self.checkpoint = initial['generation'], initial['checkpoint']
        self.token = initial['token']
        self.recovery = initial['recovered']
        self.settle = settle
        self.pending = self.finished = self.candidate = None
        self.since = self.retry = 0
        self.last_error = None

    def _error(self, status, error):
        event = {'status': status, 'error': str(error)[:4096], 'generation': str(self.generation)}
        key = (status, event['error'])
        if key == self.last_error: return None
        self.last_error = key
        return event

    def poll(self, now=None):
        now = time.monotonic() if now is None else now
        if self.recovery:
            self.recovery = False
            return {'status':'recovered','generation':str(self.generation),
                    'sha256':hashlib.sha256(self.checkpoint).hexdigest() if self.checkpoint is not None else None}
        try: source = files.read(self.project, 1048576)
        except (StoreError, FileNotFoundError) as error:
            self.pending = self.candidate = None
            return self._error('error', error)
        if source != self.pending:
            self.pending, self.since, self.candidate = source, now, None
            self.retry = 0
            self.last_error = None
            return None
        if source == self.finished or now - self.since < self.settle or now < self.retry: return None
        if self.candidate is None:
            try: self.candidate = self.validator.checkpoint(source)
            except Retryable as error:
                self.retry = now + max(1, self.settle)
                return self._error('error', error)
            except StoreError as error:
                self.finished = source
                return self._error('error', error)
        try: saved = self.store.save(self.candidate, expected=self.token)
        except Conflict as error:
            self.finished = source
            return self._error('conflict', error)
        except StoreError as error:
            self.retry = now + max(1, self.settle)
            return self._error('error', error)
        changed = saved['generation'] != self.generation
        self.generation, self.checkpoint = saved['generation'], saved['checkpoint']
        self.token = saved['token']
        self.finished, self.last_error = source, None
        return {'status': 'committed' if changed else 'unchanged', 'generation': str(self.generation),
                'sha256': saved['digest'], 'recovered': saved['recovered']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['save','load','watch'])
    parser.add_argument('paths', nargs='+', type=Path)
    parser.add_argument('--validator', type=Path, default=Path(__file__).resolve().parents[1] / 'runtime/nf_data.exe')
    parser.add_argument('--poll-ms', type=int, default=250)
    parser.add_argument('--settle-ms', type=int, default=500)
    parser.add_argument('--iterations', type=int, default=0)
    args = parser.parse_args()
    try:
        if len(args.paths) != (1 if args.command == 'load' else 2): parser.error('load requires store; save/watch require project and store')
        validator = Validator(args.validator)
        target = Store(args.paths[-1], validator)
        if args.command == 'load':
            result = target.load()
            if result['checkpoint'] is None: raise StoreError('store has no committed checkpoint')
            if result['recovered']: print('Recovered previous committed checkpoint', file=sys.stderr)
            sys.stdout.buffer.write(result['checkpoint']); sys.stdout.buffer.flush()
        elif args.command == 'save':
            source_path = files.absolute(args.paths[0])
            if files.is_within(source_path, target.root):
                raise StoreError('project source must be outside the owned store')
            source = files.read(source_path, 1048576)
            result = target.save(validator.checkpoint(source))
            print(json.dumps({'generation': str(result['generation']), 'digest': result['digest'], 'recovered': result['recovered']}), flush=True)
        else:
            if not 10 <= args.poll_ms <= 60000 or args.iterations < 0: raise StoreError('invalid polling interval/iteration limit')
            watcher = Watcher(args.paths[0], target, validator, args.settle_ms/1000)
            count = 0
            while not args.iterations or count < args.iterations:
                event = watcher.poll()
                if event: print(json.dumps(event, ensure_ascii=True), flush=True)
                count += 1
                if not args.iterations or count < args.iterations: time.sleep(args.poll_ms/1000)
        return 0
    except KeyboardInterrupt:
        return 0
    except (StoreError, OSError) as error:
        print('NEONFORGE data store: '+str(error), file=sys.stderr)
        return 1


if __name__ == '__main__': raise SystemExit(main())
