#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <memory>
#include <unordered_set>
#include <thread>
#include <future>
#include <random>
#include <queue>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/format.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string.hpp>
#include <curl/curl.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/io/coded_stream.h>

#include <valhalla/midgard/logging.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/tilehierarchy.h>
#include <valhalla/baldr/graphtile.h>
#include <valhalla/baldr/datetime.h>

#include "proto/transit.pb.h"

using namespace boost::property_tree;
using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::mjolnir;

struct logged_error_t: public std::runtime_error {
  logged_error_t(const std::string& msg):std::runtime_error(msg) {
    LOG_ERROR(msg);
  }
};

struct curler_t {
  curler_t():connection(curl_easy_init(), [](CURL* c){curl_easy_cleanup(c);}),
    generator(std::chrono::system_clock::now().time_since_epoch().count()),
    distribution(static_cast<size_t>(50), static_cast<size_t>(250)) {
    if(connection.get() == nullptr)
      throw logged_error_t("Failed to created CURL connection");
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_ERRORBUFFER, error), "Failed to set error buffer");
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_FOLLOWLOCATION, 1L), "Failed to set redirect option ");
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_WRITEDATA, &result), "Failed to set write data ");
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_WRITEFUNCTION, write_callback), "Failed to set writer ");
  }
  //for now we only need to handle json
  //with templates we could return a string or whatever
  ptree operator()(const std::string& url, const std::string& retry_if_no = "", size_t timeout = 200) {
    LOG_DEBUG(url);
    result.str("");
    ptree pt;
    assert_curl(curl_easy_setopt(connection.get(), CURLOPT_URL, url.c_str()), "Failed to set URL ");
    auto sleep_or_not = [this, &url, &retry_if_no, &pt]() {
      if(!retry_if_no.empty() && !pt.get_child_optional(retry_if_no)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(distribution(generator)));
        LOG_WARN("Retrying " + url)
        return true;
      }
      return false;
    };

    do {
      try {
        if(curl_easy_perform(connection.get()) == CURLE_OK)
          read_json(result, pt);
      }
      catch(...) { /*swallow for retry */ }
    } while(sleep_or_not());
    return pt;
  }
  std::string last() const {
    return result.str();
  }
protected:
  void assert_curl(CURLcode code, const std::string& msg){
    if(code != CURLE_OK)
      throw logged_error_t(msg + error);
  };
  static size_t write_callback(char *in, size_t block_size, size_t blocks, std::stringstream *out) {
    if(!out) return static_cast<size_t>(0);
    out->write(in, block_size * blocks);
    return block_size * blocks;
  }
  std::shared_ptr<CURL> connection;
  char error[CURL_ERROR_SIZE];
  std::stringstream result;
  std::default_random_engine generator;
  std::uniform_int_distribution<size_t> distribution;
};

