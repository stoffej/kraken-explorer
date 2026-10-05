// Measurement setup: networks, their interfaces and databases. Plain data plus free
// functions; read and written as the <setup> block of a workspace with pugixml, using
// the same element and attribute names as the Qt MeasurementSetup/Network/Interface.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <pugixml.hpp>

#include "core/bus_message.h"
#include "db/model/can_db.h"
#include "db/model/lin_db.h"

enum class LinProtocolVersion : uint8_t { V1_3, V2_0, V2_1, V2_2, V2_2A };
enum class LinNodeMode : uint8_t { Monitor, Master, Slave };

struct SetupInterface
{
    // Identity as saved in the workspace; the driver layer resolves it to `iface`.
    std::string driver;
    std::string name;
    int iface = -1;   // runtime index of the open interface, -1 = not resolved (not saved)

    BusType bus_type = BusType::CAN;
    bool configure = true;
    bool enabled = true;

    unsigned bitrate = 500000;
    unsigned sample_point = 875;   // per mille
    bool can_fd = false;
    unsigned fd_bitrate = 2000000;
    unsigned fd_sample_point = 875;

    bool listen_only = false;
    bool one_shot = false;
    bool triple_sampling = false;
    bool auto_restart = false;
    int auto_restart_ms = 100;

    bool is_custom_bitrate = false;
    bool is_custom_fd_bitrate = false;
    uint32_t custom_bitrate = 0x023407;
    uint32_t custom_fd_bitrate = 0x011508;

    unsigned lin_baudrate = 19200;
    LinProtocolVersion lin_protocol = LinProtocolVersion::V2_2A;
    LinNodeMode lin_node_mode = LinNodeMode::Master;
    bool lin_listen_only = false;
    bool lin_checksum_classic = false;
    std::string lin_ldf_path;
    std::string lin_schedule_table;
    uint8_t lin_schedule_table_index = 0;   // runtime only (not saved)
    std::string lin_slave_node;
    uint8_t lin_timebase_ms = 5;
    uint16_t lin_jitter_us = 0;
    std::map<uint8_t, std::vector<uint8_t>> lin_frame_defaults;   // frame id -> data
};

struct SetupNetwork
{
    std::string name;
    std::vector<SetupInterface> interfaces;
    // Shared, like the Qt QSharedPointer: a copied Setup (setup dialog) points at the
    // same databases, so CanDbMessage*/CanDbSignal* held elsewhere stay valid.
    std::vector<std::shared_ptr<CanDb>> can_dbs;
    std::vector<std::shared_ptr<LinDb>> lin_dbs;
    // Lookup caches, rebuilt by setup_rebuild_cache(); they point into the shared databases.
    std::unordered_map<uint32_t, CanDbMessage*> can_messages;   // key: DBC raw id, bit 31 = extended
    std::unordered_map<uint8_t, LinFrame*> lin_frames;
};

struct Setup
{
    std::vector<SetupNetwork> networks;
    uint64_t generation = 0;   // bumped on every cache rebuild; replaces onSetupChanged
};

// Reads the <network> children of `el`; DBC/LDF files are loaded from disk, failures
// are logged and skipped. Interfaces stay unresolved (iface = -1). Rebuilds the cache.
void setup_load_xml(Setup& setup, const pugi::xml_node& el);
// Appends one <network> per network to `root`.
void setup_save_xml(const Setup& setup, pugi::xml_node& root);

void setup_interface_load_xml(SetupInterface& intf, const pugi::xml_node& el);
void setup_interface_save_xml(const SetupInterface& intf, pugi::xml_node& el);

// Re-reads every DBC/LDF from its path (DBC in place via can_db_update_from, so held
// pointers survive; LinFrame* do not). Appends "path: reason" per failure to `errors`.
// Rebuilds the cache. Returns false if any file failed.
bool setup_reload_databases(Setup& setup, std::vector<std::string>* errors = nullptr);

void setup_rebuild_cache(Setup& setup);
// Loads a CAN database (DBC/DBF/SYM by extension) into `net`; a file already there is
// updated in place (held CanDbMessage*/CanDbSignal* survive). No cache rebuild. False on a
// parse failure, with `net` untouched.
bool setup_add_can_db(SetupNetwork& net, const std::string& path);
// Index of the network whose interfaces include runtime interface `iface`, -1 if none.
// Scans the interfaces on every call, so it is right even before setup_rebuild_cache()
// runs after interface resolution.
[[nodiscard]] int setup_network_of(const Setup& setup, uint16_t iface);
// Looks `m` up in the databases of the network of `m.iface` (standard and extended ids
// are distinct). Frames from an interface in no network search every network, first wins.
[[nodiscard]] const CanDbMessage* setup_find_can_message(const Setup& setup, const BusMessage& m);
[[nodiscard]] const LinFrame* setup_find_lin_frame(const Setup& setup, const BusMessage& m);
[[nodiscard]] SetupNetwork* setup_find_network(Setup& setup, std::string_view name);
