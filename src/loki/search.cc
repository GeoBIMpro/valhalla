#include "loki/search.h"
#include "midgard/linesegment2.h"
#include "midgard/distanceapproximator.h"
#include "baldr/tilehierarchy.h"

#include <unordered_set>
#include <list>
#include <cmath>
#include <algorithm>
#include <iterator>

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::loki;

namespace {
//the cutoff at which we will assume the input is too far away from civilisation to be
//worth correlating to the nearest graph elements
constexpr float SEARCH_CUTOFF = 35000.f;
//during edge correlation, if you end up < 5 meters from the beginning or end of the
//edge we just assume you were at that node and not actually along the edge
//we keep it small because point and click interfaces are more accurate than gps input
constexpr float NODE_SNAP = 5.f;
//during side of street computations we figured you're on the street if you are less than
//5 meters (16) feet from the centerline. this is actually pretty large (with accurate shape
//data for the roads it might want half that) but its better to assume on street than not
constexpr float SIDE_OF_STREET_SNAP = 5.f;
//if you are this far away from the edge we are considering and you set a heading we will
//ignore it because its not really useful at this distance from the geometry
constexpr float NO_HEADING = 30.f;
//how much of the shape should be sampled to get heading
constexpr float HEADING_SAMPLE = 30.f;
//cone width to use for cosine similarity comparisons for favoring heading
constexpr float DEFAULT_ANGLE_WIDTH = 60.f;


//TODO: move this to midgard and test the crap out of it
//we are essentially estimating the angle of the tangent
//at a point along a discretised curve. we attempt to mostly
//use the shape coming into the point on the curve but if there
//isnt enough there we will use the shape coming out of the it
float tangent_angle(size_t index, const PointLL& point, const std::vector<PointLL>& shape, bool forward) {
  //depending on if we are going forward or backward we choose a different increment
  auto increment = forward ? -1 : 1;
  auto first_end = forward ? shape.cbegin() : shape.cend() - 1 ;
  auto second_end = forward ? shape.cend() - 1 : shape.cbegin();

  //u and v will be points we move along the shape until we have enough distance between them or run out of points

  //move backwards until we have enough or run out
  float remaining = HEADING_SAMPLE;
  auto u = point;
  auto i = shape.cbegin() + index + forward;
  while(remaining > 0 && i != first_end) {
    //move along and see how much distance that added
    i += increment;
    auto d = u.Distance(*i);
    //are we done yet?
    if(remaining <= d) {
      auto coef = remaining / d;
      u = u.AffineCombination(1 - coef, coef, *i);
      return u.Heading(point);
    }
    //next one
    u = *i;
    remaining -= d;
  }

  //move forwards until we have enough or run out
  auto v = point;
  i = shape.cbegin() + index + !forward;
  while(remaining > 0 && i != second_end) {
    //move along and see how much distance that added
    i -= increment;
    auto d = v.Distance(*i);
    //are we done yet?
    if(remaining <= d) {
      auto coef = remaining / d;
      v = v.AffineCombination(1 - coef, coef, *i);
      return u.Heading(v);
    }
    //next one
    v = *i;
    remaining -= d;
  }

  return u.Heading(v);
}

bool heading_filter(const DirectedEdge* edge, const EdgeInfo& info,
  const Location& location, const PointLL& point, float distance, size_t index) {
  //if its far enough away from the edge, the heading is pretty useless
  if(!location.heading_ || distance > NO_HEADING)
    return false;

  //get the angle of the shape from this point
  auto angle = tangent_angle(index, point, info.shape(), edge->forward());
  //we want the closest distance between two angles which can be had
  //across 0 or between the two so we just need to know which is bigger
  if(*location.heading_ > angle)
    return std::min(*location.heading_ - angle, (360.f - *location.heading_) + angle) > location.heading_tolerance_.get_value_or(DEFAULT_ANGLE_WIDTH);
  return std::min(angle - *location.heading_, (360.f - angle) + *location.heading_) > location.heading_tolerance_.get_value_or(DEFAULT_ANGLE_WIDTH);
}

PathLocation::SideOfStreet flip_side(const PathLocation::SideOfStreet side) {
  if(side != PathLocation::SideOfStreet::NONE)
    return side == PathLocation::SideOfStreet::LEFT ? PathLocation::SideOfStreet::RIGHT : PathLocation::SideOfStreet::LEFT;
  return side;
}

std::function<std::tuple<int32_t, unsigned short, float>()>
make_binner(const PointLL& p, const GraphReader& reader) {
  const auto& tiles = TileHierarchy::levels().rbegin()->second.tiles;
  return tiles.ClosestFirst(p);
}

// Model a segment (2 consecutive points in an edge in a bin).
struct candidate_t {
  float sq_distance;
  PointLL point;
  size_t index;

