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

velox_add_library(
  velox_functions_spark_specialforms
  AtLeastNNonNulls.cpp
  DecimalRound.cpp
  FromJson.cpp
  GetStructField.cpp
  MakeDecimal.cpp
  SparkCastExpr.cpp
  SparkCastHooks.cpp)

velox_link_libraries(
  velox_functions_spark_specialforms
  fmt::fmt
  velox_functions_lib
  velox_functions_json
  velox_expression)