//TODO: update this call to get only the tiles that have changed since last time
struct weighted_tile_t { GraphId t; size_t w; bool operator<(const weighted_tile_t& o) const { return w == o.w ? t < o.t : w < o.w; } };
std::priority_queue<weighted_tile_t> which_tiles(const ptree& pt) {
  //now real need to catch exceptions since we can't really proceed without this stuff
  LOG_INFO("Fetching transit feeds");
  TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
  std::set<GraphId> tiles;
  const auto& tile_level = hierarchy.levels().rbegin()->second;
  curler_t curler;
  auto feeds = curler(pt.get<std::string>("base_url") + "/api/v1/feeds.geojson", "features");
  for(const auto& feature : feeds.get_child("features")) {
    //should be a polygon
    auto type = feature.second.get_optional<std::string>("geometry.type");
    if(!type || *type != "Polygon") {
      LOG_WARN("Skipping non-polygonal feature: " + feature.second.get_value<std::string>());
      continue;
    }
    //grab the tile row and column ranges for the max box around the polygon
    int32_t min_c = tile_level.tiles.ncolumns(), max_c = 0, min_r = tile_level.tiles.nrows(), max_r = 0;
    for(const auto& coord :feature.second.get_child("geometry.coordinates").front().second) {
      auto c = tile_level.tiles.Col(coord.second.front().second.get_value<float>());
      auto r = tile_level.tiles.Row(coord.second.back().second.get_value<float>());
      if(c < min_c) min_c = c;
      if(c > max_c) max_c = c;
      if(r < min_r) min_r = r;
      if(r > max_r) max_r = r;
    }
    //for each tile in the polygon figure out how heavy it is and keep track of it
    for(auto i = min_c; i <= max_c; ++i)
      for(auto j = min_r; j <= min_r; ++j)
        tiles.emplace(GraphId(tile_level.tiles.TileId(i,j), tile_level.level, 0));
  }
  //we want hardest tiles first
  std::priority_queue<weighted_tile_t> prioritized;
  auto now = time(nullptr);
  auto* utc = gmtime(&now); utc->tm_year += 1900; ++utc->tm_mon;
  for(const auto& tile : tiles) {
    auto bbox = tile_level.tiles.TileBounds(tile.tileid());
    auto request = (boost::format(pt.get<std::string>("base_url") +
      "/api/v1/schedule_stop_pairs?per_page=0&bbox=%1%,%2%,%3%,%4%&service_from_date=%5%-%6%-%7%&api_key=%8%")
      % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy() % utc->tm_year % utc->tm_mon % utc->tm_mday % pt.get<std::string>("api_key")).str();
    auto total = curler(request, "meta.total").get<size_t>("meta.total");
    if(total > 0) {
      prioritized.push(weighted_tile_t{tile, total});
      LOG_INFO(GraphTile::FileSuffix(tile, hierarchy) + ":" + std::to_string(total));
    }
  }
  LOG_INFO("Finished with " + std::to_string(prioritized.size()) + " expected transit tiles in " +
           std::to_string(feeds.get_child("features").size()) + " feeds");
  return prioritized;
}

#define set_no_null(T, pt, path, null_value, set) {\
  auto value = pt.get<T>(path, null_value); \
  if(value != null_value) \
    set(value); \
}

void get_stops(Transit& tile, std::unordered_map<std::string, uint64_t>& stops,
    const GraphId& tile_id, const ptree& response, const AABB2<PointLL>& bbox) {
  const std::vector<std::string> regions = DateTime::get_tz_db().regions;
  for(const auto& stop_pt : response.get_child("stops")) {
    const auto& ll_pt = stop_pt.second.get_child("geometry.coordinates");
    auto lon = ll_pt.front().second.get_value<float>();
    auto lat = ll_pt.back().second.get_value<float>();
    if(!bbox.Contains({lon, lat}))
      continue;
    auto* stop = tile.add_stops();
    stop->set_lon(lon);
    stop->set_lat(lat);
    set_no_null(std::string, stop_pt.second, "onestop_id", "null", stop->set_onestop_id);
    set_no_null(std::string, stop_pt.second, "name", "null", stop->set_name);
    stop->set_wheelchair_boarding(stop_pt.second.get<bool>("tags.wheelchair_boarding", false));
    set_no_null(uint64_t, stop_pt.second, "tags.osm_way_id", 0, stop->set_osm_way_id);
    GraphId stop_id = tile_id;
    stop_id.fields.id = stops.size();
    stop->set_graphid(stop_id);
    stop->set_timezone(0);
    auto timezone = stop_pt.second.get_optional<std::string>("timezone");
    if (timezone) {
      std::vector<std::string>::const_iterator it = std::find(regions.begin(), regions.end(), *timezone);
      if (it == regions.end()) {
        LOG_WARN("Timezone not found for " + *timezone);
      } else stop->set_timezone(it - regions.cbegin());
    }
    else LOG_WARN("Timezone not found for stop " + stop->name());
    stops.emplace(stop->onestop_id(), stop_id);
  }
}

