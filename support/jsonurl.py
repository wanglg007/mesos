#!/usr/bin/env python
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
A utility that expects JSON data at a particular URL and lets you
recursively extract keys from the JSON object as specified on the
command line (each argument on the command line after the first will
be used to recursively index into the JSON object). The name is a
play off of 'curl'.
"""

import json
import os
import subprocess
import sys
import urllib2


def main():
    """Expects at least one argument on the command line."""
    if len(sys.argv) < 2:
        print >> sys.stderr, "USAGE: {} URL [KEY...]".format(sys.argv[0])
        sys.exit(1)

    # TODO(ArmandGrillet): Remove this when we'll have switched to Python 3.
    dir_path = os.path.dirname(os.path.realpath(__file__))
    script_path = os.path.join(dir_path, 'check-python3.py')
    subprocess.call('python ' + script_path, shell=True, cwd=dir_path)

    url = sys.argv[1]

    data = json.loads(urllib2.urlopen(url).read())

    for arg in sys.argv[2:]:
        try:
            temp = data[arg]
            data = temp
        except KeyError:
            print >> sys.stderr, "'" + arg + "' was not found"
            sys.exit(1)

    print data.encode("utf-8")

if __name__ == '__main__':
    main()
