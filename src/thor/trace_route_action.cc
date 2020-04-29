#include "thor/worker.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "meili/map_matcher.h"

#include "meili/match_result.h"
#include "thor/attributes_controller.h"
#include "thor/map_matcher.h"
#include "thor/route_matcher.h"
#include "thor/triplegbuilder.h"

using namespace valhalla;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::thor;
using std::vector;

namespace {

struct MapMatch {
  // Coordinate of the match point
  midgard::PointLL lnglat;
  // Which edge this match point stays
  baldr::GraphId edgeid;
  // Percentage distance along the edge
  float distance_along;
  int edge_index = -1;
};

// <Confidence score, raw score, match results, trip path> tuple indexes
constexpr size_t kConfidenceScoreIndex = 0;
constexpr size_t kRawScoreIndex = 1;
constexpr size_t kMatchResultsIndex = 2;
constexpr size_t kTripLegIndex = 3;

void add_path_edge(valhalla::Location* l, const meili::MatchResult& m) {
  l->mutable_path_edges()->Clear();
  auto* edge = l->mutable_path_edges()->Add();
  edge->set_graph_id(m.edgeid);
  edge->set_percent_along(m.distance_along);
  edge->mutable_ll()->set_lng(m.lnglat.first);
  edge->mutable_ll()->set_lat(m.lnglat.second);
  edge->set_distance(m.distance_from);
  // NOTE: we dont need side of street here because the match is continuous we dont know if they were
  // starting a route from the side of the road or whatever so calling that out is not a good idea
  // edge->set_side_of_street();
  // NOTE: we dont care about reachablity because the match will have worked or not worked!
  // edge->set_minimum_reachability();
}

} // namespace