void get_routes(Transit& tile, std::unordered_map<std::string, uint64_t>& routes,
    const std::unordered_map<std::string, std::string>& websites, const ptree& response) {
  for(const auto& route_pt : response.get_child("routes")) {
    auto* route = tile.add_routes();
    set_no_null(std::string, route_pt.second, "onestop_id", "null", route->set_onestop_id);
    std::string vehicle_type = route_pt.second.get<std::string>("tags.vehicle_type", "");
    Transit_VehicleType type = Transit_VehicleType::Transit_VehicleType_kRail;
    if (vehicle_type == "tram")
      type = Transit_VehicleType::Transit_VehicleType_kTram;
    else if (vehicle_type == "metro")
      type = Transit_VehicleType::Transit_VehicleType_kMetro;
    else if (vehicle_type == "rail")
      type = Transit_VehicleType::Transit_VehicleType_kRail;
    else if (vehicle_type == "bus")
      type = Transit_VehicleType::Transit_VehicleType_kBus;
    else if (vehicle_type == "ferry")
      type = Transit_VehicleType::Transit_VehicleType_kFerry;
    else if (vehicle_type == "cablecar")
      type = Transit_VehicleType::Transit_VehicleType_kCableCar;
    else if (vehicle_type == "gondola")
      type = Transit_VehicleType::Transit_VehicleType_kGondola;
    else if (vehicle_type == "funicular")
      type = Transit_VehicleType::Transit_VehicleType_kFunicular;
    else {
      LOG_ERROR("Skipping unsupported vehicle_type: " + vehicle_type + " for route " + route->onestop_id());
      tile.mutable_routes()->RemoveLast();
      continue;
    }
    route->set_vehicle_type(type);
    set_no_null(std::string, route_pt.second, "operated_by_onestop_id", "null", route->set_operated_by_onestop_id);
    set_no_null(std::string, route_pt.second, "operated_by_name", "null", route->set_operated_by_name);
    set_no_null(std::string, route_pt.second, "name", "null", route->set_name);
    set_no_null(std::string, route_pt.second, "tags.route_long_name", "null", route->set_route_long_name);
    set_no_null(std::string, route_pt.second, "tags.route_desc", "null", route->set_route_desc);
    std::string route_color = route_pt.second.get<std::string>("tags.route_color", "FFFFFF");
    std::string route_text_color = route_pt.second.get<std::string>("tags.route_text_color", "000000");
    boost::algorithm::trim(route_color);
    boost::algorithm::trim(route_text_color);
    route_color = (route_color == "null" ? "FFFFFF" : route_color);
    route_text_color = (route_text_color == "null" ? "000000" : route_text_color);
    auto website = websites.find(route->operated_by_onestop_id());
    if(website != websites.cend())
      route->set_operated_by_website(website->second);
    route->set_route_color(strtol(route_color.c_str(), nullptr, 16));
    route->set_route_text_color(strtol(route_text_color.c_str(), nullptr, 16));
    routes.emplace(route->onestop_id(), routes.size());
  }
}

struct unique_transit_t {
  std::mutex lock;
  std::unordered_map<std::string, size_t> trips;
  std::unordered_map<std::string, size_t> block_ids;
  std::unordered_set<std::string> missing_routes;
};

