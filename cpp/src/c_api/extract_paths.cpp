/*
 * Copyright (c) 2021, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cugraph_c/algorithms.h>

#include <c_api/abstract_functor.hpp>
#include <c_api/graph.hpp>
#include <c_api/paths_result.hpp>

#include <cugraph/algorithms.hpp>
#include <cugraph/detail/utility_wrappers.hpp>
#include <cugraph/graph_functions.hpp>
#include <cugraph/visitors/generic_cascaded_dispatch.hpp>

#include <raft/handle.hpp>

namespace cugraph {
namespace c_api {

struct cugraph_extract_paths_result_t {
  size_t max_path_length_;
  cugraph_type_erased_device_array_t* paths_;
};

struct extract_paths_functor : public abstract_functor {
  raft::handle_t const& handle_;
  cugraph_graph_t* graph_;
  cugraph_type_erased_device_array_t const* sources_;
  cugraph_paths_result_t const* paths_result_;
  cugraph_type_erased_device_array_t const* destinations_;
  cugraph_extract_paths_result_t* result_{};

  extract_paths_functor(raft::handle_t const& handle,
                        cugraph_graph_t* graph,
                        cugraph_type_erased_device_array_t const* sources,
                        cugraph_paths_result_t const* paths_result,
                        cugraph_type_erased_device_array_t const* destinations)
    : abstract_functor(),
      handle_(handle),
      graph_(graph),
      sources_(sources),
      paths_result_(paths_result),
      destinations_(destinations)
  {
  }

  template <typename vertex_t,
            typename edge_t,
            typename weight_t,
            bool store_transposed,
            bool multi_gpu>
  void operator()()
  {
    // FIXME: Think about how to handle SG vice MG
    if constexpr (!cugraph::is_candidate<vertex_t, edge_t, weight_t>::value) {
      unsupported();
    } else {
      // BFS and SSSP expect store_transposed == false
      if constexpr (store_transposed) {
        error_code_ = cugraph::c_api::
          transpose_storage<vertex_t, edge_t, weight_t, store_transposed, multi_gpu>(
            handle_, graph_, error_.get());
        if (error_code_ != CUGRAPH_SUCCESS) return;
      }

      auto graph =
        reinterpret_cast<cugraph::graph_t<vertex_t, edge_t, weight_t, false, multi_gpu>*>(
          graph_->graph_);

      auto graph_view = graph->view();

      auto number_map = reinterpret_cast<rmm::device_uvector<vertex_t>*>(graph_->number_map_);

      rmm::device_uvector<vertex_t> destinations(destinations_->size_, handle_.get_stream());
      raft::copy(destinations.data(),
                 destinations_->as_type<vertex_t>(),
                 destinations_->size_,
                 handle_.get_stream());

      rmm::device_uvector<vertex_t> predecessors(paths_result_->predecessors_->size_,
                                                 handle_.get_stream());
      raft::copy(predecessors.data(),
                 paths_result_->predecessors_->as_type<vertex_t>(),
                 paths_result_->predecessors_->size_,
                 handle_.get_stream());

      //
      // Need to renumber destinations
      //
      renumber_ext_vertices<vertex_t, multi_gpu>(handle_,
                                                 destinations.data(),
                                                 destinations.size(),
                                                 number_map->data(),
                                                 graph_view.get_local_vertex_first(),
                                                 graph_view.get_local_vertex_last(),
                                                 false);

      renumber_ext_vertices<vertex_t, multi_gpu>(handle_,
                                                 predecessors.data(),
                                                 predecessors.size(),
                                                 number_map->data(),
                                                 graph_view.get_local_vertex_first(),
                                                 graph_view.get_local_vertex_last(),
                                                 false);

      auto [result, max_path_length] =
        cugraph::extract_bfs_paths<vertex_t, edge_t, weight_t, multi_gpu>(
          handle_,
          graph_view,
          paths_result_->distances_->as_type<vertex_t>(),
          predecessors.data(),
          destinations.data(),
          destinations.size());

      std::vector<vertex_t> vertex_partition_lasts = graph_view.get_vertex_partition_lasts();

      unrenumber_int_vertices<vertex_t, multi_gpu>(
        handle_, result.data(), result.size(), number_map->data(), vertex_partition_lasts, false);

      result_ = new cugraph_extract_paths_result_t{
        static_cast<size_t>(max_path_length),
        new cugraph_type_erased_device_array_t(result, graph_->vertex_type_)};
    }
  }
};

}  // namespace c_api
}  // namespace cugraph

extern "C" size_t cugraph_extract_paths_result_get_max_path_length(
  cugraph_extract_paths_result_t* result)
{
  auto internal_pointer = reinterpret_cast<cugraph::c_api::cugraph_extract_paths_result_t*>(result);
  return internal_pointer->max_path_length_;
}

cugraph_type_erased_device_array_t* cugraph_extract_paths_result_get_paths(
  cugraph_extract_paths_result_t* result)
{
  auto internal_pointer = reinterpret_cast<cugraph::c_api::cugraph_extract_paths_result_t*>(result);
  return reinterpret_cast<cugraph_type_erased_device_array_t*>(internal_pointer->paths_);
}

extern "C" void cugraph_extract_paths_result_free(cugraph_extract_paths_result_t* result)
{
  auto internal_pointer = reinterpret_cast<cugraph::c_api::cugraph_extract_paths_result_t*>(result);
  delete internal_pointer->paths_;
  delete internal_pointer;
}

extern "C" cugraph_error_code_t cugraph_extract_paths(
  const cugraph_resource_handle_t* handle,
  cugraph_graph_t* graph,
  const cugraph_type_erased_device_array_t* sources,
  const cugraph_paths_result_t* paths_result,
  const cugraph_type_erased_device_array_t* destinations,
  cugraph_extract_paths_result_t** result,
  cugraph_error_t** error)
{
  *result = nullptr;
  *error  = nullptr;

  try {
    auto p_handle = reinterpret_cast<raft::handle_t const*>(handle);
    auto p_graph  = reinterpret_cast<cugraph::c_api::cugraph_graph_t*>(graph);
    auto p_sources =
      reinterpret_cast<cugraph::c_api::cugraph_type_erased_device_array_t const*>(sources);
    auto p_paths_result =
      reinterpret_cast<cugraph::c_api::cugraph_paths_result_t const*>(paths_result);
    auto p_destinations =
      reinterpret_cast<cugraph::c_api::cugraph_type_erased_device_array_t const*>(destinations);

    cugraph::c_api::extract_paths_functor functor(
      *p_handle, p_graph, p_sources, p_paths_result, p_destinations);

    // FIXME:  This seems like a recurring pattern.  Can
    // I encapsulate
    //    The vertex_dispatcher and error handling calls
    //    into a reusable function? After all, we're in
    //    C++ here.
    cugraph::dispatch::vertex_dispatcher(cugraph::c_api::dtypes_mapping[p_graph->vertex_type_],
                                         cugraph::c_api::dtypes_mapping[p_graph->edge_type_],
                                         cugraph::c_api::dtypes_mapping[p_graph->weight_type_],
                                         p_graph->store_transposed_,
                                         p_graph->multi_gpu_,
                                         functor);

    if (functor.error_code_ != CUGRAPH_SUCCESS) {
      *error = reinterpret_cast<cugraph_error_t*>(functor.error_.release());
      return functor.error_code_;
    }

    *result = reinterpret_cast<cugraph_extract_paths_result_t*>(functor.result_);
  } catch (std::exception const& ex) {
    *error = reinterpret_cast<cugraph_error_t*>(new cugraph::c_api::cugraph_error_t{ex.what()});
    return CUGRAPH_UNKNOWN_ERROR;
  }

  return CUGRAPH_SUCCESS;
}
