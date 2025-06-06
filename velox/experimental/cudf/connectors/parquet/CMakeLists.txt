# Copyright (c) Facebook, Inc. and its affiliates.
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

add_library(velox_cudf_parquet_config ParquetConfig.cpp)

set_target_properties(
  velox_cudf_parquet_config
  PROPERTIES CUDA_ARCHITECTURES native)

target_link_libraries(
  velox_cudf_parquet_config velox_core velox_exception cudf::cudf)

add_library(
  velox_cudf_parquet_connector OBJECT
  ParquetConfig.cpp
  ParquetConnector.cpp
  ParquetConnectorSplit.cpp
  ParquetDataSource.cpp
  ParquetDataSink.cpp
  ParquetTableHandle.cpp)

set_target_properties(
  velox_cudf_parquet_connector
  PROPERTIES CUDA_ARCHITECTURES native)

target_link_libraries(
  velox_cudf_parquet_connector
  PRIVATE
    cudf::cudf
    velox_common_io
    velox_connector
    velox_type_tz
    velox_gcs)
