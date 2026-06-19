#!/usr/bin/env python3
"""
embed_binary.py - Embed binary files into kernel C source.

Generates a C file containing binary arrays and an embed_init() function
that registers them in ramfs at boot time.

Usage:
    python3 scripts/embed_binary.py userspace/hello kernel/embedded_files.c
    python3 scripts/embed_binary.py userspace/shell kernel/embedded_files.c

This is a fully self-written build tool for AuroraOS.
"""
import sys
import os

if len(sys.argv) != 3:
    print("Usage: embed_binary.py <input-binary> <output-c-path>")
    sys.exit(2)

inp = sys.argv[1]
out = sys.argv[2]
name = os.path.basename(inp)
varname = name.replace('.', '_')

with open(inp, 'rb') as f:
    data = f.read()

# If output exists, append array and add registration into embed_init
if os.path.exists(out):
    # Append new array
    with open(out, 'a') as f:
        f.write('\nstatic const unsigned char %s_data[] = {' % varname)
        for i, b in enumerate(data):
            if i % 12 == 0:
                f.write('\n    ')
            f.write('0x%02x, ' % b)
        f.write('\n};\n')

    # Insert ramfs_add_file_data call before final closing brace
    with open(out, 'r') as f:
        content = f.read()
    insert_point = content.rfind('\n}')
    if insert_point != -1:
        new_call = ('    ramfs_add_file_data("/%s", %s_data, sizeof(%s_data));\n'
                     % (name, varname, varname))
        content = content[:insert_point] + new_call + content[insert_point:]
        with open(out, 'w') as f:
            f.write(content)

    print('Appended %s to %s' % (inp, out))
else:
    # Create new file
    with open(out, 'w') as f:
        f.write('#include "include/log.h"\n')
        f.write('#include <stddef.h>\n')
        f.write('#include <stdint.h>\n')
        f.write('extern int ramfs_add_file_data(const char *name, '
                'const void *data, size_t size);\n')
        f.write('\n')
        f.write('static const unsigned char %s_data[] = {' % varname)
        for i, b in enumerate(data):
            if i % 12 == 0:
                f.write('\n    ')
            f.write('0x%02x, ' % b)
        f.write('\n};\n')
        f.write('\n')
        f.write('void embed_init(void) {\n')
        f.write('    ramfs_add_file_data("/%s", %s_data, sizeof(%s_data));\n'
                % (name, varname, varname))
        f.write('    log_printf(2, "Embedded file /%s into ramfs '
                '(size=%d)\\n", "%s", (int)sizeof(%s_data));\n'
                % (name, len(data), name, varname))
        f.write('}\n')

    print('Wrote %s' % out)
