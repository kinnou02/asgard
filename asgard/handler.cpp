// Copyright 2017-2018, CanalTP and/or its affiliates. All rights reserved.
//
// LICENCE: This program is free software; you can redistribute it
// and/or modify it under the terms of the GNU Affero General Public
// License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with this program. If not, see
// <http://www.gnu.org/licenses/>.

#include "handler.h"
#include "context.h"

#include <boost/range/join.hpp>

using namespace valhalla;

namespace asgard {

static odin::DirectionsOptions make_default_directions_options() {
    odin::DirectionsOptions default_directions_options;
    for (int i = 0; i < 12; ++i) {
        default_directions_options.add_costing_options();
    }
    return default_directions_options;
}
static const odin::DirectionsOptions default_directions_options = make_default_directions_options();

static const std::map<std::string, sif::TravelMode> mode_map = {
    {"walking", sif::TravelMode::kPedestrian},
    {"bike", sif::TravelMode::kBicycle},
    {"car", sif::TravelMode::kDrive},
};

static size_t mode_index(const std::string& mode) {
    return static_cast<size_t>(mode_map.at(mode));
}

static odin::DirectionsOptions
make_costing_option(const std::string& mode, float speed) {
    odin::DirectionsOptions options = default_directions_options;
    speed *= 3.6;
    options.mutable_costing_options(odin::Costing::pedestrian)->set_walking_speed(speed);
    options.mutable_costing_options(odin::Costing::bicycle)->set_walking_speed(speed);
    return options;
}

static float get_distance(const std::string& mode, float duration) {
    using namespace thor;
    if (mode == "walking") {
        return duration * kTimeDistCostThresholdPedestrianDivisor;
    } else if (mode == "bike") {
        return duration * kTimeDistCostThresholdBicycleDivisor;
    } else {
        return duration * kTimeDistCostThresholdAutoDivisor;
    }
}

static odin::Costing to_costing(const std::string& mode) {
    if (mode == "walking") {
        return odin::Costing::pedestrian;
    } else if (mode == "bike") {
        return odin::Costing::bicycle;
    } else if (mode == "car") {
        return odin::Costing::auto_;
    } else {
        throw std::invalid_argument("Bad to_costing parameter");
    }
}

Handler::Handler(const Context& context):
    graph(context.ptree.get_child("mjolnir")),
    matrix(),
    factory(),
    mode_costing(),
    projector(context.max_cache_size)
{
    factory.Register(odin::Costing::auto_, sif::CreateAutoCost);
    factory.Register(odin::Costing::pedestrian, sif::CreatePedestrianCost);
    factory.Register(odin::Costing::bicycle, sif::CreateBicycleCost);

    mode_costing[mode_index("car")] = factory.Create(odin::Costing::auto_, default_directions_options);
    mode_costing[mode_index("walking")] = factory.Create(odin::Costing::pedestrian, default_directions_options);
    mode_costing[mode_index("bike")] = factory.Create(odin::Costing::bicycle, default_directions_options);
}

pbnavitia::Response Handler::handle(const pbnavitia::Request& request) {
    if(request.requested_api() != pbnavitia::street_network_routing_matrix && request.requested_api() != pbnavitia::direct_path){
        //empty response, jormun should be not too sad about it
        LOG_WARN("wrong request: aborting");
        return pbnavitia::Response();
    }
    LOG_INFO("Processing matrix request " +
             std::to_string(request.sn_routing_matrix().origins_size()) + "x" +
             std::to_string(request.sn_routing_matrix().destinations_size()));
    const std::string mode = request.sn_routing_matrix().mode();
    std::vector<std::string> sources;
    sources.reserve(request.sn_routing_matrix().origins_size());

    std::vector<std::string> targets;
    targets.reserve(request.sn_routing_matrix().destinations_size());

    for(const auto& e: request.sn_routing_matrix().origins()){
        sources.push_back(e.place());
    }
    for(const auto& e: request.sn_routing_matrix().destinations()){
        targets.push_back(e.place());
    }

    mode_costing[mode_index(mode)] = factory.Create(to_costing(mode), make_costing_option(mode, request.sn_routing_matrix().speed()));
    auto costing = mode_costing[mode_index(mode)];

    LOG_INFO("Projecting " + std::to_string(sources.size() + targets.size()) + " locations...");
    auto range = boost::range::join(sources, targets);
    auto path_locations = projector(begin(range), end(range), graph, mode, costing);
    LOG_INFO("Projecting locations done.");

    google::protobuf::RepeatedPtrField<odin::Location> path_location_sources;
    google::protobuf::RepeatedPtrField<odin::Location> path_location_targets;
    for (const auto& e: sources) {
        baldr::PathLocation::toPBF(path_locations.at(e), path_location_sources.Add(), graph);
    }
    for (const auto& e: targets) {
        baldr::PathLocation::toPBF(path_locations.at(e), path_location_targets.Add(), graph);
    }

    LOG_INFO("Computing matrix...");
    auto res = matrix.SourceToTarget(path_location_sources,
                                     path_location_targets,
                                     graph,
                                     mode_costing,
                                     mode_map.at(mode),
                                     get_distance(mode, request.sn_routing_matrix().max_duration()));
    LOG_INFO("Computing matrix done.");

    pbnavitia::Response response;
    int nb_unknown = 0;
    int nb_unreached = 0;
    //in fact jormun don't want a real matrix, only a vector of solution :(
    auto* row = response.mutable_sn_routing_matrix()->add_rows();
    assert(res.size() == sources.size() * targets.size());
    for (const auto& elt: res) {
        auto* k = row->add_routing_response();
        k->set_duration(elt.time);
        if (elt.time == thor::kMaxCost) {
            k->set_routing_status(pbnavitia::RoutingStatus::unknown);
            ++nb_unknown;
        } else if (elt.time > uint32_t(request.sn_routing_matrix().max_duration())) {
            k->set_routing_status(pbnavitia::RoutingStatus::unreached);
            ++nb_unreached;
        } else {
            k->set_routing_status(pbnavitia::RoutingStatus::reached);
        }
    }

    LOG_INFO("Request done with " +
             std::to_string(nb_unknown) + " unknown and " +
             std::to_string(nb_unreached) + " unreached");

    if (graph.OverCommitted()) { graph.Clear(); }
    LOG_INFO("Everything is clear.");

    return response;
}

}