bool get_stop_pairs(Transit& tile, unique_transit_t& uniques, const ptree& response,
    const std::unordered_map<std::string, uint64_t>& stops, const std::unordered_map<std::string, size_t>& routes) {
  bool dangles = false;
  for(const auto& pair_pt : response.get_child("schedule_stop_pairs")) {
    auto* pair = tile.add_stop_pairs();

    //origin
    pair->set_origin_onestop_id(pair_pt.second.get<std::string>("origin_onestop_id"));
    auto origin = stops.find(pair->origin_onestop_id());
    if(origin != stops.cend())
      pair->set_origin_graphid(origin->second);
    else {
      dangles = true;
      //TODO: remove this when stitching is proven to work
      tile.mutable_stop_pairs()->RemoveLast();
      continue;
    }

    //destination
    pair->set_destination_onestop_id(pair_pt.second.get<std::string>("destination_onestop_id"));
    auto destination = stops.find(pair->destination_onestop_id());
    if(destination != stops.cend())
      pair->set_destination_graphid(destination->second);
    else {
      dangles = true;
      //TODO: remove this when stitching is proven to work
      tile.mutable_stop_pairs()->RemoveLast();
      continue;
    }

    //route
    auto route_id = pair_pt.second.get<std::string>("route_onestop_id");
    auto route = routes.find(route_id);
    if(route == routes.cend()) {
      uniques.lock.lock();
      if(uniques.missing_routes.find(route_id) == uniques.missing_routes.cend()) {
        LOG_ERROR("No route " + route_id);
        uniques.missing_routes.emplace(route_id);
      }
      uniques.lock.unlock();
      tile.mutable_stop_pairs()->RemoveLast();
      continue;
    }
    pair->set_route_index(route->second);

    std::string origin_time = pair_pt.second.get<std::string>("origin_departure_time", "null");
    std::string dest_time = pair_pt.second.get<std::string>("destination_arrival_time", "null");
    if (origin_time == "null" || dest_time == "null") {
      LOG_ERROR("Origin or destination time not set: " + pair->origin_onestop_id() + " --> " + pair->destination_onestop_id());
      tile.mutable_stop_pairs()->RemoveLast();
      continue;
    }
    pair->set_origin_departure_time(origin_time);
    pair->set_destination_arrival_time(origin_time);

    //trip
    std::string t = pair_pt.second.get<std::string>("trip", "null");
    if (t == "null") {
      LOG_ERROR("No trip for pair: " + pair->origin_onestop_id() + " --> " + pair->destination_onestop_id());
      tile.mutable_stop_pairs()->RemoveLast();
      continue;
    }
    uniques.lock.lock();
    auto trip = uniques.trips.find(t);
    if(trip == uniques.trips.cend()) {
      pair->set_trip_key(uniques.trips.size());
      uniques.trips.emplace(t, uniques.trips.size());
    }
    else pair->set_trip_key(trip->second);
    uniques.lock.unlock();

    std::string block_id = pair_pt.second.get<std::string>("block_id", "null");
    if (block_id == "null") {
      pair->set_block_id(0);
    }
    else {
      uniques.lock.lock();
      auto b_id = uniques.block_ids.find(block_id);
      if (b_id == uniques.block_ids.cend()) {
        pair->set_block_id(uniques.block_ids.size());
        uniques.block_ids.emplace(block_id, uniques.block_ids.size());
      }
      else pair->set_block_id(b_id->second);
      uniques.lock.unlock();
    }

    pair->set_wheelchair_accessible(pair_pt.second.get<bool>("wheelchair_accessible", false));

    set_no_null(std::string, pair_pt.second, "service_start_date", "null", pair->set_service_start_date);
    set_no_null(std::string, pair_pt.second, "service_end_date", "null", pair->set_service_end_date);
    for(const auto& service_days : pair_pt.second.get_child("service_days_of_week")) {
      pair->add_service_days_of_week(service_days.second.get_value<bool>());
    }

    set_no_null(std::string, pair_pt.second, "origin_timezone", "null", pair->set_origin_timezone);
    set_no_null(std::string, pair_pt.second, "trip_headsign", "null", pair->set_trip_headsign);
    pair->set_bikes_allowed(pair_pt.second.get<bool>("bikes_allowed", false));

    const auto& except_dates = pair_pt.second.get_child_optional("service_except_dates");
    if (except_dates && !except_dates->empty()) {
      for(const auto& service_except_dates : pair_pt.second.get_child("service_except_dates")) {
        pair->add_service_except_dates(service_except_dates.second.get_value<std::string>());
      }
    }

    const auto& added_dates = pair_pt.second.get_child_optional("service_added_dates");
    if (added_dates && !added_dates->empty()) {
      for(const auto& service_added_dates : pair_pt.second.get_child("service_added_dates")) {
        pair->add_service_added_dates(service_added_dates.second.get_value<std::string>());
      }
    }
    //TODO: copy rest of attributes
  }
  return dangles;
}