  GraphId edge_id;
  const DirectedEdge* edge;
  std::shared_ptr<const EdgeInfo> edge_info;

  const GraphTile* tile;

  bool operator<(const candidate_t& c) const {
    return sq_distance < c.sq_distance;
  }

  PathLocation::SideOfStreet get_side(const PointLL& original, float distance) const {
    //its so close to the edge that its basically on the edge
    if(distance < SIDE_OF_STREET_SNAP)
      return PathLocation::SideOfStreet::NONE;

    //if the projected point is way too close to the begin or end of the shape
    //TODO: if the original point is really far away side of street may also not make much sense..
    auto& shape = edge_info->shape();
    if(point.Distance(shape.front()) < SIDE_OF_STREET_SNAP ||
       point.Distance(shape.back())  < SIDE_OF_STREET_SNAP)
      return PathLocation::SideOfStreet::NONE;

    //get the side TODO: this can technically fail for longer segments..
    //to fix it we simply compute the plane formed by the triangle
    //through the center of the earth and the two shape points and test
    //whether the original point is above or below the plane (depending on winding)
    LineSegment2<PointLL> segment(shape[index], shape[index + 1]);
    return (segment.IsLeft(original) > 0) == edge->forward()  ? PathLocation::SideOfStreet::LEFT : PathLocation::SideOfStreet::RIGHT;
  }
};

// This structure contains the context of the projection of a
// Location.  At the creation, a bin is affected to the point.  The
// test() method should be called to each valid segment of the bin.
// When the bin is finished, next_bin() switch to the next possible
// interesting bin.  if has_bin() is false, then the best projection
// is found.
struct projector_t {
  projector_t(const Location& location, GraphReader& reader):
    binner(make_binner(location.latlng_, reader)),
    location(location), sq_radius(location.radius_ * location.radius_),
    lon_scale(cosf(location.latlng_.lat() * kRadPerDeg)),
    lat(location.latlng_.lat()),
    lng(location.latlng_.lng()),
    approx(location.latlng_) {
    //TODO: something more empirical based on radius
    unreachable.reserve(64);
    reachable.reserve(64);
    //initialize
    next_bin(reader);
  }

  // non default constructible and move only type
  projector_t() = delete;
  projector_t(const projector_t&) = delete;
  projector_t& operator=(const projector_t&) = delete;
  projector_t(projector_t&&) = default;
  projector_t& operator=(projector_t&&) = default;

  //the ones marked with null current tile are finished so put them on the end
  //otherwise we sort by bin so that ones with the same bin are next to each other
  bool operator<(const projector_t& other) const {
    //the
    if (cur_tile != other.cur_tile)
      return cur_tile > other.cur_tile;
    return bin_index < other.bin_index;
  }

  bool has_same_bin(const projector_t& other) const {
    return cur_tile == other.cur_tile && bin_index == other.bin_index;
  }

  bool has_bin() const {
    return cur_tile != nullptr;
  }

