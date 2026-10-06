"""Read every PE32 export slot, including unnamed slots and aliases."""
import struct
from pathlib import Path


class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        assert self.data[:2] == b'MZ'
        self.pe = self.u32(0x3c)
        assert self.data[self.pe:self.pe + 4] == b'PE\0\0'
        self.machine, count, _, _, self.symbol_count, optional_size, self.flags = struct.unpack_from('<HHIIIHH', self.data, self.pe + 4)
        optional = self.pe + 24
        assert self.machine == 0x14c and self.u16(optional) == 0x10b
        self.entry = self.u32(optional + 16)
        self.image_base = self.u32(optional + 28)
        self.export_rva, self.export_size = struct.unpack_from('<II', self.data, optional + 96)
        self.sections = []
        for index in range(count):
            offset = optional + optional_size + index * 40
            name = self.data[offset:offset + 8].split(b'\0')[0].decode()
            size, rva, raw_size, raw = struct.unpack_from('<IIII', self.data, offset + 8)
            self.sections.append({'name': name, 'rva': rva, 'virtual_size': size, 'raw_size': raw_size,
                                  'raw_offset': raw, 'flags': self.u32(offset + 36)})

    def u16(self, offset):
        return struct.unpack_from('<H', self.data, offset)[0]

    def u32(self, offset):
        return struct.unpack_from('<I', self.data, offset)[0]

    def offset(self, rva, size=1):
        for section in self.sections:
            delta = rva - section['rva']
            if 0 <= delta and delta + size <= section['raw_size']:
                return section['raw_offset'] + delta
        raise ValueError('RVA has no complete initialized file payload')

    def string(self, rva):
        start = self.offset(rva)
        end = self.data.index(b'\0', start)
        return self.data[start:end].decode('ascii')

    def exports(self):
        fields = struct.unpack_from('<IIHHIIIIIII', self.data, self.offset(self.export_rva, 40))
        base, count, name_count, eat, names, ordinals = fields[5:]
        by_index = {}
        name_rows = []
        for index in range(name_count):
            ordinal_index = self.u16(self.offset(ordinals + index * 2, 2))
            name = self.string(self.u32(self.offset(names + index * 4, 4)))
            by_index.setdefault(ordinal_index, []).append(name)
            name_rows.append({'name': name, 'ordinal_index': ordinal_index,
                              'ordinal_field_offset': self.offset(ordinals + index * 2, 2)})
        rows = []
        for index in range(count):
            rva = self.u32(self.offset(eat + index * 4, 4))
            executable = any(s['rva'] <= rva < s['rva'] + s['virtual_size'] and s['flags'] & 0x20000000
                             for s in self.sections)
            forwarder = self.string(rva) if self.export_rva <= rva < self.export_rva + self.export_size else None
            rows.append({'index': index, 'ordinal': base + index, 'rva': rva,
                         'names': by_index.get(index, []), 'executable': bool(executable),
                         'zero_RVA_hole': rva == 0, 'forwarder': forwarder,
                         'EAT_field_offset': self.offset(eat + index * 4, 4)})
        return rows, name_rows

    def truth(self):
        rows, _ = self.exports()
        code = {}
        for row in rows:
            if row['rva'] and row['executable'] and row['forwarder'] is None:
                code.setdefault(row['rva'], []).extend(row['names'])
        return {'machine': hex(self.machine), 'DLL_flag': bool(self.flags & 0x2000),
                'entrypoint_RVA': self.entry, 'COFF_symbol_count': self.symbol_count,
                'all_EAT_slots': rows, 'all_unique_executable_export_RVAs': sorted(code),
                'names_by_executable_RVA': {hex(rva): sorted(names) for rva, names in code.items()},
                'expected_executable_function_roots': len(code),
                'sections': self.sections}