void fetch_tiles(const ptree& pt, std::priority_queue<weighted_tile_t>& queue, unique_transit_t& uniques, std::promise<std::list<GraphId> >& promise) {
  TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
  const auto& tiles = hierarchy.levels().rbegin()->second.tiles;
  std::list<GraphId> dangling;
  curler_t curler;
  auto now = time(nullptr);
  auto* utc = gmtime(&now); utc->tm_year += 1900; ++utc->tm_mon; //TODO: use timezone code?

  //for each tile
  while(true) {
    GraphId current;
    uniques.lock.lock();
    if(queue.empty()) {
      uniques.lock.unlock();
      break;
    }
    current = queue.top().t;
    queue.pop();
    uniques.lock.unlock();
    auto bbox = tiles.TileBounds(current.tileid());
    ptree response;
    Transit tile;
    auto file_name = GraphTile::FileSuffix(current, hierarchy);
    file_name = file_name.substr(0, file_name.size() - 3) + "pbf";
    boost::filesystem::path transit_tile = pt.get<std::string>("mjolnir.transit_dir") + '/' + file_name;
    LOG_INFO("Fetching " + transit_tile.string());

    //pull out all the STOPS
    std::unordered_map<std::string, uint64_t> stops;
    auto key_param = "&api_key=" + pt.get<std::string>("api_key");
    boost::optional<std::string> request = (boost::format(pt.get<std::string>("base_url") +
      "/api/v1/stops?per_page=5000&bbox=%1%,%2%,%3%,%4%")
      % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy()).str();
    while(request) {
      //grab some stuff
      response = curler(*request + key_param, "stops");
      //copy stops in, keeping map of stopid to graphid
      get_stops(tile, stops, current, response, bbox);
      //please sir may i have some more?
      request = response.get_optional<std::string>("meta.next");
    }
    //um yeah.. we need these
    if(stops.size() == 0)
      continue;

    //pull out all operator WEBSITES
    request = (boost::format(pt.get<std::string>("base_url") +
      "/api/v1/operators?per_page=5000&bbox=%1%,%2%,%3%,%4%")
      % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy()).str();
    std::unordered_map<std::string, std::string> websites;
    while(request) {
      //grab some stuff
      response = curler(*request + key_param, "operators");
      //save the websites to a map
      for(const auto& operators_pt : response.get_child("operators")) {
        std::string onestop_id = operators_pt.second.get<std::string>("onestop_id", "");
        std::string website = operators_pt.second.get<std::string>("website", "");
        if(!onestop_id.empty() && onestop_id != "null" && !website.empty() && website != "null")
          websites.emplace(onestop_id, website);
      }
      //please sir may i have some more?
      request = response.get_optional<std::string>("meta.next");
    }

    //pull out all ROUTES
    request = (boost::format(pt.get<std::string>("base_url") +
      "/api/v1/routes?per_page=5000&bbox=%1%,%2%,%3%,%4%")
      % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy()).str();
    std::unordered_map<std::string, size_t> routes;
    while(request) {
      //grab some stuff
      response = curler(*request + key_param, "routes");
      //copy routes in, keeping track of routeid to route index
      get_routes(tile, routes, websites, response);
      //please sir may i have some more?
      request = response.get_optional<std::string>("meta.next");
    }

    //pull out all SCHEDULE_STOP_PAIRS
    bool dangles = false;
    request = (boost::format(pt.get<std::string>("base_url") +
      "/api/v1/schedule_stop_pairs?per_page=5000&bbox=%1%,%2%,%3%,%4%&service_from_date=%5%-%6%-%7%")
      % bbox.minx() % bbox.miny() % bbox.maxx() % bbox.maxy() % utc->tm_year % utc->tm_mon % utc->tm_mday).str();
    while(request) {
      //grab some stuff
      response = curler(*request + key_param, "schedule_stop_pairs");
      //copy pairs in, noting if any dont have stops
      dangles = get_stop_pairs(tile, uniques, response, stops, routes) || dangles;
      //please sir may i have some more?
      request = response.get_optional<std::string>("meta.next");
    }

    //remember who dangles
    if(dangles)
      dangling.emplace_back(current);

    //write pbf to file
    if (!boost::filesystem::exists(transit_tile.parent_path()))
      boost::filesystem::create_directories(transit_tile.parent_path());
    std::fstream stream(transit_tile.string(), std::ios::out | std::ios::trunc | std::ios::binary);
    tile.SerializeToOstream(&stream);
    LOG_INFO(transit_tile.string() + " had " + std::to_string(tile.stops_size()) + " stops " +
      std::to_string(tile.routes_size()) + " routes " + std::to_string(tile.stop_pairs_size()) + " stop pairs");
  }

  //give back the work for later
  promise.set_value(dangling);
}