  // Advance to the next bin. Must not be called if has_bin() is false.
  void next_bin(GraphReader& reader) {
    do {
      //TODO: make configurable the radius at which we give up searching
      //the closest thing in this bin is further than what we have already
      int32_t tile_index; float distance;
      std::tie(tile_index, bin_index, distance) = binner();
      if(distance > SEARCH_CUTOFF || (reachable.size() && distance > sqrt(reachable.back().sq_distance))) {
        cur_tile = nullptr;
        break;
      }

      //grab the tile the lat, lon is in
      auto tile_id = GraphId(tile_index, TileHierarchy::levels().rbegin()->first, 0);
      reader.GetGraphTile(tile_id, cur_tile);
    } while (!cur_tile);
  }

  // Test if a segment is a candidate to the projection.  This method
  // is performance critical.  Copy, function call, cache locality and
  // useless computation must be handled with care.
  PointLL project(const PointLL& u, const PointLL& v) {
    //we're done if this is a zero length edge
    if(u == v)
      return u;

    //project a onto b where b is the origin vector representing this segment
    //and a is the origin vector to the point we are projecting, (a.b/b.b)*b
    auto bx = v.first - u.first;
    auto by = v.second - u.second;

    // Scale longitude when finding the projection
    auto bx2 = bx * lon_scale;
    auto sq = bx2 * bx2 + by * by;
    auto scale = (lng - u.lng()) * lon_scale * bx2 + (lat - u.lat()) * by; //only need the numerator at first

    //projects along the ray before u
    if(scale <= 0.f)
      return u;
    //projects along the ray after v
    else if(scale >= sq)
      return v;
    //projects along the ray between u and v
    scale /= sq;
    return {u.first + bx * scale, u.second + by * scale};
  }

  std::function<std::tuple<int32_t, unsigned short, float>()> binner;
  const GraphTile* cur_tile = nullptr;
  Location location;
  unsigned short bin_index = 0;
  float sq_radius;
  std::vector<candidate_t> unreachable;
  std::vector<candidate_t> reachable;

  // critical data
  float lon_scale;
  float lat;
  float lng;
  DistanceApproximator approx;
};

struct bin_handler_t {
  std::vector<projector_t> pps;
  valhalla::baldr::GraphReader& reader;
  const EdgeFilter& edge_filter;
  const NodeFilter& node_filter;
  unsigned int max_reach_limit;
  std::vector<candidate_t> bin_candidates;

  //key is the edge id, size_t is the index into cardinalities which
  //stores the number of edges that can be reached by this edge
  std::vector<unsigned int> reaches;
  std::unordered_map<uint64_t, size_t> reach_indices;

  bin_handler_t(const std::vector<valhalla::baldr::Location>& locations, valhalla::baldr::GraphReader& reader,
    const EdgeFilter& edge_filter, const NodeFilter& node_filter):
    reader(reader), edge_filter(edge_filter), node_filter(node_filter) {
    //get the unique set of input locations and the max reachability of them all
    std::unordered_set<Location> uniq_locations(locations.begin(), locations.end());
    pps.reserve(uniq_locations.size());
    max_reach_limit = 0;
    for (const auto& loc: uniq_locations) {
      pps.emplace_back(loc, reader);
      max_reach_limit = std::max(max_reach_limit, loc.minimum_reachability_);
    }
    //very annoying but it saves a lot of time to preallocate this instead of doing it in the loop in handle_bins
    bin_candidates.resize(pps.size());
    //TODO: make space for reach check in a more empirical way
    reach_indices.reserve(std::max(max_reach_limit, static_cast<decltype(max_reach_limit)>(1)) * 1024);
    reaches.reserve(std::max(max_reach_limit, static_cast<decltype(max_reach_limit)>(1)) * 1024);
  }

  //returns -1 when we dont know it
  int get_reach(GraphId id) {
    auto itr = reach_indices.find(id);
    if(itr == reach_indices.cend())
      return -1; //TODO: if we didnt find it should we run the reachability check
    return reaches[itr->second];
  }

