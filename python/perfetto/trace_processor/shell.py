#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import subprocess
import sys
import time
from urllib import request, error

from perfetto.trace_processor.platform import PlatformDelegate

# Default port that trace_processor_shell runs on
TP_PORT = 9001


def load_shell(bin_path: str, unique_port: bool, verbose: bool,
               platform_delegate: PlatformDelegate):
  addr, port = platform_delegate.get_bind_addr(
      port=0 if unique_port else TP_PORT)
  url = f'{addr}:{str(port)}'

  shell_path = platform_delegate.get_shell_path(bin_path=bin_path)
  if os.name == 'nt' and not shell_path.endswith('.exe'):
    tp_exec = [sys.executable, shell_path]
  else:
    tp_exec = [shell_path]

  p = subprocess.Popen(
      tp_exec + ['-D', '--http-port', str(port)],
      stdout=subprocess.DEVNULL,
      stderr=None if verbose else subprocess.DEVNULL)

  while True:
    try:
      if p.poll() != None:
        if unique_port:
          raise Exception(
              "Random port allocation failed, please file a bug at https://goto.google.com/perfetto-bug"
          )
        raise Exception(
            "Trace processor failed to start, please file a bug at https://goto.google.com/perfetto-bug"
        )
      _ = request.urlretrieve(f'http://{url}/status')
      time.sleep(1)
      break
    except error.URLError:
      pass

  return url, p
