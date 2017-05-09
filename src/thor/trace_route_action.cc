#include <prime_server/prime_server.hpp>
using namespace prime_server;

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "midgard/logging.h"
#include "baldr/geojson.h"
#include "baldr/pathlocation.h"
#include "baldr/errorcode_util.h"
#include "meili/map_matcher.h"

#include "thor/service.h"
#include "thor/route_matcher.h"
#include "thor/map_matcher.h"
#include "thor/match_result.h"
#include "thor/trippathbuilder.h"
#include "thor/attributes_controller.h"

using namespace valhalla;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::odin;
using namespace valhalla::thor;

namespace {
struct MapMatch
{
  // Coordinate of the match point
  midgard::PointLL lnglat;
  // Which edge this match point stays
  baldr::GraphId edgeid;
  // Percentage distance along the edge
  float distance_along;
  int edge_index = -1;
};

}

namespace valhalla {
namespace thor {

/*
 * The trace_route action takes a GPS trace and turns it into a route result.
 */
worker_t::result_t thor_worker_t::trace_route(const boost::property_tree::ptree &request,
    const std::string &request_str, const bool header_dnt) {
  //get time for start of request
  auto s = std::chrono::system_clock::now();

  // Parse request
  parse_locations(request);
  parse_shape(request);
  parse_costing(request);
  parse_trace_config(request);
  /*
   * A flag indicating whether the input shape is a GPS trace or exact points from a
   * prior route run against the Valhalla road network.  Knowing that the input is from
   * Valhalla will allow an efficient “edge-walking” algorithm rather than a more extensive
   * map-matching method. If true, this enforces to only use exact route match algorithm.
   */
  odin::TripPath trip_path;
  std::vector<thor::MatchResult> match_results;
  std::pair<odin::TripPath, std::vector<thor::MatchResult>> trip_match;
  AttributesController controller;

  worker_t::result_t result { true };
  // Forward the original request
  result.messages.emplace_back(request_str);

  auto shape_match = STRING_TO_MATCH.find(request.get<std::string>("shape_match", "walk_or_snap"));
  if (shape_match == STRING_TO_MATCH.cend())
    throw valhalla_exception_t{400, 445};
  else {
    // If the exact points from a prior route that was run against the Valhalla road network,
    // then we can traverse the exact shape to form a path by using edge-walking algorithm
    switch (shape_match->second) {
      case EDGE_WALK:
        try {
          trip_path = route_match(controller);
          if (trip_path.node().size() == 0)
            throw valhalla_exception_t{400, 443};
        } catch (const valhalla_exception_t& e) {
          throw valhalla_exception_t{400, 443, shape_match->first + " algorithm failed to find exact route match.  Try using shape_match:'walk_or_snap' to fallback to map-matching algorithm"};
        }
        break;
      // If non-exact shape points are used, then we need to correct this shape by sending them
      // through the map-matching algorithm to snap the points to the correct shape
      case MAP_SNAP:
        try {
          trip_match = map_match(controller);
          trip_path = std::move(trip_match.first);
          match_results = std::move(trip_match.second);
        } catch(const valhalla_exception_t& e) {
          throw valhalla_exception_t{e.status_code, e.error_code, e.extra};
        }
        break;
      //If we think that we have the exact shape but there ends up being no Valhalla route match, then
      // then we want to fallback to try and use meili map matching to match to local route network.
      //No shortcuts are used and detailed information at every intersection becomes available.
      case WALK_OR_SNAP:
        trip_path = route_match(controller);
        if (trip_path.node().size() == 0) {
          LOG_WARN(shape_match->first + " algorithm failed to find exact route match; Falling back to map_match...");
          try {
            trip_match = map_match(controller);
            trip_path = std::move(trip_match.first);
            match_results = std::move(trip_match.second);
          } catch(const valhalla_exception_t& e) {
            throw valhalla_exception_t{e.status_code, e.error_code, e.extra};
          }
        }
        break;
      }
      log_admin(trip_path);
    }

  result.messages.emplace_back(trip_path.SerializeAsString());

  // Get processing time for thor
  auto e = std::chrono::system_clock::now();
  std::chrono::duration<float, std::milli> elapsed_time = e - s;
  //log request if greater than X (ms)
  if (!healthcheck && !header_dnt
      && (elapsed_time.count() / shape.size()) > (long_request / 1100)) {
    LOG_WARN(
        "thor::trace_route elapsed time (ms)::"
            + std::to_string(elapsed_time.count()));
    LOG_WARN("thor::trace_route exceeded threshold::" + request_str);
    midgard::logging::Log("valhalla_thor_long_request_trace_route",
                          " [ANALYTICS] ");
  }
  return result;
}


/*
 * Returns trip path using an “edge-walking” algorithm.
 * This is for use when the input shape is exact shape from a prior Valhalla route.
 * This will walk the input shape and compare to Valhalla edge’s end node positions to
 * form the list of edges. It will return no nodes if path not found.
 *
 */
odin::TripPath thor_worker_t::route_match(const AttributesController& controller) {
  odin::TripPath trip_path;
  std::vector<PathInfo> path_infos;
  if (RouteMatcher::FormPath(mode_costing, mode, reader, shape, correlated, path_infos)) {
    // Form the trip path based on mode costing, origin, destination, and path edges
    trip_path = thor::TripPathBuilder::Build(controller, reader, mode_costing,
                                             path_infos, correlated.front(),
                                             correlated.back(), std::list<PathLocation>{},
                                             interrupt_callback);
  }

  return trip_path;
}

// Form the path from the map-matching results. This path gets sent to TripPathBuilder.
// PathInfo is primarily a list of edge Ids but it also include elapsed time to the end
// of each edge. We will need to use the existing costing method to form the elapsed time
// the path. We will start with just using edge costs and will add transition costs.
std::pair<odin::TripPath, std::vector<thor::MatchResult>> thor_worker_t::map_match(
    const AttributesController& controller, bool trace_attributes_action) {
  odin::TripPath trip_path;
  std::vector<thor::MatchResult> match_results;

  // Call Meili for map matching to get a collection of pathLocation Edges
  // Create a matcher
  std::shared_ptr<meili::MapMatcher> matcher;
  try {
    matcher.reset(matcher_factory.Create(trace_config));
  } catch (const std::invalid_argument& ex) {
    //return jsonify_error({400, 499}, request_info, std::string(ex.what()));
    throw std::runtime_error(std::string(ex.what()));
  }

  matcher->set_interrupt(interrupt_callback);
  std::vector<meili::Measurement> sequence;
  for (const auto& coord : shape) {
    sequence.emplace_back(coord,
                          matcher->config().get<float>("gps_accuracy"),
                          matcher->config().get<float>("search_radius"));
  }

  // Create the vector of matched path results
  std::vector<meili::MatchResult> results;
  if (sequence.size() > 0) {
    results = (matcher->OfflineMatch(sequence));
  }


  // Form the path edges based on the matched points and populate disconnected edges
  std::vector<std::pair<GraphId, GraphId>> disconnected_edges;
  std::vector<PathInfo> path_edges = MapMatcher::FormPath(matcher.get(),
      results, mode_costing, mode, disconnected_edges, trace_attributes_action);

  if (trace_attributes_action) {
    // Associate match points to edges, if enabled
    if (controller.category_attribute_enabled(kMatchedCategory)) {
      // Populate for matched points so we have 1:1 with trace points
      for (const auto& match_result : results) {
        // Matched type is set in constructor
        match_results.emplace_back(match_result);
      }

      // Iterate over results to set edge_index, if found
      int edge_index = 0;
      int match_index = 0;
      auto edge = path_edges.cbegin();
      for (auto& match_result : match_results) {
        // Check for result for valid edge id
        if (match_result.edgeid.Is_Valid()) {
          // Walk edges to find matching id
          while (edge != path_edges.cend()) {
            // Find matching edge id in path
            if (match_result.edgeid == edge->edgeid) {
              // Set match result with matched edge index and break out of the loop
              match_result.edge_index = edge_index;
              break;
            } else {
              // Increment to next edge
              ++edge;
              ++edge_index;
            }
          }
        }
      }

      // Mark the disconnected route boundaries
      auto curr_match_result = match_results.begin();
      auto prev_match_result = curr_match_result;
      for (auto& disconnected_edge_pair : disconnected_edges) {

        // Find previous(first) edge within the match results
        while (curr_match_result != match_results.end()) {
          if (curr_match_result->edgeid == disconnected_edge_pair.first.value) {
            // Found previous edge therefore stop looking
            break;
          }
          // Increment previous and current match results to continue looking
          prev_match_result = curr_match_result;
          ++curr_match_result;
        }

        // Find the last match result of the previous edge
        while (curr_match_result != match_results.end()) {
          if (curr_match_result->edgeid != disconnected_edge_pair.first.value) {
            // Set previous match result as disconnected path and break
            prev_match_result->begin_route_discontinuity = true;
            break;
          }
          // Increment previous and current match results to continue looking
          prev_match_result = curr_match_result;
          ++curr_match_result;
        }

        // Find the current(second) edge within the match results
        while (curr_match_result != match_results.end()) {
          if (curr_match_result->edgeid == disconnected_edge_pair.second.value) {
            // Set current match result as disconnected and break
            curr_match_result->end_route_discontinuity = true;
            break;
          }
          // Increment previous and current match results to continue looking
          prev_match_result = curr_match_result;
          ++curr_match_result;
        }
      }
    }

#ifdef LOGGING_LEVEL_TRACE
    ////////////////////////////////////////////////////////////////////////////
    // This trace block is used to visualize the trace and matched points
    // Print geojson header
    printf("\n{\"type\":\"FeatureCollection\",\"features\":[\n");

    // Print trace points
    int index = 0;
    for (const auto& trace_point : shape) {
      printf("{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[%.6f,%.6f]},\"properties\":{\"marker-color\":\"#abd9e9\",\"marker-size\":\"small\",\"trace_point_index\":%d}},\n", trace_point.first, trace_point.second, index++);
    }

    // Print matched points
    index = 0;
    std::string marker_color;
    std::string marker_size;
    std::string matched_point_type;
    for (const auto& match_result : match_results) {
      if (match_result.begin_route_discontinuity || match_result.end_route_discontinuity) {
        marker_color = "#d7191c"; // red
        marker_size = "large";
        if (match_result.type == thor::MatchResult::Type::kMatched)
          matched_point_type = "matched";
        else
          matched_point_type = "interpolated";
      } else if (match_result.type == thor::MatchResult::Type::kMatched) {
        marker_color = "#2c7bb6"; // dark blue
        marker_size = "medium";
        matched_point_type = "matched";
      } else if (match_result.type == thor::MatchResult::Type::kInterpolated) {
        marker_color = "#ffffbf"; // yellow
        marker_size = "small";
        matched_point_type = "interpolated";
      } else {
        marker_color = "#fdae61"; // orange
        marker_size = "small";
        matched_point_type = "unmatched";
      }
      printf("{\"type\":\"Feature\",\"geometry\":{\"type\":\"Point\",\"coordinates\":[%.6f,%.6f]},\"properties\":{\"marker-color\":\"%s\",\"marker-size\":\"%s\",\"matched_point_index\":%d,\"matched_point_type\":\"%s\",\"edge_index\":%u,\"distance_along_edge\":%.3f,\"distance_from_trace_point\":%.3f}}%s\n", match_result.lnglat.lng(), match_result.lnglat.lat(), marker_color.c_str(), marker_size.c_str(), index++, matched_point_type.c_str(), match_result.edge_index, match_result.distance_along, match_result.distance_from, ((index != match_results.size()-1) ? "," : ""));
    }

    // Print geojson footer
    printf("]}\n");
    ////////////////////////////////////////////////////////////////////////////
#endif

  }

  // Set origin and destination from map matching results
  auto first_result_with_state = std::find_if(
      results.begin(), results.end(),
      [](const meili::MatchResult& result) {
        return result.HasState() && result.edgeid.Is_Valid();
      });

  auto last_result_with_state = std::find_if(
      results.rbegin(), results.rend(),
      [](const meili::MatchResult& result) {
        return result.HasState() && result.edgeid.Is_Valid();
      });

  if ((first_result_with_state != results.end())
      && (last_result_with_state != results.rend())) {
    baldr::PathLocation origin = matcher->mapmatching().state(
        first_result_with_state->stateid).candidate();
    baldr::PathLocation destination = matcher->mapmatching().state(
        last_result_with_state->stateid).candidate();

    bool found_origin = false;
    for (const auto& e : origin.edges) {
      if (e.id == path_edges.front().edgeid) {
        found_origin = true;
        break;
      }
    }

    if (!found_origin) {
      // 1. origin must be at a node, so we can reuse any one of
      // origin's edges

      // 2. path_edges.front().edgeid must be the downstream edge that
      // connects one of origin.edges (twins) at its start node
      origin.edges.emplace_back(path_edges.front().edgeid,
                                0.f,
                                origin.edges.front().projected,
                                origin.edges.front().score,
                                origin.edges.front().sos);
    }

    bool found_destination = false;
    for (const auto& e : destination.edges) {
      if (e.id == path_edges.back().edgeid) {
        found_destination = true;
        break;
      }
    }

    if (!found_destination) {
      // 1. destination must be at a node, so we can reuse any one of
      // destination's edges

      // 2. path_edges.back().edgeid must be the upstream edge that
      // connects one of destination.edges (twins) at its end node
      destination.edges.emplace_back(path_edges.back().edgeid,
                                     1.f,
                                     destination.edges.front().projected,
                                     destination.edges.front().score,
                                     destination.edges.front().sos);
    }


    // assert origin.edges contains path_edges.front() &&
    // destination.edges contains path_edges.back()

    // Form the trip path based on mode costing, origin, destination, and path edges
    trip_path = thor::TripPathBuilder::Build(controller, matcher->graphreader(),
                                             mode_costing, path_edges, origin,
                                             destination, std::list<PathLocation>{},
                                             interrupt_callback);
  } else {
    throw baldr::valhalla_exception_t { 400, 442 };
  }
  return {trip_path, match_results};
}

}
}