  void correlate_node(const Location& location, const GraphId& found_node, const candidate_t& candidate, PathLocation& correlated){
    auto distance = location.latlng_.Distance(candidate.point);
    std::list<PathLocation::PathEdge> heading_filtered;

    //we need this because we might need to go to different levels
    std::function<void (const GraphId& node_id, bool transition)> crawl;
    crawl = [&](const GraphId& node_id, bool follow_transitions) {
      //now that we have a node we can pass back all the edges leaving and entering it
      const auto* tile = reader.GetGraphTile(node_id);
      if(!tile)
        return;
      const auto* node = tile->node(node_id);
      const auto* start_edge = tile->directededge(node->edge_index());
      const auto* end_edge = start_edge + node->edge_count();
      for(const auto* edge = start_edge; edge < end_edge; ++edge) {
        //if this is an edge leaving this level then we should go do that level awhile
        if(follow_transitions && (edge->trans_down() || edge->trans_up())) {
          crawl(edge->endnode(), false);
          continue;
        }

        //get some info about this edge and the opposing
        GraphId id = tile->id();
        id.fields.id = node->edge_index() + (edge - start_edge);
        auto info = tile->edgeinfo(edge->edgeinfo_offset());

        //do we want this edge
        if(edge_filter(edge) != 0.0f) {
          PathLocation::PathEdge path_edge{std::move(id), 0.f, node->latlng(), distance, PathLocation::NONE, get_reach(id)};
          auto index = edge->forward() ? 0 : info.shape().size() - 2;
          if(!heading_filter(edge, info, location, candidate.point, distance, index))
            correlated.edges.push_back(std::move(path_edge));
          else
            heading_filtered.emplace_back(std::move(path_edge));
        }

        //do we want the evil twin
        const GraphTile* other_tile;
        const auto other_id = reader.GetOpposingEdgeId(id, other_tile);
        if(!other_tile)
          continue;
        const auto* other_edge = other_tile->directededge(other_id);
        if(edge_filter(other_edge) != 0.0f) {
          PathLocation::PathEdge path_edge{std::move(other_id), 1.f, node->latlng(), distance, PathLocation::NONE, get_reach(id)};
          auto index = other_edge->forward() ? 0 : info.shape().size() - 2;
          if(!heading_filter(other_edge, tile->edgeinfo(edge->edgeinfo_offset()), location, candidate.point, distance, index))
            correlated.edges.push_back(std::move(path_edge));
          else
            heading_filtered.emplace_back(std::move(path_edge));
        }
      }

      //if we have nothing because of heading we'll just ignore it
      if(correlated.edges.size() == 0 && heading_filtered.size())
        for(auto& path_edge : heading_filtered)
          correlated.edges.push_back(std::move(path_edge));
    };

    //start where we are and crawl from there
    crawl(found_node, true);

    //if it was a through location with a heading its pretty confusing.
    //does the user want to come into and exit the location at the preferred
    //angle? for now we are just saying that they want it to exit at the
    //heading provided. this means that if it was node snapped we only
    //want the outbound edges
    if(location.stoptype_ == Location::StopType::THROUGH && location.heading_) {
      auto new_end = std::remove_if(correlated.edges.begin(), correlated.edges.end(),
        [](const PathLocation::PathEdge& e) { return e.end_node(); });
      correlated.edges.erase(new_end, correlated.edges.end());
    }
  }

