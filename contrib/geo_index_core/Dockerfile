# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# Reference test env for geo_index_core.  libs2-dev 0.10.0 = the S2 version
# Doris ships in thirdparty on master, branch-4.1 and branch-3.1.
#   docker build -t geo-index-core contrib/geo_index_core
#   docker run --rm -v "$PWD/contrib/geo_index_core":/src geo-index-core
FROM ubuntu:24.04
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       cmake g++ make libs2-dev libgtest-dev valgrind python3 \
    && rm -rf /var/lib/apt/lists/*
ENV BUILD_ROOT=/tmp/build
WORKDIR /src
CMD ["/src/run_tests.sh"]
