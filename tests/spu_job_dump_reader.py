"""Reader for the SPU job dump files. The format is in spu_job_dump.inc.

    import spu_job_dump_reader as reader
    jobs = reader.read_dir('E:/captures/pvs')      # jobs sorted by sequence
    for job in jobs:
        print(reader.summary(job))
    for record in job.records:
        if record['tag'] == reader.TAG_DMA:
            print('%s lsa=0x%05X ea=0x%08X size=%u' % ('GET' if record['is_get'] else 'PUT',
                                                         record['lsa'], record['ea'], record['size']))

A file that does not follow the format raises DumpError at the first defect.
"""
import struct
from pathlib import Path

LS_SIZE = 256 * 1024
MAGIC = 0x55444A53
VERSION = 1
HEADER_SIZE = 192
TAG_DMA, TAG_EXIT_POINT, TAG_ENTRY_LS, TAG_EXIT_LS, TAG_END = 1, 2, 3, 4, 5
DMA_HEAD, EXIT_HEAD, END_HEAD = 64, 32, 64
DMA_CAP = 32 * 1024 * 1024
CLOSE = {1: 'next entry', 2: 'select workload', 3: 'process exit', 4: 'slot reused', 5: 'dump off'}

class DumpError(ValueError):
    """The file does not follow the format."""


def _fail(path, text):
    raise DumpError('%s: %s' % (path, text))


def _exit_head(body, path):
    if len(body) != EXIT_HEAD:
        _fail(path, 'bad exit head size')
    index, pc, r0, r1, r3, r4 = struct.unpack_from('<6I', body, 0)
    return dict(index=index, pc=pc, r0=r0, r1=r1, r3=r3, r4=r4,
                time=struct.unpack_from('<Q', body, 24)[0])