  void correlate_edge(const Location& location, const candidate_t& candidate, PathLocation& correlated) {
    //now that we have an edge we can pass back all the info about it
    auto distance = location.latlng_.Distance(candidate.point);
    if(candidate.edge != nullptr){
      //we need the ratio in the direction of the edge we are correlated to
      double partial_length = 0;
      for(size_t i = 0; i < candidate.index; ++i)
        partial_length += candidate.edge_info->shape()[i].Distance(candidate.edge_info->shape()[i + 1]);
      partial_length += candidate.edge_info->shape()[candidate.index].Distance(candidate.point);
      partial_length = std::min(partial_length, static_cast<double>(candidate.edge->length()));
      float length_ratio = static_cast<float>(partial_length / static_cast<double>(candidate.edge->length()));
      if(!candidate.edge->forward())
        length_ratio = 1.f - length_ratio;
      //side of street
      auto side = candidate.get_side(location.latlng_, distance);
      //correlate the edge we found
      std::list<PathLocation::PathEdge> heading_filtered;
      if(heading_filter(candidate.edge, *candidate.edge_info, location, candidate.point, distance, candidate.index))
        heading_filtered.emplace_back(candidate.edge_id, length_ratio, candidate.point, distance, side, get_reach(candidate.edge_id));
      else
        correlated.edges.push_back(PathLocation::PathEdge{candidate.edge_id, length_ratio, candidate.point, distance, side, get_reach(candidate.edge_id)});
      //correlate its evil twin
      const GraphTile* other_tile;
      auto opposing_edge_id = reader.GetOpposingEdgeId(candidate.edge_id, other_tile);
      const DirectedEdge* other_edge;
      if(opposing_edge_id.Is_Valid() && (other_edge = other_tile->directededge(opposing_edge_id)) && edge_filter(other_edge) != 0.0f) {
        if(heading_filter(other_edge, *candidate.edge_info, location, candidate.point, distance, candidate.index))
          heading_filtered.emplace_back(opposing_edge_id, 1 - length_ratio, candidate.point, distance, flip_side(side), get_reach(opposing_edge_id));
        else
          correlated.edges.push_back(PathLocation::PathEdge{opposing_edge_id, 1 - length_ratio, candidate.point, distance, flip_side(side), get_reach(opposing_edge_id)});
      }

      //if we have nothing because of heading we'll just ignore it
      if(correlated.edges.size() == 0 && heading_filtered.size())
        for(auto& path_edge : heading_filtered)
          correlated.edges.push_back(std::move(path_edge));
    }
  }

  //recursive depth first search for expanding nodes
  //TODO: test whether writing this non-recursively would be faster
  void depth_first(const GraphTile*& tile, const NodeInfo* node, size_t& reach_index) {
    //for each edge recurse on the usable ones
    auto* e = tile->directededge(node->edge_index());
    for(uint32_t i = 0; reaches.back() < max_reach_limit && i < node->edge_count(); ++i, ++e) {
      //if we can take the edge and we can get the node and we can pass through the node
      const NodeInfo* n = nullptr;
      if(edge_filter(e) != 0.f && (n = reader.GetEndNode(e, tile)) && !node_filter(n)) {
        //try to mark the node
        auto inserted = reach_indices.emplace(e->endnode(), reach_index);
        //we saw this one already
        if(!inserted.second) {
          //we've seen this node in this run so just skip it
          if(reach_index == inserted.first->second)
            continue;

          //this is connected to a previous island so combine them
          reaches.back() += reaches[reach_index] - 1;
          reaches[reach_index] = reaches.back();
          //signal the recursion to stop
          reach_index = inserted.first->second;
          return;
        }

        //recurse
        ++reaches.back();
        size_t previous = reach_index;
        depth_first(tile, n, reach_index);

        //if we saw the edge in a previous run we want to be done completely
        if(reach_index != previous)
          return;
      }
    }
  }

  //do a mini network expansion or maybe not
  unsigned int check_reachability(std::vector<projector_t>::iterator begin, std::vector<projector_t>::iterator end,
    const GraphTile* tile, const DirectedEdge* edge) {
    //no need when set to 0
    if(max_reach_limit == 0)
      return 0;

    //do we already know about this one?
    auto found = reach_indices.find(edge->endnode());
    if(found != reach_indices.cend())
      return reaches[found->second];

    //see if we even need to do it
    bool check = false;
    for (auto it = begin; it != end && !check; ++it)
      check = check || it->reachable.empty();

    //assume its reachable
    if(!check)
      return max_reach_limit;

    //if you cant get the node then its not reachable since you cant leave the edge
    const auto* node = reader.GetEndNode(edge, tile);
    if(!node)
      return 0;

    //store an index into cardinalities so we can tell when search paths merge
    //if the index changes then we know its been merged with another path
    size_t reach_index = reaches.size();
    reaches.push_back(0);
    depth_first(tile, node, reach_index);
    return reaches.back();
  }