namespace valhalla {
namespace thor {

/*
 * The trace_route action takes a GPS trace and turns it into a route result.
 */
void thor_worker_t::trace_route(Api& request) {
  // Parse request
  parse_locations(request);
  parse_costing(request);
  parse_measurements(request);
  parse_filter_attributes(request);

  const auto& options = *request.mutable_options();

  switch (options.shape_match()) {
    // If the exact points from a prior route that was run against the Valhalla road network,
    // then we can traverse the exact shape to form a path by using edge-walking algorithm
    case ShapeMatch::edge_walk:
      try {
        route_match(request);
      } catch (...) {
        throw valhalla_exception_t{
            443, ShapeMatch_Enum_Name(options.shape_match()) +
                     " algorithm failed to find exact route match.  Try using "
                     "shape_match:'walk_or_snap' to fallback to map-matching algorithm"};
      }
      break;
    // If non-exact shape points are used, then we need to correct this shape by sending them
    // through the map-matching algorithm to snap the points to the correct shape
    case ShapeMatch::map_snap:
      try {
        map_match(request);
      } catch (...) { throw valhalla_exception_t{442}; }
      break;
    // If we think that we have the exact shape but there ends up being no Valhalla route match,
    // then we want to fallback to try and use meili map matching to match to local route
    // network. No shortcuts are used and detailed information at every intersection becomes
    // available.
    case ShapeMatch::walk_or_snap:
      try {
        route_match(request);
      } catch (...) {
        LOG_WARN(ShapeMatch_Enum_Name(options.shape_match()) +
                 " algorithm failed to find exact route match; Falling back to map_match...");
        try {
          map_match(request);
        } catch (...) { throw valhalla_exception_t{442}; }
      }
      break;
  }

  // log admin areas
  if (!options.do_not_track()) {
    for (const auto& route : request.trip().routes()) {
      for (const auto& leg : route.legs()) {
        log_admin(leg);
      }
    }
  }
}

/*
 * Returns trip path using an “edge-walking” algorithm.
 * This is for use when the input shape is exact shape from a prior Valhalla route.
 * This will walk the input shape and compare to Valhalla edge’s end node positions to
 * form the list of edges. It will return no nodes if path not found.
 *
 */
void thor_worker_t::route_match(Api& request) {
  // TODO - make sure the trace has timestamps..
  auto& options = *request.mutable_options();
  std::vector<PathInfo> path;
  if (RouteMatcher::FormPath(mode_costing, mode, *reader, trace, options, path)) {
    // TODO: we dont support multileg here as it ignores location types but...
    // if this were a time dependent match you need to propogate the date time
    // information to each legs origin location because triplegbuilder relies on it.
    // form path set the first one but on the subsequent legs we will need to set them
    // by doing time offsetting like is done in route_action.cc thor_worker_t::depart_at

    // For now we ignore multileg complications and just make sure the searched locations
    // get the same data information the shape informations had
    if (options.shape(0).has_date_time())
      options.mutable_locations(0)->set_date_time(options.shape(0).date_time());

    // Form the trip path based on mode costing, origin, destination, and path edges
    auto& leg = *request.mutable_trip()->mutable_routes()->Add()->mutable_legs()->Add();
    thor::TripLegBuilder::Build(controller, *reader, mode_costing, path.begin(), path.end(),
                                *options.mutable_locations()->begin(),
                                *options.mutable_locations()->rbegin(),
                                std::list<valhalla::Location>{}, leg, interrupt);
  } else {
    throw std::exception{};
  }
}

// Form the path from the map-matching results. This path gets sent to TripLegBuilder.
// PathInfo is primarily a list of edge Ids but it also include elapsed time to the end
// of each edge. We will need to use the existing costing method to form the elapsed time
// the path. We will start with just using edge costs and will add transition costs.
std::vector<std::tuple<float, float, std::vector<meili::MatchResult>>>
thor_worker_t::map_match(Api& request) {
  auto& options = *request.mutable_options();
  // Call Meili for map matching to get a collection of Location Edges
  matcher->set_interrupt(interrupt);
  // Create the vector of matched path results
  if (trace.size() == 0) {
    return {};
  }

  // we don't allow multi path for trace route at the moment, discontinuities force multi route
  int topk =
      request.options().action() == Options::trace_attributes ? request.options().best_paths() : 1;
  auto topk_match_results = matcher->OfflineMatch(trace, topk);

  // Process each score/match result
  std::vector<std::tuple<float, float, std::vector<meili::MatchResult>>> map_match_results;
  for (auto& result : topk_match_results) {
    // There is no path so you're done
    if (result.segments.empty()) {
      throw std::exception{};
    }

    // Form the path edges based on the matched points and populate disconnected edges
    auto paths = MapMatcher::FormPath(matcher.get(), result.results, result.segments, mode_costing,
                                      mode, options);

    // TODO: revisit this, should we always do this? can it go into the functions below?
    // OSRM map matching format has both the match points and the route, fill out the match points
    // here Note that we only support trace_route as OSRM format so best_paths == 1
    if (options.action() == Options::trace_route && options.format() == Options::osrm) {
      const GraphTile* tile = nullptr;
      for (int i = 0; i < result.results.size(); ++i) {
        // Get the match
        const auto& match = result.results[i];
        if (!match.edgeid.Is_Valid()) {
          continue;
        }

        // Make one path edge from it
        reader->GetGraphTile(match.edgeid, tile);
        auto* pe = options.mutable_shape(i)->mutable_path_edges()->Add();
        pe->mutable_ll()->set_lat(match.lnglat.lat());
        pe->mutable_ll()->set_lng(match.lnglat.lng());
        for (const auto& n : reader->edgeinfo(match.edgeid).GetNames()) {
          pe->mutable_names()->Add()->assign(n);
        }

        // signal how many edge candidates there were at this stateid by adding empty path edges
        if (!match.HasState()) {
          continue;
        }
        for (int j = 0;
             j < matcher->state_container().state(match.stateid).candidate().edges.size() - 1; ++j) {
          options.mutable_shape(i)->mutable_path_edges()->Add();
        }
      }
    }

    // trace_attributes always returns a single trip path and may have discontinuities
    if (options.action() == Options::trace_attributes) {
      build_trace(paths, result.results, options, request);
    } // trace_route can return multiple trip paths
    else {
      build_route(paths, result.results, options, request);
    }

    // TODO: move this info to the trip leg
    // Keep the result
    map_match_results.emplace_back(map_match_results.empty()
                                       ? 1.0f
                                       : std::get<kRawScoreIndex>(map_match_results.front()) /
                                             result.score,
                                   result.score, std::move(result.results));
  }

  return map_match_results;
}

void thor_worker_t::build_trace(
    const std::deque<std::pair<std::vector<PathInfo>, std::vector<const meili::EdgeSegment*>>>& paths,
    std::vector<meili::MatchResult>& match_results,
    Options& options,
    Api& request) {

  // loop over all the segments to figure out which edge index belongs to with match result,
  // to set the discontinuities and to remember the first and last matches we saw
  std::unordered_map<size_t, std::pair<RouteDiscontinuity, RouteDiscontinuity>> route_discontinuities;
  baldr::GraphId last_id;
  size_t edge_index = 0;
  const meili::MatchResult* origin_match = nullptr;
  const meili::MatchResult* dest_match = nullptr;
  for (const auto& path : paths) {
    // remember the global edge index
    for (const auto& segment : path.second) {
      if (segment->first_match_idx >= 0) {
        match_results[segment->first_match_idx].edge_index = edge_index;
        if (!origin_match) {
          origin_match = &match_results[segment->first_match_idx];
        }
      }
      if (segment->last_match_idx >= 0) {
        match_results[segment->last_match_idx].edge_index = edge_index;
        if (!dest_match) {
          dest_match = &match_results[segment->last_match_idx];
        }
      }
      if (last_id != segment->edgeid) {
        ++edge_index;
      }
      last_id = segment->edgeid;
    }

    // handle the end of a discontinuity, assumes that there is no start of a discontinuity that is
    // handled below
    const auto& first_segment = path.second.front();
    const auto& first_match = match_results[first_segment->first_match_idx];
    if (first_match.ends_discontinuity) {
      route_discontinuities[first_match.edge_index] = {{true, first_match.lnglat,
                                                        first_match.distance_along},
                                                       {false, {}, 1.f}};
    }

    // handle the start of a discontinuity, could be on the same edge where we just ended one. in that
    // case we only touch .second. if there was no discontinuity ending on this edge then we rely on
    // the default initializer for .first when we index the map which sets the distance to 0.f
    const auto& last_segment = path.second.back();
    const auto& last_match = match_results[last_segment->last_match_idx]; // cant use edge_index
    if (last_match.begins_discontinuity) {
      auto found = route_discontinuities[last_match.edge_index].second = {true, last_match.lnglat,
                                                                          last_match.distance_along};
    }
  }

  // couldnt find any match
  if (!origin_match || !dest_match)
    throw valhalla_exception_t{442};

  // initialize the origin and destination location for route
  Location* origin_location = options.mutable_shape(origin_match - &match_results.front());
  Location* destination_location = options.mutable_shape(dest_match - &match_results.front());

  // we fake up something that looks like the output of loki
  add_path_edge(origin_location, *origin_match);
  add_path_edge(destination_location, *dest_match);

  // smash all the path edges into a single vector
  std::vector<PathInfo> path_edges;
  path_edges.reserve(edge_index);
  for (const auto& path : paths) {
    bool merge_last_edge =
        !path_edges.empty() && path_edges.back().edgeid == path.first.front().edgeid;
    path_edges.insert(path_edges.end(), path.first.begin() + merge_last_edge, path.first.end());
  }

  // Form the trip path based on mode costing, origin, destination, and path edges
  auto* leg = request.mutable_trip()->add_routes()->add_legs();
  thor::TripLegBuilder::Build(controller, matcher->graphreader(), mode_costing, path_edges.begin(),
                              path_edges.end(), *origin_location, *destination_location,
                              std::list<valhalla::Location>{}, *leg, interrupt,
                              &route_discontinuities);
}

void thor_worker_t::build_route(
    const std::deque<std::pair<std::vector<PathInfo>, std::vector<const meili::EdgeSegment*>>>& paths,
    const std::vector<meili::MatchResult>& match_results,
    Options& options,
    Api& request) {
  // The following logic put break points (matches results) on edge candidates to form legs
  // logic assumes the both match results and edge candidates are topologically sorted in correct
  // order. Only the first location will be populated with corresponding input date_time

  int way_point_index = 0;
  valhalla::TripRoute* route = nullptr;
  std::vector<PathInfo> edges;
  int route_index = 0;
  for (const auto& path : paths) {
    std::cout << "Path:" << std::endl;
    for (const auto& p : path.first) {
      std::cout << p << std::endl;
    }
    std::cout << "EdgeSegments:" << std::endl;
    for (const auto* s : path.second) {
      std::cout << *s << std::endl;
    }
    std::cout << std::endl;

    if (route == nullptr) {
      route = request.mutable_trip()->mutable_routes()->Add();
      way_point_index = 0;
    }

    int origin_match_idx = path.second.front()->first_match_idx;
    int dest_match_idx = path.second.back()->last_match_idx;

    for (int i = origin_match_idx; i <= dest_match_idx; ++i) {
      options.mutable_shape(i)->set_route_index(route_index);
      options.mutable_shape(i)->set_shape_index(std::numeric_limits<uint32_t>::max());
    }

    // initialize the origin and destination location for route
    Location* origin_location = options.mutable_shape(origin_match_idx);
    Location* destination_location = options.mutable_shape(dest_match_idx);

    // when handling multi routes, orsm serializer need to know both the
    // matching_index(route_index) and the waypoint_index(shape_index).
    origin_location->set_shape_index(way_point_index);
    destination_location->set_shape_index(++way_point_index);

    // we fake up something that looks like the output of loki
    const auto& origin_match = match_results[origin_match_idx];
    const auto& dest_match = match_results[dest_match_idx];
    add_path_edge(origin_location, origin_match);
    add_path_edge(destination_location, dest_match);

    TripLegBuilder::Build(controller, matcher->graphreader(), mode_costing, path.first.cbegin(),
                          path.first.cend(), *origin_location, *destination_location,
                          std::list<valhalla::Location>{}, *route->mutable_legs()->Add(), interrupt);

    if (path.second.back()->discontinuity) {
      ++route_index;
      route = nullptr;
    }
  }
}

} // namespace thor
} // namespace valhalla