class Job:
    """One job: the header fields, the records in file order, two local stores."""

    def __init__(self, path):
        self.path = str(path)
        data = Path(path).read_bytes()
        if len(data) < HEADER_SIZE + 16 + LS_SIZE + 16 + END_HEAD:
            _fail(self.path, '%d bytes is too short' % len(data))
        (magic, version, header_size, self.seq, self.image, self.entry, self.ctx_image,
         self.spu_id, self.thread, self.spans) = struct.unpack_from('<10I', data, 0)
        if magic != MAGIC:
            _fail(self.path, 'magic 0x%08X' % magic)
        if version != VERSION or header_size != HEADER_SIZE:
            _fail(self.path, 'version %u, header size %u' % (version, header_size))
        if self.spans > 4 or any(data[0xA0:HEADER_SIZE]):
            _fail(self.path, 'bad span count, or a reserved byte is set')
        self.ctx = struct.unpack_from('<Q', data, 0x28)[0]
        self.r3 = struct.unpack_from('<4I', data, 0x30)
        self.r4 = struct.unpack_from('<4I', data, 0x40)
        self.t_entry, self.t_close, self.t_exit = struct.unpack_from('<3Q', data, 0x50)
        self.exit_pc, self.close_reason = struct.unpack_from('<2I', data, 0x68)
        self.resident = [struct.unpack_from('<3I', data, 0x70 + 12 * i) for i in range(4)]
        if any(any(self.resident[i]) for i in range(self.spans, 4)):
            _fail(self.path, 'a resident row past the span count is set')
        self.records = []
        self.entry_ls = None
        self.exit_ls = None
        self.exit_head = None
        self.end = None
        pos, number, dma, exits = HEADER_SIZE, 0, 0, 0
        tags = []
        while pos < len(data):
            if len(data) - pos < 16:
                _fail(self.path, 'record head is cut')
            tag, payload, recno, zero = struct.unpack_from('<4I', data, pos)
            if recno != number or zero or payload % 16 or len(data) - pos - 16 < payload:
                _fail(self.path, 'bad record head at 0x%X' % pos)
            body = data[pos + 16:pos + 16 + payload]
            pos += 16 + payload
            number += 1
            tags.append(tag)
            if self.end is not None:
                _fail(self.path, 'a record follows the end record')
            if self.exit_ls is not None and tag != TAG_END:
                _fail(self.path, 'a record follows the exit local store')
            if tag == TAG_ENTRY_LS:
                if number != 1 or payload != LS_SIZE:
                    _fail(self.path, 'bad entry local store record')
                self.entry_ls = body
            elif tag == TAG_DMA:
                if payload < DMA_HEAD or any(body[56:DMA_HEAD]):
                    _fail(self.path, 'short DMA record, or a reserved DMA byte is set')
                (is_get, cmd, lsa, ea, size, pc, image, before, in_span, flags) = struct.unpack_from('<10I', body, 0)
                time, index, pad = struct.unpack_from('<Q2I', body, 40)
                if pad or flags & ~1 or is_get not in (0, 1) or in_span not in (0, 1) or not size:
                    _fail(self.path, 'bad DMA fields')
                dma += 1
                stored = not (flags & 1)
                want = DMA_HEAD + ((size + 15) // 16 * 16 if stored else 0)
                if payload != want:
                    _fail(self.path, 'DMA payload %u, expected %u' % (payload, want))
                if stored and any(body[DMA_HEAD + size:want]):
                    _fail(self.path, 'DMA padding is not zero')
                self.records.append(dict(tag=tag, is_get=is_get, cmd=cmd, lsa=lsa, ea=ea, size=size,
                                         pc=pc, image=image, exits_before=before, in_span=in_span,
                                         flags=flags, time=time, index=index,
                                         bytes=body[DMA_HEAD:DMA_HEAD + size] if stored else None))
            elif tag == TAG_EXIT_POINT:
                if payload != EXIT_HEAD:
                    _fail(self.path, 'bad exit point record')
                exits += 1
                head = _exit_head(body, self.path)
                if head['index'] != exits:
                    _fail(self.path, 'exit number %u, expected %u' % (head['index'], exits))
                self.records.append(dict(tag=tag, **head))
            elif tag == TAG_EXIT_LS:
                if payload != EXIT_HEAD + LS_SIZE or not exits:
                    _fail(self.path, 'bad exit local store record')
                head = _exit_head(body[:EXIT_HEAD], self.path)
                last = [r for r in self.records if r['tag'] == TAG_EXIT_POINT][-1]
                if any(head[k] != last[k] for k in ('index', 'pc', 'r0', 'r1', 'r3', 'r4', 'time')):
                    _fail(self.path, 'exit local store head differs from the last exit')
                self.exit_head = head
                self.exit_ls = body[EXIT_HEAD:]
            elif tag == TAG_END:
                if payload != END_HEAD or any(body[56:END_HEAD]):
                    _fail(self.path, 'bad end record')
                (dma_n, no_data, lost, exit_n, buffer, reason, get_bytes, put_bytes, get_n, put_n,
                 job_ns) = struct.unpack_from('<6I2Q2IQ', body, 0)
                self.end = dict(dma=dma_n, no_data=no_data, lost=lost, exits=exit_n, buffer=buffer,
                                reason=reason, get_bytes=get_bytes, put_bytes=put_bytes,
                                get_n=get_n, put_n=put_n, job_ns=job_ns)
            else:
                _fail(self.path, 'unknown record tag %u' % tag)
        end = self.end
        if tags[:1] != [TAG_ENTRY_LS] or tags[-1:] != [TAG_END] or end is None:
            _fail(self.path, 'records %s' % tags)
        if (end['dma'], end['exits'], end['buffer'], end['reason']) != (
                dma, exits, dma + exits, self.close_reason):
            _fail(self.path, 'end record counts differ from the records')
        if bool(self.exit_ls) != (exits > 0):
            _fail(self.path, 'exit local store record and exit count differ')
        if self.exit_ls:
            if self.exit_pc != self.exit_head['pc'] or self.t_exit != self.exit_head['time']:
                _fail(self.path, 'header exit fields differ from the last exit')
        elif self.exit_pc != 0xFFFFFFFF or self.t_exit:
            _fail(self.path, 'exit fields are set without an exit')
        gets = [r for r in self.records if r['tag'] == TAG_DMA and r['is_get']]
        puts = [r for r in self.records if r['tag'] == TAG_DMA and not r['is_get']]
        stored = [r for r in self.records if r['tag'] == TAG_DMA and r['bytes'] is not None]
        if (end['get_n'], end['put_n'], end['no_data'], end['dma']) != (
                len(gets), len(puts), dma - len(stored), len(stored) + end['no_data']):
            _fail(self.path, 'end record record counts differ from the DMA records')
        if (end['get_bytes'], end['put_bytes']) != (sum(r['size'] for r in gets),
                                                    sum(r['size'] for r in puts)):
            _fail(self.path, 'end record byte counts differ from the DMA records')
        if end['get_bytes'] + end['put_bytes'] > DMA_CAP:
            _fail(self.path, 'more DMA bytes than the limit')
        if [r['index'] for r in self.records if r['tag'] == TAG_DMA] != list(range(1, dma + 1)):
            _fail(self.path, 'DMA record numbers are not 1..%u' % dma)
        times = [r['time'] for r in self.records if r['tag'] != TAG_ENTRY_LS]
        if not (self.t_entry <= self.t_close) or times != sorted(times) or (
                exits and not self.t_entry <= self.t_exit <= self.t_close) or (
                times and not self.t_entry <= times[0] <= times[-1] <= self.t_close):
            _fail(self.path, 'times are not in order')
        if end['job_ns'] != self.t_close - self.t_entry:
            _fail(self.path, 'job time differs from the header')

    def dma(self):
        """The DMA records in time order."""
        return [r for r in self.records if r['tag'] == TAG_DMA]

    def exits(self):
        """The exit point records in time order."""
        return [r for r in self.records if r['tag'] == TAG_EXIT_POINT]


def summary(job):
    """One line per job."""
    end = job.end
    return ('job %u: image=%u spu=%u close=%s dma=%u get=%u/%u put=%u/%u exits=%u job_ms=%.3f' % (
        job.seq, job.ctx_image, job.spu_id, CLOSE.get(job.close_reason, '?'), end['dma'],
        end['get_n'], end['get_bytes'], end['put_n'], end['put_bytes'], end['exits'],
        end['job_ns'] / 1e6))


def read_dir(path):
    """The jobs of a directory, sorted by sequence number."""
    jobs = [Job(p) for p in sorted(Path(path).glob('job_*.bin'))]
    if [job.seq for job in jobs] != list(range(1, len(jobs) + 1)):
        _fail(path, 'sequence numbers %s' % [job.seq for job in jobs])
    return jobs


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Summarize SPU job dump files.')
    parser.add_argument('path', help='a dump directory or one job file')
    parser.add_argument('--dma', action='store_true', help='list the DMA records of each job')
    args = parser.parse_args()
    path = Path(args.path)
    jobs = read_dir(path) if path.is_dir() else [Job(path)]
    for job in jobs:
        print(summary(job))
        if not args.dma:
            continue
        for record in job.records:
            if record['tag'] == TAG_DMA:
                print('  %s lsa=0x%05X ea=0x%08X size=%u cmd=0x%02X pc=0x%05X exits=%u span=%u' % (
                    'GET' if record['is_get'] else 'PUT', record['lsa'], record['ea'], record['size'],
                    record['cmd'], record['pc'], record['exits_before'], record['in_span']))
            elif record['tag'] == TAG_EXIT_POINT:
                print('  exit %u at 0x%05X r0=0x%05X r3=0x%08X r4=0x%08X' % (
                    record['index'], record['pc'], record['r0'], record['r3'], record['r4']))


if __name__ == '__main__':
    main()
