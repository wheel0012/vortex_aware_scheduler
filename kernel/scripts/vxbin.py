#!/usr/bin/env python3

# Copyright 2019-2023
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import shutil
import subprocess
import struct
import sys
import re

def get_vma_size(elf_file):
    try:
        cmd = ['readelf', '-l', '-W', elf_file]
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        output, errors = process.communicate()
        if process.returncode != 0:
            print("Error running readelf: {}".format(errors.strip()))
            sys.exit(-1)

        min_vma = 2**64 - 1
        max_vma = 0
        regex = re.compile(r'\s*LOAD\s+(\w+)\s+(\w+)\s+(\w+)\s+(\w+)\s+(\w+)')

        for line in output.splitlines():
            match = regex.match(line)
            if match:
                vma = int(match.group(2), 16)
                size = int(match.group(5), 16)
                end_vma = vma + size
                min_vma = min(min_vma, vma)
                max_vma = max(max_vma, end_vma)
                vma_size = max_vma - min_vma
                #print("vma={0:x}, size={1}, min_vma=0x{2:x}, max_vma=0x{3:x}, vma_size={4}".format(vma, size, min_vma, max_vma, vma_size))

        return min_vma, max_vma

    except Exception as e:
        print("Failed to calculate vma size due to an error: {}".format(str(e)))
        sys.exit(-1)

def create_vxbin_binary(input_elf, output_bin, objcopy_path):
    min_vma, max_vma = get_vma_size(input_elf)

    save_elf_dir = os.getenv('VXBIN_SAVE_ELF_DIR')
    if save_elf_dir:
        os.makedirs(save_elf_dir, exist_ok=True)
        base_name = os.path.basename(input_elf) or 'program.elf'
        if not base_name.endswith('.elf'):
            base_name += '.elf'
        save_path = os.path.join(save_elf_dir, base_name)
        index = 1
        while os.path.exists(save_path):
            stem, ext = os.path.splitext(base_name)
            save_path = os.path.join(save_elf_dir, '{}_{}{}'.format(stem, index, ext))
            index += 1
        shutil.copy2(input_elf, save_path)
        objdump_path = os.getenv('OBJDUMP')
        if objdump_path is None:
            objcopy_name = os.path.basename(objcopy_path)
            if objcopy_name == 'llvm-objcopy':
                objdump_path = os.path.join(os.path.dirname(objcopy_path), 'llvm-objdump')
            else:
                objdump_path = 'objdump'
        dump_path = os.path.splitext(save_path)[0] + '.dump'
        try:
            with open(dump_path, 'w') as dump_file:
                subprocess.check_call(
                    [objdump_path, '-d', input_elf],
                    stdout=dump_file,
                )
        except Exception as e:
            print("Warning: failed to create objdump file {}: {}".format(dump_path, str(e)), file=sys.stderr)

    # Create a binary data from the ELF file using objcopy
    temp_bin_path = '/tmp/temp_kernel.bin'
    subprocess.check_call([objcopy_path, '-O', 'binary', input_elf, temp_bin_path])

    # Read the binary file to determine its size
    with open(temp_bin_path, 'rb') as temp_file:
        binary_data = temp_file.read()

    # Pack addresses into 64-bit unsigned integer
    min_vma_bytes = struct.pack('<Q', min_vma)
    max_vma_bytes = struct.pack('<Q', max_vma)

    # Write the total size and binary data to the final output file
    with open(output_bin, 'wb') as bin_file:
        bin_file.write(min_vma_bytes)
        bin_file.write(max_vma_bytes)
        bin_file.write(binary_data)

    # Remove the temporary binary file
    os.remove(temp_bin_path)
    # print("Binary created successfully: {}, min_vma={:x}, max_vma={:x}".format(output_bin, min_vma, max_vma))

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: vxbin.py <input>.elf <output>.vxbin")
        sys.exit(-1)

    objcopy_path = os.getenv('OBJCOPY', 'objcopy')  # Default to 'objcopy' if not set

    create_vxbin_binary(sys.argv[1], sys.argv[2], objcopy_path)