std::list<GraphId> fetch(const ptree& pt, std::priority_queue<weighted_tile_t>& tiles) {
  unsigned int thread_count = std::max(static_cast<unsigned int>(1),
                                  pt.get<unsigned int>("mjolnir.concurrency", std::thread::hardware_concurrency()));
  LOG_INFO("Fetching " + std::to_string(tiles.size()) + " transit tiles with " + std::to_string(thread_count) + " threads...");

  //schedule some work
  unique_transit_t uniques;
  std::vector<std::shared_ptr<std::thread> > threads(thread_count);
  std::vector<std::promise<std::list<GraphId> > > promises(threads.size());
  for (size_t i = 0; i < threads.size(); ++i)
    threads[i].reset(new std::thread(fetch_tiles, std::cref(pt), std::ref(tiles), std::ref(uniques), std::ref(promises[i])));

  //let the threads finish and get the dangling list
  for (auto& thread : threads)
    thread->join();
  std::list<GraphId> dangling;
  for (auto& promise : promises) {
    try {
      dangling.splice(dangling.end(), promise.get_future().get());
    }
    catch(std::exception& e) {
      //TODO: throw further up the chain?
    }
  }

  LOG_INFO("Finished");
  return dangling;
}

Transit read_pbf(const std::string& file_name, std::mutex& lock) {
  lock.lock();
  std::fstream file(file_name, std::ios::in | std::ios::binary);
  if(!file) {
    throw std::runtime_error("Couldn't load " + file_name);
    lock.unlock();
  }
  std::string buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  lock.unlock();
  google::protobuf::io::ArrayInputStream as(static_cast<const void*>(buffer.c_str()), buffer.size());
  google::protobuf::io::CodedInputStream cs(static_cast<google::protobuf::io::ZeroCopyInputStream*>(&as));
  cs.SetTotalBytesLimit(buffer.size() * 2, buffer.size() * 2);
  Transit transit;
  if(!transit.ParseFromCodedStream(&cs))
    throw std::runtime_error("Couldn't load " + file_name);
  return transit;
}

//the tiles object didnt seem to jive with this notion of a neighbor
//so instead of implement this there its here.. maybe we can reevaluate later
std::list<GraphId> neighborhood(const Tiles<PointLL>& tiles, const GraphId& id) {
  auto uv = tiles.GetRowColumn(id.tileid());
  uv.first += tiles.ncolumns();
  std::list<GraphId> ids;
  for(auto x = uv.first - 1; x < uv.first + 1; ++x) {
    for(auto y = uv.second - 1; y < uv.second + 1; ++y) {
      if(x != uv.first && y != uv.second) {
        if(y < 0)
          ids.push_back(GraphId(tiles.TileId(x + tiles.ncolumns() / 2, tiles.nrows() + y), id.level(), 0));
        else if(tiles.nrows() - 1 < y)
          ids.push_back(GraphId(tiles.TileId(x + tiles.ncolumns() / 2, y - tiles.nrows()), id.level(), 0));
        else
          ids.push_back(GraphId(tiles.TileId(x % tiles.ncolumns(), y), id.level(), 0));
      }
    }
  }
  return ids;
}