  //handle a bin for the range of candidates that share it
  void handle_bin(std::vector<projector_t>::iterator begin,
                  std::vector<projector_t>::iterator end) {
    //iterate over the edges in the bin
    auto tile = begin->cur_tile;
    auto edges = tile->GetBin(begin->bin_index);
    for(auto e : edges) {
      //get the tile and edge
      if(!reader.GetGraphTile(e, tile))
        continue;

      //no thanks on this one or its evil twin
      const auto* edge = tile->directededge(e);
      if(edge_filter(edge) == 0.0f &&
        (!(e = reader.GetOpposingEdgeId(e, tile)).Is_Valid() ||
        edge_filter(edge = tile->directededge(e)) == 0.0f)) {
        continue;
      }

      //check for island or dont
      auto reachability = check_reachability(begin, end, tile, edge);

      //get some shape of the edge
      auto edge_info = std::make_shared<const EdgeInfo>(tile->edgeinfo(edge->edgeinfo_offset()));
      auto shape = edge_info->lazy_shape();
      PointLL v;
      if (!shape.empty())
        v = shape.pop();

      //reset these so we know the best point along the edge
      auto c_itr = bin_candidates.begin();
      decltype(begin) p_itr;
      for (p_itr = begin; p_itr != end; ++p_itr, ++c_itr)
        c_itr->sq_distance = std::numeric_limits<float>::max();

      //TODO: can we speed this up? the majority of edges will be short and far away enough
      //such that the closest point on the edge will be one of the edges end points, we can get
      //these coordinates them from the nodes in the graph. we can then find whichever end is
      //closest to the input point p, call it n. we can then define an half plane h intersecting n
      //so that its orthogonal to the ray from p to n. using h, we only need to test segments
      //of the shape which are on the same side of h that p is. to make this fast we would need a
      //mathematically trivial half plane test

      //iterate along this edges segments projecting each of the points
      for(size_t i = 0; !shape.empty(); ++i) {
        auto u = v;
        v = shape.pop();
        //for each input point
        c_itr = bin_candidates.begin();
        for (p_itr = begin; p_itr != end; ++p_itr, ++c_itr) {
          //how close is the input to this segment
          auto point = p_itr->project(u, v);
          auto sq_distance = p_itr->approx.DistanceSquared(point);
          //do we want to keep it
          if(sq_distance < c_itr->sq_distance) {
            c_itr->sq_distance = sq_distance;
            c_itr->point = std::move(point);
            c_itr->index = i;
          }
        }
      }

      //keep the best point along this edge if it makes sense
      c_itr = bin_candidates.begin();
      for (p_itr = begin; p_itr != end; ++p_itr, ++c_itr) {
        //which batch of findings
        auto* batch = reachability < p_itr->location.minimum_reachability_ ? &p_itr->unreachable : &p_itr->reachable;

        //if its empty append
        if(batch->empty()) {
          c_itr->edge = edge; c_itr->edge_id = e; c_itr->edge_info = edge_info; c_itr->tile = tile;
          batch->emplace_back(std::move(*c_itr));
          continue;
        }

        //get some info about possibilities
        bool in_radius = c_itr->sq_distance < p_itr->sq_radius;
        bool better = c_itr->sq_distance < batch->back().sq_distance;
        bool last_in_radius = batch->back().sq_distance < p_itr->sq_radius;

        //it has to either be better or in the radius to move on
        if(in_radius || better) {
          c_itr->edge = edge; c_itr->edge_id = e; c_itr->edge_info = edge_info; c_itr->tile = tile;
          //the last one wasnt in the radius so replace it with this one because its better or is in the radius
          if(!last_in_radius)
            batch->back() = std::move(*c_itr);
          //last one is in the radius but this one is better so put it on the end
          else if(better)
            batch->emplace_back(std::move(*c_itr));
          //last one is in the radius and this one is not as good so put it on before it
          else {
            batch->emplace_back(std::move(*c_itr));
            std::swap(*(batch->end() - 1), *(batch->end() - 2));
          }
        }
      }
    }

    //bin is finished, advance the candidates to their respective next bins
    for (auto p_itr = begin; p_itr != end; ++p_itr)
      p_itr->next_bin(reader);
  }