using stitch_itr_t = std::list<GraphId>::const_iterator;
void stitch_tiles(const ptree& pt, stitch_itr_t start, stitch_itr_t end, std::mutex& lock) {
  TileHierarchy hierarchy(pt.get_child("mjolnir.hierarchy"));
  auto grid = hierarchy.levels().rbegin()->second.tiles;
  std::list<GraphId> dangling;
  auto tile_name = [&hierarchy, &pt](const GraphId& id){
    auto file_name = GraphTile::FileSuffix(id, hierarchy);
    file_name = file_name.substr(0, file_name.size() - 3) + "pbf";
    return pt.get<std::string>("mjolnir.transit_dir") + '/' + file_name;
  };

  //for each tile
  for(; start != end; ++start) {
    //open tile make a hash of missing stop to invalid graphid
    auto file_name = tile_name(*start);
    auto tile = read_pbf(file_name, lock);
    std::unordered_map<std::string, GraphId> needed;
    for(int i = 0; i < tile.stop_pairs_size(); ++i) {
      const auto& stop_pair = tile.stop_pairs(i);
      if(!stop_pair.has_origin_graphid())
        needed.emplace(stop_pair.origin_onestop_id(), GraphId{});
      if(!stop_pair.has_destination_onestop_id())
        needed.emplace(stop_pair.destination_onestop_id(), GraphId{});
    }

    //do while we have more to find and arent sick of searching
    std::unordered_set<GraphId> checked, last_round { *start };
    size_t found = 0;
    while(needed.size() != found) {
      //get the neighbors of the ones we just checked that havent been checked before
      std::unordered_set<GraphId> next_round;
      for(const auto& l : last_round) {
        checked.emplace(l);
        auto neighbors = neighborhood(grid, l);
        for(const auto& n : neighbors)
          if(checked.find(n) != checked.cend())
            next_round.emplace(n);
      }

      //crack each one open to see if it has anything we need
      for(const auto& neighbor_id : next_round) {
        auto neighbor_file_name = tile_name(neighbor_id);
        auto neighbor = read_pbf(neighbor_file_name, lock);
        for(int i = 0; i < neighbor.stops_size(); ++i) {
          const auto& stop = neighbor.stops(i);
          auto stop_itr = needed.find(stop.onestop_id());
          if(stop_itr != needed.cend()) {
            stop_itr->second = neighbor_id;
            ++found;
          }
        }
      }

      //next round
      last_round.swap(next_round);
    }

    //get the ids fixed up and write pbf to file
    for(int i = 0; i < tile.stop_pairs_size(); ++i) {
      auto* stop_pair = tile.mutable_stop_pairs(i);
      if(!stop_pair->has_origin_graphid()) {
        auto found = needed.find(stop_pair->origin_onestop_id())->second;
        if(found.Is_Valid())
          stop_pair->set_origin_graphid(found);
        else
          LOG_ERROR("Stop not found: " + stop_pair->origin_onestop_id());
      }
      if(!stop_pair->has_destination_onestop_id()) {
        auto found = needed.find(stop_pair->destination_onestop_id())->second;
        if(found.Is_Valid())
          stop_pair->set_destination_graphid(found);
        else
          LOG_ERROR("Stop not found: " + stop_pair->destination_onestop_id());
      }
    }
    lock.lock();
    std::fstream stream(file_name, std::ios::out | std::ios::trunc | std::ios::binary);
    tile.SerializeToOstream(&stream);
    lock.unlock();
    LOG_INFO(file_name + " had " + std::to_string(needed.size()) + " stitched stops");
  }
}

void stitch(const ptree& pt, const std::list<GraphId>& tiles) {
  unsigned int thread_count = std::max(static_cast<unsigned int>(1),
    pt.get<unsigned int>("mjolnir.concurrency", std::thread::hardware_concurrency()));
  LOG_INFO("Stitching " + std::to_string(tiles.size()) + " transit tiles with " + std::to_string(thread_count) + " threads...");

  //figure out where the work should go
  std::vector<std::shared_ptr<std::thread> > threads(thread_count);
  size_t floor = tiles.size() / threads.size();
  size_t at_ceiling = tiles.size() - (threads.size() * floor);
  stitch_itr_t tile_start, tile_end = tiles.begin();
  std::mutex lock;

  //make let them rip
  for (size_t i = 0; i < threads.size(); ++i) {
    size_t tile_count = (i < at_ceiling ? floor + 1 : floor);
    tile_start = tile_end;
    std::advance(tile_end, tile_count);
    threads[i].reset(
      new std::thread(stitch_tiles, std::cref(pt), tile_start, tile_end, std::ref(lock))
    );
  }

  //wait for them to finish
  for (auto& thread : threads)
    thread->join();

  LOG_INFO("Finished");
}

int main(int argc, char** argv) {
  if(argc < 2) {
    std::cerr << "Usage: " << std::string(argv[0]) << " valhalla_config transit_land_url transit_land_api_key " << std::endl;
    std::cerr << "Sample: " << std::string(argv[0]) << " conf/valhalla.json http://transit.land/ transitland-YOUR_KEY_SUFFIX" << std::endl;
    return 1;
  }

  //args and config file loading
  ptree pt;
  boost::property_tree::read_json(std::string(argv[1]), pt);
  pt.add("base_url", std::string(argv[2]));
  pt.add("api_key", std::string(argv[3]));

  //yes we want to curl
  curl_global_init(CURL_GLOBAL_DEFAULT);

  //go get information about what transit tiles we should be fetching
  auto transit_tiles = which_tiles(pt);

  //spawn threads to download all the tiles returning a list of
  //tiles that ended up having dangling stop pairs
  auto dangling_tiles = fetch(pt, transit_tiles);
  curl_global_cleanup();

  //spawn threads to connect dangling stop pairs to adjacent tiles' stops
  //stitch(pt, dangling_tiles);

  //TODO: show some summary informant?
  return 0;
}