  // Find the best range to do.  The given vector should be sorted for
  // interesting grouping.  Returns the greatest range of non empty
  // equal bins.
  std::pair<std::vector<projector_t>::iterator, std::vector<projector_t>::iterator>
  find_best_range(std::vector<projector_t>& pps) const {
    auto best = std::make_pair(pps.begin(), pps.begin());
    auto cur = best;
    while (cur.second != pps.end()) {
      cur.first = cur.second;
      cur.second = std::find_if_not(cur.first, pps.end(), [&cur](const projector_t& pp) {
        return cur.first->has_same_bin(pp);
      });
      if (cur.first->has_bin() && cur.second - cur.first > best.second - best.first) {
        best = cur;
      }
    }
    return best;
  }

  //we keep the points sorted at each round such that unfinished ones
  //are at the front of the sorted list
  void search() {
    std::sort(pps.begin(), pps.end());
    while(pps.front().has_bin()) {
      auto range = find_best_range(pps);
      handle_bin(range.first, range.second);
      std::sort(pps.begin(), pps.end());
    }
  }

  //create the PathLocation corresponding to the best projection of the given candidate
  std::unordered_map<Location, PathLocation> finalize() {
    //at this point we have candidates for each location so now we
    //need to go get the actual correlated location with edge_id etc.
    std::unordered_map<Location, PathLocation> searched;
    for (auto& pp: pps) {
      //concatenate
      pp.reachable.reserve(pp.reachable.size() + pp.unreachable.size());
      std::move(pp.unreachable.begin(), pp.unreachable.end(), std::back_inserter(pp.reachable));
      //go through getting all the results for this one
      PathLocation correlated(pp.location);
      for (const auto& candidate : pp.reachable) {
        //this may be at a node, either because it was the closest thing or from snap tolerance
        bool front = candidate.point == candidate.edge_info->shape().front() ||
          pp.location.latlng_.Distance(candidate.edge_info->shape().front()) < NODE_SNAP;
        bool back = candidate.point == candidate.edge_info->shape().back() ||
          pp.location.latlng_.Distance(candidate.edge_info->shape().back()) < NODE_SNAP;
        //it was the begin node
        if((front && candidate.edge->forward()) || (back && !candidate.edge->forward())) {
          const GraphTile* other_tile;
          auto opposing_edge = reader.GetOpposingEdge(candidate.edge_id, other_tile);
          if(!other_tile) continue; //TODO: do an edge snap instead, but you'll only get one direction
          correlate_node(pp.location, opposing_edge->endnode(), candidate, correlated);
        }//it was the end node
        else if((back && candidate.edge->forward()) || (front && !candidate.edge->forward())) {
          correlate_node(pp.location, candidate.edge->endnode(), candidate, correlated);
        }//it was along the edge
        else {
          correlate_edge(pp.location, candidate, correlated);
        }
      }
      //if we found nothing that is no good but if its batch maybe throwing makes no sense?
      if(correlated.edges.size() != 0)
        searched.insert({pp.location, correlated});
      //else
      //  throw std::runtime_error("No suitable edges near location");
      //sort by id and then score
      //remove duplicate ids
      //sort by score
    }
    //give back all the results
    return searched;
  }

};

}

namespace valhalla {
namespace loki {

std::unordered_map<Location, PathLocation>
Search(const std::vector<Location>& locations, GraphReader& reader, const EdgeFilter& edge_filter, const NodeFilter& node_filter) {
  //trivially finished already
  if(locations.empty())
    return {};
  //setup the unique list of locations
  bin_handler_t handler(locations, reader, edge_filter, node_filter);
  //search over the bins doing multiple locations per bin
  handler.search();
  //turn each locations candidate set into path locations
  return handler.finalize();
}

}
}
